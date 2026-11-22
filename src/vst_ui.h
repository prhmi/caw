#pragma once
// The window of a VSTi / VSTe track (the track header has a Show button that opens it again):
//   choose the plugin by name (search) -> "Show plugin window" (its real interface) -> fill P1 .. P8: Learn (turn a knob in the plugin) or tick in the list.
// The clips of the track then get one automation lane per picked parameter and code (CAW code, see vst_lang.h) that drives them.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <regex>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <set>
#include <set>
#include <thread>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "imgui.h"
#include "arrow_widgets.h"
#include "model.h"
#ifdef CAW_VST3
#include "plugin_host.h"
#include "vst_lang.h"
#ifdef _WIN32
#include <objbase.h>
#include <ole2.h>
#endif
#endif

namespace vstui {
struct Slot {
    bool open = false, autoTried = false;
    bool systemScaling = true, loadedSystemScaling = true;   // systemScaling unused: the setting is global now (g_editorSystemScaling)
    int learn = -1;                       // slot (0..7) that waits for a knob turn in the plugin window, -1 = none
    std::string err, pluginFilter, paramFilter, loadedPath, loadedClassId;
    std::string note;                     // one-line result of the last load (e.g. which picks moved to the new plugin)
#ifdef CAW_VST3
    std::shared_ptr<caw::IPlugin> plugin;   // shared: the editor housekeeping worker's in-flight task keeps an instance alive while the slot reloads
    // Live parameter sync: plugin parameter index -> the value both the editor GUI and the engine
    // agreed on last frame. A knob moved away from the agreed value is pushed to the engine;
    // an engine value (automation) away from it is mirrored into the editor GUI.
    std::map<int, double> sync;
    std::set<int> syncSeen;
    std::map<size_t, int> slotParam;   // slot k -> the parameter index it held last frame (a re-pick reseeds the baseline)
    // ALL parameter values of the editor instance as of the last sweep, for plugins whose GUI does
    // not call back through performEdit (kilohearts): a change here is a knob the engine has not
    // seen yet. Cheap getters, ~25 ms cadence - never getState().
    std::vector<double> paramSnapshot;
    std::chrono::steady_clock::time_point paramPollT{};
    std::set<int> hiddenInputs;        // input slots the user collapsed in the AUDIO INPUTS list (UI only)
    std::string pushedState;           // SEPARATED ENGINES: last full plugin state captured from this editor instance
    std::string restoredState;         // SEPARATED ENGINES: the saved patch state already queued to this editor instance
                                       // (Kontakt's loaded .nki - binary state, not a parameter) and pushed to the engine
#endif
};
inline std::map<int, Slot>& slots() { static std::map<int, Slot> s; return s; }
inline void openWindow(int trackId) { slots()[trackId].open = true; }

// Called only once the VST audio engine has a valid format. The visible/editor instance is
// deliberately not run through process(); this only gives its component the same negotiated
// sample rate and block size as the live engine, which some complex editors require for UI state.
inline void prepareEditorPlugin(int trackId, double sampleRate, int blockSize) {
#ifdef CAW_VST3
    auto it = slots().find(trackId);
    if (it == slots().end() || !it->second.plugin) return;
    try { it->second.plugin->prepareEditor(sampleRate, blockSize); }
    catch (...) { /* An editor setup failure must not take down the UI or audio engine. */ }
#else
    (void)trackId; (void)sampleRate; (void)blockSize;
#endif
}
inline void prepareAllEditorPlugins(Project& project, double sampleRate, int blockSize) {
#ifdef CAW_VST3
    for (const Track& track : project.tracks)
        if (track.kind == 3 || track.kind == 4) prepareEditorPlugin(track.id, sampleRate, blockSize);
#else
    (void)project; (void)sampleRate; (void)blockSize;
#endif
}

// SEPARATED ENGINES: the editor instance's idle tick. Kontakt (and other sampler-style plugins)
// only finish their loading screen ("LOADING ... please check your audio connection") and move
// their meters when their component is processed. This runs their EDITOR instance a small block
// at ~12 Hz on the UI thread; the engine instance on the audio path is a different object and is
// untouched. With the engine running, the block carries the ENGINE instance's real input audio
// and dispatched MIDI (editorFeedSource), so an EQ's analyzer draws the actual signal and a
// synth's meters follow the notes that sound - the plugin window stops looking dead.
#ifdef CAW_VST3
inline std::function<std::shared_ptr<caw::IPlugin>(int)> editorFeedSource;   // app wires it: the ENGINE instance of a track
inline std::function<void(int, const std::string&, const std::string&, const std::string&)> pushVstStateLive;   // (trackId, blob, originPath, originClassId) the editor's loaded patch reaches the engine (wired by app.cpp)
// The signal-display option (Settings > General): send the engine's audio + notes into the
// plugin's own window (EQ analyzers, scopes, synth meters, Kontakt keys). Off = the editor
// instances are still ticked silently (loading screens stay alive) but receive no signal.
inline bool g_vstGuiSignal = true;
// General for ALL VST editors (Settings > Editor): let Windows scale the plugin editor
// (bitmap scaling). Off if editors draw cropped / blurry or clicks land on the wrong knob.
inline bool g_editorSystemScaling = true;

inline std::string niceName(const std::string& path, const std::string& classId = std::string());   // defined below

// Applies the editor scaling to every loaded plugin (only the window is recreated, the patch
// and the sound keep running).
inline void applyEditorSystemScaling(Project& proj, bool enabled) {
#ifdef CAW_VST3
    for (const Track& track : proj.tracks) {
        if (track.kind != 3 && track.kind != 4) continue;
        auto it = slots().find(track.id);
        if (it == slots().end() || !it->second.plugin) continue;
        Slot& s = it->second;
        if (s.loadedSystemScaling == enabled) continue;
        s.loadedSystemScaling = enabled;
        try {
            s.plugin->setEditorSystemScaling(enabled);
            if (s.plugin->editorVisible()) {
                s.plugin->closeEditor();
                std::string err;
                if (!s.plugin->showEditor(niceName(track.vstPath, track.vstClassId) + " (" + track.name + ")", err)) s.err = err;
            }
        } catch (...) { /* a scaling change must not take down the UI */ }
    }
#else
    (void)proj; (void)enabled;
#endif
}

// The EDITOR REALTIME TICK (one worker for every plugin window). A real DAW runs one plugin
// instance and its GUI is fed while the audio runs. CAW runs a separate EDITOR instance, so this
// worker replays the ENGINE instance's own audio (lock-free ring, Vst3Plugin::takeEditorFeed) at
// ~60 Hz: EQ analyzers, scopes and meters move at realtime speed, with no timer jumps.
//   - ProcessIdle must never run on the UI thread: Kontakt-class plugins serialize component
//     work with locks that also involve their own GUI thread (which IS the host's UI thread).
//   - The full plugin state is captured ONLY after the user changed something in the window
//     (the plugin marks itself dirty on every knob / preset message - see
//     IPlugin::stateDirty). No timer polls getState(): a kHs-class editor stalls while its state
//     is serialized, and a blob with volatile fields would be re-applied forever.
struct EditorTask {
    std::shared_ptr<caw::IPlugin> plugin;
    std::string originPath, originClassId;   // which plugin instance a state capture came from: a blob must never reach a different plugin (kHs bundles reject a foreign node type with an error dialog)
    std::string setState;              // restore: apply a saved patch state (never on the UI / audio thread)
    uint64_t restoreGen = 0;           // a published restore is applied once, even if the UI thread stalls
    bool signal = true;                // g_vstGuiSignal when published: feed the engine's audio / notes into the window
    bool visible = true;               // hidden after an edit: only the state capture runs (no feed, no idle DSP)
};
inline std::map<int, EditorTask> g_editorTaskMap;
inline std::mutex g_editorTaskMutex;
inline std::condition_variable g_editorTaskCv;
inline std::atomic<bool> g_editorWorkerOn{false};
inline std::thread g_editorWorkerThread;
inline bool g_editorWorkerStarted = false;
inline std::atomic<bool> g_editorFeedLogged{false};   // one-shot log: the first feed that reaches an editor instance
inline std::atomic<uint64_t> g_restoreGen{0};
inline std::function<bool()> editorPlaybackBusy;      // true while any engine renders sound (diagnostics / hooks)
inline std::function<bool()> editorStateCaptureBlocked;   // true while sound is AUDIBLE: the capture then runs under the render-pause guard below
inline std::function<bool()> engineRestartBusy;           // the rack is creating / freeing plugin instances: no editor work this round (a framework must not see an instance freed under it)
inline std::function<bool(const std::function<void()>&)> editorStateCaptureGuard;   // runs fn while the engine's rendering is paused (the engine instance is idle, the audio plays from the FIFO): the plugin->getState capture cannot contend with the engine instance, so the patch still reaches the engine while the transport plays
inline std::atomic<int> g_stateApplyTrackId{-1};      // the engine's state worker is applying a patch to THIS track's engine instance right now: the editor housekeeping pauses that plugin (kHs-class frameworks deadlock when two of their instances are touched at once)
inline std::mutex g_capturedStateMutex;
struct CapturedState { int trackId = 0; std::string blob, originPath, originClassId; };
inline std::deque<CapturedState> g_capturedStates;   // worker -> UI: captured blobs (Slot.pushedState, collectVstJobs embedding)

inline void startEditorWorker() {
    if (g_editorWorkerStarted) return;
    g_editorWorkerStarted = true;
    g_editorWorkerOn = true;
    g_editorWorkerThread = std::thread([] {
        std::map<int, uint64_t> appliedRestoreGen;   // worker thread only
        std::map<int, int> skipTicks;                // a heavy GUI processes less often; the ring keeps the recent audio
        // Last patch pushed per track. A plugin that re-arms stateDirty after every apply (some
        // editors bump volatile fields in getState) would otherwise push a "new" blob forever:
        // each engine apply resets the window's analyzer mid-view. Same blob = nothing to push.
        struct LastCapture { std::string path, classId, blob; };
        std::map<int, LastCapture> lastCapture;
        while (g_editorWorkerOn) {
            {
                std::unique_lock<std::mutex> lock(g_editorTaskMutex);
                if (g_editorTaskMap.empty())
                    g_editorTaskCv.wait(lock, [] { return !g_editorTaskMap.empty() || !g_editorWorkerOn.load(); });
                else
                    g_editorTaskCv.wait_for(lock, std::chrono::milliseconds(16), [] { return !g_editorWorkerOn.load(); });
            }
            if (!g_editorWorkerOn) break;
            std::map<int, EditorTask> tasks;
            { std::lock_guard<std::mutex> lock(g_editorTaskMutex); tasks = g_editorTaskMap; }
            for (auto& entry : tasks) {
                if (!g_editorWorkerOn) break;
                EditorTask& task = entry.second;
                if (!task.plugin) continue;
                if (g_stateApplyTrackId.load(std::memory_order_acquire) == entry.first) continue;   // a patch is being applied to this track's ENGINE instance: keep the editor instance completely idle (framework deadlock guard)
                if (engineRestartBusy && engineRestartBusy()) continue;   // instances are being created / freed: touch nothing this round
                // Restore a saved patch (project load) - once per published generation.
                if (!task.setState.empty() && task.restoreGen > appliedRestoreGen[entry.first]) {
                    appliedRestoreGen[entry.first] = task.restoreGen;
                    lastCapture.erase(entry.first);   // the restored patch is not a capture: a later edit pushes once
                    try { task.plugin->setState(task.setState); } catch (...) { /* a state restore failure must not kill the worker */ }
                }
                // Capture the full state only when the USER changed something and the mouse has
                // settled (a window being moved never captures: editors freeze mid-drag).
                if (task.plugin->stateDirty() && task.plugin->msSinceLastTouch() > 250.0 && !task.plugin->windowBusy()) {
                    auto capture = [&]() {
                        const std::string blob = task.plugin->getState();
                        if (!blob.empty()) {
                            LastCapture& last = lastCapture[entry.first];
                            if (last.path == task.originPath && last.classId == task.originClassId && last.blob == blob) return;
                            last.path = task.originPath; last.classId = task.originClassId; last.blob = blob;
                            if (pushVstStateLive) pushVstStateLive(entry.first, blob, task.originPath, task.originClassId);   // -> the engine's state worker (never the audio thread)
                            std::lock_guard<std::mutex> l(g_capturedStateMutex);
                            g_capturedStates.push_back({entry.first, blob, task.originPath, task.originClassId});
                        }
                    };
                    bool ran = true;
                    try {
                        const bool audible = editorStateCaptureBlocked && editorStateCaptureBlocked();
                        // Sound is audible: hold the engine quiet around the capture (the engine
                        // instance is idle then, the audio keeps playing from the FIFO) - the patch
                        // STILL reaches the engine instance while the transport plays. An apply in
                        // flight or an engine reload just skips this round.
                        if (audible && editorStateCaptureGuard) ran = editorStateCaptureGuard(capture);
                        else capture();   // nothing audible: a stall cannot glitch the audio
                    } catch (...) { /* a state capture failure must not kill the worker */ }
                    if (ran) task.plugin->markStateCaptured();
                }
                int& skip = skipTicks[entry.first];
                if (!task.visible) continue;   // window closed right after an edit: its capture above still runs
                if (skip > 0) { --skip; continue; }
                const auto tickStart = std::chrono::steady_clock::now();
                // Realtime feed: drain the engine's ring, one prepared block per processIdle, so
                // the editor instance hears exactly what the engine rendered since the last tick.
                bool fed = false;
                std::shared_ptr<caw::IPlugin> src;
                if (task.signal && g_vstGuiSignal && editorFeedSource) src = editorFeedSource(entry.first);
                if (src) {
                    caw::EditorFeed feed;
                    for (int k = 0; k < 8; ++k) {
                        try { if (!src->takeEditorFeed(feed)) break; } catch (...) { break; }
                        if (feed.frames <= 0 && feed.events.empty()) break;
                        try { task.plugin->processIdle(feed.frames > 0 ? feed.frames : 128, &feed); } catch (...) { /* the app must survive a misbehaving plugin */ }
                        fed = true;
                        if (feed.frames > 0 && !g_editorFeedLogged.load(std::memory_order_relaxed)) {
                            g_editorFeedLogged.store(true, std::memory_order_relaxed);
                            try { caw::vstLog("[caw] editor tick: engine audio reaches the plugin window (" + std::to_string(feed.frames) + " frames per block)"); } catch (...) {}
                        }
                    }
                }
                // No engine audio (stopped, or signal display off): a silent block keeps loading
                // screens finishing and meters decaying - the window never looks dead.
                if (!fed) {
                    try { task.plugin->processIdle(256, nullptr); } catch (...) { /* a misbehaving plugin must not kill the worker */ }
                }
                const double ms = (double)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - tickStart).count() / 1000.0;
                if (ms > 12.0) skip = (int)std::min(4.0, ms / 8.0);   // a heavy GUI (Kontakt-class) ticks less often: the audio core keeps its headroom
            }
        }
    });
}
inline void stopEditorWorker() {
    if (!g_editorWorkerStarted) return;
    g_editorWorkerOn = false;
    g_editorTaskCv.notify_all();
    if (g_editorWorkerThread.joinable()) g_editorWorkerThread.join();
    g_editorWorkerStarted = false;
}
// Net for an exit path that never reaches appRequestClose: join the worker before its
// globals are torn down. appRequestClose() already stops it on the normal path.
inline struct EditorWorkerGuard {
    ~EditorWorkerGuard() { stopEditorWorker(); }
} g_editorWorkerGuard;
// UI thread, once per frame: publish which plugin windows exist and are visible. The worker
// ticks them at 60 Hz on its own; here we only copy a shared_ptr per window and one-shot
// restore data. The map is replaced every frame, so a closed window stops being ticked.
inline void queueEditorTasks(Project& project) {
    static std::map<int, EditorTask> next;   // UI thread only
    next.clear();
    for (Track& t : project.tracks) {
        if (t.kind != 3 && t.kind != 4) continue;
        auto it = slots().find(t.id);
        if (it == slots().end() || !it->second.plugin) continue;
        Slot& s = it->second;
        const bool visible = s.plugin->editorVisible();
        if (!visible && !s.plugin->stateDirty()) continue;   // hidden window: nothing to tick (an un-captured edit still gets its capture)
        EditorTask task;
        task.plugin = s.plugin;
        task.visible = visible;
        task.originPath = s.loadedPath; task.originClassId = s.loadedClassId;   // the capture must carry its source plugin: a blob captured from the PREVIOUS plugin must never reach the new one
        task.signal = g_vstGuiSignal;
        // restore: a saved patch state (project v58) reaches the plugin here - one shot per state change
        if (!t.vstPatchState.empty() && t.vstPatchState != s.restoredState) {
            task.setState = t.vstPatchState;
            s.restoredState = t.vstPatchState;
            task.restoreGen = ++g_restoreGen;
        }
        next[t.id] = std::move(task);
    }
    { std::lock_guard<std::mutex> lock(g_editorTaskMutex); g_editorTaskMap.swap(next); }
    if (!g_editorTaskMap.empty()) { g_editorTaskCv.notify_one(); startEditorWorker(); }
}
#endif


// SEPARATED ENGINES: the editor instance's latest full plugin state (empty = nothing captured
// yet). collectVstJobs embeds it in the track's VstJob, so a fresh engine instance and an offline
// render start with the patch the user loaded in the editor (Kontakt's .nki etc.).
inline std::string editorStateBlob(int trackId) {
#ifdef CAW_VST3
    const auto it = slots().find(trackId);
    return it == slots().end() ? std::string() : it->second.pushedState;
#else
    (void)trackId; return {};
#endif
}

// Keep the Win32 message pump at interactive cadence while any native VST editor is open.
// Some plugin UIs (including animated parameter graphs) rely on timely paint/timer dispatch.
inline bool hasVisiblePluginEditor() {
#ifdef CAW_VST3
    for (const auto& entry : slots()) {
        const Slot& slot = entry.second;
        if (slot.plugin && slot.plugin->editorVisible()) return true;
    }
#endif
    return false;
}

// Hooks the app wires to the running Csound engine so the VSTE panel can edit live:
//   pushControlChannels: one-shot writes into named Csound control channels (the P1..P8 sliders
//                        write the engine channel cawvst_<id>_p<k> AND the clip lane channels, so
//                        the change is instant and survives whether or not a clip is playing).
//   pushVstParamOverride: the SAME value delivered straight to the rack - applied on the very
//                        block it arrives, no channel involved, nothing can delay or revert it.
//   pushVstActive:       the set of VST track ids that still sound (live M / S).
//   pushVstInputMasks:   bit i = input slot i of that VSTe feeds the plugin (live on/off).
//   takeVstEngineParams: the kP1..kP8 values the engine is actually using (for GUI reflection).
inline std::function<void(const std::vector<std::pair<std::string, double>>&)> pushControlChannels;
inline std::function<void(const std::set<int>&)> pushVstActive;
inline std::function<void(const std::map<int, unsigned>&)> pushVstInputMasks;
inline std::function<void(int, int, double)> pushVstParamOverride;
inline std::function<bool(int, double[8])> takeVstEngineParams;
inline std::function<void(int, const std::vector<int>&)> pushVstPickRefresh;   // P1..P8 re-picked while running
inline std::function<void(int, int, double)> pushVstParamLive;   // (trackId, parameter index, value) EVERY editor knob, live
inline std::function<void(int, int)> pushVstOutModeLive;         // (trackId, 0 = master replace, 1 = mix) AUDIO OUTPUT switched live
inline std::function<void()> pushVstWiring;                      // an input mode / name changed: re-resolve the routing in the running VST engine now
inline std::function<void(int)> notifyPluginLoaded;              // a .vst3 finished loading into the editor: the engine may apply it now

inline std::string lower(const std::string& s) { std::string o; for (char c : s) o += (char)std::tolower((unsigned char)c); return o; }

#ifdef CAW_VST3
// Discovered paths appear immediately; classification continues in the background.
struct Catalog { std::vector<caw::PluginInfo> list; std::mutex mutex; std::atomic<bool> done{false}, started{false}; };
// The detached scanner can outlive UI teardown, so its catalog has process lifetime.
inline Catalog& catalog() { static Catalog* c = new Catalog; return *c; }
inline std::vector<caw::PluginInfo> catalogSnapshot() {
    Catalog& c = catalog(); std::lock_guard<std::mutex> lock(c.mutex); return c.list;
}
inline void startScan(bool rescan = false) {
    Catalog& c = catalog();
    if (rescan) { if (!c.done) return; c.started = false; c.done = false; }
    if (c.started.exchange(true)) return;
    std::thread([&c]() {
#ifdef _WIN32
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        auto publish = [&c](const std::vector<caw::PluginInfo>& list) {
            std::lock_guard<std::mutex> lock(c.mutex); c.list = list;
        };
        const auto extra = caw::vst3ExtraDirs();
        try { publish(caw::scanVst3Info(extra, publish)); }
        catch (const std::exception& e) { caw::vstLog(std::string("scan error: ") + e.what()); }
        catch (...) { caw::vstLog("scan error: unknown exception"); }
#ifdef _WIN32
        if (SUCCEEDED(com)) CoUninitialize();
#endif
        c.done = true;
    }).detach();
}
inline std::string niceName(const std::string& path, const std::string& classId) {   // default declared above
    std::string first;   // no class chosen (old project): the first class the scanner found for the file
    for (const auto& i : catalogSnapshot()) {
        if (i.path != path) continue;
        if (!classId.empty()) { if (i.classId == classId) return i.name; }
        else if (first.empty()) first = i.name;
    }
    if (!first.empty()) return first;
    return std::filesystem::path(path).stem().string();
}

inline void captureTrackParameters(Track& t, Slot& s) {
    if (!s.plugin || s.loadedPath != t.vstPath || s.loadedClassId != t.vstClassId) return;
    std::map<uint32_t, double> snapshot;
    for (const caw::PluginParam& p : s.plugin->params()) {
        if (p.readOnly) continue;
        const double v = s.plugin->getParam(p.index);
        if (std::isfinite(v) && v >= 0. && v <= 1.) snapshot[p.id] = v;
    }
    // Do not erase a previously captured snapshot if a plugin reports no parameters.
    if (!snapshot.empty()) t.vstParameterState = std::move(snapshot);
}

// Capture the CURRENT editor values of every loaded plugin into the tracks. Run then bakes
// exactly what the GUI shows - all parameters, not only the picked ones.
inline void captureAllParameters(Project& project) {
#ifdef CAW_VST3
    for (Track& t : project.tracks) {
        if (t.kind != 3 && t.kind != 4) continue;
        auto it = slots().find(t.id);
        if (it != slots().end() && it->second.plugin) captureTrackParameters(t, it->second);
    }
#else
    (void)project;
#endif
}

inline bool loadPlugin(Slot& s, Track& t, const std::string& path, const std::string& classId, double sr) {
    s.err.clear(); s.note.clear();
    // A bundle (.vst3) can hold several plugin classes: the identity is path + class id, not the
    // file alone - otherwise picking a second plugin from the same bundle looks like "same file".
    const bool changed = path != t.vstPath || classId != t.vstClassId;
    // Pinning the class of an old project (the track has no class id yet) is not a plugin change.
    const bool newPlugin = path != t.vstPath || (!classId.empty() && !t.vstClassId.empty() && classId != t.vstClassId);
    if (!changed) captureTrackParameters(t, s);
    // NEVER pour the previous plugin's saved values into another plugin. Parameter ids are reused
    // across plugins of the same vendor (all Kilohearts plugins share one id space, Arturia's too),
    // so the new plugin would open with the old plugin's settings - wrong on screen, often silent.
    // The working 38_bridge_19 build did exactly this clear; it is why picking a plugin from
    // another company first "fixed" the next pick from the same company.
    if (newPlugin) {
        t.vstParameterState.clear();
        // The project-saved patch state belongs to the plugin that was on this track when it was
        // saved (kHs: "Expected 'filter' but got 'equalizer'"). A fresh pick must not inherit it:
        // collectVstJobs falls back to this blob for a new engine instance, and the editor restore
        // queues it to the plugin window. The project file still has it - reloading the project
        // restores the original plugin with its patch.
        t.vstPatchState.clear();
    }
#ifdef _WIN32
    static bool com = false; if (!com) { OleInitialize(nullptr); com = true; }   // many plugins expect COM on the thread that loads them
#endif
    // Another plugin takes over the SAME picks: the mapping follows the parameter NAMES, because
    // the indices of another plugin mean something else (they could even drive a mute / volume
    // knob). The editor instance that is on screen right now still knows what P1..P8 were.
    std::vector<std::string> oldTitles;
    if (changed && s.plugin && !t.vstPath.empty() && s.loadedPath == t.vstPath && s.loadedClassId == t.vstClassId) {
        const auto& old = s.plugin->params();
        oldTitles.reserve(t.vstPick.size());
        for (int idx : t.vstPick)
            oldTitles.push_back(idx >= 0 && (size_t)idx < old.size() ? old[(size_t)idx].title : std::string());
    }
    auto p = caw::makeVst3Plugin(); std::string err;
    p->setEditorSystemScaling(g_editorSystemScaling);
    // This copy only shows the plugin; the sound is made by a separate engine instance.
    bool ok = false;
    try { ok = p->load(path, classId, err); } catch (...) { err = "the plugin failed while loading"; }
    if (ok) {
        if (changed) {
            int moved = 0, lost = 0;
            const auto& fresh = p->params();
            for (size_t k = 0; k < t.vstPick.size() && k < oldTitles.size(); ++k) {
                const std::string& title = oldTitles[k];
                if (title.empty()) continue;               // nothing to match against: the raw index stays
                int found = -1;
                for (const caw::PluginParam& fp : fresh)
                    if (!fp.readOnly && fp.title == title) { found = fp.index; break; }
                if (found < 0) for (const caw::PluginParam& fp : fresh)
                    if (!fp.readOnly && lower(fp.title) == lower(title)) { found = fp.index; break; }
                if (found >= 0) { t.vstPick[k] = found; ++moved; }
                else { t.vstPick[k] = -1; ++lost; }        // not in this plugin: show the slot as empty instead of driving a random knob
            }
            if (!oldTitles.empty() && (moved || lost))
                s.note = "Mapping carried over: " + std::to_string(moved) + " of " + std::to_string(oldTitles.size()) +
                    " picks matched this plugin." + (lost ? " " + std::to_string(lost) + " were not found - pick those again." : "");
        }
        // The saved state is keyed by parameter ID: a reloaded plugin (same path / class) gets its
        // patch back. Only the IDs this plugin reports are applied - nothing is erased. Values that
        // are already what the plugin reports are left alone: plugins choke on thousands of edits.
        for (const caw::PluginParam& param : p->params()) {
            const auto saved = t.vstParameterState.find(param.id);
            if (!param.readOnly && saved != t.vstParameterState.end() && std::fabs(saved->second - param.value) > 1e-6)
                p->setParam(param.index, saved->second);
        }
        // An empty class id (an old project) keeps the class the plugin actually loaded, so the
        // track is pinned to that exact class from now on.
        t.vstClassId = p->classId().empty() ? classId : p->classId();
        s.plugin = std::move(p); t.vstPath = path; s.loadedPath = path; s.loadedClassId = t.vstClassId;
        s.loadedSystemScaling = g_editorSystemScaling; s.learn = -1;
        s.autoTried = true;   // loaded here: the panel must not load the same plugin a second time next frame
        // Fresh baselines: the values this plugin has right now are the agreed ones. Without this
        // the poll below reads the new defaults as "the user moved the knob", overwrites vstInit
        // and pushes them to the engine - which is how a mapping got reset by picking a plugin.
        s.sync.clear(); s.syncSeen.clear(); s.slotParam.clear();
        s.paramSnapshot.clear(); s.paramPollT = {};   // a new plugin seeds its own values: never push the previous plugin's
        s.pushedState.clear();   // SEPARATED ENGINES: a new plugin must not receive the previous one's patch state
        s.restoredState.clear();   // the saved patch state (project v58) is queued to the plugin by the editor tick
        // Every knob turned in this plugin's own window reaches the running engine at once
        // (all parameters, not just the 8 picked slots). trackId by value: no dangling lane refs.
        const int liveTrackId = t.id;
        s.plugin->setLiveEditHook([liveTrackId](int index, double value) {
            if (pushVstParamLive) pushVstParamLive(liveTrackId, index, value);
        });
        if (pushVstPickRefresh) pushVstPickRefresh(t.id, t.vstPick);   // a running engine follows the (re-mapped) picks at once
        if (notifyPluginLoaded) notifyPluginLoaded(t.id);              // the engine applies it now, no 0.5 s wait
    }
    else { s.err = err.empty() ? "could not load that plugin" : err; }
    return ok;
}
// the name of the automation lane of a picked parameter: letters, digits and _ only, never twice on one track
inline std::string laneName(const Track& t, const std::string& title, size_t slot) {
    std::string n;
    for (char c : title) n += (std::isalnum((unsigned char)c) ? c : '_');
    while (!n.empty() && n.front() == '_') n.erase(n.begin());
    while (!n.empty() && n.back() == '_') n.pop_back();
    if (n.empty()) n = "P" + std::to_string(slot + 1);
    std::string out = n; int k = 2;
    auto taken = [&](const std::string& x) { for (size_t i = 0; i < t.vstPickName.size(); ++i) if (i != slot && t.vstPickName[i] == x) return true; return false; };
    while (taken(out)) out = n + std::to_string(k++);
    return out;
}
inline void assign(Track& t, caw::IPlugin& plugin, size_t slot, const caw::PluginParam& p) {
    while (t.vstPickName.size() < t.vstPick.size()) t.vstPickName.push_back("P" + std::to_string(t.vstPickName.size() + 1));
    while (t.vstInit.size() < t.vstPick.size()) t.vstInit.push_back(0.5);
    const double init = plugin.getParam(p.index);
    if (slot >= t.vstPick.size())   // a slot left free by a plugin change (-1) or by Clear is reused before a new one is added
        for (size_t k = 0; k < t.vstPick.size(); ++k) if (t.vstPick[k] < 0) { slot = k; break; }
    if (slot >= t.vstPick.size()) { slot = t.vstPick.size(); t.vstPick.push_back(p.index); t.vstInit.push_back(init); t.vstPickName.push_back(""); }
    else { t.vstPick[slot] = p.index; t.vstInit[slot] = init; }
    t.vstPickName[slot] = laneName(t, p.title, slot);
    CawParam cp{t.vstPickName[slot], 0.0, 1.0, 0.001, init};
    t.automationRanges[cp.chn] = cp;                     // the lane starts at the plugin's current value
    if (pushVstPickRefresh) pushVstPickRefresh(t.id, t.vstPick);   // the running engine maps P1..P8 to the new picks at once
}
inline void clearSlot(Track& t, size_t k) {
    if (k < t.vstPick.size()) t.vstPick.erase(t.vstPick.begin() + (long)k);
    if (k < t.vstInit.size()) t.vstInit.erase(t.vstInit.begin() + (long)k);
    if (k < t.vstPickName.size()) t.vstPickName.erase(t.vstPickName.begin() + (long)k);
    if (pushVstPickRefresh) pushVstPickRefresh(t.id, t.vstPick);
}

// Push a manual parameter change to the running engine, both paths at once so it is heard
// immediately in every situation:
//   1. the engine channel cawvst_<id>_p<k> that the rack reads directly. The write lands after
//      Csound's block and before the rack reads it, so the plugin changes in the SAME block -
//      and it sticks even when no clip of the track is playing (nothing overwrites the channel).
//   2. every clip's scoped lane channel (<clipName>_<laneName>). A running clip's cawget picks
//      the value up on the next block and keeps forwarding it, so the change survives while the
//      clip code runs. The write sticks until an automation point moves the lane again.
inline void pushParamLive(Track& t, size_t slot, double value) {
#ifdef CAW_VST3
    if (slot >= 8) return;
    // 1. the direct override: the rack applies it on the very block it arrives - nothing can
    //    delay or revert it (no clip running, a running clip, automation, anything).
    if (pushVstParamOverride) pushVstParamOverride(t.id, (int)slot, value);
    // 2. the channel writes: the engine channel the rack reads AND every clip's scoped lane, so
    //    the value sticks once the override window ends and the clip code keeps forwarding it.
    if (!pushControlChannels) return;
    std::vector<std::pair<std::string, double>> writes;
    writes.push_back({"cawvst_" + std::to_string(t.id) + "_p" + std::to_string(slot + 1), value});
    if (slot < t.vstPickName.size() && !t.vstPickName[slot].empty())
        for (const Clip& c : t.clips) writes.push_back({c.name + "_" + t.vstPickName[slot], value});
    pushControlChannels(writes);
#else
    (void)t; (void)slot; (void)value;
#endif
}

// Push the current on/off state of a VSTe's 8 input slots to the engine (live input checkboxes).
inline void pushInputMaskLive(Track& t) {
    if (!pushVstInputMasks) return;
    unsigned mask = 0;
    for (int s = 0; s < 8; ++s) if (t.vstInputs[s].on) mask |= (1u << s);
    std::map<int, unsigned> masks; masks[t.id] = mask;
    pushVstInputMasks(masks);
}

// Live two-way parameter sync, run once per frame (even when no VST panel is focused):
//   - a knob turned in the PLUGIN's own window (or the P1..P8 slider) is detected and pushed to
//     the running engine at once, like an effect VST in a host;
//   - engine-side changes (automation on a lane, another sender of the same channel) are mirrored
//     back into the plugin GUI so the knobs follow what is audible.
// Every parameter reaches the engine: the 8 picks through the lane/P-slider path above, all the
// others through the value sweep below (plugins like kilohearts' do not report GUI edits).
inline void syncLiveParameters(Project& project) {
#ifdef CAW_VST3
    // The editor instances are ticked and their state captured on the HOUSEKEEPING WORKER (see
    // queueEditorTasks) - a plugin whose getState()/processIdle() blocks for seconds must never
    // run on the UI thread: opening Kontakt's window while the engine ran froze the whole app.
    queueEditorTasks(project);
    {   // the worker's captured blobs -> Slot.pushedState (collectVstJobs embeds it at engine start / reload / export)
        // A capture that was in flight while the user switched this track's plugin is STALE: only
        // accept a blob whose origin (path + class) still matches the slot's loaded plugin -
        // applying a Slice EQ blob to kHs Filter is exactly the "Expected 'filter' but got
        // 'equalizer'" error dialog.
        std::deque<CapturedState> got;
        { std::lock_guard<std::mutex> l(g_capturedStateMutex); got.swap(g_capturedStates); }
        for (const auto& e : got) {
            auto cit = slots().find(e.trackId);
            const bool samePlugin = cit != slots().end() && cit->second.plugin &&
                cit->second.loadedPath == e.originPath &&
                (cit->second.loadedClassId.empty() || e.originClassId.empty() || cit->second.loadedClassId == e.originClassId);
            if (!samePlugin) continue;
            cit->second.pushedState = e.blob;
            cit->second.restoredState = e.blob;   // the editor produced it: never echo it back as a restore
            // keep the loaded patch on the TRACK: the project save writes it (Kontakt's .nki survives save/load)
            for (Track& t : project.tracks)
                if (t.id == e.trackId && (t.kind == 3 || t.kind == 4)) t.vstPatchState = e.blob;
        }
    }
    for (Track& t : project.tracks) {
        if (t.kind != 3 && t.kind != 4) continue;
        auto it = slots().find(t.id);
        if (it == slots().end() || !it->second.plugin) continue;
        Slot& s = it->second;
        if (!s.open) continue;   // the knob sync below mirrors CAW's own panel: only while it is open
        const auto& params = s.plugin->params();
        double eng[8] = {};   // the engine mirror is absent when the engine is off: then only GUI -> engine works
        const bool haveEngine = takeVstEngineParams && takeVstEngineParams(t.id, eng);
        const size_t n = std::min<size_t>(8, t.vstPick.size());
        for (size_t k = 0; k < n; ++k) {
            const int idx = t.vstPick[k];
            if (idx < 0 || (size_t)idx >= params.size() || params[(size_t)idx].readOnly) continue;
            const double editor = s.plugin->getParam(idx);
            if (!std::isfinite(editor) || editor < 0.0 || editor > 1.0) continue;
            // first sight of this parameter (picked, re-picked, or the slot's content changed):
            // seed the baseline from the engine / editor, never push
            const auto held = s.slotParam.find(k);
            const bool slotChanged = held == s.slotParam.end() || held->second != idx;
            if (slotChanged) s.slotParam[k] = idx;
            const bool first = slotChanged || s.syncSeen.insert(idx).second;
            double& agree = s.sync[idx];
            if (first) { agree = haveEngine ? std::min(1.0, std::max(0.0, eng[k])) : editor; continue; }
            if (std::fabs(editor - agree) > 1e-6) {                 // the user moved the editor knob / P slider
                agree = editor;
                if (k < t.vstInit.size()) t.vstInit[k] = editor;
                for (const caw::PluginParam& p : params)
                    if (p.index == idx) { t.vstParameterState[p.id] = editor; break; }
                pushParamLive(t, k, editor);
                continue;
            }
            if (haveEngine && !ImGui::IsMouseDown(0)) {             // the engine moved (automation): follow in the GUI.
                                                                    // While the user holds the mouse anywhere, the GUI never fights their hand.
                const double engine = std::min(1.0, std::max(0.0, eng[k]));
                if (std::fabs(engine - agree) > 1e-6) {
                    agree = engine;
                    s.plugin->setParam(idx, engine);
                    if (k < t.vstInit.size()) t.vstInit[k] = engine;
                    for (const caw::PluginParam& p : params)
                        if (p.index == idx) { t.vstParameterState[p.id] = engine; break; }
                }
            }
        }
        // EVERY other parameter: kilohearts-class editors do NOT report a knob turn through
        // IComponentHandler::performEdit, so without this sweep only the 8 picks ever reach the
        // engine instance - the VSTe EQ then processes with its old/default curve while the
        // single-instance master FX follows its own GUI exactly. Sweep the editor instance's
        // values (cheap getters - no getState(), no timer stall) and push every change to the
        // engine instance. Picked parameters stay with the automation path above.
        const bool watched = s.open || s.plugin->editorVisible();
        const auto now = std::chrono::steady_clock::now();
        if (!watched) { s.paramSnapshot.clear(); s.paramPollT = {}; continue; }
        if (now - s.paramPollT < std::chrono::milliseconds(25)) continue;
        s.paramPollT = now;
        std::vector<double> cur;
        const int np = s.plugin->readParameterValues(cur);
        if (np <= 0) continue;
        if ((int)s.paramSnapshot.size() != np) { s.paramSnapshot.swap(cur); continue; }   // first sweep / plugin change: seed only
        for (int i = 0; i < np; ++i) {
            const double v = cur[(size_t)i];
            if (std::fabs(v - s.paramSnapshot[(size_t)i]) <= 1e-6) continue;
            s.paramSnapshot[(size_t)i] = v;
            if ((size_t)i < params.size() && params[(size_t)i].readOnly) continue;
            if (std::find(t.vstPick.begin(), t.vstPick.end(), i) != t.vstPick.end()) continue;   // the lane / P-slider path owns the picks
            if (pushVstParamLive) pushVstParamLive(t.id, i, v);
            for (const caw::PluginParam& p : params)
                if (p.index == i) { t.vstParameterState[p.id] = v; break; }   // the saved params stay current for the next Run / export
        }
    }
#else
    (void)project;
#endif
}

inline void processLearn(Track& t, Slot& s) {
    if (!s.plugin || s.learn < 0) return;
    const auto& params = s.plugin->params();
    int touched;
    while ((touched = s.plugin->takeTouchedParam()) >= 0) {
        auto existing = std::find(t.vstPick.begin(), t.vstPick.end(), touched);
        if (existing != t.vstPick.end() && (size_t)(existing - t.vstPick.begin()) != (size_t)s.learn) continue;
        if (touched < (int)params.size() && !params[(size_t)touched].readOnly) {
            const int next = s.learn + 1;
            assign(t, *s.plugin, (size_t)s.learn, params[(size_t)touched]);
            s.learn = next < 8 ? next : -1;
            // Clear queued edits and reset the polling baseline between slots.
            s.plugin->setLearnEnabled(s.learn >= 0);
            break;
        }
    }
}

// A compact mixer-strip layout: fixed-size slot numbers, flexible names, visible actions.
inline void drawMappings(Project& proj, Track& t, Slot& s, float scale) {
    // Slots bound in the code (p2 = cawget "lane", see vst_lang.h): a binding only drives the
    // plugin once the slot HAS a picked parameter - then the code wins over the slot's own lane.
    std::string bound[8];
    {
        static const std::regex bindRe(R"re(^[ \t]*[pP]([1-8])[ \t]*=[ \t]*cawget[ \t]*(?:\([ \t]*)?"([^"]*)")re");
        for (const Clip& c : t.clips)
            for (std::sregex_iterator it = std::sregex_iterator(c.code.begin(), c.code.end(), bindRe), end; it != end; ++it)
                bound[(*it)[1].str()[0] - '1'] = (*it)[2].str();
    }
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "AUTOMATION SLOTS");
    ImGui::SameLine(); ImGui::TextDisabled("%d / 8", (int)t.vstPick.size());
    ImGui::TextWrapped("The code can also bind a parameter to any automation lane:  p2 = cawget(\"test1\")  (wins over this slot's lane). A binding only works once the slot has a picked parameter (Parameters tab) - the code only replaces where the value comes from.");
    if (ImGui::BeginTable("##slots", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34.f * scale);
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 150.f * scale);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 132.f * scale);
        const auto& params = s.plugin->params();
        for (size_t k = 0; k < 8; ++k) {
            ImGui::PushID((int)k); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(ImVec4(1.f, .72f, .30f, 1.f), "P%d", (int)k + 1);
            const bool filled = k < t.vstPick.size();
            std::string name = !filled ? "(empty)"
                : t.vstPick[k] < 0 ? "(not in this plugin - pick it again)" : "(parameter unavailable)";
            if (filled) for (const auto& p : params) if (p.index == t.vstPick[k]) { name = p.title; break; }
            ImGui::TableSetColumnIndex(1);
            if (!filled && !bound[k].empty()) {
                ImGui::TextColored(ImVec4(1.f, .72f, .30f, 1.f), "code-bound:  p%d = cawget \"%s\"", (int)k + 1, bound[k].c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("The code binds this slot to the automation lane \"%s\", but no parameter is picked here yet - pick one in the Parameters tab and the code binding drives it.", bound[k].c_str());
            } else {
                ImGui::TextUnformatted(name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            if (filled && t.vstPick[k] >= 0 && (size_t)t.vstPick[k] < params.size()) {
                // Live value slider: edits the editor plugin instance, the saved parameter state
                // and the running engine's lane channel in one go, so the change is heard while
                // Csound plays (no restart). Automation on the lane still wins once it moves.
                float v = (float)s.plugin->getParam(t.vstPick[k]);
                ImGui::SetNextItemWidth(-1);
                const bool changed = ImGui::SliderFloat("##pv", &v, 0.f, 1.f, "%.3f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to change the parameter live - the running engine follows at once (0 to 1).");
                if (changed) {
                    s.plugin->setParam(t.vstPick[k], (double)v);
                    if (k < t.vstInit.size()) t.vstInit[k] = (double)v;
                    for (const caw::PluginParam& p : params)
                        if (p.index == t.vstPick[k]) { t.vstParameterState[p.id] = (double)v; break; }
                    s.sync[t.vstPick[k]] = (double)v; s.syncSeen.insert(t.vstPick[k]);   // the sync poll would push the same value again otherwise
                    pushParamLive(t, k, (double)v);
                }
            }
            ImGui::TableSetColumnIndex(3);
            const bool learning = s.learn == (int)k, unavailable = k > t.vstPick.size();
            if (unavailable) ImGui::BeginDisabled();
            if (learning) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.80f, .34f, .08f, 1.f));
            if (ImGui::SmallButton(learning ? "Listening..." : "Learn")) {
                s.learn = learning ? -1 : (int)k; s.plugin->setLearnEnabled(s.learn >= 0);
                if (s.learn >= 0) {
                    // Clicking CAW activates its main HWND, which can cover a standalone editor.
                    // Bring the same editor back to the foreground without reloading it.
                    std::string err;
                    if (!s.plugin->showEditor(niceName(t.vstPath, t.vstClassId) + " (" + t.name + ")", err)) s.err = err;
                    else s.err.clear();
                }
            }
            if (learning) ImGui::PopStyleColor();
            if (unavailable) ImGui::EndDisabled();
            if (filled) { ImGui::SameLine(); if (ImGui::SmallButton("Clear")) {
                clearSlot(t, k); s.learn = -1; s.plugin->setLearnEnabled(false);
            }}
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (false) {   // the FX only / FX+Source output mode now sits in the track header, next to the plugin window button
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "AUDIO OUTPUT");
    const char* outLabels[] = { "Master (replace source)", "Mix (source + VSTe)" };
    int om = t.vstOutMode == 1 ? 1 : 0;
    ImGui::SetNextItemWidth(-1);
    if (caw_ui::combo("##audio_out_mode", &om, outLabels, 2)) {
        t.vstOutMode = om == 1 ? 1 : 0;
        t.vstOutMaster = t.vstOutMode == 1;   // keep the legacy field in sync for older builds / save migration
        if (pushVstOutModeLive) pushVstOutModeLive(t.id, t.vstOutMode);   // live: flips the source gates + the rack's replace in the running engine, no Run needed
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master: the VSTe output replaces the source tracks' own master output (their meters go gray).\nMix: the source tracks keep sounding to the master alongside the VSTe (all meters keep their color).\n\nSwitching works while the engine plays. Inputs can be re-wired while it plays too - no Run needed.");
    ImGui::TextWrapped(t.vstOutMode == 0
        ? "Replace: the source clips' own `out` lines are muted; only the processed VSTe signal reaches the master. Source-track meters are drawn gray."
        : "Mix: the source clips and the VSTe both reach the master. Every meter keeps its default color.");
    }
#ifdef _WIN32
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "EDITOR");
    if (ImGui::Button("Reopen editor##editorreattach")) {
        s.plugin->closeEditor();
        std::string err;
        if (!s.plugin->showEditor(niceName(t.vstPath, t.vstClassId) + " (" + t.name + ")", err)) s.err = err;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Closes and reopens the plugin's own window. Use it if the editor drew wrong or lost its graphics.\n\nThe Windows compatibility scaling of ALL editors is now a general setting: Settings > Editor > Plugin windows.");
#endif
}

inline void drawParameterBrowser(Track& t, Slot& s) {
    const auto& params = s.plugin->params();
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "PARAMETER LIBRARY");
    char fb[128]; std::snprintf(fb, sizeof fb, "%s", s.paramFilter.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter_params", "Search parameters...", fb, sizeof fb)) s.paramFilter = fb;
    const std::string low = lower(s.paramFilter);
    std::vector<const caw::PluginParam*> matches;
    for (const auto& p : params) if (!p.readOnly && (low.empty() || lower(p.title).find(low) != std::string::npos)) matches.push_back(&p);
    ImGui::TextDisabled("%d shown", (int)matches.size());
    if (params.empty()) ImGui::TextWrapped("No parameters were reported by this plugin. Check the plugin log if its editor also fails.");
    else if (matches.empty()) ImGui::TextDisabled("No writable parameters match your search.");
    ImGui::BeginChild("##parameter_list", ImVec2(0, 0), true);
    ImGuiListClipper clipper; clipper.Begin((int)matches.size());
    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const auto& p = *matches[(size_t)row];
        auto it = std::find(t.vstPick.begin(), t.vstPick.end(), p.index);
        bool on = it != t.vstPick.end();
        // empty slots (a plugin change left some picks at -1) are always available for a new pick
        const bool full = !on && (int)std::count_if(t.vstPick.begin(), t.vstPick.end(), [](int v) { return v >= 0; }) >= 8;
        if (full) ImGui::BeginDisabled();
        const std::string label = (on ? "P" + std::to_string((int)(it - t.vstPick.begin()) + 1) + "  " : std::string("     ")) + p.title + "##parameter" + std::to_string(p.index);
        if (ImGui::Checkbox(label.c_str(), &on)) {
            if (on) assign(t, *s.plugin, t.vstPick.size(), p);
            else clearSlot(t, (size_t)(it - t.vstPick.begin()));
            s.learn = -1; s.plugin->setLearnEnabled(false);
        }
        if (full) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.title.c_str());
    }
    ImGui::EndChild();
}
#endif

inline void captureParameters(Project& project) {
#ifdef CAW_VST3
    for (Track& track : project.tracks) {
        auto it = slots().find(track.id);
        if ((track.kind == 3 || track.kind == 4) && it != slots().end())
            captureTrackParameters(track, it->second);
    }
    syncLiveParameters(project);   // plugin GUI knobs <-> running engine, every frame
#else
    (void)project;
#endif
}

// Continue parameter learning while CAW's main window is minimized, even
// though its ImGui windows are not being laid out.
inline void serviceLearn(Project& project) {
#ifdef CAW_VST3
    for (Track& track : project.tracks) {
        if (track.kind != 3 && track.kind != 4) continue;
        auto it = slots().find(track.id);
        if (it != slots().end() && it->second.open && it->second.plugin)
            processLearn(track, it->second);
    }
#else
    (void)project;
#endif
}

inline void drawWindows(Project& proj) {
    for (Track& t : proj.tracks) {
        if (t.kind != 3 && t.kind != 4) continue;
        Slot& s = slots()[t.id];
        if (!s.open) continue;
        const bool inst = t.kind == 3;
        const float scale = ImGui::GetFontSize() / 17.f;
        ImGui::SetNextWindowSize(ImVec2(530.f * scale, 460.f * scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(500.f * scale, 320.f * scale), ImVec2(10000, 10000));
        const std::string title = std::string(inst ? "VSTi: " : "VSTe: ") + t.name + "##vst" + std::to_string(t.id);
        if (ImGui::Begin(title.c_str(), &s.open)) {
#ifndef CAW_VST3
            ImGui::TextColored(ImVec4(1.f, .45f, .35f, 1.f), "This build has no VST3 host: plugins cannot load, run or edit live. Rebuild with CAW_VST3=ON (the default).");
#endif
#ifdef CAW_VST3
            startScan();
            if (!s.autoTried && !t.vstPath.empty()) { s.autoTried = true; loadPlugin(s, t, t.vstPath, t.vstClassId, proj.cs.sr); }   // a saved project: load the plugin again
            // The top strip stays visible while the mapping and library panels scroll independently.
            const auto plugins = catalogSnapshot();
            const bool scanDone = catalog().done;
            // The class this track is on. A project saved before class ids existed has none: treat
            // whatever the editor loaded as selected, instead of marking a whole bundle selected.
            const std::string selClass = t.vstClassId.empty() ? (s.loadedPath == t.vstPath ? s.loadedClassId : std::string()) : t.vstClassId;
            const std::string current = t.vstPath.empty() ? std::string("Choose a plugin...") : niceName(t.vstPath, selClass);
            ImGui::SetNextItemWidth(-1);
            if (caw_ui::beginCombo("##plugin", current.c_str())) {
                char fb[128]; std::snprintf(fb, sizeof fb, "%s", s.pluginFilter.c_str());
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputTextWithHint("##pf", "search...", fb, sizeof fb)) s.pluginFilter = fb;
                const std::string low = lower(s.pluginFilter);
                if (!scanDone) ImGui::TextDisabled("Scanning plugin types... unknown types appear in both lists.");
                int visible = 0;
                for (const caw::PluginInfo& info : plugins) {
                    if (info.kind != 0 && info.kind != (inst ? 1 : 2)) continue;
                    const std::string nm = info.name, f = info.path, cid = info.classId;
                    if (!low.empty() && lower(nm).find(low) == std::string::npos) continue;
                    ++visible;
                    // Two-line card: the plugin name over a small gray line with the type and the
                    // folder it lives in. One full-row Selectable carries hover / selected accent
                    // over both lines, so rows stay readable on the dark popup background.
                    const char* kindText = info.kind == 1 ? "instrument" : info.kind == 2 ? "effect" : "type unknown";
                    const size_t lastSlash = f.find_last_of("/\\");
                    std::string folder = lastSlash == std::string::npos ? f : f.substr(0, lastSlash);
                    const size_t folderSlash = folder.find_last_of("/\\");
                    folder = folderSlash == std::string::npos ? folder : folder.substr(folderSlash + 1);
                    const std::string sub = std::string(kindText) + "   (" + folder + ")";
                    const float rowH = ImGui::GetTextLineHeight() * 2.f + 6.f * scale;
                    const float rowW = ImGui::GetContentRegionAvail().x;
                    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
                    const bool selectedRow = f == t.vstPath && (cid == selClass || (cid.empty() && selClass.empty()));
                    // The row id carries the class id too: several rows share one bundle path.
                    const std::string rowId = "##plugrow" + f + "##" + cid;
                    const bool pickedRow = ImGui::Selectable(rowId.c_str(), selectedRow, ImGuiSelectableFlags_None, ImVec2(rowW, rowH));
                    ImGui::SetCursorScreenPos(ImVec2(rowPos.x + 8.f * scale, rowPos.y + 3.f * scale));
                    ImGui::TextUnformatted(nm.c_str());
                    ImGui::SetCursorScreenPos(ImVec2(rowPos.x + 8.f * scale, rowPos.y + 3.f * scale + ImGui::GetTextLineHeight()));
                    ImGui::TextDisabled("%s", sub.c_str());
                    ImGui::SetCursorScreenPos(ImVec2(rowPos.x, rowPos.y + rowH + ImGui::GetStyle().ItemSpacing.y));
                    if (pickedRow) loadPlugin(s, t, f, cid, proj.cs.sr);   // picks, names and start values stay: a new plugin matches them by name
                }
                if (scanDone && plugins.empty()) ImGui::TextDisabled("No .vst3 found in the standard VST3 folders.");
                else if (scanDone && visible == 0) ImGui::TextDisabled("No plugins match this type or search.");
                ImGui::EndCombo();
            }
            if (!s.plugin) ImGui::BeginDisabled();
            if (ImGui::Button(s.plugin && s.plugin->editorVisible() ? "Show plugin editor" : "Open plugin editor")) {
                std::string err; if (!s.plugin->showEditor(niceName(t.vstPath, t.vstClassId) + " (" + t.name + ")", err)) s.err = err; else s.err.clear();
            }
            if (!s.plugin) ImGui::EndDisabled();
            if (t.kind == 4) {   // the output mode right next to the open-plugin button: renamed Master/Mix
                ImGui::SameLine();
                static const char* fxModes[] = {"FX only", "FX+Source"};
                int om = t.vstOutMode == 1 ? 1 : 0;
                ImGui::SetNextItemWidth(84.f * scale);
                if (caw_ui::combo("##audio_out_mode2", &om, fxModes, 2)) {
                    t.vstOutMode = om == 1 ? 1 : 0;
                    t.vstOutMaster = t.vstOutMode == 1;   // keep the legacy field in sync for older builds / save migration
                    if (pushVstOutModeLive) pushVstOutModeLive(t.id, t.vstOutMode);   // live: flips the source gates + the rack's replace, no Run needed
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("FX only: the VSTe output replaces the source tracks' own master output (their meters go gray).\nFX+Source: the source tracks keep sounding to the master alongside the VSTe (all meters keep their color).\n\nSwitching works while the engine plays. Inputs can be re-wired while it plays too - no Run needed.");
            }
            if (!scanDone) { ImGui::SameLine(); ImGui::TextDisabled("Scanning..."); }
            if (inst) {   // VSTi: the sound only starts once NOTES reach the plugin
                bool hasNotes = false;
                for (const MidiClip& mc : t.midiClips) if (!mc.muted)
                    for (const MidiNote& n : mc.notes) if (n.start < mc.dur) { hasNotes = true; break; }
                bool hasNoteCode = false;
                for (const Clip& c : t.clips)
                    if (c.code.find("note ") != std::string::npos || c.code.find("cc ") != std::string::npos || c.code.find("msg ") != std::string::npos) { hasNoteCode = true; break; }
                if (!hasNotes && !hasNoteCode)
                    ImGui::TextColored(ImVec4(1.f, .72f, .30f, 1.f), "This instrument needs notes to make sound: right-click the track > Add MIDI clip (its notes go straight to the plugin - no code clip needed), then draw notes in the MIDI editor and press Play. The VST engine must be running (the transport Run button - Run VST when it is the only engine checked).");
            }
            if (!inst) {   // VSTe: the audio inputs - one row per configured input, more can be added
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "AUDIO INPUTS");
                ImGui::TextWrapped("Each insert feeds the plugin on its own line(s); a stereo source takes two lines, nothing is mixed down. master = the project master mix. Unchecked = silent. '-' removes the insert (the slot is freed).");
                static const char* inModes[] = { "track", "clips" };   // master was removed: the mix goes through the track / clip sources
                int configured = 0, shown = 0, hidden = 0;
                for (int i = 0; i < 8; ++i) if (!t.vstInputs[i].name.empty() || t.vstInputs[i].mode == 2 || t.vstInputs[i].on) ++configured;
                if (configured == 0)
                    ImGui::TextColored(ImVec4(1.f, .72f, .30f, 1.f), "No audio input yet: this effect processes silence (the source plays dry and the plugin seems to do nothing). Turn row 1 on and pick a source track or clip.");
                for (int i = 0; i < 8; ++i) if (s.hiddenInputs.count(i)) ++hidden;
                for (int i = 0; i < 8; ++i) {
                    Track::VstInput& in = t.vstInputs[i];
                    if (s.hiddenInputs.count(i)) continue;                       // collapsed by the user (the small "-")
                    if (i > 0 && !(in.on || !in.name.empty() || in.mode == 2)) continue;  // never configured: not shown - row 1 is ALWAYS visible so the first input needs no '+ Add input' click
                    ImGui::PushID(i);
                    ++shown;
                    const bool wasOn = in.on;
                    ImGui::Checkbox("##in_on", &in.on);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn this input on / off. Works while the engine plays - the later inputs move up, the row stays here.");
                    if (in.on != wasOn) pushInputMaskLive(t);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-##in_remove")) {   // REMOVE: the slot is freed (settings cleared), not hidden
                        in.on = false;
                        in.name.clear();
                        in.mode = 0;
                        pushInputMaskLive(t);
                        if (pushVstWiring) pushVstWiring();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this input: the slot is freed and another input can use it (the row disappears). Works while the engine plays.");
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d", shown);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.f * scale);
                    int m = in.mode == 2 ? 0 : in.mode;
                    if (caw_ui::combo("##in_mode", &m, inModes, 2)) { in.mode = std::min(1, std::max(0, m)); if (pushVstWiring) pushVstWiring(); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("track = every clip of the named track; clips = the clip with that name.\n\nOn/off and a new source are wired at once, with Csound running or with the VST engine - no Run needed. A clip added after Run has no tap yet: Run once for it.");
                    ImGui::SameLine();
                    if (in.mode != 2) {   // track / clips: the source as the SAME dropbox style, listing all possible inputs
                        ImGui::SetNextItemWidth(160.f * scale);
                        const std::string preview = in.name.empty()
                            ? (in.mode == 0 ? "(no source)" : "(no clip)")
                            : in.name;
                        bool changed = false;
                        if (caw_ui::beginCombo("##in_name", preview.c_str())) {
                            if (in.mode == 0) {
                                for (const Track& o : proj.tracks) if ((o.kind == 0 || o.kind == 3 || o.kind == 4 || o.kind == 5 || o.kind == 6 || o.kind == 7) && o.id != t.id) {
                                    const char* kindText = o.kind == 0 ? "Csound" : o.kind == 3 ? "VSTi" : o.kind == 4 ? "VSTe" : o.kind == 5 ? "SuperCollider" : o.kind == 7 ? "Sample" : "Pure Data";
                                    const std::string tag = std::string(o.name) + "  (" + kindText + ")";
                                    if (ImGui::Selectable(tag.c_str(), o.name == in.name)) {
                                        in.name = o.name;
                                        changed = true;
                                        if (!in.on) { in.on = true; pushInputMaskLive(t); }   // picking a source also turns the row on
                                    }
                                }
                            } else {
                                std::set<std::string> seen;
                                for (const Track& o : proj.tracks) if (o.kind != 1 && o.kind != 2) for (const Clip& c : o.clips)
                                    if (seen.insert(c.name).second && ImGui::Selectable(c.name.c_str(), c.name == in.name)) {   // sample clips resolve to their track's mix (like VST clips)
                                        in.name = c.name;
                                        changed = true;
                                        if (!in.on) { in.on = true; pushInputMaskLive(t); }
                                    }
                            }
                            ImGui::EndCombo();
                        }
                        if (changed && pushVstWiring) pushVstWiring();
                    } else {
                        ImGui::TextDisabled("(master mix)");
                    }
                    ImGui::PopID();
                }
                if (hidden > 0 && ImGui::SmallButton("Show hidden")) s.hiddenInputs.clear();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bring back the collapsed input rows");
                if (hidden > 0) { ImGui::SameLine(); ImGui::TextDisabled("%d hidden", hidden); }
                if (configured < 8) {
                    if (ImGui::SmallButton("+##in_add")) {   // next line, not beside the rows
                        for (int i = 0; i < 8; ++i)
                            if (!(t.vstInputs[i].on || !t.vstInputs[i].name.empty() || t.vstInputs[i].mode == 2)) {
                                t.vstInputs[i].on = true;
                                s.hiddenInputs.erase(i);
                                break;
                            }
                        pushInputMaskLive(t);   // the new slot's on/off state
                        if (pushVstWiring) pushVstWiring();   // its (empty) source is resolved at once
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add another insert (input) to the VSTe chain, up to 8. The checkbox works live; the VST engine wires the new input at once, with Csound running press Run Csound again.");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%d / 8", configured);
            }
            if (!s.err.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .45f, .40f, 1.f));
                ImGui::TextWrapped("%s", s.err.c_str()); ImGui::PopStyleColor();
            }
            if (!s.note.empty()) ImGui::TextDisabled("%s", s.note.c_str());
            if (!s.plugin) { ImGui::TextDisabled("Choose a plugin to start."); ImGui::End(); continue; }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            // Learn keeps running regardless of which pane or tab is visible.
            processLearn(t, s);
            const float width = ImGui::GetContentRegionAvail().x;
            if (width >= 760.f * scale) {
                const float height = std::max(120.f * scale, ImGui::GetContentRegionAvail().y - 4.f * scale);
                if (ImGui::BeginTable("##workbench", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, height))) {
                    ImGui::TableSetupColumn("Mappings", ImGuiTableColumnFlags_WidthStretch, 1.05f);
                    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 1.f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    ImGui::BeginChild("##mappings_panel", ImVec2(0, height), true); drawMappings(proj, t, s, scale); ImGui::EndChild();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::BeginChild("##library_panel", ImVec2(0, height), true); drawParameterBrowser(t, s); ImGui::EndChild();
                    ImGui::EndTable();
                }
            } else if (ImGui::BeginTabBar("##plugin_tabs")) {
                if (ImGui::BeginTabItem("Mappings & routing")) {
                    ImGui::BeginChild("##mappings_compact", ImVec2(0, 0), true); drawMappings(proj, t, s, scale); ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Parameters")) {
                    ImGui::BeginChild("##library_compact", ImVec2(0, 0), true); drawParameterBrowser(t, s); ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
#else
            ImGui::TextWrapped("This build of CAW has no VST3 support (built with CAW_VST3=OFF).");
#endif
        }
        ImGui::End();
    }
}
}
