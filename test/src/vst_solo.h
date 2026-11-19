#pragma once
// CAW's 4th engine: the independent VSTi rack.
//
// The instrument tracks render HERE, in the app, the way a DAW plays a VST:
//   - notes are scheduled by CAW itself from the MIDI clips (audio-rate accurate,
//     loop-aware), with no Csound instrument, no score and no transport rows;
//   - when Csound IS running, the notes fired by clip code (note / cc / msg) and
//     the parameter channels arrive through a small forwarder (see the runner)
//     and are staged into the same plugins;
//   - the plugins render into a small FIFO and the sound is mixed into whatever
//     output stream CAW has open, through the same realtime hook the
//     SuperCollider engine uses. No Csound engine needed at all.
//   - It is its own engine with its own button (Run VST / Stop VST): start() loads the
//     plugins, setEngineOn(true) keeps it rendering, stop() unloads everything.
//
// Threading: one audio-callback thread runs renderMix/renderBlock and is the
// only thread that touches the rack's MIDI ring. Everyone else (UI, Csound
// forwarder) goes through stageMidi(), which the callback drains.
#include "vst_rack.h"
#include "vst_script.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// One scheduled note of the app-side transport (built by the app from the MIDI clips).
struct VstSoloNote {
    double start = 0.0;   // seconds on the timeline
    double dur = 0.0;
    int pitch = 60, velocity = 100;
    int trackId = 0;
};

// One clip-code program of a VSTi track (see vst_script.h): the engine evaluates it on the
// transport clock while Csound is OFF, so Run VST plays `note / cc / msg` code by itself.
struct VstScript {
    int trackId = 0, clipId = 0;
    double start = 0.0, dur = 1.0;
    std::string code;
    double params[8] = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};   // P1..P8 start values (normalized)
};

class VstSoloEngine {
public:
    using LogFn = std::function<void(const std::string&)>;

    void setLog(LogFn log) { log_ = std::move(log); }

    // The identity of a set of instrument jobs: a change means the plugins must be (re)loaded.
    static std::string signature(const std::vector<VstJob>& jobs) {
        std::string sig;
        for (const VstJob& j : jobs) sig += std::to_string(j.id) + "|" + j.path + ";";
        return sig;
    }
    bool sameJobs(const std::vector<VstJob>& jobs) const {   // UI thread
        return rackReady_ && jobsSig_.rfind(signature(jobs) + "@", 0) == 0;
    }

    // Start (or restart) the engine with these instrument jobs. A rack whose jobs
    // did not change is reused. sr / channels / block come from the audio settings;
    // the engine renders block-sized chunks into a FIFO so any callback size works.
    // The audio callback skips its mix (never blocks) while the plugins load here.
    bool start(const std::vector<VstJob>& jobs, int sr, int channels, int block) {
        std::lock_guard<std::mutex> render(renderMutex_);
        std::lock_guard<std::mutex> lock(control_);
        sr_ = sr < 1000 ? 48000 : sr;
        channels_ = channels < 1 ? 2 : channels;
        block_ = block < 64 ? 512 : block;
        const std::string sig = signature(jobs) + "@" + std::to_string(sr_) + "/" + std::to_string(channels_) + "/" + std::to_string(block_);
        if (!(rackReady_ && sig == jobsSig_)) {
            rackReady_ = false;
            rack_.clear();
            rack_.clearSoloPeaks();
            { std::lock_guard<std::mutex> busLock(cawVstSoloOutBus().mutex); cawVstSoloOutBus().outs.clear(); }
            { std::lock_guard<std::mutex> mirrorLock(mirror_.mutex); mirror_.values.clear(); }
            rack_.setSoloMode(true);
            const bool ok = rack_.init(jobs, sr_, block_, channels_, 4,
                [](const std::string&, int) -> void* { return nullptr; },          // no Csound channels in the 4th engine
                [](int) -> const void* { return nullptr; },                        // no Csound tables either
                [this](const std::string& s) { if (log_) log_(s); },
                false, &mirror_, nullptr);
            jobsSig_ = sig;
            activeIds_.clear();
            for (const VstJob& j : jobs) activeIds_.insert(j.id);
            spout_.assign((size_t)block_ * (size_t)channels_, 0.f);
            fifo_.assign((size_t)std::max<size_t>(8, 16384 / (size_t)block_) * (size_t)block_ * (size_t)channels_, 0.f);
            fifoRead_ = 0; fifoFrames_ = 0;
            notesA_.clear(); notesB_.clear(); notesIndex_.store(0);
            cursor_.store(0);
            active_.clear();
            firstNoteLogged_ = false;
            { std::lock_guard<std::mutex> sl(stageMutex_); staged_.clear(); liveActive_ = activeIds_; }
            const size_t loadedNow = rack_.loaded();
            rackReady_ = ok && loadedNow > 0;
            if (log_) {
                if (jobs.empty()) log_("[caw] VST engine: no instrument tracks.\n");
                else if (loadedNow == 0) log_("[caw] VST engine: NO plugin could be loaded - see the lines above (is a .vst3 chosen in the instrument panel?).\n");
                else log_("[caw] VST engine: " + std::to_string(loadedNow) + " of " + std::to_string(jobs.size()) + " instrument(s) loaded and running.\n");
            }
        }
        return rackReady_;
    }

    // Unload every plugin and stop rendering (the Stop VST button).
    void stop() {
        std::lock_guard<std::mutex> render(renderMutex_);
        std::lock_guard<std::mutex> lock(control_);
        rackReady_ = false;
        playing_.store(false, std::memory_order_release);
        engineOn_.store(false, std::memory_order_release);
        rack_.clear();
        rack_.clearSoloPeaks();
        { std::lock_guard<std::mutex> busLock(cawVstSoloOutBus().mutex); cawVstSoloOutBus().outs.clear(); }
        { std::lock_guard<std::mutex> mirrorLock(mirror_.mutex); mirror_.values.clear(); }
        { std::lock_guard<std::mutex> sl(stageMutex_); staged_.clear(); liveActive_.clear(); }
        { std::lock_guard<std::mutex> sl(scriptMutex_); scripts_.clear(); scriptsSig_ = 0; }
        jobsSig_.clear(); activeIds_.clear();
        notesA_.clear(); notesB_.clear(); notesIndex_.store(0); cursor_.store(0);
        active_.clear();
        fifoRead_ = 0; fifoFrames_ = 0;
    }

    // While on, the engine is audible (renders) even when the transport is stopped, so
    // live parameter edits and note-offs are always heard.
    void setEngineOn(bool on) { engineOn_.store(on, std::memory_order_release); touch(); }
    bool engineOn() const { return engineOn_.load(std::memory_order_acquire); }
    size_t loadedCount() const { return rack_.loaded(); }

    // Live M / S: only these instrument ids sound (the others are skipped by the rack).
    void setActive(const std::set<int>& ids) {
        std::lock_guard<std::mutex> lock(stageMutex_);
        liveActive_ = ids;
        touch();
    }
    void pushPickUpdate(int trackId, const std::vector<int>& pick) { rack_.pushPickUpdate(trackId, pick); touch(); }

    // Clip code for the VST-only path (no Csound engine): compiled here, evaluated between
    // audio blocks on the transport clock. An empty list disables it - the app passes one only
    // while Csound is off, so a running Csound (which forwards its own clip-code notes) never
    // doubles these. Unchanged programs keep their runtime state (LFO phase, every timers).
    void setScripts(std::vector<VstScript> scripts) {
        unsigned long long sig = 1469598103934665603ull;
        auto mix = [&sig](const std::string& s) {
            for (unsigned char ch : s) { sig ^= ch; sig *= 1099511628211ull; }
        };
        for (const VstScript& s : scripts) {
            mix(s.code); mix(std::to_string(s.trackId)); mix(std::to_string(s.clipId));
            mix(std::to_string(s.start)); mix(std::to_string(s.dur));
        }
        // The start values (params) are NOT part of the identity: while the engine plays, the UI
        // mirrors the live engine values back into vstInit, and that must never restart a script.
        { std::lock_guard<std::mutex> lock(scriptMutex_); if (sig == scriptsSig_) return; }
        // Compile outside the lock: the audio thread takes scriptMutex_ once per block.
        std::vector<std::pair<VstScript, vstscript::Program>> compiled;
        compiled.reserve(scripts.size());
        for (VstScript& s : scripts) {
            vstscript::Program prog = vstscript::Compiler().compile(s.code);
            compiled.emplace_back(std::move(s), std::move(prog));
        }
        std::lock_guard<std::mutex> lock(scriptMutex_);
        if (sig == scriptsSig_) return;
        std::map<int, ScriptEntry*> old;
        for (ScriptEntry& e : scripts_) old[e.s.clipId] = &e;
        std::vector<ScriptEntry> next;
        next.reserve(compiled.size());
        for (auto& item : compiled) {
            ScriptEntry e;
            e.s = std::move(item.first);
            e.prog = std::move(item.second);
            auto it = old.find(e.s.clipId);
            bool same = it != old.end() && it->second->s.code == e.s.code &&
                        it->second->s.start == e.s.start && it->second->s.dur == e.s.dur;
            if (same) {
                e.rt = it->second->rt;
                e.started = it->second->started;
                for (int k = 0; k < 8; ++k) { e.lastP[k] = it->second->lastP[k]; e.lastPValid[k] = it->second->lastPValid[k]; }
                if (!e.started) for (int k = 0; k < 8; ++k) e.rt.p[k] = e.s.params[k];   // a clip that has not started yet takes the new start values
            } else {
                e.rt.init(e.prog, e.s.params);
            }
            next.push_back(std::move(e));
        }
        scripts_ = std::move(next);
        scriptsSig_ = sig;
    }

    bool ready() const { return rackReady_; }
    bool hasInstruments() const { return rackReady_ && !activeIds_.empty(); }

    // The app rebuilds the schedule on play / seek / edit-while-stopped.
    void setNotes(std::vector<VstSoloNote> notes) {
        std::lock_guard<std::mutex> lock(control_);
        std::vector<VstSoloNote>& dst = notesIndex_.load(std::memory_order_relaxed) == 0 ? notesB_ : notesA_;
        std::stable_sort(notes.begin(), notes.end(),
                         [](const VstSoloNote& a, const VstSoloNote& b) { return a.start < b.start; });
        dst = std::move(notes);
        notesIndex_.store(notesIndex_.load(std::memory_order_relaxed) == 0 ? 1 : 0, std::memory_order_release);
        cursor_.store(0, std::memory_order_release);
    }

    void setLoop(bool on, double start, double end) {
        std::lock_guard<std::mutex> lock(control_);
        loop_ = on; loopStart_ = start; loopEnd_ = end;
    }

    // Transport from the UI thread. Notes that are sounding are stopped through
    // the staging queue so the audio thread stays the only ring producer.
    void play(double fromSeconds) {
        { std::lock_guard<std::mutex> lock(stageMutex_); stageAllNotesOffLocked(); }
        std::lock_guard<std::mutex> render(renderMutex_);
        std::lock_guard<std::mutex> lock(control_);
        time_.store(fromSeconds, std::memory_order_release);
        cursor_.store(seekCursorLocked(fromSeconds), std::memory_order_release);
        active_.clear();
        fifoRead_ = 0; fifoFrames_ = 0;
        playing_.store(true, std::memory_order_release);
        scriptsReset_.store(true, std::memory_order_release);
        touch();
    }
    void resync(double toSeconds) {   // reposition, keep the play state
        { std::lock_guard<std::mutex> lock(stageMutex_); stageAllNotesOffLocked(); }
        std::lock_guard<std::mutex> render(renderMutex_);
        std::lock_guard<std::mutex> lock(control_);
        time_.store(toSeconds, std::memory_order_release);
        cursor_.store(seekCursorLocked(toSeconds), std::memory_order_release);
        active_.clear();
        fifoRead_ = 0; fifoFrames_ = 0;
        scriptsReset_.store(true, std::memory_order_release);
        touch();
    }
    void halt() {   // transport stopped: stop scheduling, deliver note-offs, keep the plugins loaded
        playing_.store(false, std::memory_order_release);
        scriptsReset_.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(stageMutex_); stageAllNotesOffLocked(); }
        touch();
    }
    // When Csound (or another engine) owns the transport, this engine follows the frame
    // clock instead of driving it: re-anchor whenever the two clocks drift apart, so the
    // instruments stay in time with the rest of the mix.
    void followClock(double t) {
        if (!playing_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(control_);
        if (std::fabs(time_.load(std::memory_order_relaxed) - t) > 0.06) {
            time_.store(t, std::memory_order_release);
            cursor_.store(seekCursorLocked(t), std::memory_order_release);
            scriptsReset_.store(true, std::memory_order_release);
        }
    }

    // MIDI / params from other threads (UI sliders, the Csound forwarder).
    void stageMidi(int trackId, int status, int d1, int d2) {
        std::lock_guard<std::mutex> lock(stageMutex_);
        Staged s; s.trackId = trackId; s.status = (uint8_t)status; s.d1 = (uint8_t)d1; s.d2 = (uint8_t)d2;
        staged_.push_back(s);
        touch();
    }
    void pushParamOverride(int trackId, int slot, double value) { rack_.pushParamOverride(trackId, slot, value); touch(); }
    void pushParamValue(int trackId, int paramIndex, double value) { rack_.pushParamValue(trackId, paramIndex, value); touch(); }
    // What the engine's plugins are actually using right now (P1..P8 of a track). Lets the UI follow
    // clip-code P writes and engine-side changes in this engine too, not only in the Csound rack.
    bool params(int trackId, double out[8]) {
        std::lock_guard<std::mutex> lock(mirror_.mutex);
        const auto it = mirror_.values.find(trackId);
        if (it == mirror_.values.end()) return false;
        for (int k = 0; k < 8; ++k) out[k] = it->second[(size_t)k];
        return true;
    }
    // Per-track output peaks since the last call (the lane meters of the VSTi tracks).
    void takePeaks(std::vector<VstRack::SoloPeak>& out) { rack_.takeSoloPeaks(out); }

    // --- the audio-callback side -------------------------------------------------
    // Mixes this engine into the device stream (the SuperCollider hook contract).
    void renderMix(float* out, int frames, int channels) noexcept {
        if (!rackReady_ || !out || frames <= 0 || channels < 1) return;
        if (!renderDue()) return;
        std::unique_lock<std::mutex> render(renderMutex_, std::try_to_lock);   // never block the soundcard callback
        if (!render.owns_lock() || !rackReady_) return;
        const size_t cap = fifo_.size() / (size_t)std::max(1, channels_);
        int guard = 0;
        while ((size_t)fifoFrames_ < (size_t)frames && ++guard < 64) renderBlock(cap);
        const int ch = std::min(channels, channels_);
        for (int i = 0; i < frames; ++i) {
            if ((size_t)i >= (size_t)fifoFrames_) break;
            const size_t src = ((fifoRead_ + (size_t)i) % cap) * (size_t)channels_;
            float* dst = out + (size_t)i * (size_t)channels;
            for (int c = 0; c < ch; ++c) dst[c] += fifo_[src + (size_t)c];
        }
        fifoRead_ = (fifoRead_ + (size_t)frames) % cap;
        fifoFrames_ = fifoFrames_ >= (size_t)frames ? fifoFrames_ - (size_t)frames : 0;
    }

    bool playing() const { return playing_.load(std::memory_order_acquire); }
    // The engine is audible while it plays and for a short tail after the last
    // note activity (so note-offs delivered on stop still render).
    bool audible() const {
        return rackReady_ && (engineOn_.load(std::memory_order_acquire) || playing_.load(std::memory_order_acquire)
              || nowMs() - lastActivityMs_.load(std::memory_order_acquire) < 2500);
    }
    void setDrivesTransport(bool drive) { driving_.store(drive, std::memory_order_release); }
    bool drivesTransport() const { return rackReady_ && driving_.load(std::memory_order_acquire) && playing(); }
    double timeSeconds() const { return time_.load(std::memory_order_acquire); }

private:
    struct Staged { int trackId = 0; uint8_t status = 0, d1 = 0, d2 = 0; };
    struct ActiveNote { int trackId = 0; int pitch = 0; double end = 0.0; };
    struct ScriptEntry {
        VstScript s;
        vstscript::Program prog;
        vstscript::Runtime rt;
        bool started = false;
        double lastP[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        bool lastPValid[8] = {false, false, false, false, false, false, false, false};
    };

    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void touch() { lastActivityMs_.store(nowMs(), std::memory_order_release); }
    bool renderDue() const { return audible(); }

    size_t seekCursorLocked(double t) const {
        const std::vector<VstSoloNote>& notes = notesIndex_.load(std::memory_order_relaxed) == 0 ? notesA_ : notesB_;
        size_t i = 0;
        while (i < notes.size() && notes[i].start < t - 1e-9) ++i;
        return i;
    }
    void stageAllNotesOffLocked() {   // stageMutex_ held
        for (int id : activeIds_) { Staged s; s.trackId = id; s.status = 0xB0; s.d1 = 123; s.d2 = 0; staged_.push_back(s); }
    }
    void fireNote(int trackId, int status, int pitch, int d2) {
        caw::MidiMsg m; m.status = (uint8_t)status; m.d1 = (uint8_t)pitch; m.d2 = (uint8_t)d2;
        rack_.pushMidi(trackId, m);
        if (!firstNoteLogged_ && status >= 0x80) {
            firstNoteLogged_ = true;
            if (log_) log_("[caw] VSTi engine: notes are reaching the plugins.\n");
        }
    }

    // Clip code (VST-only path): one interpreter tick per rack block, on the transport clock.
    // Notes go straight to the rack (this is the rack thread); note-offs ride the same active_
    // list as the scheduled MIDI notes. P1..P8 written by the code become live plugin overrides.
    void tickScripts(double t0, double dt) noexcept {
        std::lock_guard<std::mutex> lock(scriptMutex_);
        if (scriptsReset_.exchange(false, std::memory_order_acq_rel)) {
            for (ScriptEntry& e : scripts_) {
                e.started = false;
                e.rt.init(e.prog, e.s.params);
                for (int k = 0; k < 8; ++k) e.lastPValid[k] = false;
            }
        }
        if (scripts_.empty()) return;
        for (ScriptEntry& e : scripts_) {
            const VstScript& s = e.s;
            if (t0 + 1e-9 < s.start || t0 >= s.start + s.dur - 1e-9) continue;
            bool first = false;
            if (!e.started) {
                e.started = true;
                first = true;
                e.rt.init(e.prog, s.params);
                for (int k = 0; k < 8; ++k) e.lastPValid[k] = false;
            }
            vstscript::Env env;
            env.t = std::max(0.0, t0 - s.start);
            env.dur = s.dur;
            env.dt = first ? 0.0 : dt;
            for (int k = 0; k < 8; ++k) env.p[k] = e.rt.p[k];
            vstscript::Events ev;
            vstscript::tick(e.prog, env, e.rt, ev);
            for (int k = 0; k < 8; ++k) e.rt.p[k] = env.p[k];
            for (const vstscript::Events::Note& n : ev.notes) {
                fireNote(s.trackId, 0x90, n.pitch, std::min(127, std::max(0, n.vel)));
                if (active_.size() < 256) {
                    ActiveNote a;
                    a.trackId = s.trackId;
                    a.pitch = n.pitch;
                    a.end = t0 + std::max(0.005, n.len);
                    active_.push_back(a);
                }
            }
            for (const vstscript::Events::Midi& m : ev.midi) fireNote(s.trackId, m.status, m.d1, m.d2);
            for (int k = 0; k < 8; ++k) {
                if (!e.prog.writesP[k]) continue;
                const double v = std::min(1.0, std::max(0.0, env.p[k]));
                if (!e.lastPValid[k] || std::fabs(v - e.lastP[k]) > 1e-4) {
                    e.lastP[k] = v;
                    e.lastPValid[k] = true;
                    rack_.pushParamOverride(s.trackId, k, v);
                }
            }
        }
    }

    // One rack block: drain staged MIDI, schedule notes, render, publish.
    void renderBlock(size_t fifoCap) noexcept {
        const double dt = (double)block_ / (double)sr_;
        {   // staged MIDI from the UI and the Csound forwarder
            std::vector<Staged> now;
            { std::lock_guard<std::mutex> lock(stageMutex_); now.swap(staged_); }
            for (const Staged& s : now) fireNote(s.trackId, s.status, s.d1, s.d2);
        }
        if (playing_.load(std::memory_order_acquire)) {
            const double t0 = time_.load(std::memory_order_acquire);
            double t1 = t0 + dt;
            bool wrapped = false;
            if (loop_ && loopEnd_ > loopStart_ && t1 >= loopEnd_) { t1 = loopEnd_; wrapped = true; }
            const std::vector<VstSoloNote>& notes = notesIndex_.load(std::memory_order_acquire) == 0 ? notesA_ : notesB_;
            const size_t n = notes.size();
            size_t cur = cursor_.load(std::memory_order_relaxed);
            // note-offs for notes that end inside this window
            for (size_t a = 0; a < active_.size();) {
                if (active_[a].end <= t1) {
                    fireNote(active_[a].trackId, 0x80, active_[a].pitch, 0);
                    active_[a] = active_.back(); active_.pop_back();
                } else ++a;
            }
            // note-ons whose start falls inside this window
            while (cur < n && notes[cur].start < t1) {
                if (notes[cur].start >= t0 - 1e-9 && active_.size() < 256) {
                    const int pitch = std::min(127, std::max(0, notes[cur].pitch));
                    const int vel = std::min(127, std::max(1, notes[cur].velocity));
                    fireNote(notes[cur].trackId, 0x90, pitch, vel);
                    ActiveNote an; an.trackId = notes[cur].trackId; an.pitch = pitch;
                    an.end = notes[cur].start + std::max(0.005, notes[cur].dur);
                    active_.push_back(an);
                }
                ++cur;
            }
            cursor_.store(cur, std::memory_order_relaxed);
            tickScripts(t0, dt);
            if (wrapped) {
                for (const ActiveNote& a : active_) fireNote(a.trackId, 0x80, a.pitch, 0);
                active_.clear();
                cursor_.store(0, std::memory_order_relaxed);
                time_.store(loopStart_ + std::max(0.0, t0 + dt - loopEnd_), std::memory_order_release);
                scriptsReset_.store(true, std::memory_order_release);
            } else {
                time_.store(t1, std::memory_order_release);
            }
        } else {
            active_.clear();   // nothing scheduled while stopped
        }
        // render the plugins into the spout and append to the FIFO
        std::fill(spout_.begin(), spout_.end(), 0.f);
        {   // live mute / solo
            std::lock_guard<std::mutex> lock(stageMutex_);
            renderActive_ = liveActive_;
        }
        rack_.process(spout_.data(), nullptr, 1.f, &renderActive_, true, nullptr);
        if (fifoFrames_ + (size_t)block_ <= fifoCap) {
            for (int i = 0; i < block_; ++i) {
                const size_t dst = ((fifoRead_ + fifoFrames_ + (size_t)i) % fifoCap) * (size_t)channels_;
                for (int c = 0; c < channels_; ++c) fifo_[dst + (size_t)c] = spout_[(size_t)i * (size_t)channels_ + (size_t)c];
            }
            fifoFrames_ += (size_t)block_;
        }
    }

    LogFn log_;
    VstRack rack_;
    VstParamMirror mirror_;   // engine -> UI: the P1..P8 values the plugins are actually using
    std::atomic<bool> rackReady_{false};
    std::mutex renderMutex_;            // audio callback (try-lock) vs load / unload / transport resets
    std::atomic<bool> engineOn_{false};
    std::set<int> liveActive_;          // stageMutex_ guarded: the instruments that sound (live M / S)
    std::set<int> renderActive_;        // audio thread copy of liveActive_
    std::string jobsSig_;
    int sr_ = 48000, channels_ = 2, block_ = 512;
    std::set<int> activeIds_;
    std::vector<float> spout_;          // one rack block, interleaved (channels_)
    std::vector<float> fifo_;           // ring of rendered audio
    size_t fifoRead_ = 0, fifoFrames_ = 0;

    std::mutex control_;                // transport / schedule (UI thread) vs render constants
    std::vector<VstSoloNote> notesA_, notesB_;
    std::atomic<int> notesIndex_{0};    // which buffer the audio thread reads
    std::atomic<size_t> cursor_{0};
    std::atomic<double> time_{0.0};
    std::atomic<bool> playing_{false};
    std::atomic<bool> driving_{false};
    std::atomic<long long> lastActivityMs_{0};
    bool loop_ = false; double loopStart_ = 0.0, loopEnd_ = 0.0;   // control_ guarded
    std::vector<ActiveNote> active_;    // audio thread only
    bool firstNoteLogged_ = false;      // audio thread only

    std::mutex stageMutex_;             // UI / forwarder -> audio thread
    std::vector<Staged> staged_;

    std::mutex scriptMutex_;            // UI: (re)load the clip-code programs vs renderBlock ticking them
    std::vector<ScriptEntry> scripts_;  // the VST-only clip code (empty while Csound owns it)
    unsigned long long scriptsSig_ = 0;
    std::atomic<bool> scriptsReset_{false};
};

// The single 4th-engine instance (defined once; the runner and the app share it).
inline VstSoloEngine& vstSolo() { static VstSoloEngine engine; return engine; }
