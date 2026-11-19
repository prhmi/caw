#pragma once
// The VST graph of one engine: once per audio block it reads what the track code wrote (kP1..kP8,
// the note ring, the audio input lines), runs the plugins in track order - instruments first, so
// a VSTi can feed a VSTe in the SAME block - and mixes the result into the engine's master (the
// Csound spout). Effects, instruments and their replacements all live in this one rack: that is
// the whole VST graph, wherever it is hosted. Notes arrive on the host's own transport: while
// Csound hosts, its score plays the MIDI clips into the note ring (instr CawVstNote_<id>), so the
// instrument and the Csound clips share one clock; while the 4th engine hosts, the app fires
// them through pushMidi on its block clock.
//
// Live plugin changes NEVER load anything on the audio thread: queueJobs() (UI thread) compares
// the graph signature and hands the new job set to a loader thread, which loads ONLY the new /
// changed tracks (every other instance keeps playing, voices intact), builds a fresh graph and
// publishes it. The audio thread adopts the published graph at the top of a block - a pointer
// swap - and the old graph is destroyed by the loader thread, never inside the callback. A
// plugin that fails or throws only loses its own track; the previous graph keeps sounding.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "vst_job.h"
#include "monitor_capture.h"
#include "caw_bus.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>   // the loader thread needs its own COM apartment
#endif
#ifdef CAW_VST3
#include "plugin_host.h"
#endif

class VstRack {
public:
    using PtrFn = std::function<void*(const std::string&, int)>;     // channel name, flags -> memory (or nullptr)
    using TableFn = std::function<const void*(int)>;                 // table number -> memory (or nullptr)
    using LogFn = std::function<void(const std::string&)>;
    // Per-track output peaks of the 4th engine (its instruments never reach the Csound meter
    // layout, so the lane meters read them here). Values are max-held since the last take.
    struct SoloPeak { int trackId = 0; float l = 0.f, r = 0.f; };
    ~VstRack() { clear(); }
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
    // Live VSTe input on/off while the engine runs (the 4th engine uses this; the Csound rack
    // receives the same masks through process()). UI thread.
    void setInputMasks(const std::map<int, unsigned>& masks) {
        std::lock_guard<std::mutex> lock(maskMutex_);
        masks_ = masks;
    }
    // Live AUDIO OUTPUT of a VSTe: 0 = replace the source in the master, 1 = mix. UI thread.
    void setOutMode(int trackId, int mode) {
        std::lock_guard<std::mutex> lock(outModeMutex_);
        outModes_[trackId] = mode == 1 ? 1 : 0;
    }
    // Live re-wire of the effect inputs / output mode while the engine runs (the plugin
    // instances stay loaded - nothing is destroyed). Applied on the next block. UI thread.
    void setWiring(const std::vector<VstJob>& jobs) {
        std::lock_guard<std::mutex> lock(wiringMutex_);
        wiring_ = jobs;
        wiringValid_ = true;
    }
    bool pushMidi(int trackId, const caw::MidiMsg& m) {
#ifdef CAW_VST3
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return false;
        for (Rt& r : g->rts)
            if (r.job.id == trackId && r.job.instrument && r.ep) { r.ep->midiIn.push(m); return true; }
        return false;
#else
        (void)trackId; (void)m;
        return false;
#endif
    }
    void sendAllNotesOff() {
#ifdef CAW_VST3
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return;
        for (Rt& r : g->rts)
            if (r.job.instrument && r.ep) { caw::MidiMsg m; m.status = 0xB0; m.d1 = 123; m.d2 = 0; r.ep->midiIn.push(m); }
#endif
    }
    // The jobs the rack was last initialized with (the 4th engine skips re-init
    // when nothing changed, so Play does not reload the plugins every time).
    const std::vector<VstJob>& jobs() const { return jobs_; }
#ifndef CAW_VST3
    bool init(const std::vector<VstJob>& jobsIn, int, int, int, int, PtrFn, TableFn, LogFn log, bool = false, VstParamMirror* = nullptr, const VstEngineAudio* = nullptr) { log_ = log; jobs_ = jobsIn; return true; }
    void queueJobs(const std::vector<VstJob>& jobs) { jobs_ = jobs; }
    void clear() {}
    size_t loaded() const { return 0; }
    void process(void*, MonitorCapture* = nullptr, float = 1.f, const std::set<int>* = nullptr, bool = false, const std::map<int, unsigned>* = nullptr) {}
#else
    size_t loaded() const {
        std::shared_ptr<Graph> g = currentGraph();
        return g ? g->rts.size() : 0;   // how many plugins really loaded
    }
    // Unload every plugin. A real teardown (a new project, Unload, the rack going away) - NOT a
    // plugin change: that reuses every instance that still matches. The caller makes sure
    // process() is not running on another thread while this runs.
    void clear() {
        stopLoader();
        std::vector<std::shared_ptr<Graph>> dead;
        {
            std::lock_guard<std::mutex> lock(graphMutex_);
            if (graph_) dead.push_back(std::move(graph_));
            graph_.reset();
            published_.reset();
        }
        { std::lock_guard<std::mutex> lock(graveMutex_); for (auto& g : graves_) dead.push_back(std::move(g)); graves_.clear(); }
        dead.clear();   // destroyed here, off every audio path
        insts_.clear();
        jobs_.clear();
        chans_.clear();
        initialized_ = false;
        { std::lock_guard<std::mutex> lock(sigMutex_); liveSig_.clear(); }
        { std::lock_guard<std::mutex> lock(reqMutex_); reqJobs_.clear(); reqValid_ = false; }
        std::lock_guard<std::mutex> lock(overrideMutex_);
        pending_.clear(); pendingPicks_.clear(); pendingValues_.clear();
    }
    // Live job update for the Csound-hosted rack (UI thread). Only the WIRING changed: the
    // instances stay and process() re-points the inputs on the next block. A track was added /
    // removed or its .vst3 changed: a loader thread creates ONLY the affected plugin(s) and
    // publishes the new graph; the audio thread swaps it in between two blocks. The sound never
    // stops, the other tracks keep their instances (and their voices), and a plugin that fails
    // to load only costs its own track.
    void queueJobs(const std::vector<VstJob>& jobs) {
        if (!initialized_) {
            std::lock_guard<std::mutex> lock(reqMutex_);
            reqJobs_ = jobs; reqValid_ = true;
            return;
        }
        const std::string sig = loadSig(jobs);
        { std::lock_guard<std::mutex> lock(sigMutex_); if (sig == liveSig_) { setWiring(jobs); return; } }
        {
            std::lock_guard<std::mutex> lock(reqMutex_);
            if (reqValid_ && loadSig(reqJobs_) == sig) return;   // that very set is already on its way
            reqJobs_ = jobs;
            reqValid_ = true;
        }
        startLoader();
        reqCv_.notify_one();
    }
    bool init(const std::vector<VstJob>& jobs, int sr, int block, int nchan, int sampleBytes, PtrFn ptr, TableFn table, LogFn log,
              bool strict = false, VstParamMirror* mirror = nullptr, const VstEngineAudio* engineAudio = nullptr) {
        stopLoader();
        sr_ = sr; block_ = block; nchan_ = nchan; sb_ = sampleBytes; strict_ = strict;
        mirror_ = mirror; ea_ = engineAudio; log_ = log; ptr_ = ptr; tab_ = table;
        {
            std::vector<std::shared_ptr<Graph>> dead;
            { std::lock_guard<std::mutex> lock(graphMutex_); if (graph_) dead.push_back(std::move(graph_)); graph_.reset(); published_.reset(); }
            { std::lock_guard<std::mutex> lock(graveMutex_); for (auto& g : graves_) dead.push_back(std::move(g)); graves_.clear(); }
        }
        insts_.clear();
        chans_.clear();
        { std::lock_guard<std::mutex> lock(sigMutex_); liveSig_.clear(); }
        {   // Live input on/off starts all on; the app pushes the project's masks right after init.
            std::lock_guard<std::mutex> lock(maskMutex_);
            masks_.clear();
        }
        initialized_ = true;
        std::shared_ptr<Graph> g;
        {   // Serialized with startLoader(): a queueJobs() from the UI during the sync build must not
            // build a second graph concurrently (insts_ has one owner at a time).
            std::lock_guard<std::mutex> life(loaderLifeMutex_);
            try { g = buildGraph(jobs, true); }
            catch (...) { say("[caw] VST initialization threw an exception.\n"); }
        }
        if (!g) return false;
        jobs_ = jobs;
        { std::lock_guard<std::mutex> lock(sigMutex_); liveSig_ = g->sig; }
        { std::lock_guard<std::mutex> lock(outModeMutex_); outModes_.clear(); for (const auto& kv : g->outModes) outModes_[kv.first] = kv.second; }
        { std::lock_guard<std::mutex> lock(graphMutex_); graph_ = std::move(g); }
        // A wiring / plugin push that landed during the init window is applied on the first block.
        bool pending = false;
        { std::lock_guard<std::mutex> lock(reqMutex_); pending = reqValid_; }
        if (pending) { startLoader(); reqCv_.notify_one(); }
        return true;
    }
    // active / activeValid: when activeValid is true, only tracks whose id is in `active` are processed
    // (live M / S). inputMasks: bit i set = input slot i of that track feeds the plugin (live on/off);
    // a missing entry means all configured inputs are on.
    void process(void* spout, MonitorCapture* capture = nullptr, float fullScale = 1.f,
                 const std::set<int>* active = nullptr, bool activeValid = false,
                 const std::map<int, unsigned>* inputMasks = nullptr) {
        if (!std::isfinite(fullScale) || fullScale <= 0.f) fullScale = 1.f;
        // A graph built by the loader thread is swapped in here, between two blocks (a pointer
        // swap - nothing is loaded, nothing is destroyed on this thread).
        std::shared_ptr<Graph> graph;
        {
            std::shared_ptr<Graph> old;
            bool adopted = false;
            {
                std::lock_guard<std::mutex> lock(graphMutex_);
                if (published_) { graph = std::move(published_); published_.reset(); old = std::move(graph_); graph_ = graph; adopted = true; }
                else graph = graph_;
            }
            if (old) retire(std::move(old));
            if (adopted) {
                { std::lock_guard<std::mutex> lock(sigMutex_); liveSig_ = graph->sig; }
                { std::lock_guard<std::mutex> lock(outModeMutex_); for (const auto& kv : graph->outModes) outModes_[kv.first] = kv.second; }
                for (Rt& r : graph->rts) resolveChans(r);   // Csound channel pointers are resolved on this thread only
                if (log_) log_("[caw] VST graph reloaded live (no Run needed); every track kept its plugin instance.\n");
            }
        }
        if (!graph) return;
        std::vector<Rt>& rts = graph->rts;
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
            for (Rt& r : rts)
                for (const VstParamValue& pv : valuesNow)
                    if (pv.trackId == r.job.id && pv.index < (int)r.plug->params().size()
                        && !r.plug->params()[(size_t)pv.index].readOnly) {
                        caw::ParamMsg m; m.paramIndex = pv.index; m.value = pv.value;
                        r.ep->paramIn.push(m);   // the engine instance applies it on this very block
                    }
            for (Rt& r : rts)
                for (const VstPickUpdate& pu : picks)
                    if (pu.trackId == r.job.id && pu.pick != r.job.pick) {
                        r.job.pick = pu.pick;
                        if (r.job.pick.size() > 8) r.job.pick.resize(8);
                        for (int k = 0; k < 8; ++k) { r.last[k] = -1.0; r.manualActive[k] = false; r.loggedActive[k] = false; }
                        r.chansResolved = false;   // the newly picked slots get their channels on this block
                        if (log_) log_("[caw] live picks of \"" + r.job.name + "\" updated (" + std::to_string(r.job.pick.size()) + " slots).\n");
                    }
            for (Rt& r : rts)
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
        // Instruments a master-mode VSTe replaces in the master: derived per block from the effect
        // jobs themselves (out-mode and input on/off are live). Everything a source can be is in
        // this rack now, so the rule needs no second list.
        {   // a re-wire pushed by the app while the engine runs: update the jobs in place (no reload)
            std::vector<VstJob> wiring;
            { std::lock_guard<std::mutex> lock(wiringMutex_); if (wiringValid_) { wiring.swap(wiring_); wiringValid_ = false; } }
            if (!wiring.empty()) applyWiring(wiring);
        }
        std::map<int, unsigned> masksLocal;
        if (!inputMasks) { std::lock_guard<std::mutex> lock(maskMutex_); masksLocal = masks_; }
        const std::map<int, unsigned>* masksUse = inputMasks ? inputMasks : &masksLocal;
        std::map<int, int> outModesLocal;
        { std::lock_guard<std::mutex> lock(outModeMutex_); outModesLocal = outModes_; }
        std::vector<int> replacedNow;
        for (const Rt& e : rts) {
            if (e.job.instrument) continue;
            // an effect that is not processed in this rack (Csound runs it) cannot replace anything here
            if (activeValid && active && active->find(e.job.id) == active->end()) continue;
            const auto om = outModesLocal.find(e.job.id);
            if ((om != outModesLocal.end() ? om->second : e.job.outMode) != 0) continue;
            for (int s = 0; s < 8; ++s) {
                const auto& src = e.job.inputs[s];
                if (!src.on || src.lineCount <= 0 || src.engine != 3) continue;
                if (src.srcId == e.job.id) continue;   // never wire an effect to itself
                const auto mk = masksUse->find(e.job.id);
                if (mk != masksUse->end() && !((mk->second >> s) & 1u)) continue;   // input switched off live
                replacedNow.push_back(src.srcId);
            }
        }
        const auto isReplaced = [&](int id) {
            return std::find(replacedNow.begin(), replacedNow.end(), id) != replacedNow.end();
        };
        // A "master" input hears the whole master: the replaced instruments are part of it (they
        // only stop being mixed themselves - the effect still gets their sound, like any source).
        std::vector<const Rt*> replacedInstr;
        for (const Rt& o : rts)
            if (o.job.instrument && isReplaced(o.job.id)
                && !(activeValid && active && active->find(o.job.id) == active->end())) replacedInstr.push_back(&o);
        // One SC-mix pull per block (several VSTe tracks may consume the same mix).
        bool needSc = false;
        for (const Rt& r : rts)
            for (const auto& src : r.job.inputs)
                if (src.on && src.engine == 2 && src.lineCount > 0) needSc = true;
        if (needSc && ea_ && ea_->scRing) {
            scScratch_.resize((size_t)block_ * 2);
            ea_->scRing->pop(scScratch_.data(), (size_t)block_);
        } else needSc = false;
        // Notes of the VSTi tracks arrive through the note ring above (the host's transport wrote
        // them: Csound's score via CawVstNote_<id>, or the 4th engine via pushMidi).
        for (Rt& r : rts) {
            const bool isActive = !(activeValid && active && active->find(r.job.id) == active->end());   // live M / S
            unsigned mask = 0xFFu;
            if (masksUse) { const auto it = masksUse->find(r.job.id); if (it != masksUse->end()) mask = it->second; }
            const size_t np = r.plug->params().size();
            // the 8 picked parameters: the channel value, with a fresh manual override winning on
            // the SAME block it arrives. Channel value and UI mirror are updated even for a muted
            // track, so the plugin GUI never fights a stale mirror; a track that comes back from
            // mute force-syncs its plugin to the current values.
            const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!r.chansResolved) resolveChans(r);   // a live pick invalidated them: resolve on this thread
            for (int k = 0; k < 8 && k < (int)r.job.pick.size(); ++k) {
                if (r.job.pick[k] < 0 || (size_t)r.job.pick[k] >= np) continue;
                // The 4th engine has no Csound channels (r.p[k] is null): without a lane to read,
                // fall back to the pick's start value so the parameter still reaches the plugin.
                // Otherwise an effect in the standalone engine stays at its loaded defaults and the
                // picked parameters never apply - "effect does nothing while Csound is off". The
                // manual override below still wins on the block a UI drag arrives (and holds, since
                // no automation can take it back in the standalone engine).
                double lane; bool laneMoving = false;
                if (r.p[k]) {
                    lane = std::min(1.0, std::max(0.0, rd(r.p[k])));
                    laneMoving = std::fabs(lane - r.prevLane[(size_t)k]) > 1e-6;   // automation is writing the lane
                    r.prevLane[(size_t)k] = lane;
                } else if (k < (int)r.job.init.size()) {
                    lane = std::min(1.0, std::max(0.0, r.job.init[(size_t)k]));
                } else {
                    continue;   // no channel and no start value: leave the parameter at its loaded state
                }
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
            const bool wasActiveBefore = r.wasActive;
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
            if (!isActive) {   // muted / not soloed: nothing below runs, the above bookkeeping keeps the UI honest
                if (!r.job.instrument && !r.inactiveWarned) {   // a VSTe that is skipped never processes: its source plays dry and knobs do nothing - say so once
                    r.inactiveWarned = true;
                    if (log_) log_("[caw] VSTe \"" + r.job.name + "\": NOT active (muted, or another track is soloed) - it is skipped, processes nothing, and any source wired to it plays dry. Unmute it (or un-solo the other track) for the effect to run.\n");
                }
                if (wasActiveBefore && r.ep) {   // a note held at the moment of the mute must not hang forever
                    caw::MidiMsg m; m.status = 0xB0; m.d1 = 123; m.d2 = 0;
                    r.ep->midiIn.push(m);
                }
                continue;
            }
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
                if (r.inst) r.inst->wr.store(r.lastW, std::memory_order_relaxed);   // a rebuilt graph resumes here, no replay
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
            r.tapsMissing = 0;
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
                        if (src.mode == 2) {   // master: the whole master mix - Csound's spout plus the VSTi output
                            const int mc = k == 0 ? 0 : (nchan_ > 1 ? 1 : 0);
                            for (int n = 0; n < block_; ++n) {
                                float v = spout ? (float)(rdSpout(spout, n, mc) / fullScale) : 0.f;
                                for (const Rt* o : replacedInstr) {
                                    const int sCh = o->job.routedIn ? 8 : 2;
                                    if (mc < sCh) v += o->out[(std::size_t)n * sCh + mc];
                                }
                                r.in[(std::size_t)n * 8 + outLine] = v;
                            }
                        } else if (src.engine == 1) {   // the Pure Data mix (device scale): line 1 = L, line 2 = R
                            const int mc = k == 0 ? 0 : 1;
                            if (pdMix && mc < pdChans)
                                for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = pdMix[(std::size_t)n * pdChans + mc];
                        } else if (src.engine == 2) {   // the SuperCollider mix (device scale): line 1 = L, line 2 = R
                            const int mc = k == 0 ? 0 : 1;
                            if (needSc) for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = scScratch_[(std::size_t)n * 2 + mc];
                        } else if (src.engine == 3) {   // a VSTi / VSTe source: wire its stereo output here directly
                            const int mc = k == 0 ? 0 : 1;   // line 1 = L, line 2 = R - the same channels the source adds to the master
                            const Rt* srcRt = nullptr;
                            if (src.srcId != r.job.id)
                                for (const Rt& o : rts) if (o.job.id == src.srcId) { srcRt = &o; break; }
                            if (srcRt) {   // the source renders in this rack, right above (ordered by dependency)
                                r.srcMissingWarned = false;
                                const bool srcActive = !(activeValid && active && active->find(srcRt->job.id) == active->end());
                                const int sCh = srcRt->job.routedIn ? 8 : 2;
                                if (srcActive && mc < sCh)
                                    for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = srcRt->out[(std::size_t)n * sCh + mc];
                            } else if (!r.srcMissingWarned) {   // the source is not in the rack: added after Run, or its plugin never loaded
                                r.srcMissingWarned = true;
                                if (log_) log_("[caw] VSTe \"" + r.job.name + "\": input \"" + src.name + "\" is a VSTi / VSTe source that is NOT in the rack - it was added after Run, or its plugin did not load. Press Run Csound once so its note ring and audio are wired in.\n");
                            }
                        } else {   // a Csound clip: its named tap channel (read live, so a re-wire needs no Run)
                            void* ch = k < (int)src.csChans.size() ? chanOf(src.csChans[(size_t)k]) : nullptr;
                            if (ch) {
                                for (int n = 0; n < block_; ++n) r.in[(std::size_t)n * 8 + outLine] = (float)rdAt(ch, n) / fullScale;
                            } else if (src.lineCount > 0) {
                                ++r.tapsMissing;
                            }
                        }
                    }
                }
                packedLines = outLine;
                in = r.in.data();   // 8 lines, one signal per channel - no mixdown (the plugin hears min(8, its input bus) of them)
                // Publish the first two lines as the VSTe's input audio: the clip code reads them
                // as `in` / `level(in)` (cawvstin_<id>_1/2, audio channels the engine creates from
                // C before the CSD compiles - the generated CSD contains no VST channels).
                // Csound runs before this call, so the code sees the previous block's input - one
                // ksmps late, which is inaudible for a level follower.
                if (!r.inChDone) {
                    r.inChDone = true;
                    if (ptr_) {
                        const std::string base = "cawvstin_" + std::to_string(r.job.id) + "_";
                        r.inCh[0] = ptr_(base + "1", 2 | 16 | 32);
                        r.inCh[1] = ptr_(base + "2", 2 | 16 | 32);
                    }
                }
                if (r.inCh[0] || r.inCh[1]) {
                    const int lineR = packedLines > 1 ? 1 : 0;   // a mono input feeds both channels
                    for (int n = 0; n < block_; ++n) {
                        if (r.inCh[0]) wrAt(r.inCh[0], n, r.in[(std::size_t)n * 8]);
                        if (r.inCh[1]) wrAt(r.inCh[1], n, r.in[(std::size_t)n * 8 + lineR]);
                    }
                }
            }
            r.plug->process(in, r.out.data(), block_, chPass, *r.ep);
            // One-shot VSTe state diagnostic. "Sound but no effect" + "the plugin's own knobs do
            // nothing" can mean: the plugin gets no input, outputs nothing, is in bypass / wet=0,
            // or is even skipped entirely (see the inactive log below). The generic hints are gated
            // so most of those print nothing. So the first time EACH VSTe runs (and after every
            // re-wire), print the exact state: routedIn, how many input lines it hears, the INPUT
            // peak, the OUTPUT peak (is the plugin producing anything at all?), and each source.
            // One line, one shot - this is the line that says which side of the effect is dead.
            if (!r.engine3DiagLogged) {
                r.engine3DiagLogged = true;
                float inPk = 0.f;
                if (packedLines > 0) for (int n = 0; n < block_; ++n) for (int c = 0; c < packedLines; ++c) inPk = std::max(inPk, std::fabs(r.in[(size_t)n * 8 + c]));
                float outPk = 0.f; for (int n = 0; n < block_; ++n) outPk = std::max(outPk, std::fabs(r.out[(size_t)n * chPass]));
                std::string diag;
                for (int i = 0; i < 8; ++i) {
                    const auto& src = r.job.inputs[i];
                    if (!src.on || src.lineCount <= 0) continue;
                    diag += "    in" + std::to_string(i + 1) + " \"" + src.name + "\" engine=" + std::to_string(src.engine);
                    if (src.engine == 3) {
                        const Rt* s = nullptr;
                        if (src.srcId != r.job.id) for (const Rt& o : rts) if (o.job.id == src.srcId) { s = &o; break; }
                        const bool srcActive = !(activeValid && active && active->find(src.srcId) == active->end());
                        float sPk = 0.f;
                        if (s && srcActive) { const int sCh = s->job.routedIn ? 8 : 2; for (int n = 0; n < block_; ++n) sPk = std::max(sPk, std::fabs(s->out[(size_t)n * sCh])); }
                        diag += std::string(": ") + (s ? (srcActive ? ("in rack, source peak " + std::to_string(sPk)) : "in rack but MUTED") : "NOT in rack");
                    }
                    diag += "\n";
                }
                if (log_) log_("[caw] VSTe \"" + r.job.name + "\": routedIn=" + (r.job.routedIn ? "yes" : "NO")
                             + ", packedLines=" + std::to_string(packedLines) + ", input peak=" + std::to_string(inPk)
                             + ", output peak=" + std::to_string(outPk)
                             + (outPk < 1e-6f ? "  <-- PLUGIN OUTPUTS NOTHING (wet/dry=0, bypass, or no preset loaded)" : "")
                             + (inPk < 1e-6f && packedLines > 0 ? "  <-- INPUT IS SILENT (the source is not sounding)" : "") + "\n" + diag);
            }
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
            if (soloMode_ || capture) {   // the lane meters of the VST tracks read these (max-held until the UI takes them)
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
                        if (log_) log_("[caw] VSTe \"" + r.job.name + "\": input audio is SILENT while other audio plays. Sources must PLAY on the timeline; a MUTED source stays silent"
                                       + std::string(r.tapsMissing > 0 ? "; the Csound clip taps of its source are missing - that clip was added after Run, press Run once.\n" : ".\n"));
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
            }
            // The VSTe output always goes to the master (spout). Whether the SOURCE track also
            // reaches the master (mix) or is replaced (master) follows from the live out-mode: a
            // Csound source clip's `out` lines are gated by the CSD (cawvstmix_<clip>_<id>, written
            // by the app), a VSTi / VSTe source is skipped here (it already published its output
            // for the effects above) - so a re-wire or an AUDIO OUTPUT switch never needs a Run.
            if (isReplaced(r.job.id)) continue;
            for (int i = 0; i < block_; ++i) for (int c = 0; c < nchan_ && c < chPass; ++c) add(spout, (size_t)i * nchan_ + c, r.out[(size_t)i * chPass + c] * fullScale);
        }
        // The clip taps chnmixed the current pass into the cawtap_* channels while Csound ran
        // (perform happens before this call), and they were read in the loop above - so clear them
        // NOW, for every channel the rack resolved, and the next pass starts from zero again.
        for (auto& entry : chans_) clearBus(entry.second);
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
    std::vector<VstJob> jobs_;      // the loaded job set (the 4th engine reuses a matching init)
    bool soloMode_ = false;         // true when this rack is the app-side 4th engine (no Csound channels)
#ifdef CAW_VST3
    // One loaded plugin instance. Lives in the loader's registry and is shared (by reference count)
    // with every graph that uses it: a re-wire / a changed track never re-creates the others.
    struct Inst {
        std::string path, classId; bool instrument = false;
        std::shared_ptr<caw::IPlugin> plug;
        std::atomic<long> wr{0};   // the note-ring position the audio thread has played up to
        std::atomic<bool> fresh{true};   // never resolved yet: start at the ring head, do not re-fire old notes
    };
    struct Rt {
        VstJob job; std::shared_ptr<Inst> inst; std::shared_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
        void* p[8] = {}; void *wr = nullptr; const void* tab = nullptr; long lastW = 0; bool chansResolved = false;
        long midiCount = 0; bool midiSeen = false; bool bridgeWarned = false; int silentHints = 0;   // MIDI bridge diagnostics
        int tapsMissing = 0;   // engine-0 input lines whose tap channel could not be resolved this block
        void* inCh[2] = {}; bool inChDone = false;   // cawvstin_<id>_1/2: the input audio the clip code reads as `in`
        bool srcMissingWarned = false;   // a VSTi/VSTe source (engine 3) named by an input was not in the rack
        bool engine3DiagLogged = false;   // one-shot: print the engine-3 source state the first time a VSTe runs (reset on rewire)
        bool inactiveWarned = false;   // one-shot: a VSTe skipped as not-active logs once why (so it is not silently dead)
        std::vector<double> last; std::vector<float> in, out;
        bool wasActive = true;   // a track coming back from mute force-syncs its plugin to the channels
        double manual[8] = {}; bool manualActive[8] = {}; long long manualUntil[8] = {};   // live manual overrides
        double prevLane[8] = {};   // the raw lane values of the previous block (detects automation movement)
        double loggedManual[8] = {}; bool loggedActive[8] = {};   // live-param logging state
        bool unpickedWarned[8] = {};   // solo mode: P slot written by clip code but not picked (warn once)
        long long inSilentSince = 0, inHintAt = 0, outSilentSince = 0, outHintAt = 0;   // silence diagnostics
        long long instSilentSince = 0, instHintAt = 0;   // VSTi: silence while other audio plays = no notes reached the plugin
    };
    // What the audio thread runs. Built by init (sync) or by the loader thread, never mutated
    // in place afterwards except the per-track bookkeeping the audio thread owns.
    struct Graph {
        std::vector<Rt> rts;
        std::string sig;
        std::map<int, int> outModes;
    };
    double rd(const void* p) const { return sb_ == 8 ? *static_cast<const double*>(p) : (double)*static_cast<const float*>(p); }
    double rdAt(const void* p, int i) const { return sb_ == 8 ? static_cast<const double*>(p)[i] : (double)static_cast<const float*>(p)[i]; }
    void wrAt(void* p, int i, float v) const { if (sb_ == 8) static_cast<double*>(p)[i] = (double)v; else static_cast<float*>(p)[i] = v; }
    float rdSpout(const void* spout, int i, int c) const { return sb_ == 8 ? (float)static_cast<const double*>(spout)[(std::size_t)i * nchan_ + c] : static_cast<const float*>(spout)[(std::size_t)i * nchan_ + c]; }
    void clearBus(void* p) const { if (sb_ == 8) std::fill_n(static_cast<double*>(p), block_, 0.0); else std::fill_n(static_cast<float*>(p), block_, 0.f); }
    void add(void* b, size_t i, float v) const { if (sb_ == 8) static_cast<double*>(b)[i] += v; else static_cast<float*>(b)[i] += v; }
    // A clip tap channel, resolved by name on this thread and cached. A late-created channel is
    // picked up on a later block; a new one is zeroed on first resolution (it may hold last run's
    // leftovers). Cleared after every block (see process) so each pass starts from zero.
    void* chanOf(const std::string& name) {
        const auto it = chans_.find(name);
        if (it != chans_.end()) return it->second;
        if (!ptr_) return nullptr;
        void* p = ptr_(name, 2 | 16 | 32);
        if (!p) return nullptr;
        clearBus(p);
        chans_[name] = p;
        return p;
    }
    void say(const std::string& s) const { if (log_) log_(s); }
    static std::string loadSig(const std::vector<VstJob>& jobs) {
        std::string s;
        for (const VstJob& j : jobs) s += std::to_string(j.id) + "|" + j.path + "|" + j.classId + "|" + (j.instrument ? "i" : "e") + ";";
        return s;
    }
    static bool sameInput(const VstJob::Input& a, const VstJob::Input& b) {
        return a.on == b.on && a.mode == b.mode && a.name == b.name && a.lineStart == b.lineStart
            && a.lineCount == b.lineCount && a.engine == b.engine && a.srcId == b.srcId && a.csChans == b.csChans;
    }
    std::shared_ptr<Graph> currentGraph() const {
        std::lock_guard<std::mutex> lock(graphMutex_);
        return graph_;
    }
    // Old graphs are destroyed by the loader thread: a plugin destructor must never run inside
    // the audio callback (it can block for as long as the plugin wants).
    void retire(std::shared_ptr<Graph> g) {
        if (!g) return;
        std::lock_guard<std::mutex> lock(graveMutex_);
        graves_.push_back(std::move(g));
    }
    void drainGraves() {
        std::vector<std::shared_ptr<Graph>> dead;
        { std::lock_guard<std::mutex> lock(graveMutex_); dead.swap(graves_); }
        // destroyed on this (loader) thread
    }
    // The Csound channel pointers of one track: resolved on the thread that owns Csound's channels
    // (the engine thread), never on the loader thread. All 8 slots are tried - a P slot picked
    // while the engine runs starts working on the next block instead of needing a Run.
    void resolveChans(Rt& r) {
        if (r.chansResolved) return;
        r.chansResolved = true;
        if (!ptr_) return;
        const std::string base = "cawvst_" + std::to_string(r.job.id) + "_";
        for (size_t k = 0; k < 8; ++k) r.p[k] = ptr_(base + "p" + std::to_string(k + 1), 1 | 32);
        r.wr = ptr_(base + "wr", 1 | 32);
        void* tabNo = ptr_(base + "tab", 1 | 32);
        if (tabNo && tab_) r.tab = tab_(int(std::lround(rd(tabNo))));
        if (r.inst) {
            if (r.inst->fresh.exchange(false, std::memory_order_relaxed))
                r.lastW = r.wr ? std::lround(rd(r.wr)) : 0;   // a new instance starts at the ring head: notes it never saw are not re-fired
            else
                r.lastW = std::max(r.lastW, r.inst->wr.load(std::memory_order_relaxed));   // continue the ring, never replay it
        }
    }
    void startLoader() {
        std::lock_guard<std::mutex> life(loaderLifeMutex_);
        if (loader_.joinable()) return;
        { std::lock_guard<std::mutex> lock(reqMutex_); loaderStop_ = false; }
        loader_ = std::thread([this] { loaderMain(); });
    }
    void stopLoader() {
        std::lock_guard<std::mutex> life(loaderLifeMutex_);
        if (!loader_.joinable()) return;
        { std::lock_guard<std::mutex> lock(reqMutex_); loaderStop_ = true; }
        reqCv_.notify_all();
        loader_.join();
        { std::lock_guard<std::mutex> lock(reqMutex_); loaderStop_ = false; }
    }
    // The loader thread: builds the graph for the latest request with every plugin load happening
    // here - the audio callback only ever adopts the finished graph.
    void loaderMain() {
#ifdef _WIN32
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // VST3 plugins expect COM on the thread that loads them
#endif
        for (;;) {
            std::vector<VstJob> req;
            {
                std::unique_lock<std::mutex> lock(reqMutex_);
                reqCv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return loaderStop_ || reqValid_; });
                if (loaderStop_) break;
                if (!reqValid_) continue;   // timeout: nothing to do
                req.swap(reqJobs_);
                reqValid_ = false;
            }
            drainGraves();
            std::shared_ptr<Graph> g;
            try { g = buildGraph(req, false); }
            catch (const std::exception& e) { say("[caw] VST graph reload failed (" + std::string(e.what()) + ") - the previous graph keeps playing.\n"); }
            catch (...) { say("[caw] VST graph reload failed - the previous graph keeps playing.\n"); }
            if (!g) continue;
            std::lock_guard<std::mutex> lock(graphMutex_);
            published_ = std::move(g);
        }
        drainGraves();
#ifdef _WIN32
        CoUninitialize();
#endif
    }
    // Make one job set current: an instance whose (id, path, kind) still matches is REUSED (only a
    // new / changed plugin is created), the rest of the graph is rebuilt from those instances.
    // Runs on the init thread (sync) or the loader thread - both off the audio callback.
    // resolveNow = the caller is the thread that owns Csound's channels, so it can resolve them.
    std::shared_ptr<Graph> buildGraph(const std::vector<VstJob>& jobs, bool resolveNow) {
        std::shared_ptr<Graph> g = std::make_shared<Graph>();
        g->sig = loadSig(jobs);
        std::set<int> keep;
        for (const VstJob& j : jobs) {
            if (j.path.empty()) { say("[caw] VST track \"" + j.name + "\": no plugin loaded, skipped.\n"); if (strict_) return {}; continue; }
            std::shared_ptr<Inst> inst;
            const auto known = insts_.find(j.id);
            if (known != insts_.end() && known->second->path == j.path && known->second->classId == j.classId && known->second->instrument == j.instrument)
                inst = known->second;
            if (!inst) {
                // A new plugin (picked / changed while the engine runs): created HERE, never on the audio thread.
                auto made = std::make_shared<Inst>();
                made->path = j.path;
                made->classId = j.classId;
                made->instrument = j.instrument;
                try {
                    made->plug = caw::makeVst3Plugin();
                    std::string err;
                    if (!made->plug->load(j.path, j.classId, err)) { say("[caw] VST track \"" + j.name + "\": " + err + "\n"); made.reset(); }
                } catch (const std::exception& e) {
                    say("[caw] VST track \"" + j.name + "\": the plugin threw while loading (" + std::string(e.what()) + ") - that track stays silent.\n");
                    made.reset();
                } catch (...) {
                    say("[caw] VST track \"" + j.name + "\": the plugin threw while loading - that track stays silent.\n");
                    made.reset();
                }
                if (made) {
                    try {
                        for (const auto& parameter : made->plug->params()) {
                            const auto saved = j.parameterState.find(parameter.id);
                            if (parameter.readOnly || saved == j.parameterState.end()) continue;
                            if (std::fabs(saved->second - parameter.value) <= 1e-6) continue;   // already what the plugin reports
                            made->plug->setParam(parameter.index, saved->second);
                        }
                        made->plug->prepare(sr_, block_);
                    } catch (...) {
                        say("[caw] VST track \"" + j.name + "\": the plugin threw while starting - that track stays silent.\n");
                        made.reset();
                    }
                }
                insts_.erase(j.id);
                if (!made) { if (strict_) return {}; continue; }
                insts_[j.id] = made;
                say("[caw] VST track \"" + j.name + "\" loaded (" + (j.instrument ? "instrument" : "effect") + ").\n");
                inst = made;
            }
            keep.insert(j.id);
            Rt r;
            r.job = j;
            r.inst = inst;
            r.plug = inst->plug;
            if (r.job.pick.size() > 8) r.job.pick.resize(8);
            r.ep = std::make_shared<caw::Endpoint>();
            r.in.assign((size_t)block_ * 8, 0.f); r.out.assign((size_t)block_ * 8, 0.f);   // 8 lines max; the pass stride decides how they are read
            r.last.assign(8, -1.0);
            r.lastW = inst->wr.load(std::memory_order_relaxed);   // a reused instance continues its ring instead of replaying it
            if (!j.instrument && j.routedIn) {   // say what the VSTe hears (the sources are resolved live, by name)
                std::string sum;
                for (int s = 0; s < 8; ++s) {
                    const auto& src = j.inputs[s];
                    if (!src.on || src.lineCount <= 0) continue;
                    if (!sum.empty()) sum += ", ";
                    sum += "in" + std::to_string(s + 1) + ": ";
                    if (src.mode == 2) sum += "master";
                    else if (src.engine == 1) sum += "PureData mix";
                    else if (src.engine == 2) sum += "SuperCollider mix";
                    else if (src.engine == 3) sum += "\"" + src.name + "\" (VST output)";
                    else sum += "\"" + src.name + "\" (Csound clip taps, " + std::to_string(src.csChans.size()) + " ch)";
                }
                say("[caw] VSTe \"" + j.name + "\" inputs: " + (sum.empty() ? "(none enabled)" : sum) + "\n");
            }
            if (!r.job.instrument) g->outModes[r.job.id] = r.job.outMode == 1 ? 1 : 0;
            g->rts.push_back(std::move(r));
        }
        // Instances of tracks that are no longer part of the graph are released here (off the audio thread).
        for (auto it = insts_.begin(); it != insts_.end();) {
            if (keep.count(it->first)) ++it;
            else it = insts_.erase(it);
        }
        orderGraph(g->rts);
        if (resolveNow) {
            for (Rt& r : g->rts) resolveChans(r);
            if (strict_) {
                for (const Rt& r : g->rts)
                    if (!r.wr || !r.tab) { say("[caw] VST MIDI bridge unavailable.\n"); return {}; }
            }
        }
        return g;
    }
    // Order the rack so a source renders BEFORE the effect that reads it in the same block:
    // dependency first, everything else in project order (instruments before effects by default).
    static void orderGraph(std::vector<Rt>& rts) {
        std::stable_sort(rts.begin(), rts.end(), [](const Rt& a, const Rt& b) { return a.job.instrument && !b.job.instrument; });
        const int n = (int)rts.size();
        std::map<int, int> byId;
        for (int i = 0; i < n; ++i) byId[rts[(size_t)i].job.id] = i;
        std::vector<std::vector<int>> deps((size_t)n);
        for (int i = 0; i < n; ++i) {
            const Rt& e = rts[(size_t)i];
            if (e.job.instrument) continue;
            for (const auto& src : e.job.inputs) {
                if (!src.on || src.lineCount <= 0 || src.engine != 3 || src.srcId == e.job.id) continue;
                const auto it = byId.find(src.srcId);
                if (it != byId.end()) deps[(size_t)i].push_back(it->second);
            }
        }
        std::vector<int> order; order.reserve((size_t)n);
        std::vector<char> state((size_t)n, 0);   // 0 = new, 1 = visiting, 2 = done
        std::function<void(int)> visit = [&](int i) {
            if (state[(size_t)i]) return;        // visiting = a cycle: that edge reads last block's output
            state[(size_t)i] = 1;
            for (int d : deps[(size_t)i]) if (state[(size_t)d] != 1) visit(d);
            state[(size_t)i] = 2;
            order.push_back(i);
        };
        for (int i = 0; i < n; ++i) visit(i);
        std::vector<Rt> sorted; sorted.reserve((size_t)n);
        for (int i : order) sorted.push_back(std::move(rts[(size_t)i]));
        rts = std::move(sorted);
    }
    // Point every effect at the wiring in `jobs` (inputs, routed flag, out-mode) without touching
    // the plugin instances. Re-orders when a dependency changed. Called from process().
    void applyWiring(const std::vector<VstJob>& jobs) {
        bool changed = false;
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return;
        {
            std::lock_guard<std::mutex> lock(outModeMutex_);
            for (const VstJob& j : jobs) {
                for (Rt& r : g->rts) {
                    if (r.job.id != j.id) continue;
                    if (r.job.instrument) break;
                    bool diff = r.job.routedIn != j.routedIn || (r.job.outMode == 1) != (j.outMode == 1);
                    for (int s = 0; s < 8 && !diff; ++s) diff = !sameInput(r.job.inputs[s], j.inputs[s]);
                    if (!diff) break;
                    for (int s = 0; s < 8; ++s) r.job.inputs[s] = j.inputs[s];
                    r.engine3DiagLogged = false;   // a re-wire: re-report the VSTe state on the next block
                    r.inactiveWarned = false;
                    r.job.routedIn = j.routedIn;
                    r.job.outMode = j.outMode;
                    outModes_[j.id] = j.outMode == 1 ? 1 : 0;
                    changed = true;
                    break;
                }
            }
            if (changed) orderGraph(g->rts);
        }
    }
    std::shared_ptr<Graph> graph_;      // the graph process() runs (replaced on the audio thread only)
    std::shared_ptr<Graph> published_;  // finished by the loader thread, adopted on the next block
    mutable std::mutex graphMutex_;     // guards graph_ / published_
    std::vector<std::shared_ptr<Graph>> graves_;   // old graphs waiting for the loader thread to destroy them
    std::mutex graveMutex_;
    std::map<int, std::shared_ptr<Inst>> insts_;   // init / loader thread only: the loaded instances, reused across graphs
    std::thread loader_;
    std::mutex loaderLifeMutex_;                   // start / stop of the loader thread
    std::mutex reqMutex_; std::condition_variable reqCv_;
    std::vector<VstJob> reqJobs_; bool reqValid_ = false; bool loaderStop_ = false;
    std::atomic<bool> initialized_{false};
    std::string liveSig_; std::mutex sigMutex_;    // the signature of the graph that is actually installed (UI reads it)
    int sb_ = 8, block_ = 0, nchan_ = 2, sr_ = 48000; VstParamMirror* mirror_ = nullptr;
    bool strict_ = false;
    PtrFn ptr_{}; TableFn tab_{};   // kept from init so process() can resolve the note table and the clip taps
    std::map<std::string, void*> chans_;   // resolved clip tap channels (engine thread only)
    const VstEngineAudio* ea_ = nullptr;
#endif
    std::mutex peakMutex_;                                        // VST track meter peaks, rack thread -> UI
    std::map<int, std::array<float, 2>> soloPeaks_;
    std::mutex maskMutex_;                                        // live VSTe input on/off (UI) -> process (4th engine)
    std::map<int, unsigned> masks_;
    std::mutex outModeMutex_;                                     // live AUDIO OUTPUT of a VSTe (UI) -> process
    std::map<int, int> outModes_;
    std::mutex wiringMutex_;                                      // live effect re-wire (UI) -> process
    std::vector<VstJob> wiring_; bool wiringValid_ = false;
};
