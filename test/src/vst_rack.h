#pragma once
// Runs the VST3 plugins next to Csound on the engine thread: once per Csound block it reads the channels the track code wrote
// (kP1..kP8, kNote/kVel, kMsg*, the audio input lines), runs the plugin and adds the plugin sound to Csound's master (spout).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "vst_job.h"
#include "monitor_capture.h"
#ifdef CAW_VST3
#include "plugin_host.h"
#endif

// Published instrument outputs of CAW's 4th engine (the app-side VSTi rack). A
// VSTe in the Csound-side rack that is wired to a VSTi source reads the source's
// latest stereo block here when the source is not in the same rack any more.
struct VstSoloOutBus {
    std::mutex mutex;
    std::map<int, std::vector<float>> outs;   // trackId -> interleaved stereo (one rendered block)
};
inline VstSoloOutBus& cawVstSoloOutBus() { static VstSoloOutBus bus; return bus; }

class VstRack {
public:
    using PtrFn = std::function<void*(const std::string&, int)>;     // channel name, flags -> memory (or nullptr)
    using TableFn = std::function<const void*(int)>;                 // table number -> memory (or nullptr)
    using LogFn = std::function<void(const std::string&)>;
    // Per-track output peaks of the 4th engine (its instruments never reach the Csound meter
    // layout, so the lane meters read them here). Values are max-held since the last take.
    struct SoloPeak { int trackId = 0; float l = 0.f, r = 0.f; };
    void takeSoloPeaks(std::vector<SoloPeak>& out) {
        std::lock_guard<std::mutex> lock(peakMutex_);
        out.clear();
        for (auto& entry : soloPeaks_) {
            out.push_back({entry.first, entry.second[0], entry.second[1]});
            entry.second[0] = entry.second[1] = 0.f;
        }
    }
    void clearSoloPeaks() {
        std::lock_guard<std::mutex> lock(peakMutex_);
        soloPeaks_.clear();
    }
    // Manual parameter override from the UI thread. Applied on the SAME block: it does not go
    // through any Csound channel, so nothing (a stopped clip, a running clip, automation) can
    // delay or revert it. It keeps winning for a short window after each push and releases as
    // soon as the channel value catches up (no automation) or the window ends (automation resumes).
    void pushParamOverride(int trackId, int slot, double value) {
        if (slot < 0 || slot >= 8 || !std::isfinite(value)) return;
        std::lock_guard<std::mutex> lock(overrideMutex_);
        VstParamOverride ov; ov.trackId = trackId; ov.slot = slot; ov.value = std::min(1.0, std::max(0.0, value));
        pending_.push_back(ov);
    }
    // Live re-pick of the P1..P8 parameter mapping (the UI picked or cleared a slot while the
    // engine runs): queued like a parameter override, applied before the next block's bookkeeping.
    struct VstPickUpdate { int trackId = 0; std::vector<int> pick; };
    void pushPickUpdate(int trackId, const std::vector<int>& pick) {
        std::lock_guard<std::mutex> lock(overrideMutex_);
        VstPickUpdate pu; pu.trackId = trackId; pu.pick = pick;
        pendingPicks_.push_back(std::move(pu));
    }
    // Live parameter edit from the plugin's own GUI for ANY parameter (not just the 8 picked
    // slots): queued on the UI thread, delivered to the engine instance's param ring so it is
    // applied on the SAME block. This is the "open the plugin and play with it live" path.
    struct VstParamValue { int trackId = 0; int index = 0; double value = 0; };
    void pushParamValue(int trackId, int paramIndex, double value) {
        if (paramIndex < 0 || paramIndex > 4095 || !std::isfinite(value)) return;
        std::lock_guard<std::mutex> lock(overrideMutex_);
        VstParamValue pv; pv.trackId = trackId; pv.index = paramIndex; pv.value = std::min(1.0, std::max(0.0, value));
        pendingValues_.push_back(pv);
    }
    // The 4th engine (app-side VSTi rack) has no Csound channels or note ring:
    // MIDI arrives directly. Call from the ONE thread that also calls process().
    void setSoloMode(bool solo) { soloMode_ = solo; }
    bool pushMidi(int trackId, const caw::MidiMsg& m) {
        for (Rt& r : rts_)
            if (r.job.id == trackId && r.job.instrument && r.ep) { r.ep->midiIn.push(m); return true; }
        return false;
    }
    void sendAllNotesOff() {
        for (Rt& r : rts_)
            if (r.job.instrument && r.ep) { caw::MidiMsg m; m.status = 0xB0; m.d1 = 123; m.d2 = 0; r.ep->midiIn.push(m); }
    }
    // The jobs the rack was last initialized with (the 4th engine skips re-init
    // when nothing changed, so Play does not reload the plugins every time).
    const std::vector<VstJob>& jobs() const { return jobs_; }
#ifndef CAW_VST3
    bool init(const std::vector<VstJob>& jobsIn, int, int, int, int, PtrFn, TableFn, LogFn log, bool = false, VstParamMirror* = nullptr, const VstEngineAudio* = nullptr) { log_ = log; jobs_ = jobsIn; return true; }
    void clear() {}
    size_t loaded() const { return 0; }
    void process(void*, MonitorCapture* = nullptr, float = 1.f, const std::set<int>* = nullptr, bool = false, const std::map<int, unsigned>* = nullptr) {}
#else
    // Unload every plugin (the 4th engine restarts with a new set of instruments). The caller makes
    // sure process() is not running on another thread while this runs.
    void clear() {
        rts_.clear(); jobs_.clear();
        std::lock_guard<std::mutex> lock(overrideMutex_);
        pending_.clear(); pendingPicks_.clear(); pendingValues_.clear();
    }
    size_t loaded() const { return rts_.size(); }   // how many plugins really loaded
    bool init(const std::vector<VstJob>& jobs, int sr, int block, int nchan, int sampleBytes, PtrFn ptr, TableFn table, LogFn log,
              bool strict = false, VstParamMirror* mirror = nullptr, const VstEngineAudio* engineAudio = nullptr) {
        sb_ = sampleBytes; block_ = block; nchan_ = nchan; mirror_ = mirror; ea_ = engineAudio; log_ = log;
        ptr_ = ptr; tab_ = table; jobs_ = jobs;
        for (const VstJob& j : jobs) {
            if (j.path.empty()) { log("[caw] VST track \"" + j.name + "\": no plugin loaded, skipped.\n"); if (strict) return false; continue; }
            Rt r; r.job = j; r.plug = caw::makeVst3Plugin(); std::string err;
            if (!r.plug->load(j.path, err)) { log("[caw] VST track \"" + j.name + "\": " + err + "\n"); if (strict) return false; continue; }
            for (const auto& parameter : r.plug->params()) {
                const auto saved = j.parameterState.find(parameter.id);
                if (!parameter.readOnly && saved != j.parameterState.end())
                    r.plug->setParam(parameter.index, saved->second);
            }
            r.plug->prepare(sr, block);
            r.ep = std::make_shared<caw::Endpoint>();
            const std::string base = "cawvst_" + std::to_string(j.id) + "_";
            for (size_t k = 0; k < 8 && k < j.pick.size(); ++k) r.p[k] = ptr(base + "p" + std::to_string(k + 1), 1 | 32);
            r.wr = ptr(base + "wr", 1 | 32);
            void* tabNo = ptr(base + "tab", 1 | 32);
            if (tabNo) r.tab = table((int)std::lround(rd(tabNo)));
            if (strict && (!r.wr || !r.tab)) { log("[caw] VST MIDI bridge unavailable.\n"); return false; }
            if (!j.instrument && j.routedIn) {   // the input lines: track / clips are Csound channels; master slots read the spout
                for (int k = 0; k < 8; ++k) r.inCh[k] = ptr("cawvstin_" + std::to_string(j.id) + "_" + std::to_string(k + 1), 2 | 16 | 32);
                // Say what the VSTe is wired to, and WARN when the Csound channels are missing: the
                // usual reason is a routing that was configured after Run (the running CSD then has
                // no chnmix for it and the slots stay silent until the next Run).
                std::string sum; int needed = 0, missing = 0;
                for (int s = 0; s < 8; ++s) {
                    const auto& src = j.inputs[s];
                    if (!src.on || src.lineCount <= 0) continue;
                    if (!sum.empty()) sum += ", ";
                    sum += "in" + std::to_string(s + 1) + ": ";
                    if (src.mode == 2) sum += "master";
                    else if (src.engine == 1) sum += "PureData mix";
                    else if (src.engine == 2) sum += "SuperCollider mix";
                    else if (src.engine == 3) sum += "\"" + src.name + "\" (VST output, rack-wired) -> lines " + std::to_string(src.lineStart + 1) + "-" + std::to_string(src.lineStart + src.lineCount);
                    else {
                        sum += "\"" + src.name + "\" -> lines " + std::to_string(src.lineStart + 1) + "-" + std::to_string(src.lineStart + src.lineCount);
                        for (int k = 0; k < src.lineCount && src.lineStart + k < 8; ++k) {
                            ++needed;
                            if (!r.inCh[src.lineStart + k]) ++missing;
                        }
                    }
                }
                log("[caw] VSTe \"" + j.name + "\" inputs: " + (sum.empty() ? "(none enabled)" : sum) + "\n");
                if (missing > 0)
                    log("[caw] VSTe \"" + j.name + "\": " + std::to_string(missing) + " of " + std::to_string(needed)
                        + " input channels are MISSING from the running Csound - the routing was configured after Run. Press Run again.\n");
            }
            const int chPass = j.routedIn ? 8 : 2;
            r.in.assign((size_t)block * chPass, 0.f); r.out.assign((size_t)block * chPass, 0.f);
            r.last.assign(8, -1.0);
            log("[caw] VST track \"" + j.name + "\" loaded (" + (j.instrument ? "instrument" : "effect") + ").\n");
            rts_.push_back(std::move(r));
        }
        // instruments first, then effects (an instrument can feed an effect in the same block; the other way round costs one block)
        std::stable_sort(rts_.begin(), rts_.end(), [](const Rt& a, const Rt& b) { return a.job.instrument && !b.job.instrument; });
        return true;
    }
    // active / activeValid: when activeValid is true, only tracks whose id is in `active` are processed
    // (live M / S). inputMasks: bit i set = input slot i of that track feeds the plugin (live on/off);
    // a missing entry means all configured inputs are on.
    void process(void* spout, MonitorCapture* capture = nullptr, float fullScale = 1.f,
                 const std::set<int>* active = nullptr, bool activeValid = false,
                 const std::map<int, unsigned>* inputMasks = nullptr) {
        if (!std::isfinite(fullScale) || fullScale <= 0.f) fullScale = 1.f;
        // How loud is the rest of the mix right now? Only used by the silence diagnostics below:
        // when NOTHING else sounds, a silent VSTe input is normal (the project is idle) and must
        // not produce a hint.
        float spoutPeak = 0.f;
        if (spout)
            for (int n = 0; n < block_; ++n)
                for (int c = 0; c < nchan_; ++c) spoutPeak = std::max(spoutPeak, std::fabs(rdSpout(spout, n, c)));
        // Take the manual overrides queued by the UI and refresh the per-slot manual state, any
        // live pick updates (slots re-picked while running) and the live GUI parameter edits.
        {
            std::vector<VstParamOverride> now;
            std::vector<VstPickUpdate> picks;
            std::vector<VstParamValue> valuesNow;
            { std::lock_guard<std::mutex> lock(overrideMutex_); now.swap(pending_); picks.swap(pendingPicks_); valuesNow.swap(pendingValues_); }
            for (Rt& r : rts_)
                for (const VstParamValue& pv : valuesNow)
                    if (pv.trackId == r.job.id && pv.index < (int)r.plug->params().size()
                        && !r.plug->params()[(size_t)pv.index].readOnly) {
                        caw::ParamMsg m; m.paramIndex = pv.index; m.value = pv.value;
                        r.ep->paramIn.push(m);   // the engine instance applies it on this very block
                    }
            for (Rt& r : rts_)
                for (const VstPickUpdate& pu : picks)
                    if (pu.trackId == r.job.id && pu.pick != r.job.pick) {
                        r.job.pick = pu.pick;
                        if (r.job.pick.size() > 8) r.job.pick.resize(8);
                        for (int k = 0; k < 8; ++k) { r.last[k] = -1.0; r.manualActive[k] = false; r.loggedActive[k] = false; }
                        if (log_) log_("[caw] live picks of \"" + r.job.name + "\" updated (" + std::to_string(r.job.pick.size()) + " slots).\n");
                    }
            for (Rt& r : rts_)
                for (const VstParamOverride& ov : now)
                    if (ov.trackId == r.job.id) {
                        r.manual[(size_t)ov.slot] = ov.value;
                        r.manualActive[(size_t)ov.slot] = true;
                        r.manualUntil[(size_t)ov.slot] = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count() + 2500;   // each drag event refreshes the hold: a quick tweak never snaps back
                        // In the 4th engine (or before the channels exist) there is no lane to
                        // compare against: the slot value goes straight to the plugin.
                        if (soloMode_ && !r.p[ov.slot]) {
                            const int pick = ov.slot < (int)r.job.pick.size() ? r.job.pick[(size_t)ov.slot] : -1;
                            if (pick >= 0 && (size_t)pick < r.plug->params().size()) {
                                caw::ParamMsg m; m.paramIndex = pick; m.value = ov.value;
                                r.ep->paramIn.push(m);
                                r.last[(size_t)ov.slot] = ov.value;   // the UI mirror follows the scripted value
                            } else if (!r.unpickedWarned[(size_t)ov.slot]) {
                                r.unpickedWarned[(size_t)ov.slot] = true;
                                if (log_) log_("[caw] clip code writes P" + std::to_string(ov.slot + 1) + " but no parameter is picked for P"
                                    + std::to_string(ov.slot + 1) + " on \"" + r.job.name + "\" (open the instrument panel and pick one).\n");
                            }
                        }
                    }
        }
        // One SC-mix pull per block (several VSTe tracks may consume the same mix).
        bool needSc = false;
        for (const Rt& r : rts_)
            for (const auto& src : r.job.inputs)
                if (src.on && src.engine == 2 && src.lineCount > 0) needSc = true;
        if (needSc && ea_ && ea_->scRing) {
            scScratch_.resize((size_t)block_ * 2);
            ea_->scRing->pop(scScratch_.data(), (size_t)block_);
        } else needSc = false;
        for (Rt& r : rts_) {
            const bool isActive = !(activeValid && active && active->find(r.job.id) == active->end());   // live M / S
            unsigned mask = 0xFFu;
            if (inputMasks) { const auto it = inputMasks->find(r.job.id); if (it != inputMasks->end()) mask = it->second; }
            const size_t np = r.plug->params().size();
            // the 8 picked parameters: the channel value, with a fresh manual override winning on
            // the SAME block it arrives. Channel value and UI mirror are updated even for a muted
            // track, so the plugin GUI never fights a stale mirror; a track that comes back from
            // mute force-syncs its plugin to the current values.
            const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            for (int k = 0; k < 8 && k < (int)r.job.pick.size(); ++k) {
                if (!r.p[k] || r.job.pick[k] < 0 || (size_t)r.job.pick[k] >= np) continue;
                const double lane = std::min(1.0, std::max(0.0, rd(r.p[k])));
                const bool laneMoving = std::fabs(lane - r.prevLane[(size_t)k]) > 1e-6;   // automation is writing the lane
                r.prevLane[(size_t)k] = lane;
                double v = lane;
                if (r.manualActive[(size_t)k]) {
                    if (std::fabs(lane - r.manual[(size_t)k]) <= 1e-6)
                        r.manualActive[(size_t)k] = false;        // the channel caught up: the lane carries the manual value
                    else if (nowMs > r.manualUntil[(size_t)k] && laneMoving)
                        r.manualActive[(size_t)k] = false;        // automation moved the lane after the window: it resumes
                    else v = r.manual[(size_t)k];                 // the manual value wins on this very block (and holds while the lane is static)
                }
                const bool changed = std::fabs(v - r.last[k]) > 1e-6;
                r.last[k] = v;
                // Prove in the console that a live edit arrived (and that automation took back over).
                if (r.manualActive[(size_t)k] && (!r.loggedActive[(size_t)k] || std::fabs(r.manual[(size_t)k] - r.loggedManual[(size_t)k]) > 1e-6)) {
                    r.loggedActive[(size_t)k] = true; r.loggedManual[(size_t)k] = r.manual[(size_t)k];
                    if (log_) log_("[caw] live param: \"" + r.job.name + "\" P" + std::to_string(k + 1) + " -> " + std::to_string(r.manual[(size_t)k]) + "\n");
                } else if (!r.manualActive[(size_t)k] && r.loggedActive[(size_t)k]) {
                    r.loggedActive[(size_t)k] = false;
                    if (log_) log_("[caw] live param: \"" + r.job.name + "\" P" + std::to_string(k + 1) + " released (lane automation took over)\n");
                }
                if (isActive && (changed || !r.wasActive)) { caw::ParamMsg m; m.paramIndex = r.job.pick[k]; m.value = v; r.ep->paramIn.push(m); }
            }
            r.wasActive = isActive;
            if (mirror_) {   // publish what the engine is actually using (the UI mirrors it into the plugin GUI)
                std::lock_guard<std::mutex> lock(mirror_->mutex);
                auto& dst = mirror_->values[r.job.id];
                for (int k = 0; k < 8 && k < (int)r.job.pick.size(); ++k) {
                    double v = r.last[k];
                    if (v < 0.0) v = k < (int)r.job.init.size() ? std::min(1.0, std::max(0.0, r.job.init[(size_t)k])) : 0.5;   // never seen: the pick's start value
                    dst[(size_t)k] = v;
                }
            }
            if (!isActive) continue;   // muted / not soloed: nothing below runs, the above bookkeeping keeps the UI honest
            // MIDI written by the code (note, cc, msg) and by MIDI clips: a ring of 1024 messages in a Csound table
            if (r.wr && !r.tab && ptr_ && tab_) {   // the table publishes right after the compile - keep trying until it shows up
                void* tabNo = ptr_("cawvst_" + std::to_string(r.job.id) + "_tab", 1 | 32);
                if (tabNo) {
                    const int no = (int)std::lround(rd(tabNo));
                    if (no > 0) { r.tab = tab_(no); if (r.tab) { r.bridgeWarned = false; r.lastW = 0; } }
                }
            }
            if (r.wr && r.tab) {
                long w = std::lround(rd(r.wr));
                if (w - r.lastW > 1024) r.lastW = w - 1024;
                for (long n = r.lastW + 1; n <= w; ++n) {
                    const int idx = (int)(n % 1024) * 4;
                    caw::MidiMsg m;
                    const int st = (int)std::lround(rdAt(r.tab, idx));
                    if (st < 0x80 || st > 0xFF) continue;
                    m.status = (uint8_t)st; m.d1 = (uint8_t)std::min(127L, std::max(0L, std::lround(rdAt(r.tab, idx + 1))));
                    m.d2 = (uint8_t)std::min(127L, std::max(0L, std::lround(rdAt(r.tab, idx + 2))));
                    r.ep->midiIn.push(m);
                    ++r.midiCount;
                }
                if (w > r.lastW) r.lastW = w;
                if (!r.midiSeen && r.midiCount > 0) {
                    r.midiSeen = true;
                    if (log_) log_("[caw] VSTi \"" + r.job.name + "\": MIDI bridge live - notes are reaching the plugin.\n");
                }
            } else if (!r.bridgeWarned && !soloMode_) {
                r.bridgeWarned = true;
                if (log_) log_("[caw] VSTi \"" + r.job.name + "\": the MIDI bridge is NOT connected"
                               + std::string(r.wr ? "" : " (the note counter channel is missing)")
                               + std::string(r.tab ? "" : " (the note ring table did not publish)")
                               + " - notes cannot reach the plugin; start the engine (Run) again.\n");
            }
            const float* in = nullptr;
            const int chPass = r.job.routedIn ? 8 : 2;
            int packedLines = 0;               // how many input lines the plugin actually hears (for the diagnostics)
            if (!r.job.instrument && r.job.routedIn) {
                for (std::size_t i = 0; i < (std::size_t)block_ * 8; ++i) r.in[i] = 0.f;
                // Pack the ENABLED input slots from line 1: a slot switched off live just disappears
                // and the later inputs move up, so a stereo plugin always hears its first enabled input.
                const float* pdMix = ea_ && ea_->pdMix ? ea_->pdMix->load(std::memory_order_acquire) : nullptr;
                const int pdChans = ea_ ? ea_->pdMixChannels : 0;
                int outLine = 0;
                for (int i = 0; i < 8 && outLine < 8; ++i) {
                    const auto& src = r.job.inputs[i];
                    if (!src.on || !(mask & (1u << i)) || src.lineCount <= 0) continue;
                    for (int k = 0; k < src.lineCount && outLine < 8; ++k, ++outLine) {
                        if (src.mode == 2) {   // master: first line = L, second = R (from the pre-gain master mix)
                            const int mc = k == 0 ? 0 : (nchan_ > 1 ? 1 : 0);
                            if (spout) for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = rdSpout(spout, n, mc) / fullScale;
                        } else if (src.engine == 1) {   // the Pure Data mix (device scale): line 1 = L, line 2 = R
                            const int mc = k == 0 ? 0 : 1;
                            if (pdMix && mc < pdChans)
                                for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = pdMix[(std::size_t)n * pdChans + mc];
                        } else if (src.engine == 2) {   // the SuperCollider mix (device scale): line 1 = L, line 2 = R
                            const int mc = k == 0 ? 0 : 1;
                            if (needSc) for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = scScratch_[(std::size_t)n * 2 + mc];
                        } else if (src.engine == 3) {   // a VSTi / VSTe source: the rack renders it too, so wire its stereo output here directly
                            const Rt* srcRt = nullptr;
                            for (const Rt& o : rts_) if (o.job.id == src.srcId) { srcRt = &o; break; }
                            const bool srcActive = srcRt && !(activeValid && active && active->find(srcRt->job.id) == active->end());
                            const int sCh = srcRt ? (srcRt->job.routedIn ? 8 : 2) : 2;
                            const int mc = k == 0 ? 0 : (sCh > 1 ? 1 : 0);   // line 1 = L, line 2 = R - the same channels the source adds to the master
                            if (srcRt && srcActive && mc < sCh)
                                for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = srcRt->out[(std::size_t)n * sCh + mc];
                            if (!srcRt) {   // the source instrument renders in the 4th engine (app side): take its latest block from the shared bus
                                std::lock_guard<std::mutex> busLock(cawVstSoloOutBus().mutex);
                                const auto it = cawVstSoloOutBus().outs.find(src.srcId);
                                if (it != cawVstSoloOutBus().outs.end() && it->second.size() >= (size_t)2 * block_)
                                    for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = it->second[(std::size_t)n * 2 + mc];
                            }
                        } else {
                            const int line = src.lineStart + k;
                            if (line < 8 && r.inCh[line])
                                for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = (float)rdAt(r.inCh[line], n) / fullScale;
                        }
                    }
                }
                packedLines = outLine;
                in = r.in.data();   // 8 lines, one signal per channel - no mixdown (the plugin hears min(8, its input bus) of them)
            }
            r.plug->process(in, r.out.data(), block_, chPass, *r.ep);
            float peakL = 0.f, peakR = 0.f;
            if (capture || soloMode_) {
                const int rc = chPass > 1 ? 1 : 0;   // a mono plugin has one output channel
                for (int i = 0; i < block_; ++i) {
                    const float l = r.out[(size_t)i * chPass];
                    const float rr = r.out[(size_t)i * chPass + rc];
                    if (std::isfinite(l)) peakL = std::max(peakL, std::fabs(l));
                    if (std::isfinite(rr)) peakR = std::max(peakR, std::fabs(rr));
                }
            }
            if (capture && r.job.meterFirst >= 0) {
                capture->addMeter(r.job.meterFirst, peakL);
                capture->addMeter(r.job.meterFirst + 1, peakR);
            }
            if (soloMode_) {   // the lane meters of the VSTi tracks read these (max-held until the UI takes them)
                std::lock_guard<std::mutex> peakLock(peakMutex_);
                auto& entry = soloPeaks_[r.job.id];
                entry[0] = std::max(entry[0], peakL);
                entry[1] = std::max(entry[1], peakR);
            }
            // Silence diagnostics (rate-limited): instead of an unexplained silence, the console
            // says WHY a routed VSTe hears nothing. Fires only when the rest of the mix IS sounding
            // (spoutPeak > 0), so an idle project stays quiet.
            if (packedLines > 0) {
                float inPk = 0.f;
                for (int n = 0; n < block_; ++n)
                    for (int c = 0; c < packedLines; ++c) inPk = std::max(inPk, std::fabs(r.in[(std::size_t)n * 8 + c]));
                float outPk = 0.f;
                for (int n = 0; n < block_; ++n)
                    outPk = std::max(outPk, std::fabs(r.out[(std::size_t)n * chPass]));
                if (inPk < 1e-6f) {
                    if (r.inSilentSince == 0) r.inSilentSince = nowMs;
                    if (spoutPeak > 1e-6f && nowMs - r.inSilentSince > 1000 && nowMs - r.inHintAt > 10000) {
                        r.inHintAt = nowMs;
                        if (log_) log_("[caw] VSTe \"" + r.job.name + "\": input audio is SILENT while other audio plays. Sources must PLAY on the timeline; a source configured after Run needs a new Run; a MUTED source stays silent; a VSTi / VSTe source is wired by the rack (needs the Run with the routing already set).\n");
                    }
                } else r.inSilentSince = 0;
                if (inPk >= 1e-6f && outPk < 1e-6f) {
                    if (r.outSilentSince == 0) r.outSilentSince = nowMs;
                    if (nowMs - r.outSilentSince > 1000 && nowMs - r.outHintAt > 10000) {
                        r.outHintAt = nowMs;
                        if (log_) log_("[caw] VSTe \"" + r.job.name + "\": the input HAS signal but the plugin outputs silence - check its parameters (e.g. wet 0%) or open the editor.\n");
                    }
                } else r.outSilentSince = 0;
            }
            // VSTi diagnostics: an instrument outputs silence when no notes reach it - the plugin
            // editor's own keyboard does not feed the engine. Fires only while the rest of the mix
            // sounds, so a stopped or silent project stays quiet.
            if (r.job.instrument) {
                float instPk = 0.f;
                for (int n = 0; n < block_; ++n)
                    for (int c = 0; c < chPass; ++c) instPk = std::max(instPk, std::fabs(r.out[(std::size_t)n * chPass + c]));
                if (instPk < 1e-6f) {
                    if (r.instSilentSince == 0) r.instSilentSince = nowMs;
                    // Fires even when the VSTi is the ONLY thing in the project (15 s of running
                    // silence), at most twice per run, and says which side of the bridge is mute.
                    if (nowMs - r.instSilentSince > 15000 && nowMs - r.instHintAt > 20000 && r.silentHints < 2) {
                        r.instHintAt = nowMs; ++r.silentHints;
                        if (log_) log_(r.midiSeen
                            ? "[caw] VSTi \"" + r.job.name + "\": notes ARE reaching the plugin but it outputs silence - check the plugin's own state (preset / patch loaded? master volume? many synths start silent until a preset is chosen). Open its editor.\n"
                            : "[caw] VSTi \"" + r.job.name + "\": the plugin runs but NO notes reached it. Right-click the track > Add MIDI clip and draw notes, or write  note 60, 100, 0.4  in a clip's CAW code, then press Play. The plugin editor's keyboard does not feed the engine.\n");
                    }
                } else r.instSilentSince = 0;
                // The 4th engine publishes each instrument's stereo output so a
                // VSTe in the Csound-side rack can still be wired to a VSTi source.
                if (soloMode_) {
                    std::lock_guard<std::mutex> busLock(cawVstSoloOutBus().mutex);
                    std::vector<float>& dst = cawVstSoloOutBus().outs[r.job.id];
                    dst.resize((size_t)block_ * 2);
                    for (int n = 0; n < block_; ++n) {
                        dst[(size_t)n * 2] = r.out[(size_t)n * chPass];
                        dst[(size_t)n * 2 + 1] = r.out[(size_t)n * chPass + 1];
                    }
                }
            }
            // The VSTe output always goes to the master (spout). Whether the SOURCE track also
            // reaches the master (mix) or is replaced (master) is decided in the CSD builder, which
            // suppresses the source clip's own `out` lines for a master-mode VSTe.
            for (int i = 0; i < block_; ++i) for (int c = 0; c < nchan_ && c < chPass; ++c) add(spout, (size_t)i * nchan_ + c, r.out[(size_t)i * chPass + c] * fullScale);
        }
        // The senders chnmixed the current pass into the input channels while Csound ran (perform
        // happens before this call), and they were read in the loop above - so clear them NOW, for
        // every routed VSTe (active or muted), and the next pass mixes into fresh channels again.
        // Clearing BEFORE the read destroys exactly the block Csound just wrote: the rack would read
        // silence on every block (that was the fixed3 bug).
        for (Rt& r : rts_)
            if (!r.job.instrument && r.job.routedIn)
                for (int c = 0; c < 8; ++c) if (r.inCh[c]) clearBus(r.inCh[c]);
    }
#endif
private:
    // The manual override queue and the SC-mix scratch do not depend on the VST3 SDK, so they
    // exist in both builds (pushParamOverride is a public method of this class either way).
    std::mutex overrideMutex_; std::vector<VstParamOverride> pending_;
    std::vector<VstPickUpdate> pendingPicks_;          // live re-picks queued by the UI
    std::vector<VstParamValue> pendingValues_;         // live GUI edits for ANY parameter, queued by the UI
    std::vector<float> scScratch_;                     // one SC-mix pull per block, shared by all consumers
    LogFn log_;                                        // kept from init so process() can log diagnostics
#ifdef CAW_VST3
    struct Rt {
        VstJob job; std::unique_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
        void* p[8] = {}; void *wr = nullptr; const void* tab = nullptr; long lastW = 0;
        long midiCount = 0; bool midiSeen = false; bool bridgeWarned = false; int silentHints = 0;   // MIDI bridge diagnostics
        void* inCh[8] = {};
        std::vector<double> last; std::vector<float> in, out;
        bool wasActive = true;   // a track coming back from mute force-syncs its plugin to the channels
        double manual[8] = {}; bool manualActive[8] = {}; long long manualUntil[8] = {};   // live manual overrides
        double prevLane[8] = {};   // the raw lane values of the previous block (detects automation movement)
        double loggedManual[8] = {}; bool loggedActive[8] = {};   // live-param logging state
        bool unpickedWarned[8] = {};   // solo mode: P slot written by clip code but not picked (warn once)
        long long inSilentSince = 0, inHintAt = 0, outSilentSince = 0, outHintAt = 0;   // silence diagnostics
        long long instSilentSince = 0, instHintAt = 0;   // VSTi: silence while other audio plays = no notes reached the plugin
    };
    double rd(const void* p) const { return sb_ == 8 ? *static_cast<const double*>(p) : (double)*static_cast<const float*>(p); }
    double rdAt(const void* p, int i) const { return sb_ == 8 ? static_cast<const double*>(p)[i] : (double)static_cast<const float*>(p)[i]; }
    float rdSpout(const void* spout, int i, int c) const { return sb_ == 8 ? (float)static_cast<const double*>(spout)[(std::size_t)i * nchan_ + c] : static_cast<const float*>(spout)[(std::size_t)i * nchan_ + c]; }
    void clearBus(void* p) const { if (sb_ == 8) std::fill_n(static_cast<double*>(p), block_, 0.0); else std::fill_n(static_cast<float*>(p), block_, 0.f); }
    void add(void* b, size_t i, float v) const { if (sb_ == 8) static_cast<double*>(b)[i] += v; else static_cast<float*>(b)[i] += v; }
    std::vector<Rt> rts_; int sb_ = 8, block_ = 0, nchan_ = 2; VstParamMirror* mirror_ = nullptr;
    PtrFn ptr_{}; TableFn tab_{};   // kept from init so process() can re-resolve a late-publishing note table
    std::vector<VstJob> jobs_;      // the init arguments (the 4th engine reuses a matching init)
    bool soloMode_ = false;         // true when this rack is the app-side 4th engine (no Csound channels)
    const VstEngineAudio* ea_ = nullptr;
#endif
    std::mutex peakMutex_;                                        // soloMode_ meter peaks, audio thread -> UI
    std::map<int, std::array<float, 2>> soloPeaks_;
};
