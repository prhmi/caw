#pragma once
#include "volume_limits.h"
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

// Named Csound clip tap channels captured by the Csound worker (one push per ksmps cycle) for a
// VST graph that runs OUTSIDE the Csound thread (the 4th engine). SEPARATED ENGINES: Csound never
// loads VST plugins - it only publishes these taps, so a VST-side stall (editor work, plugin
// churn) can never reach Csound's audio thread. The pusher never blocks (try_lock: one ksmps
// chunk is dropped on contention) and a short read is silence, never a held sample.
namespace vsttaps {
    struct Ring {
        std::mutex m;
        std::vector<float> buf;
        size_t cap = 16384, w = 0, r = 0, count = 0;
        bool primed = false;
    };
    inline std::mutex& registryLock() { static std::mutex m; return m; }
    inline std::map<std::string, std::shared_ptr<Ring>>& registry() { static std::map<std::string, std::shared_ptr<Ring>> r; return r; }
    inline void push(const std::string& name, const float* src, int n) {
        if (!src || n <= 0) return;
        std::shared_ptr<Ring> ring;
        { std::lock_guard<std::mutex> g(registryLock()); auto& slot = registry()[name]; if (!slot) slot = std::make_shared<Ring>(); ring = slot; }
        // Always written (a short wait on the reader's copy, microseconds): dropping one channel's
        // chunk while another channel of the same clip got through shifted them against each other.
        std::lock_guard<std::mutex> g(ring->m);
        if (ring->buf.size() < ring->cap) ring->buf.assign(ring->cap, 0.f);
        for (int i = 0; i < n; ++i) {
            ring->buf[ring->w] = src[i];
            ring->w = (ring->w + 1) % ring->cap;
            if (ring->count < ring->cap) ++ring->count; else ring->r = (ring->r + 1) % ring->cap;
        }
        ring->primed = true;
    }
    // Fills dst (n frames). Returns false only if the tap was never seen at all (UI warns).
    inline bool pop(const std::string& name, float* dst, int n) {
        if (!dst || n <= 0) return false;
        std::shared_ptr<Ring> ring;
        { std::lock_guard<std::mutex> g(registryLock()); auto it = registry().find(name); if (it == registry().end()) return false; ring = it->second; }
        int got = 0;
        bool known = false;
        std::unique_lock<std::mutex> g(ring->m, std::try_to_lock);
        if (g.owns_lock()) {
            got = (int)std::min((size_t)n, ring->count);
            for (int i = 0; i < got; ++i) { dst[i] = ring->buf[ring->r]; ring->r = (ring->r + 1) % ring->cap; }
            ring->count -= (size_t)got;
            known = ring->primed;
        }
        for (int i = got; i < n; ++i) dst[i] = 0.f;
        return known || got > 0 || !g.owns_lock();
    }
    // How many frames are queued on a tap (0 if unknown). Used to pop SEVERAL taps of one source
    // with the SAME frame count: each channel has its own ring, and popping "what is there" per
    // channel let L / R (or ch1..ch4) of one clip drift by a few frames whenever the producer had
    // pushed one of them and not yet the other - a random phase shift (two traces on the scope).
    inline size_t queued(const std::string& name) {
        std::shared_ptr<Ring> ring;
        { std::lock_guard<std::mutex> g(registryLock()); auto it = registry().find(name); if (it == registry().end()) return 0; ring = it->second; }
        std::lock_guard<std::mutex> g(ring->m);
        return ring->count;
    }
    // Pop exactly `n` frames from EVERY named tap in lock-step (silence-padded together when they
    // are short). The ring that has the fewest frames decides; extra frames on the others wait for
    // the next block instead of being consumed early. Returns false if none of the taps exist.
    inline bool popAligned(const std::vector<std::string>& names, std::vector<float*>& dst, int n) {
        if (names.empty() || n <= 0) return false;
        std::vector<std::shared_ptr<Ring>> rings(names.size());
        bool any = false;
        { std::lock_guard<std::mutex> g(registryLock());
          for (size_t k = 0; k < names.size(); ++k) { auto it = registry().find(names[k]); if (it != registry().end()) { rings[k] = it->second; any = true; } } }
        for (size_t k = 0; k < dst.size(); ++k) if (dst[k]) std::fill_n(dst[k], n, 0.f);
        if (!any) return false;
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(rings.size());
        size_t common = (size_t)n;
        for (auto& r : rings) {
            if (!r) continue;
            locks.emplace_back(r->m);
            common = std::min(common, r->count);
        }
        // a backlog (the producer ran ahead in a burst): every channel skips the SAME number of frames
        size_t backlog = (size_t)-1;
        for (auto& r : rings) if (r) backlog = std::min(backlog, r->count);
        const size_t keep = (size_t)n * 4;
        const size_t drop = backlog != (size_t)-1 && backlog > keep ? backlog - keep : 0;
        for (size_t k = 0; k < rings.size(); ++k) {
            auto& r = rings[k];
            if (!r) continue;
            if (drop) { r->r = (r->r + drop) % r->cap; r->count -= drop; }
            const size_t take = std::min(common, r->count);
            float* out = k < dst.size() ? dst[k] : nullptr;
            for (size_t i = 0; i < take; ++i) { if (out) out[i] = r->buf[r->r]; r->r = (r->r + 1) % r->cap; }
            r->count -= take;
        }
        return true;
    }
    inline void clear() { std::lock_guard<std::mutex> g(registryLock()); registry().clear(); }
}
#include "monitor_view.h"   // (was monitor_capture.h, phase-1 merge)
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

// ONE INSTANCE PER PLUGIN (like a DAW): the live engine asks the UI for the plugin object that is
// already loaded in a slot (the one whose window the user sees) and renders THAT object - no second
// copy, no parameter sync, no state copy, no window feed. Offline renders (freeze / export) do not
// set the provider and keep building their own temporary copies from the saved state.
// key = trackId * 8 + stage, path / classId must match. Called on the loader / init thread.
#ifdef CAW_VST3
using VstSharedProvider = std::function<std::shared_ptr<caw::IPlugin>(int key, const std::string& path, const std::string& classId)>;
inline VstSharedProvider& vstSharedProvider() { static VstSharedProvider p; return p; }
#endif
class VstRack {
public:
    using PtrFn = std::function<void*(const std::string&, int)>;     // channel name, flags -> memory (or nullptr)
    using TableFn = std::function<const void*(int)>;                 // table number -> memory (or nullptr)
    using LogFn = std::function<void(const std::string&)>;
    // Per-track output peaks of the 4th engine (its instruments never reach the Csound meter
    // layout, so the lane meters read them here). Values are max-held since the last take.
    struct SoloPeak { int trackId = 0; std::array<float, 8> value{}; };   // one peak per plugin output channel (up to 8)
    ~VstRack() { clear(); }
    void takeSoloPeaks(std::vector<SoloPeak>& out) {
        std::lock_guard<std::mutex> lock(peakMutex_);
        out.clear();
        for (auto& entry : soloPeaks_) {
            SoloPeak sp; sp.trackId = entry.first;
            for (int c = 0; c < 8; ++c) sp.value[(size_t)c] = entry.second[(size_t)c];
            out.push_back(sp);
            entry.second.fill(0.f);
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
    void pushParamOverride(int trackId, int slot, double value, int stage = 0) {
        if (slot < 0 || slot >= 8 || !std::isfinite(value)) return;
        std::lock_guard<std::mutex> lock(overrideMutex_);
        VstParamOverride ov; ov.trackId = trackId; ov.slot = slot; ov.value = std::min(1.0, std::max(0.0, value));
        ov.stage = std::min(7, std::max(0, stage));
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
    struct VstParamValue { int trackId = 0; int index = 0; double value = 0; int stage = 0; };
    void pushParamValue(int trackId, int paramIndex, double value, int stage = 0) {
        if (paramIndex < 0 || paramIndex > 4095 || !std::isfinite(value)) return;
        std::lock_guard<std::mutex> lock(overrideMutex_);
        VstParamValue pv; pv.trackId = trackId; pv.index = paramIndex; pv.value = std::min(1.0, std::max(0.0, value));
        pv.stage = std::min(7, std::max(0, stage));
        pendingValues_.push_back(pv);
    }
    // SEPARATED ENGINES: full plugin state from the editor instance (Kontakt's loaded .nki, FX
    // presets with internal state - binary state, not a parameter). NEVER applied on the audio
    // thread: a sampler loading a patch blocks for seconds and may synchronize with the plugin's
    // own GUI thread (the host UI thread) - doing that inside the audio callback froze the app.
    // applyStateNow() is called by the engine's state worker while it HOLDS the render mutex,
    // so the audio callback skips blocks instead of deadlocking.
#ifdef CAW_VST3
    void applyStateNow(int trackId, const std::string& blob, const std::string& originPath = {}, const std::string& originClassId = {}, int stage = 0) {
        if (blob.empty()) return;
        std::shared_ptr<Graph> graph;
        { std::lock_guard<std::mutex> lock(graphMutex_); graph = graph_; }
        if (!graph) return;
        for (Rt& r : graph->rts) {
            if (r.job.id != trackId) continue;
            std::shared_ptr<Inst> inst; std::shared_ptr<caw::IPlugin> plug; std::string jobPath, jobClass;
            if (stage <= 0) { inst = r.inst; plug = r.plug; jobPath = r.job.path; jobClass = r.job.classId; }
            else {
                const int st = std::min(7, std::max(1, stage));
                inst = r.stage[st - 1].inst; plug = r.stage[st - 1].plug;
                jobPath = (size_t)(st - 1) < r.job.chain.size() ? r.job.chain[(size_t)(st - 1)].path : std::string();
                jobClass = (size_t)(st - 1) < r.job.chain.size() ? r.job.chain[(size_t)(st - 1)].classId : std::string();
            }
            if (inst && inst->shared) return;   // ONE INSTANCE: the window edited this very object - nothing to copy
            if (!inst || !plug) continue;
            // The blob must come from the plugin the track runs NOW: a capture that was in flight
            // while the user switched the plugin is stale and would make the new plugin reject its
            // own load (kHs: "Expected 'filter' but got 'equalizer'").
            if (!originPath.empty() && jobPath != originPath) return;
            if (!originClassId.empty() && !jobClass.empty() && jobClass != originClassId) return;
            if (inst->appliedState == blob) return;   // already applied - also dedupes the creation path
            try {
                plug->setState(blob);
                inst->appliedState = blob;
                if (log_) log_("[caw] VST track \"" + r.job.name + "\": patch state applied to the engine (" + std::to_string(blob.size()) + " bytes).\n");
            } catch (...) {
                if (log_) log_("[caw] VST track \"" + r.job.name + "\": applying the patch state threw - the track keeps its previous state.\n");
            }
            return;
        }
    }
#endif
    // SEPARATED ENGINES: the engine instance of a track, for the UI thread's editor feed (EQ
    // analyzers, synth meters replay the engine's audio + notes in processIdle). A shared
    // pointer, so it stays alive even while the loader thread replaces the graph. Any thread.
    // (Only exists in VST3 builds; without plugin hosting there is no feed source.)
#ifdef CAW_VST3
    std::shared_ptr<caw::IPlugin> pluginOfTrack(int trackId) {
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return {};
        for (Rt& r : g->rts) if (r.job.id == trackId && r.plug) return r.plug;
        return {};
    }
    // The engine instance of ONE chain stage (0 = the first effect): the editor feed source for
    // that stage's plugin window, and the plugin the editor's state capture is keyed to.
    std::shared_ptr<caw::IPlugin> pluginOfTrackStage(int trackId, int stage) {
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return {};
        for (Rt& r : g->rts) {
            if (r.job.id != trackId) continue;
            if (stage <= 0) return r.plug;
            const int st = std::min(7, std::max(1, stage));
            return r.stage[st - 1].plug;
        }
        return {};
    }
    // The chain END: the last loaded stage's plug (the plugin that decides the track's meter
    // width). A single-plugin VSTe / VSTi returns the first plugin.
    std::shared_ptr<caw::IPlugin> pluginOfTrackEnd(int trackId) {
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return {};
        for (Rt& r : g->rts) {
            if (r.job.id != trackId) continue;
            for (int st = 6; st >= 0; --st) if (r.stage[st].plug) return r.stage[st].plug;
            return r.plug;
        }
        return {};
    }
#endif
    // The 4th engine (app-side VSTi rack) has no Csound channels or note ring:
    // MIDI arrives directly. Call from the ONE thread that also calls process().
    void setSoloMode(bool solo) { soloMode_ = solo; }
    void setShareUiInstances(bool on) { shareUi_ = on; }   // the live VST engine: render the UI's own plugin objects
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
    // Live output width of a VSTi / VSTe track: 0 = stereo (the plugin's channels are mixed as
    // stereo like the master fold; 2 meters), 1 = as source (the plugin's own channels 1:1). UI thread.
    void setOutWide(int trackId, int mode) {
        std::lock_guard<std::mutex> lock(outModeMutex_);
        outWides_[trackId] = mode == 1 ? 1 : 0;
    }
    // Live track fader: applied where the track's output joins the master (0..2). UI thread.
    void setTrackVolume(int trackId, float v) {
        std::lock_guard<std::mutex> lock(volumeMutex_);
        volumes_[trackId] = std::max(0.f, std::min(kMaxVolumeGain, v));
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
    // The plugin's reported latency for one track's instance (samples), 0 when absent. Used to
    // align the raw source with the effect's output in mix mode (dry/wet delay fix).
    int latencyFor(int trackId) const {
        std::shared_ptr<Graph> g = currentGraph();
        if (!g) return 0;
        for (const Rt& rt : g->rts) {
            if (rt.job.id != trackId) continue;
            for (int st = 6; st >= 0; --st) if (rt.stage[st].inst && rt.stage[st].plug) return rt.stage[st].plug->latencySamples();
            if (rt.inst && rt.inst->plug) return rt.inst->plug->latencySamples();
        }
        return 0;
    }
    // All loaded instances' reported latencies (track id -> samples). Read on the UI thread only.
    std::map<int, int> latencies() const {
        std::shared_ptr<Graph> g = currentGraph();
        std::map<int, int> m;
        if (g) for (const Rt& rt : g->rts) {
            if (rt.inst && rt.inst->plug) m[rt.job.id] = rt.inst->plug->latencySamples();
            for (int st = 6; st >= 0; --st) if (rt.stage[st].inst && rt.stage[st].plug) { m[rt.job.id] = rt.stage[st].plug->latencySamples(); break; }
        }
        return m;
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
        scMixPending_.clear(); scPending_.clear(); samplePending_.clear();
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
        tapBuf_.assign((size_t)std::max(1, block), 0.f);   // the tap-ring scratch follows the block
        scMixPending_.clear(); scPending_.clear(); samplePending_.clear();
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
        { std::lock_guard<std::mutex> lock(outModeMutex_); outModes_.clear(); for (const auto& kv : g->outModes) outModes_[kv.first] = kv.second; outWides_.clear(); for (const auto& kv : g->outWides) outWides_[kv.first] = kv.second; }
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
                scMixPending_.clear(); scPending_.clear(); samplePending_.clear();
                { std::lock_guard<std::mutex> lock(sigMutex_); liveSig_ = graph->sig; }
                { std::lock_guard<std::mutex> lock(outModeMutex_); for (const auto& kv : graph->outModes) outModes_[kv.first] = kv.second; for (const auto& kv : graph->outWides) outWides_[kv.first] = kv.second; }
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
                    if (pv.trackId == r.job.id) {
                        std::shared_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
                        if (pv.stage <= 0) { plug = r.plug; ep = r.ep; }
                        else if (pv.stage <= 7) { plug = r.stage[pv.stage - 1].plug; ep = r.stage[pv.stage - 1].ep; }
                        if (!plug || !ep) continue;
                        if (pv.index < (int)plug->params().size() && !plug->params()[(size_t)pv.index].readOnly) {
                            caw::ParamMsg m; m.paramIndex = pv.index; m.value = pv.value;
                            ep->paramIn.push(m);   // the engine instance applies it on this very block
                        }
                    }
            // Patch state (Kontakt's loaded .nki) is applied by the engine's STATE WORKER while
            // it holds the render mutex (applyStateNow) - NEVER here: a sampler loading a patch
            // blocks for seconds and must not run on the audio thread. The creation path applies
            // the job's blob on the loader thread; a reused instance keeps its Inst, so its
            // appliedState dedupe carries over the graph swap untouched.
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
                            std::shared_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
                            const int ovStage = ov.slot < (int)r.job.slotStage.size() ? r.job.slotStage[(size_t)ov.slot] : ov.stage;
                            if (ovStage <= 0) { plug = r.plug; ep = r.ep; }
                            else if (ovStage <= 7) { plug = r.stage[ovStage - 1].plug; ep = r.stage[ovStage - 1].ep; }
                            const int pick = ov.slot < (int)r.job.pick.size() ? r.job.pick[(size_t)ov.slot] : -1;
                            if (plug && ep && pick >= 0 && (size_t)pick < plug->params().size()) {
                                caw::ParamMsg m; m.paramIndex = pick; m.value = ov.value;
                                ep->paramIn.push(m);
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
        std::map<int, float> volumesLocal;
        { std::lock_guard<std::mutex> lock(volumeMutex_); volumesLocal = volumes_; }
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
        // Dry/wet alignment for VSTi sources in mix mode: a latency-reporting effect delivers its
        // output later than the source's direct master mix, so the direct mix is delayed by the
        // effect's reported latency (the effect itself still receives the UNDELAYED source).
        std::map<int, int> dryDelayOf;
        for (const Rt& e : rts) {
            if (e.job.instrument) continue;
            if (activeValid && active && active->find(e.job.id) == active->end()) continue;
            const auto om = outModesLocal.find(e.job.id);
            if ((om != outModesLocal.end() ? om->second : e.job.outMode) != 1) continue;   // mix mode only (replace mutes the source anyway)
            const int lat = e.inst && e.inst->plug ? e.inst->plug->latencySamples() : 0;
            if (lat <= 0 || lat > 20000) continue;
            for (int s = 0; s < 8; ++s) {
                const auto& src = e.job.inputs[s];
                if (!src.on || src.lineCount <= 0 || src.engine != 3 || src.srcId == e.job.id) continue;
                const auto mk = masksUse->find(e.job.id);
                if (mk != masksUse->end() && !((mk->second >> s) & 1u)) continue;   // input switched off live
                int& d = dryDelayOf[src.srcId];
                if (lat > d) d = lat;
            }
        }
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
        if (needSc && ea_ && ea_->scRing) pullRing(ea_->scRing, scMixPending_, scScratch_);
        else needSc = false;
        // One pull per block per TRACK-SLOT ring (several VSTe tracks may consume the same SC track).
        // Every SuperCollider track has its own ring now - a VSTe hears ITS track, not the whole SC mix.
        scPop_.clear();
        std::set<int> scNeeded;
        for (const Rt& r : rts)
            for (const auto& src : r.job.inputs)
                if (src.on && src.engine == 2 && src.lineCount > 0 && !scPop_.count(src.srcId)) {
                    scNeeded.insert(src.srcId);
                    CawScRing* ring = (ea_ && ea_->scTrackRing) ? ea_->scTrackRing(src.srcId) : nullptr;
                    if (ring) pullRing(ring, scPending_[src.srcId], scPop_[src.srcId]);
                }
        for (auto it = scPending_.begin(); it != scPending_.end(); )
            if (!scNeeded.count(it->first)) it = scPending_.erase(it); else ++it;
        // One pull per block per sample-track ring (several VSTe tracks may consume the same mix).
        samplePop_.clear();
        std::set<int> sampleNeeded;
        for (const Rt& r : rts)
            for (const auto& src : r.job.inputs)
                if (src.on && src.engine == 4 && src.lineCount > 0 && !samplePop_.count(src.srcId)) {
                    sampleNeeded.insert(src.srcId);
                    CawScRing* ring = (ea_ && ea_->sampleRing) ? ea_->sampleRing(src.srcId) : nullptr;
                    if (ring) pullRing(ring, samplePending_[src.srcId], samplePop_[src.srcId]);
                }
        for (auto it = samplePending_.begin(); it != samplePending_.end(); )
            if (!sampleNeeded.count(it->first)) it = samplePending_.erase(it); else ++it;
        // One pull per block: the dedicated Pure Data engine's live stereo mix ring (engine 1).
        // Empty when the ring is absent (mixed mode / the offline render) - the pdMix pointer
        // fallback in the engine-1 branch serves those paths.
        bool needPd = false;
        for (const Rt& r : rts)
            for (const auto& src : r.job.inputs)
                if (src.on && src.engine == 1 && src.lineCount > 0) needPd = true;
        if (needPd && ea_ && ea_->pdRing) {
            pdPop_.assign((size_t)block_ * 2, 0.f);
            pullRing(ea_->pdRing, pdPending_, pdPop_);
        } else pdPop_.clear();
        // Notes of the VSTi tracks arrive through the note ring above (the host's transport wrote
        // them: Csound's score via CawVstNote_<id>, or the 4th engine via pushMidi).
        for (Rt& r : rts) {
            const bool isActive = !(activeValid && active && active->find(r.job.id) == active->end());   // live M / S
            unsigned mask = 0xFFu;
            if (masksUse) { const auto it = masksUse->find(r.job.id); if (it != masksUse->end()) mask = it->second; }
            // the 8 picked parameters: the channel value, with a fresh manual override winning on
            // the SAME block it arrives. Channel value and UI mirror are updated even for a muted
            // track, so the plugin GUI never fights a stale mirror; a track that comes back from
            // mute force-syncs its plugin to the current values.
            const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!r.chansResolved) resolveChans(r);   // a live pick invalidated them: resolve on this thread
            for (int k = 0; k < 8 && k < (int)r.job.pick.size(); ++k) {
                // A slot can sit on any chain stage: P1..P8 lanes stay keyed by the SLOT, the
                // value is applied to the OWNING stage's engine instance.
                std::shared_ptr<caw::IPlugin> slotPlug; std::shared_ptr<caw::Endpoint> slotEp;
                const int slotSt = k < (int)r.job.slotStage.size() ? std::min(7, std::max(0, r.job.slotStage[(size_t)k])) : 0;
                if (slotSt <= 0) { slotPlug = r.plug; slotEp = r.ep; }
                else { slotPlug = r.stage[slotSt - 1].plug; slotEp = r.stage[slotSt - 1].ep; }
                if (!slotPlug || !slotEp) continue;
                const size_t np = slotPlug->params().size();
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
                if (isActive && (changed || !r.wasActive)) { caw::ParamMsg m; m.paramIndex = r.job.pick[k]; m.value = v; slotEp->paramIn.push(m); }
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
            bool anySidechainThisBlock = false;
            // The plugin's real output width: a 4-channel VSTi plays 4 channels (its own
            // bus info), not a forced stereo pair. routedIn effects keep 8 packed lines.
            // The TRACK's channel count (the user's choice, like a DAW track format) is the width the
            // plugin is processed at - never re-detected from the plugin, so a plugin that "follows
            // the host" cannot change width mid-play (which shifted channels against each other).
            // Routed effects still pack up to 8 input lines; the output is read at the track width.
            const int trackCh = std::max(1, std::min(8, r.job.channels));
            const int chPass = r.job.routedIn ? std::max(trackCh, 2) : trackCh;
            int packedLines = 0;               // how many input lines the plugin actually hears (for the diagnostics)
            r.tapsMissing = 0;
            const bool pdMixSet = ea_ && ea_->pdMix && ea_->pdMix->load(std::memory_order_acquire) != nullptr;   // for the diagnostics below
            if (!r.job.instrument && r.job.routedIn) {
                for (std::size_t i = 0; i < (std::size_t)block_ * 8; ++i) r.in[i] = 0.f;
                for (std::size_t i = 0; i < (std::size_t)block_ * 2; ++i) r.sc[i] = 0.f;
                bool anySidechain = false;   // an enabled insert feeds the plugin's sidechain bus
                // SUM the ENABLED input slots channel by channel: every insert's line 1 (L) adds into
                // plugin line 1, line 2 (R) into line 2, and so on - the plugin hears ONE mixed signal.
                // (They used to be stacked on separate lines 1-2, 3-4, ...: a stereo plugin only has
                // its first two input channels, so it heard the first insert and the others were lost.)
                // A slot switched off live simply drops out of the sum.
                const float* pdMix = ea_ && ea_->pdMix ? ea_->pdMix->load(std::memory_order_acquire) : nullptr;
                const int pdChans = ea_ ? ea_->pdMixChannels : 0;
                int outLine = 0;   // highest line any enabled slot writes (+1)
                for (int i = 0; i < 8; ++i) {
                    const auto& src = r.job.inputs[i];
                    if (!src.on || !(mask & (1u << i)) || src.lineCount <= 0) continue;
                    // A Csound source outside the Csound thread: pop ALL its channel taps in lock-step
                    // first (one frame count for every channel - no inter-channel phase drift).
                    bool tapsAligned = false;
                    if (src.engine == 0 && !src.csChans.empty()) {
                        bool live = false;
                        for (const std::string& nm : src.csChans) if (chanOf(nm)) { live = true; break; }
                        if (!live) {
                            const size_t nch = std::min<size_t>(8, src.csChans.size());
                            if (tapAligned_.size() < nch * (size_t)block_) tapAligned_.assign(nch * (size_t)block_, 0.f);
                            std::vector<std::string> names(src.csChans.begin(), src.csChans.begin() + (long)nch);
                            std::vector<float*> ptrs(nch);
                            for (size_t c = 0; c < nch; ++c) ptrs[c] = tapAligned_.data() + c * (size_t)block_;
                            tapsAligned = vsttaps::popAligned(names, ptrs, block_);
                        }
                    }
                    for (int k = 0; k < src.lineCount && k < 8; ++k) {
                        // a SIDECHAIN insert adds into its own stereo block (L / R), every other insert into the shared main input lines
                        float* dst = src.sidechain ? r.sc.data() : r.in.data();
                        const std::size_t stride = src.sidechain ? 2 : 8;
                        const int line = src.sidechain ? std::min(k, 1) : k;   // the shared plugin input line this slot channel adds into
                        if (src.sidechain) anySidechain = true; else outLine = std::max(outLine, k + 1);
                        if (src.mode == 2) {   // master: the whole master mix - Csound's spout plus the VSTi output
                            const int mc = k == 0 ? 0 : (nchan_ > 1 ? 1 : 0);
                            for (int n = 0; n < block_; ++n) {
                                float v = spout ? (float)(rdSpout(spout, n, mc) / fullScale) : 0.f;
                                for (const Rt* o : replacedInstr) {
                                    const int sCh = std::max(1, o->pubCh);
                                    if (mc < sCh && !o->pub.empty()) v += o->pub[(std::size_t)n * sCh + mc];
                                }
                                dst[(std::size_t)n * stride + line] += v;
                            }
                        } else if (src.engine == 1) {   // the Pure Data mix (device scale): line 1 = L, line 2 = R
                            const int mc = k == 0 ? 0 : 1;
                            if (!pdPop_.empty()) {   // the dedicated PD engine's ring: block-aligned, preferred
                                for (int n = 0; n < block_; ++n) dst[(std::size_t)n * stride + line] += pdPop_[(std::size_t)n * CawScRing::CH + mc];
                            } else if (pdMix && pdChans > 0) {   // the shared render: fold the patch's channels (k mod 2) like every engine
                                for (int n = 0; n < block_; ++n) {
                                    float v = 0.f;
                                    for (int c = mc; c < pdChans; c += 2) v += pdMix[(std::size_t)n * pdChans + c];
                                    dst[(std::size_t)n * stride + line] += v;
                                }
                            }
                        } else if (src.engine == 2 || src.engine == 4) {   // a wide TRACK source (2 = a SuperCollider track slot, 4 = a sample track):
                            // its own mix from its per-slot ring (device scale, up to 8 channels). The
                            // receive mode decides the fold: 0 = stereo (default - channel k plays on
                            // plugin line k mod 2, the virtual soundcard rule, so ch1+3 -> L, ch2+4 -> R),
                            // 1 = multichannel (the channels go 1:1 into the plugin's input lines - a
                            // stereo plugin then hears ch1 | ch2 un-folded).
                            auto& pops = src.engine == 2 ? scPop_ : samplePop_;
                            auto pit = pops.find(src.srcId);
                            const std::vector<float>* view = pit != pops.end() ? &pit->second : nullptr;
                            if (!view && src.engine == 2 && needSc) view = &scScratch_;   // fallback: the whole SC device mix
                            if (view) {
                                if (src.receive == 1 && !src.sidechain) {
                                    if (k == 0) {   // the wide 1:1 sum happens once; the per-line loop has nothing more to add
                                        for (int c = 0; c < CawScRing::CH; ++c) {
                                            outLine = std::max(outLine, c + 1);
                                            for (int n = 0; n < block_; ++n)
                                                dst[(std::size_t)n * stride + c] += (*view)[(std::size_t)n * CawScRing::CH + c];
                                        }
                                    }
                                    continue;   // skip the remaining saved lines: the wide sum already covered them
                                }
                                const int mc = k == 0 ? 0 : 1;
                                for (int n = 0; n < block_; ++n) {
                                    float v = 0.f;   // the virtual soundcard fold: channel k plays on line k mod 2
                                    for (int c = mc; c < CawScRing::CH; c += 2) v += (*view)[(std::size_t)n * CawScRing::CH + c];
                                    dst[(std::size_t)n * stride + line] += v;
                                }
                            }
                        } else if (src.engine == 3) {   // a VSTi / VSTe source: wire its stereo output here directly
                            const int mc = k == 0 ? 0 : 1;   // line 1 = L, line 2 = R - the same channels the source adds to the master
                            const Rt* srcRt = nullptr;
                            if (src.srcId != r.job.id)
                                for (const Rt& o : rts) if (o.job.id == src.srcId) { srcRt = &o; break; }
                            if (srcRt) {   // the source renders in this rack, right above (ordered by dependency)
                                r.srcMissingWarned = false;
                                const bool srcActive = !(activeValid && active && active->find(srcRt->job.id) == active->end());
                                const int sCh = std::max(1, srcRt->pubCh);   // the source's published track-width output
                                if (srcActive && mc < sCh && !srcRt->pub.empty())
                                    for (int n = 0; n < block_; ++n) dst[(std::size_t)n * stride + line] += srcRt->pub[(std::size_t)n * sCh + mc];
                            } else if (!r.srcMissingWarned) {   // the source is not in the rack: added after Run, or its plugin never loaded
                                r.srcMissingWarned = true;
                                if (log_) log_("[caw] VSTe \"" + r.job.name + "\": input \"" + src.name + "\" is a VSTi / VSTe source that is NOT in the rack - it was added after Run, or its plugin did not load. Press Run Csound once so its note ring and audio are wired in.\n");
                            }
                        } else {   // a Csound clip: its named tap channel. Inside the Csound worker
                                   // (ptr_ set) it is read live from Csound; outside it (the 4th
                                   // engine) the worker captures the same channels per ksmps cycle
                                   // into the vsttaps rings and this side pops them - so a VST-side
                                   // stall can never stall Csound's audio.
                            void* ch = k < (int)src.csChans.size() ? chanOf(src.csChans[(size_t)k]) : nullptr;
                            if (ch) {
                                for (int n = 0; n < block_; ++n) dst[(std::size_t)n * stride + line] += (float)rdAt(ch, n) / fullScale;
                            } else if (tapsAligned && k < (int)src.csChans.size() && k < 8) {
                                const float* tb = tapAligned_.data() + (size_t)k * (size_t)block_;
                                for (int n = 0; n < block_; ++n) dst[(std::size_t)n * stride + line] += tb[(size_t)n];
                            } else if (src.lineCount > 0) {
                                ++r.tapsMissing;
                            }
                        }
                    }
                }
                packedLines = outLine;
                anySidechainThisBlock = anySidechain;
                in = r.in.data();   // the summed inputs: line 1 = L, line 2 = R (the plugin hears min(8, its input bus) of them)
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
            r.plug->setSidechain(!r.job.instrument && r.job.routedIn && anySidechainThisBlock ? r.sc.data() : nullptr);
            if (anySidechainThisBlock && r.plug->sidechainChannels() <= 0 && !r.scNoBusWarned) {
                r.scNoBusWarned = true;
                if (log_) log_("[caw] VSTe \"" + r.job.name + "\": an insert is set to SIDECHAIN, but this plugin has no sidechain input bus - that insert is not heard.\n");
            }
            if (in && in == r.in.data()) {   // the inputs were summed at stride 8: compact them to the process stride
                if (chPass != 8)
                    for (int n = 0; n < block_; ++n)
                        for (int c = 0; c < chPass; ++c) r.in[(std::size_t)n * chPass + c] = r.in[(std::size_t)n * 8 + c];
            }
            if (!r.job.instrument && (r.job.bypass & 1u)) {   // effect 1 OFF: its input passes straight through (same width)
                if ((int)r.out.size() < block_ * chPass) r.out.assign((size_t)block_ * (size_t)chPass, 0.f);
                if (in) std::copy_n(in, (size_t)block_ * (size_t)chPass, r.out.begin());
                else std::fill_n(r.out.begin(), (size_t)block_ * (size_t)chPass, 0.f);
            } else
            r.plug->process(in, r.out.data(), block_, chPass, *r.ep);
            // THE EFFECT CHAIN: stage k's input is stage k-1's output. A stage that cannot accept
            // all of the previous stage's channels folds them at its OWN input width - the
            // virtual-soundcard rule generalized per stage (channel c plays on input c mod M):
            // a 4-channel stage into a stereo one mixes 1+3 -> L / 2+4 -> R (the automatic
            // mixdown), into a mono one sums everything; a wider stage pads with silence.
            // Everything downstream (meters, master join, diagnostics) reads the CHAIN END.
            float* curOut = r.out.data();
            int curStride = chPass;
            for (int cst = 1; cst < 8; ++cst) {
                Rt::Stage& sg = r.stage[cst - 1];
                if (!sg.plug) break;
                if (r.job.bypass & (1u << cst)) continue;   // effect OFF: the previous stage's output goes on unchanged
                const int inW = std::min(8, std::max(1, sg.plug->inputChannels()));
                const int outW = std::min(8, std::max(1, sg.plug->outputChannels()));
                // process() reads the input AND writes the output with ONE stride: use the wider of the two
                // bus widths for both (the input used to be packed with stride 8 and read with the output
                // width - the stage heard scrambled channels), then compact the output to outW.
                const int pw = std::max(inW, outW);
                if ((int)sg.in.size() < block_ * 8) sg.in.assign((size_t)block_ * 8, 0.f);
                else std::fill_n(sg.in.begin(), (size_t)block_ * 8, 0.f);
                if ((int)sg.out.size() < block_ * 8) sg.out.assign((size_t)block_ * 8, 0.f);
                for (int i = 0; i < block_; ++i)
                    for (int c = 0; c < curStride; ++c) sg.in[(size_t)i * pw + (size_t)(c % inW)] += curOut[(size_t)i * curStride + c];
                sg.plug->process(sg.in.data(), sg.out.data(), block_, pw, *sg.ep);
                if (pw != outW)   // compact in place (forward copy: the target index never passes the source)
                    for (int i = 0; i < block_; ++i)
                        for (int c = 0; c < outW; ++c) sg.out[(size_t)i * outW + c] = sg.out[(size_t)i * pw + c];
                curOut = sg.out.data();
                curStride = outW;
            }
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
                float outPk = 0.f; for (int n = 0; n < block_; ++n) outPk = std::max(outPk, std::fabs(curOut[(size_t)n * curStride]));
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
                        if (s && srcActive && !s->pub.empty()) { const int sCh = std::max(1, s->pubCh); for (int n = 0; n < block_; ++n) sPk = std::max(sPk, std::fabs(s->pub[(size_t)n * sCh])); }
                        diag += std::string(": ") + (s ? (srcActive ? ("in rack, source peak " + std::to_string(sPk)) : "in rack but MUTED") : "NOT in rack");
                    }
                    if (src.engine == 1) {
                        const bool ringWired = ea_ && ea_->pdRing;
                        diag += std::string(": pdRing=") + (ringWired ? "yes" : "no");
                        if (ringWired) diag += " queued=" + std::to_string(ea_->pdRing->queued()) + " frames";
                        float pdPk = 0.f;
                        if (!pdPop_.empty()) for (int n = 0; n < block_ * 2; ++n) pdPk = std::max(pdPk, std::fabs(pdPop_[(size_t)n]));
                        diag += std::string(", pdPop=") + (pdPop_.empty() ? "off" : (pdPk > 1e-6f ? "audio" : "silence"))
                              + ", pdMix=" + (pdMixSet ? "set" : "null");
                    }
                    diag += "\n";
                }
                if (log_) log_("[caw] VSTe \"" + r.job.name + "\": routedIn=" + (r.job.routedIn ? "yes" : "NO")
                             + ", packedLines=" + std::to_string(packedLines) + ", input peak=" + std::to_string(inPk)
                             + ", output peak=" + std::to_string(outPk)
                             + (outPk < 1e-6f ? "  <-- PLUGIN OUTPUTS NOTHING (wet/dry=0, bypass, or no preset loaded)" : "")
                             + (inPk < 1e-6f && packedLines > 0 ? "  <-- INPUT IS SILENT (the source is not sounding)" : "") + "\n" + diag);
            }
            std::array<float, 8> chPeak{};
            int outCh = std::max(1, std::min(8, r.job.channels));   // the track's channel count: the master join and the meters use it
            // A VSTe never plays more channels than it RECEIVES and its chain can produce: a 6-ch
            // track fed by a 4-ch Csound clip through a 4-ch effect shows / plays 4 (the extra track
            // channels would only ever carry silence). Narrower settings still fold (6 -> 2 etc.).
            if (!r.job.instrument && r.job.routedIn && packedLines > 0)
                outCh = std::max(1, std::min(outCh, std::max(packedLines, std::min(curStride, packedLines))));
            // Fold the chain end to the TRACK width once (channel c plays on track line c mod N, a
            // narrower chain end pads with silence). Everything below reads this one buffer, so the
            // meters, the master join and the VSTe feeds can never disagree about the channel count.
            // OUTPUT ROUTING (out<n> = in<m>, VSTi and VSTe): applied ONCE, here, to the chain end.
            // Output n plays plugin channel m; an unlisted output is silent (out3 missing = empty).
            // Everything after this - meters, the VSTe feeds, the track fader, the master fold
            // (line k -> master k mod N) - reads the routed lines 1:1, like a sample clip's routing.
            bool mapOn = r.job.outMapOn;
            if (mapOn) {
                int maxOut = 0;
                for (int o = 0; o < 8; ++o) if (r.job.outMap[(size_t)o] >= 0) maxOut = o + 1;
                if (maxOut < 1) maxOut = 1;   // every line removed: one silent line
                if ((int)r.trackOut.size() < block_ * 8) r.trackOut.assign((size_t)block_ * 8, 0.f);
                for (int i = 0; i < block_; ++i)
                    for (int o = 0; o < maxOut; ++o) {
                        const int src = r.job.outMap[(size_t)o];
                        r.trackOut[(size_t)i * maxOut + o] = (src >= 0 && src < curStride) ? curOut[(size_t)i * curStride + src] : 0.f;
                    }
                curOut = r.trackOut.data();
                curStride = maxOut;
                outCh = maxOut;
                mapOn = false;   // from here on the routed lines play 1:1
            }
            if (!mapOn && curStride != outCh) {
                if ((int)r.trackOut.size() < block_ * 8) r.trackOut.assign((size_t)block_ * 8, 0.f);
                std::fill_n(r.trackOut.begin(), (size_t)block_ * (size_t)outCh, 0.f);
                for (int i = 0; i < block_; ++i)
                    for (int c = 0; c < curStride; ++c)
                        r.trackOut[(size_t)i * outCh + (size_t)(c % outCh)] += curOut[(size_t)i * curStride + c];
                curOut = r.trackOut.data();
                curStride = outCh;
            }
            if (!mapOn) {   // publish the track-width output for the VSTe inputs that read this track
                if (r.pub.size() < (size_t)block_ * (size_t)curStride) r.pub.resize((size_t)block_ * (size_t)curStride);
                std::copy_n(curOut, (size_t)block_ * (size_t)curStride, r.pub.begin());
                r.pubCh = curStride;
            } else {
                if (r.pub.size() < (size_t)block_ * (size_t)curStride) r.pub.resize((size_t)block_ * (size_t)curStride);
                std::copy_n(curOut, (size_t)block_ * (size_t)curStride, r.pub.begin());
                r.pubCh = curStride;
            }
            if (mapOn) {   // the routing text: one meter per OUTPUT line it uses
                outCh = 0;
                for (int o = 0; o < 8; ++o) if (r.job.outMap[(size_t)o] >= 0) outCh = o + 1;
            }
            // The track METER always shows the plugin's own channels (a 4-out plugin = 4 meters), in both
            // output modes: "Out stereo" only decides how the channels fold into the master (below), so
            // the meter no longer hides channels 3+. The routing text still shows its output lines.
            const int meterCh = outCh;   // one meter per track channel
            if (capture || soloMode_) {
                for (int i = 0; i < block_; ++i) {
                    if (!mapOn) {
                        for (int c = 0; c < meterCh; ++c) {
                            const float v = curOut[(size_t)i * curStride + c];
                            if (std::isfinite(v)) chPeak[(size_t)c] = std::max(chPeak[(size_t)c], std::fabs(v));
                        }
                        continue;
                    }
                    if (mapOn) {
                        for (int o = 0; o < outCh; ++o) {
                            const int srcCh = r.job.outMap[(size_t)o];
                            if (srcCh < 0 || srcCh >= curStride) continue;
                            const float v = curOut[(size_t)i * curStride + (size_t)srcCh];
                            if (std::isfinite(v)) chPeak[(size_t)o] = std::max(chPeak[(size_t)o], std::fabs(v));
                        }
                        continue;
                    }
                    float fold[8]{};   // per line: the SUM of the chain-end's channels that fold onto it (master-style 1+3 -> L, 2+4 -> R)
                    for (int c = 0; c < curStride && c < 8; ++c) {
                        const float v = curOut[(size_t)i * curStride + c];
                        if (std::isfinite(v)) fold[(size_t)(c % outCh)] += v;
                    }
                    for (int c = 0; c < outCh; ++c) chPeak[(size_t)c] = std::max(chPeak[(size_t)c], std::fabs(fold[(size_t)c]));
                }
            }
            if (capture && r.job.meterFirst >= 0)   // one meter per line the mode keeps: as source = the plugin's own channels, stereo = L | R, routing = the outs the text uses
                for (int c = 0; c < std::min(meterCh, r.job.meterCount > 0 ? r.job.meterCount : meterCh); ++c) capture->addMeter(r.job.meterFirst + c, chPeak[(size_t)c]);   // the Run-time slots only (never the next track's); the track header reads the full width by track id below   // never into the next track's slots
            if (soloMode_ || capture) {   // the lane meters of the VST tracks read these (max-held until the UI takes them)
                std::lock_guard<std::mutex> peakLock(peakMutex_);
                auto& entry = soloPeaks_[r.job.id];
                for (int c = 0; c < 8; ++c) entry[(size_t)c] = std::max(entry[(size_t)c], chPeak[(size_t)c]);
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
                    outPk = std::max(outPk, std::fabs(curOut[(std::size_t)n * curStride]));
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
                    for (int c = 0; c < curStride; ++c) instPk = std::max(instPk, std::fabs(curOut[(std::size_t)n * curStride + c]));
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
            // the track fader lives where the track's output joins the master (live map first, then the graph's)
            float trackGain = 1.f;
            {
                const auto lv = volumesLocal.find(r.job.id);
                if (lv != volumesLocal.end()) trackGain = std::max(0.f, std::min(kMaxVolumeGain, lv->second));
                else {
                    const auto gv = graph->volumes.find(r.job.id);
                    if (gv != graph->volumes.end()) trackGain = std::max(0.f, std::min(kMaxVolumeGain, gv->second));
                }
            }
            const auto dd = dryDelayOf.find(r.job.id);
            if (dd != dryDelayOf.end() && dd->second > 0) {
                // Delay the source's own master output by the consuming effect's reported latency.
                constexpr long long kDryCap = 60000;   // frames the ring holds (max compensable latency ~1.25 s @ 48 kHz)
                const size_t need = (size_t)kDryCap * (size_t)curStride;
                if ((int)r.dryDelayBuf.size() < (int)need) { r.dryDelayBuf.assign(need, 0.f); r.dryW = 0; }
                for (int i = 0; i < block_; ++i) {
                    const size_t pos = (size_t)(r.dryW % kDryCap);
                    for (int c = 0; c < curStride; ++c) r.dryDelayBuf[pos * (size_t)curStride + (size_t)c] = curOut[(size_t)i * curStride + c];
                    ++r.dryW;
                }
                for (int i = 0; i < block_; ++i) {
                    const long long idx = r.dryW - (long long)block_ - (long long)dd->second + (long long)i;
                    if (mapOn) {   // the routing text: output n plays the input it names (master index (n-1) mod N)
                        for (int o = 0; o < 8; ++o) {
                            const int srcCh = r.job.outMap[(size_t)o];
                            if (srcCh < 0 || srcCh >= curStride) continue;
                            float v = 0.f;
                            if (idx >= 0 && idx < r.dryW - kDryCap) { /* slot overwritten by an over-long delay: silence */ }
                            else if (idx >= 0) v = r.dryDelayBuf[(size_t)(idx % kDryCap) * (size_t)curStride + (size_t)srcCh];
                            add(spout, (size_t)i * nchan_ + (size_t)(o % nchan_), v * fullScale * trackGain);
                        }
                        continue;
                    }
                    float fold[8]{};   // stereo mode first mixes the chain's channels as stereo (1+3 -> L, 2+4 -> R), like the meters
                    for (int c = 0; c < curStride; ++c) {
                        float v = 0.f;
                        if (idx >= 0 && idx < r.dryW - kDryCap) { /* slot overwritten by an over-long delay: silence */ }
                        else if (idx >= 0) v = r.dryDelayBuf[(size_t)(idx % kDryCap) * (size_t)curStride + (size_t)c];
                        fold[(size_t)(c % outCh)] += v;
                    }
                    for (int c = 0; c < outCh; ++c) add(spout, (size_t)i * nchan_ + (size_t)(c % nchan_), fold[(size_t)c] * fullScale * trackGain);
                }
            } else {
                // The virtual soundcard fold: channel k of the plugin plays on master channel k mod N.
                // Stereo mode mixes the plugin as stereo first (the same fold the meters show).
                if (mapOn) {
                    // The routing text: output n plays the chain-end channel it names; the master
                    // fold (channel n -> n mod N) applies after. Two outputs may share one input
                    // (they both add), an input may feed several outputs.
                    for (int i = 0; i < block_; ++i)
                        for (int o = 0; o < 8; ++o) {
                            const int srcCh = r.job.outMap[(size_t)o];
                            if (srcCh < 0 || srcCh >= curStride) continue;
                            add(spout, (size_t)i * nchan_ + (size_t)(o % nchan_), curOut[(size_t)i * curStride + (size_t)srcCh] * fullScale * trackGain);
                        }
                } else for (int i = 0; i < block_; ++i) {
                    float fold[8]{};
                    for (int c = 0; c < curStride; ++c) fold[(size_t)(c % outCh)] += curOut[(size_t)i * curStride + c];
                    for (int c = 0; c < outCh; ++c) add(spout, (size_t)i * nchan_ + (size_t)(c % nchan_), fold[(size_t)c] * fullScale * trackGain);
                }
            }
        }
        // The clip taps chnmixed the current pass into the cawtap_* channels while Csound ran
        // (perform happens before this call), and they were read in the loop above - so clear them
        // NOW, for every channel the rack resolved, and the next pass starts from zero again.
        for (auto& entry : chans_) clearBus(entry.second);
    }
#endif

public:
    // Resolve every Csound channel pointer BEFORE the perform loop starts. The offline rack used
    // to call csoundGetChannelPtr from INSIDE the perform callback (chanOf on a first-seen name,
    // resolveChans after a re-pick) - that call into Csound's channel table from the audio path
    // faulted inside csound7_64.dll during the offline rack bounce. After preResolve the audio
    // path only ever reads the cached pointers.
    void preResolve() {
        std::shared_ptr<Graph> g = currentGraph();
        if (!g || !ptr_) return;
        for (Rt& r : g->rts) {
            resolveChans(r);
            for (int s = 0; s < 8; ++s)
                for (const std::string& c : r.job.inputs[(size_t)s].csChans)
                    chanOf(c);
        }
    }
    void say(const std::string& s) const { if (log_) log_(s); }

private:
    // The manual override queue and the SC-mix scratch do not depend on the VST3 SDK, so they
    // exist in both builds (pushParamOverride is a public method of this class either way).
    std::mutex overrideMutex_; std::vector<VstParamOverride> pending_;
    std::vector<VstPickUpdate> pendingPicks_;          // live re-picks queued by the UI
    std::vector<VstParamValue> pendingValues_;         // live GUI edits for ANY parameter, queued by the UI
    std::vector<float> scScratch_;                     // one SC-mix pull per block, shared by all consumers (the fallback: no per-track rings)
    std::vector<float> scMixPending_;
    std::map<int, std::vector<float>> scPending_;      // partial device blocks waiting for a complete rack block
    std::map<int, std::vector<float>> scPop_;          // per-track-slot SC pulls (engine 2), keyed by the track slot
    std::vector<float> pdPending_;                     // partial dedicated-PD blocks waiting for a complete rack block
    std::vector<float> pdPop_;                         // the dedicated PD ring pull (engine 1), once per block
    std::map<int, std::vector<float>> samplePending_;  // partial sample blocks waiting for a complete rack block
    std::map<int, std::vector<float>> samplePop_;      // one pull per block per sample-track ring (engine 4 inputs)
    LogFn log_;                                        // kept from init so process() can log diagnostics
    std::vector<VstJob> jobs_;      // the loaded job set (the 4th engine reuses a matching init)
    bool soloMode_ = false;         // true when this rack is the app-side 4th engine (no Csound channels)
    bool shareUi_ = false;          // ONE INSTANCE PER PLUGIN: take the UI's loaded plugin instead of loading a copy
#ifdef CAW_VST3
    // One loaded plugin instance. Lives in the loader's registry and is shared (by reference count)
    // with every graph that uses it: a re-wire / a changed track never re-creates the others.
    struct Inst {
        // the window takes it back only when NO live Inst wraps it any more (a graph swap holds two for a moment:
        // the retired one used to switch the plugin back to window mode while the new graph played it - the
        // window then deactivated it and the stage went silent)
        ~Inst() { if (shared && plug && plug->engineOwnedCount.fetch_sub(1, std::memory_order_acq_rel) == 1) plug->engineOwned.store(false, std::memory_order_release); }
        std::string path, classId; bool instrument = false;
        std::shared_ptr<caw::IPlugin> plug;
        std::atomic<long> wr{0};   // the note-ring position the audio thread has played up to
        std::atomic<bool> fresh{true};   // never resolved yet: start at the ring head, do not re-fire old notes
        int preparedCh = 0;         // the channel layout this instance was prepared (asked) for
        bool shared = false;        // the UI's own plugin object (one instance): the window is attached to it
        std::string appliedState;   // SEPARATED ENGINES: last patch-state blob this instance has (written by buildGraph
                                    // before the instance renders, and by process() on the render thread - no other writer)
    };
    struct Rt {
        // A chain stage beyond the first effect: effects 2..8 run IN ORDER, stage k's input is
        // stage k-1's output folded to this stage's input width (the virtual-soundcard rule).
        struct Stage {
            std::shared_ptr<Inst> inst; std::shared_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
            std::vector<float> in, out;
            std::vector<double> last; bool wasActive = true;
        };
        Stage stage[7];
        VstJob job; std::shared_ptr<Inst> inst; std::shared_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
        void* p[8] = {}; void *wr = nullptr; const void* tab = nullptr; long lastW = 0; bool chansResolved = false;
        long midiCount = 0; bool midiSeen = false; bool bridgeWarned = false; int silentHints = 0;   // MIDI bridge diagnostics
        int tapsMissing = 0;   // engine-0 input lines whose tap channel could not be resolved this block
        void* inCh[2] = {}; bool inChDone = false;   // cawvstin_<id>_1/2: the input audio the clip code reads as `in`
        bool srcMissingWarned = false; bool scNoBusWarned = false;   // a VSTi/VSTe source (engine 3) named by an input was not in the rack
        bool engine3DiagLogged = false;   // one-shot: print the engine-3 source state the first time a VSTe runs (reset on rewire)
        bool inactiveWarned = false;   // one-shot: a VSTe skipped as not-active logs once why (so it is not silently dead)
        std::vector<double> last; std::vector<float> in, out, sc;   // sc: the sidechain block (interleaved stereo)
        std::vector<float> trackOut;   // the chain end folded to the track's channel count (when they differ)
        std::vector<float> pub; int pubCh = 2;   // what this track published THIS block for the effects that read it (track width)
        bool wasActive = true;   // a track coming back from mute force-syncs its plugin to the channels
        double manual[8] = {}; bool manualActive[8] = {}; long long manualUntil[8] = {};   // live manual overrides
        double prevLane[8] = {};   // the raw lane values of the previous block (detects automation movement)
        int dryDelay = 0;                  // the source's own master output is delayed by this (samples): dry/wet alignment with a latency-reporting consuming VSTe (mix mode)
        std::vector<float> dryDelayBuf;    // ring for that delay (cap frames * chPass), allocated on first use
        long long dryW = 0;                // absolute frames pushed into dryDelayBuf
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
        std::map<int, int> outWides;    // 0 = stereo fold at the meters / master, 1 = the plugin's own channels 1:1
        std::map<int, float> volumes;   // the track fader (0..2), applied where the output joins the master
    };
    double rd(const void* p) const { return sb_ == 8 ? *static_cast<const double*>(p) : (double)*static_cast<const float*>(p); }
    double rdAt(const void* p, int i) const { return sb_ == 8 ? static_cast<const double*>(p)[i] : (double)static_cast<const float*>(p)[i]; }
    void wrAt(void* p, int i, float v) const { if (sb_ == 8) static_cast<double*>(p)[i] = (double)v; else static_cast<float*>(p)[i] = v; }
    float rdSpout(const void* spout, int i, int c) const { return sb_ == 8 ? (float)static_cast<const double*>(spout)[(std::size_t)i * nchan_ + c] : static_cast<const float*>(spout)[(std::size_t)i * nchan_ + c]; }
    void clearBus(void* p) const { if (sb_ == 8) std::fill_n(static_cast<double*>(p), block_, 0.0); else std::fill_n(static_cast<float*>(p), block_, 0.f); }
    void add(void* b, size_t i, float v) const { if (sb_ == 8) static_cast<double*>(b)[i] += v; else static_cast<float*>(b)[i] += v; }
    size_t pullRing(CawScRing* ring, std::vector<float>& pending, std::vector<float>& out) {
        const size_t frames = (size_t)block_;
        const size_t queued = ring->queued();
        if (queued > frames * 4) ring->skip(queued - frames);
        const size_t take = std::min(frames - pending.size() / CawScRing::CH, ring->queued());
        const size_t old = pending.size();
        pending.resize(old + take * CawScRing::CH);
        ring->popSome(pending.data() + old, take);
        out.resize(frames * CawScRing::CH);
        if (pending.size() < frames * CawScRing::CH) {
            std::fill(out.begin(), out.end(), 0.f);
            return 0;
        }
        std::copy_n(pending.begin(), frames * CawScRing::CH, out.begin());
        pending.erase(pending.begin(), pending.begin() + (ptrdiff_t)(frames * CawScRing::CH));
        return frames;
    }
    std::vector<float> tapBuf_;    // scratch for a popped Csound tap ring (separated engines)
    std::vector<float> tapAligned_;   // all channel taps of one Csound source, popped together (block_ per channel)
    // A clip tap channel, resolved by name on this thread and cached. A late-created channel is
    // picked up on a later block; a new one is zeroed on first resolution (it may hold last run's
    // leftovers). Cleared after every block (see process) so each pass starts from zero.
    void* chanOf(const std::string& name) {
        const auto it = chans_.find(name);
        if (it != chans_.end()) return it->second;
        if (chansTried_.count(name)) return nullptr;   // tried once, not there: never call into Csound's channel API again from the audio path
        if (!ptr_) return nullptr;
        void* p = ptr_(name, 2 | 16 | 32);
        chansTried_.insert(name);
        if (!p) return nullptr;
        clearBus(p);
        chans_[name] = p;
        return p;
    }
    // Resolve EVERY Csound channel pointer BEFORE the perform loop starts. The offline rack used to
    // call csoundGetChannelPtr from INSIDE the perform callback (chanOf on a first-seen name,
    // resolveChans after a re-pick) - that call into Csound's channel table from the audio path is
    // what faulted inside csound7_64.dll during the offline rack bounce. preResolve (public,
    // above) walks every job's channels once so the audio path only reads cached pointers.
    // insts_ keys: the main plugin and the chain stages live in separate key ranges (they used to share
    // one, so track 9's plugin and track 1's stage 2 were the same entry - one replaced the other).
    static int mainKey(int trackId) { return trackId * 16; }
    static int stageKey(int trackId, int stage) { return trackId * 16 + stage; }
    static std::string loadSig(const std::vector<VstJob>& jobs) {
        std::string s;
        for (const VstJob& j : jobs) {
            s += std::to_string(j.id) + "|" + j.path + "|" + j.classId + "|" + (j.instrument ? "i" : "e") + "|ch" + std::to_string(j.channels);   // a channel change rebuilds: the plugin is asked for the new layout
            for (const auto& cs : j.chain) s += ">" + cs.path + "|" + cs.classId;   // a chain stage added / removed / changed reloads the graph
            s += ";";
        }
        return s;
    }
    static bool sameInput(const VstJob::Input& a, const VstJob::Input& b) {
        return a.on == b.on && a.mode == b.mode && a.name == b.name && a.lineStart == b.lineStart
            && a.lineCount == b.lineCount && a.engine == b.engine && a.srcId == b.srcId && a.csChans == b.csChans && a.sidechain == b.sidechain && a.receive == b.receive;
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
            const auto known = insts_.find(mainKey(j.id));
            if (known != insts_.end() && known->second->path == j.path && known->second->classId == j.classId && known->second->instrument == j.instrument)
                inst = known->second;
            // TRACK CHANNELS changed: the plugin must be asked for the new layout while it is NOT
            // processing - never re-prepare an instance the audio thread still runs. A private copy
            // is rebuilt fresh (below); a shared UI instance is taken again and re-prepared only
            // after the graph swap retired the old Inst (see prepareShared).
            bool widthChanged = false;
            if (inst && inst->preparedCh != j.channels) { widthChanged = inst->shared; inst.reset(); }
            if (inst && inst->shared && shareUi_ && vstSharedProvider()) {   // the UI loaded a NEW object for this slot (re-pick): follow it
                std::shared_ptr<caw::IPlugin> ui;
                try { ui = vstSharedProvider()(j.id * 8, j.path, j.classId); } catch (...) {}
                if (ui && ui != inst->plug) inst.reset();
            }
            // ONE INSTANCE: the plugin the UI already loaded. Not on a live width change of a shared
            // plugin - the audio thread may still be inside it; that track gets a private copy now
            // and goes back to the shared object at the next Run.
            if (!inst && !widthChanged && shareUi_ && vstSharedProvider()) {
                std::shared_ptr<caw::IPlugin> ui;
                try { ui = vstSharedProvider()(j.id * 8, j.path, j.classId); } catch (...) {}
                if (ui) {
                    auto made = std::make_shared<Inst>();
                    made->path = j.path; made->classId = j.classId; made->instrument = j.instrument;
                    made->plug = ui;
                    made->shared = true;
                    ui->engineOwnedCount.fetch_add(1, std::memory_order_acq_rel); ui->engineOwned.store(true, std::memory_order_release);
                    try { made->plug->setWantedChannels(std::max(1, std::min(8, j.channels))); made->plug->prepare(sr_, block_); made->preparedCh = j.channels; } catch (...) { made.reset(); }
                    if (made) { insts_[mainKey(j.id)] = made; inst = made; say("[caw] VST track \"" + j.name + "\": plays the plugin you see (one instance).\n"); }
                }
            }
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
                        if (!j.stateBlob.empty()) {   // SEPARATED ENGINES: the editor instance's loaded patch (Kontakt .nki etc.)
                            try { made->plug->setState(j.stateBlob); made->appliedState = j.stateBlob; }
                            catch (...) { say("[caw] VST track \"" + j.name + "\": the editor's patch state could not be applied at load - the plugin keeps its defaults.\n"); }
                        }
                        made->plug->setWantedChannels(std::max(1, std::min(8, j.channels)));
                        made->plug->prepare(sr_, block_);
                        made->preparedCh = j.channels;
                    } catch (...) {
                        say("[caw] VST track \"" + j.name + "\": the plugin threw while starting - that track stays silent.\n");
                        made.reset();
                    }
                }
                insts_.erase(mainKey(j.id));
                if (!made) { if (strict_) return {}; continue; }
                insts_[mainKey(j.id)] = made;
                say("[caw] VST track \"" + j.name + "\" loaded (" + (j.instrument ? "instrument" : "effect") + ").\n");
                inst = made;
            }
            keep.insert(mainKey(j.id));
            Rt r;
            r.job = j;
            r.inst = inst;
            r.plug = inst->plug;
            if (r.job.pick.size() > 8) r.job.pick.resize(8);
            r.ep = std::make_shared<caw::Endpoint>();
            r.sc.assign((size_t)block_ * 2, 0.f); r.in.assign((size_t)block_ * 8, 0.f); r.out.assign((size_t)block_ * 8, 0.f);   // 8 lines max; the pass stride decides how they are read
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
                    else if (src.engine == 4) sum += "\"" + src.name + "\" (sample track mix)";
                    else if (src.engine == 3) sum += "\"" + src.name + "\" (VST output)";
                    else sum += "\"" + src.name + "\" (Csound clip taps, " + std::to_string(src.csChans.size()) + " ch)";
                }
                say("[caw] VSTe \"" + j.name + "\" inputs: " + (sum.empty() ? "(none enabled)" : sum) + "\n");
            }
            if (!r.job.instrument) g->outModes[r.job.id] = r.job.outMode == 1 ? 1 : 0;
            g->outWides[r.job.id] = r.job.outWide == 1 ? 1 : 0;
            g->volumes[r.job.id] = std::max(0.f, std::min(kMaxVolumeGain, r.job.trackVolume));
            // The effect chain: load (or reuse) every stage's plugin. Stage instances live in
            // insts_ under id*8+stage so a reused instance keeps its state across graph rebuilds.
            for (int cst = 1; cst < 8 && cst <= (int)j.chain.size(); ++cst) {
                const VstJob::ChainStage& cs = j.chain[(size_t)cst - 1];
                if (cs.path.empty()) break;
                const int key = stageKey(j.id, cst);
                std::shared_ptr<Inst> sInst;
                const auto known = insts_.find(key);
                if (known != insts_.end() && known->second->path == cs.path && known->second->classId == cs.classId)
                    sInst = known->second;
                bool stageWidthChanged = false;
                if (sInst && sInst->preparedCh != j.channels) { stageWidthChanged = sInst->shared; sInst.reset(); }
                if (sInst && sInst->shared && shareUi_ && vstSharedProvider()) {
                    std::shared_ptr<caw::IPlugin> ui;
                    try { ui = vstSharedProvider()(j.id * 8 + cst, cs.path, cs.classId); } catch (...) {}
                    if (ui && ui != sInst->plug) sInst.reset();
                }
                if (!sInst && !stageWidthChanged && shareUi_ && vstSharedProvider()) {   // ONE INSTANCE for chain stages too (never re-prepare one the audio thread still runs)
                    std::shared_ptr<caw::IPlugin> ui;
                    try { ui = vstSharedProvider()(j.id * 8 + cst, cs.path, cs.classId); } catch (...) {}
                    if (ui) {
                        auto made = std::make_shared<Inst>();
                        made->path = cs.path; made->classId = cs.classId; made->plug = ui; made->shared = true;
                        ui->engineOwnedCount.fetch_add(1, std::memory_order_acq_rel); ui->engineOwned.store(true, std::memory_order_release);
                        try { made->plug->setWantedChannels(std::max(1, std::min(8, j.channels))); made->plug->prepare(sr_, block_); made->preparedCh = j.channels; } catch (...) { made.reset(); }
                        if (made) { insts_[key] = made; sInst = made; }
                    }
                }
                if (!sInst) {
                    auto made = std::make_shared<Inst>();
                    made->path = cs.path;
                    made->classId = cs.classId;
                    try {
                        made->plug = caw::makeVst3Plugin();
                        std::string err;
                        if (!made->plug->load(cs.path, cs.classId, err)) { say("[caw] VSTe chain " + std::to_string(cst + 1) + " of \"" + j.name + "\": " + err + "\n"); made.reset(); }
                    } catch (const std::exception& e) {
                        say("[caw] VSTe chain " + std::to_string(cst + 1) + " of \"" + j.name + "\" threw while loading (" + std::string(e.what()) + ") - the chain stops after stage " + std::to_string(cst) + ".\n");
                        made.reset();
                    } catch (...) {
                        say("[caw] VSTe chain " + std::to_string(cst + 1) + " of \"" + j.name + "\" threw while loading - the chain stops after stage " + std::to_string(cst) + ".\n");
                        made.reset();
                    }
                    if (made) {
                        try {
                            for (const auto& parameter : made->plug->params()) {
                                const auto saved = cs.parameterState.find(parameter.id);
                                if (parameter.readOnly || saved == cs.parameterState.end()) continue;
                                if (std::fabs(saved->second - parameter.value) <= 1e-6) continue;
                                made->plug->setParam(parameter.index, saved->second);
                            }
                            if (!cs.stateBlob.empty()) {
                                try { made->plug->setState(cs.stateBlob); made->appliedState = cs.stateBlob; }
                                catch (...) { say("[caw] VSTe chain " + std::to_string(cst + 1) + " of \"" + j.name + "\": the editor's patch state could not be applied at load - the plugin keeps its defaults.\n"); }
                            }
                            made->plug->setWantedChannels(std::max(1, std::min(8, j.channels)));
                        made->plug->prepare(sr_, block_);
                        made->preparedCh = j.channels;
                        } catch (...) {
                            say("[caw] VSTe chain " + std::to_string(cst + 1) + " of \"" + j.name + "\" threw while starting - the chain stops after stage " + std::to_string(cst) + ".\n");
                            made.reset();
                        }
                    }
                    insts_.erase(key);
                    if (!made) { if (strict_) return {}; break; }
                    insts_[key] = made;
                    say("[caw] VSTe \"" + j.name + "\" chain stage " + std::to_string(cst + 1) + " loaded (effect).\n");
                    sInst = made;
                }
                keep.insert(key);
                Rt::Stage st;
                st.inst = sInst;
                st.plug = sInst->plug;
                st.ep = std::make_shared<caw::Endpoint>();
                st.in.assign((size_t)block_ * 8, 0.f);
                st.out.assign((size_t)block_ * 8, 0.f);
                st.last.assign(8, -1.0);
                r.stage[cst - 1] = std::move(st);
            }
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
                    // The OUTPUT ROUTING text (out<n> = in<m>) applies live to every VST track - a VSTi
                    // and a VSTe alike: the next block reads the new map. It needs no reload.
                    if (r.job.outMapOn != j.outMapOn || r.job.outMap != j.outMap) {
                        r.job.outMapOn = j.outMapOn;
                        r.job.outMap = j.outMap;
                        changed = true;
                    }
                    if (r.job.instrument) {   // a VSTi re-wires no inputs - only its channel count, routing and fader follow live
                        if (r.job.channels != j.channels) { r.job.channels = j.channels; changed = true; }
                        volumes_[j.id] = std::max(0.f, std::min(kMaxVolumeGain, j.trackVolume));
                        break;
                    }
                    bool chainDiff = r.job.chain.size() != j.chain.size()
                        || r.job.slotStage != j.slotStage;   // the routing text is NOT a chain change (applied above)
                    for (size_t ci = 0; ci < r.job.chain.size() && !chainDiff; ++ci)
                        chainDiff = r.job.chain[ci].path != j.chain[ci].path || r.job.chain[ci].classId != j.chain[ci].classId;
                    if (chainDiff) break;   // a chain change needs loaded plugins: the loader graph rebuild handles it (loadSig changed)
                    bool diff = r.job.routedIn != j.routedIn || (r.job.outMode == 1) != (j.outMode == 1)
                             || (r.job.outWide == 1) != (j.outWide == 1) || r.job.channels != j.channels || r.job.bypass != j.bypass;
                    for (int s = 0; s < 8 && !diff; ++s) diff = !sameInput(r.job.inputs[s], j.inputs[s]);
                    if (!diff) { volumes_[j.id] = std::max(0.f, std::min(kMaxVolumeGain, j.trackVolume)); break; }
                    for (int s = 0; s < 8; ++s) r.job.inputs[s] = j.inputs[s];
                    r.engine3DiagLogged = false;   // a re-wire: re-report the VSTe state on the next block
                    r.inactiveWarned = false;
                    r.job.routedIn = j.routedIn;
                    r.job.outMode = j.outMode;
                    outModes_[j.id] = j.outMode == 1 ? 1 : 0;
                    r.job.outWide = j.outWide;
                    r.job.channels = j.channels;   // TRACK CHANNELS: the next block processes at the new width
                    r.job.bypass = j.bypass;       // effect on/off: live, no reload
                    outWides_[j.id] = j.outWide == 1 ? 1 : 0;
                    r.job.outMapOn = j.outMapOn;   // the routing text: live too (a text edit re-wires without a Run)
                    r.job.outMap = j.outMap;
                    volumes_[j.id] = std::max(0.f, std::min(kMaxVolumeGain, j.trackVolume));
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
    std::set<std::string> chansTried_;     // names that resolved to null: chanOf never retries them mid-perform
    const VstEngineAudio* ea_ = nullptr;
#endif
    std::mutex peakMutex_;                                        // VST track meter peaks, rack thread -> UI
    std::map<int, std::array<float, 8>> soloPeaks_;
    std::mutex maskMutex_;                                        // live VSTe input on/off (UI) -> process (4th engine)
    std::map<int, unsigned> masks_;
    std::mutex outModeMutex_;                                     // live AUDIO OUTPUT of a VSTe (UI) -> process
    std::map<int, int> outModes_;
    std::map<int, int> outWides_;                  // live output width (0 = stereo, 1 = as source) per track
    mutable std::mutex volumeMutex_;                              // live track fader (UI) -> process
    std::map<int, float> volumes_;
    std::mutex wiringMutex_;                                      // live effect re-wire (UI) -> process
    std::vector<VstJob> wiring_; bool wiringValid_ = false;
};
