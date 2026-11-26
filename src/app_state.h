#pragma once

// app_state.h - the app's shared globals (phase 2: moved verbatim out of app.cpp).
// Every variable is 'inline' (C++17): one shared copy across all translation units.
// Consumers must include app_shared.h first (it pulls the full engine + UI stack).

#include "model.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

inline Project g_proj;

inline bool g_langCsound = true, g_langPd = false, g_langSc = false;

inline bool g_langVst = true;      // VST3 instruments (VSTi, the VST engine) and effects (VSTe, Csound rack)

inline bool g_langAudio = false;   // audio tracks (planned: nothing to load yet - kept so the choice round-trips)

inline bool g_chooseLanguages = true, g_languageChoiceMade = false;

inline bool g_pdEngineOn = false;

inline bool g_mixedPd = false; // Csound host owns the one device for both sources

inline std::string g_pdLibraryPath;

inline std::string g_scLangPath;

inline std::string g_scSynthPath;

inline ScEngine g_scEngine;

inline std::unique_ptr<CawAudioOutput> g_scSoloOutput;

inline std::unique_ptr<CawAudioOutput> g_sampleOutput;   // the sample engine's own stream (Run sample alone)

inline std::unique_ptr<MonitorCapture> g_sampleCapture;  // its meter / scope capture (the sample engine is "in the chain" like the others)

inline uint64_t g_sampleSnapshotSeq = 0;

inline std::unique_ptr<MonitorCapture> g_scSoloCapture;

inline uint64_t g_scSoloSnapshotSeq = 0;

inline std::unique_ptr<CawAudioOutput> g_vstSoloOutput;

inline std::unique_ptr<MonitorCapture> g_vstSoloCapture;

inline uint64_t g_vstSoloSnapshotSeq = 0;

inline bool g_vstEngineOn = false;            // the "Run VST" button: CAW's own instrument engine is running

inline bool g_vstIdleNoPlugin = false;        // the engine runs but no plugin is picked yet: not "starting", just idle (the pill stays on)

inline bool g_sampleEngineOn = false;         // the "DSP" button: sample tracks (kind 7) play their clips

extern std::map<int, unsigned long long> g_audioVoiceSig;   // clipId -> the running voice's signature (defined below)

inline int g_spectroQuality = 0;              // 0 = high (2048 FFT, one cell per pixel), 1 = minimal (512 FFT, chunky cells)

inline bool g_masterFxOpen = false;           // the Master FX window (8 effects in order on the master mix)

inline bool g_vstHostedInCsound = false;      // SEPARATED ENGINES: always false - the 4th engine owns the VST graph in every mode

inline double g_vstRetryT = 0.0;              // seconds until the dedicated output may be retried

inline std::string g_vstFailedSig;            // the instrument set that failed to load; cleared by a re-pick / a new set

inline double g_vstFailedRetryT = 0.0;        // seconds until a failed instrument load is tried again

inline double g_vstJobsT = 0.0;               // 0.5 s timers: the job diff and the clip-code mode check

inline std::mutex g_vstLogMutex;              // the engine logs from the audio thread: queue, drain on the UI thread

inline std::vector<std::string> g_vstLogQueue;

inline bool g_scEngineOn = false, g_scReady = false;

inline int g_scPendingClipId = 0;

inline int g_scNextInstanceId = 1;

struct ScActiveInstance { int clipId = 0; std::chrono::steady_clock::time_point until; };

inline std::map<int, ScActiveInstance> g_scActiveInstances;

inline std::map<int, std::chrono::steady_clock::time_point> g_scManualRuns;

inline std::map<int, std::string> g_scClipFileCache;

inline int g_scErrorClipId = -1, g_scErrorContextLines = 0, g_scErrorLogIndex = -1;

inline int g_scOutputClipId = 0, g_scOutputInstanceId = 0;

inline bool g_scErrorHeadlineSeen = false;

inline std::string g_scErrorMessage;

inline unsigned long long g_scTimelineSig = 0;

inline double g_scPlanT = 0.0;

inline double g_scLastPDTimelineTime = -1.0;

inline std::chrono::steady_clock::time_point g_scStartTime{};

inline CawScRing g_scRing;                        // the whole SuperCollider device mix (mode-2 "master" inputs)

inline std::map<int, std::unique_ptr<CawScRing>> g_sampleRings;

inline std::map<int, std::unique_ptr<CawScRing>> g_scTrackRings;   // per-track-slot SC mix (engine 2): one ring per SuperCollider track

inline std::mutex g_scTrackRingMutex;

inline std::mutex g_sampleRingMutex;

inline std::atomic<bool> g_scRawSuppress{false};  // true while an active master-mode VSTe replaces the SC mix

inline std::atomic<bool> g_sampleRawSuppress{false};   // true while an active master-mode ("FX only") VSTe consumes a sample track: the raw sample mix is silenced, the effect's output replaces it

inline std::atomic<int> g_sampleDryLatencyComp{0};   // reported latency (samples) of the largest active sample-consuming VSTe: the raw dry mix is delayed by it in mix mode so dry and wet stay in sync
// Play diagnostics (frozen-track noise hunt): the shared audio callback measures the SuperCollider engine's
// own mix level and the sample engine's level separately, so a console line can name the noisy source.
inline std::atomic<float> g_diagScRms{0.f};
inline std::atomic<float> g_diagCawRms{0.f};

struct DryDelay {
    std::vector<float> buf;
    long long w = 0;                 // absolute frames written
    long long cap = 0; int ch = 2;
    void ensure(int frames, int channels) {
        (void)frames;
        if (cap < 65536 || ch != channels) { cap = 65536; ch = channels; buf.assign((size_t)cap * (size_t)ch, 0.f); w = 0; }
    }
    void push(const float* in, unsigned long frames) {
        for (unsigned long i = 0; i < frames; ++i) {
            const long long pos = w % cap;
            for (int c = 0; c < ch; ++c) buf[(size_t)pos * (size_t)ch + (size_t)c] = in[(size_t)i * (size_t)ch + (size_t)c];
            ++w;
        }
    }
    long long written() const { return w; }
    // reads the frame written at absolute index idx (0 / silence before anything was written)
    void frameAt(long long idx, float* dst) const {
        if (idx < 0 || idx < w - cap) { for (int c = 0; c < ch; ++c) dst[c] = 0.f; return; }   // unwritten, or overwritten long ago
        const long long pos = idx % cap;
        for (int c = 0; c < ch; ++c) dst[c] = buf[(size_t)pos * (size_t)ch + (size_t)c];
    }
};

inline std::map<std::string, pd_patch_editor::Buffer> g_pdPatchBuffers;

inline bool g_askPdOnClose = false;

inline PdPreview g_pdPreview;

inline PdDawEngine g_pdTimelineEngine;

inline CawHub g_cawHub;                         // shared caws/cawr message bus (see collectCawHubNames / pollCawHub)

inline bool g_pdTimelineMode = false;

inline bool g_pdResumeAfterReload = false;   // the PD engine is being rebuilt: rejoin the running transport when it is ready

inline int g_pdPlayingClip = 0;

inline double g_pdPreviousTime = -1.0;

inline unsigned long long g_pdTransportEpoch = 0;

inline std::vector<pd_timeline::Event> g_pdTimelineEvents;

inline pd_timeline::Event g_pdPlayingEvent{};   // the clip that sounds now (to notice a changed patch)

inline double g_pdPlanT = 0.0;                  // time since the PD clips were last compared with the project

inline std::string g_playNotice;                // visible in the transport bar for a few seconds (the console log is easy to miss)

inline double g_playNoticeT = 0.0;

inline std::string g_pdLastPlanError;           // so a problem is logged once, not four times a second

inline bool g_pdNeedsReload = false;

inline bool g_pdPlanUpdateFailed = false;

inline double g_play = 0.0;          // playhead (seconds)

inline std::map<std::string, float> g_scLaneSent;   // lane values already delivered to ~cawr (SuperCollider)

inline bool g_playing = false;

inline bool g_liveCsound = false;

inline float g_pps = 80.f;           // pixels per second (zoom)

inline const float kPpsMin = 2.f, kPpsMax = 2000.f;   // zoom range: whole piece on one screen ... single milliseconds

inline float g_scrollX = 0.f;        // seconds

inline float g_scrollY = 0.f;        // pixels

inline int g_selTrack = -1, g_selClip = -1;

inline int g_nextTrackId = 1;

inline std::string g_projectPath;

inline bool g_renderInProjectFolder = true;   // lock renders (LKD WAVs) inside the project folder (exports/lkd) vs next to the .caw file (Settings > Engines)

inline std::string g_savedSnap;               // the project as it was when it was last saved / opened (to know if there are unsaved changes)

inline std::string g_autoSavedSnap, g_unsavedAutosavePath;

inline float g_autoSaveElapsed = 0.f;

inline bool g_askSaveOnClose = false;

inline bool g_confirmDeleteSelected = false;

inline ImFont* g_codeFonts[3] = {nullptr, nullptr, nullptr};

inline ImFont* g_markUiFont = nullptr;

inline ImFont* g_markCodeFont = nullptr;

inline ImVec2 g_logoUv0, g_logoUv1;      // fallback: old 512 px bitmap logo (startup_logo.h)

inline bool g_logoReady = false;

struct LogoVariant { int px = 0; ImVec2 uv0, uv1; };

inline constexpr int kLogoVariantCount = 3;

inline constexpr float kLogoSizes[kLogoVariantCount] = {112.f, 220.f, 240.f};   // before DPI scaling

inline LogoVariant g_logoVariants[kLogoVariantCount];

inline int g_logoVariantsReady = 0;

inline std::unordered_map<int, std::unique_ptr<TextEditor>> g_codeEditors;

inline int g_autoDragTrackId = -1;

inline int g_autoDragPoint = -1;

inline std::string g_autoDragChannel;

enum class Drag { None, Move, Resize, Playhead, ResizeTrack, ResizeAutomation, Marquee, ResizeHeader, ScrollX, ScrollY, ResizeLeft };

inline Drag g_drag = Drag::None;

inline double g_dragAnchorT = 0.0;                       // timeline position under the mouse when a clip drag started

inline std::vector<std::pair<int, double>> g_dragOrig;   // (clip id, start) of every dragged clip

inline ClipAutomationMove g_clipAutomationMove;

inline std::set<int> g_selMidiClips;                      // ids of the selected MIDI clips (MIDI lane)

inline int g_mcDragId = -1;                               // MIDI clip being dragged in the MIDI lane

inline int g_mcDragMode = 0;                              // 1 move, 2 resize right edge, 3 resize left edge

inline double g_mcAnchorT = 0.0, g_mcOrigStart = 0.0, g_mcOrigEnd = 0.0;

inline std::vector<std::pair<int, double>> g_mcOrig;      // (MIDI clip id, start) of everything moved together

inline std::vector<MidiNote> g_mcOrigNotes;

inline int g_ctxMidiId = 0;                               // MIDI clip of the right-click menu

inline std::set<int> g_selClips;                         // ids of all selected clips (g_selTrack/g_selClip = primary one)

inline ImVec2 g_clipMarqueeStart;

inline bool g_clipMarqueeMoved = false, g_clipMarqueeAdd = false;

inline float g_headerW = 370.f;                          // width of the left lane panel (unscaled px), drag its edge to resize

inline bool g_timelineRun = false;                       // Play: the timeline moves and triggers the instruments of the running Csound

inline bool g_loopSelecting = false;

inline double g_loopAnchor = 0.0;

inline double g_hold = 0.0;                               // after Csound has started, the playhead waits one sound-card buffer so it meets the sound

inline int g_resync = 0;                                 // not 0 = the timeline has to be (re)started from the playhead (Play pressed, playhead moved)

inline std::string g_runName;                            // clip started with "Run clip" in the running engine

inline double g_runLeft = 0.0;                           // seconds until that clip ends by itself
// Run with the DSP OFF plays the clip: the engine is started and the clip fires when it is up.
inline int g_csoundPendingClipId = 0;                    // the clip waiting for the DSP to come up
inline double g_csoundPendingAt = 0.0;                   // when the pending Run was pressed (steady-clock seconds)

inline double g_pdRunLeft = 0.0;                         // SUPERCOLLIDER-STYLE: seconds until the PD Run-clip ends by itself (Run plays the patch for the clip duration, then stops)

struct LiveCodeEdit { double at = 0.0; bool pending = false; };

inline std::map<int, LiveCodeEdit> g_liveCodeEdits;      // clip id -> last code edit (recompiled after a short pause)

inline std::set<int> g_clipCodeDirty;                    // SUPERCOLLIDER-STYLE LIVE CODING: clip ids whose code was edited but not yet pushed to the engine. Cleared by Alt+Enter / Run; edits are NEVER applied while typing.

inline float g_midiPps = 120.f, g_midiScrollX = 0.f;     // MIDI editor zoom/scroll, independent from the project timeline

struct MidiOrig { int idx; double start; int pitch; };

inline std::set<int> g_midiSel;                          // selected note indices (clip = g_midiSelectedTrackId)

inline std::vector<MidiOrig> g_midiMoveOrig;

inline double g_midiEmptyPressT = -10.0;   // time of the last press on an empty cell of the piano roll (double-click adds a note)

inline ImVec2 g_midiEmptyPressPos(0.f, 0.f);

inline bool g_midiMarqueeDbl = false;

inline bool g_midiMarquee = false, g_midiMarqueeMoved = false, g_midiMarqueeAdd = false;

inline int g_midiMarqueeClip = -1;

inline ImVec2 g_midiMarqueeStart;

inline float g_sbGrab = 0.f;                             // where inside a scrollbar thumb the mouse grabbed it

inline int g_resizeTrack = -1;

inline std::string g_resizeAutomationChannel;

inline float g_resizeStartY = 0.f, g_resizeStartH = 0.f;

inline int g_midiDragTrackId = -1, g_midiDragNote = -1;

inline int g_midiSelectedTrackId = -1, g_midiSelectedNote = -1;

inline int g_midiDragMode = 0; // 1 move, 2 resize

inline int g_midiDragPitchOffset = 0;

inline double g_midiDragTimeOffset = 0.0;

inline int g_trackCounter = 0;

inline std::vector<int> g_multiMidiIds;       // MIDI clips shown together in ONE piano roll (select them, press Enter)

inline bool g_multiMidiOpen = false;

inline int g_ctxTrack = -1;                   // track of the right-click menu in the left panel (-1 = empty panel space)

inline int g_markTrack = -1, g_markIdx = -1;  // marker being edited

inline int g_markDrag = -1, g_markDragTrack = -1, g_markDragMode = 0;   // marker being dragged (1 move, 2 resize)

inline double g_markGrab = 0.0;

inline int g_nextClipId = 1;

inline std::unordered_map<int, std::unique_ptr<TextEditor>> g_clipCodeEditors;  // one code editor per clip instrument

inline bool g_quitRequested = false;             // Ctrl+Q / File > Exit

inline bool g_codeTyping = false;               // the text cursor is inside a code editor text area (Space types a space)

inline bool g_codeClickHit = false;

inline bool g_textEditorFocused = false;     // a code editor window had focus last frame (its own Ctrl+Z wins)

inline int g_ctxClipId = -1;                 // clip shown in the right-click clip menu

enum class Tool { Select, Pen, Cut, Join, Pen2 };   // one tool at a time: 1 Select, 8 Pen 1 (simplified), 9 Pen 2 (every dot kept), 3 Cut, 4 Join

inline Tool g_tool = Tool::Select;

enum class AutoMode { None, Move, Marquee, Pen };

inline double g_trimStart = 0.0, g_trimEnd = 0.0;       // clip edges when a left-edge trim started

inline std::vector<MidiNote> g_trimNotes;               // the clip's notes when the trim started

inline AutoMode g_autoMode = AutoMode::None;

inline int g_autoSelTrackId = -1;            // lane that owns the point selection

inline std::string g_autoSelChannel;

inline double g_autoLastPressT = -10.0;      // own double-click detection for the automation lanes (does not depend on the ImGui double-click)

inline ImVec2 g_autoLastPressPos(0.f, 0.f);

inline int g_autoLastTrackId = -1;

inline std::string g_autoLastChannel;

inline bool g_autoLastAddedValid = false;    // the first click of a double-click dropped a dot (kept here so the double-click can move it)

inline AutoPoint g_autoLastAdded{0.0, 0.0};

inline double g_autoPenMinT = 0.0, g_autoPenMaxT = 0.0;   // time span of the pen stroke being drawn (simplified to few dots on release)

inline bool g_autoPressAdded = false;        // a plain click on an empty lane already dropped its dot on mouse-down (no wait for release)

inline AutoPoint g_autoPressPoint{0.0, 0.0};

inline std::set<int> g_autoSel;              // selected point indices in that lane

inline std::map<std::pair<int, std::string>, std::set<int>> g_autoSelMulti;   // dots picked by a box over several lanes: (track id, channel) -> indices

inline std::vector<AutoPoint> g_autoDragOrig;
inline std::map<std::pair<int, std::string>, std::vector<AutoPoint>> g_autoDragOrigMulti;   // the other selected lanes as they were when a dot drag began
inline std::map<std::pair<int, std::string>, std::set<int>> g_dragDots;   // the dots that travel with a clip drag (all lanes)

inline ImVec2 g_autoDragStartMouse;

inline double g_autoDragStartT = 0.0, g_autoDragStartV = 0.0;

inline bool g_autoDragMoved = false, g_autoMarqueeAdd = false;

inline double g_autoPenLastT = 0.0;

inline float g_autoPenLastX = 0.f;

inline bool g_autoOpenPointPopup = false;

inline int g_marqDblKind = 0, g_marqLane = -1;

inline double g_marqT = 0.0;

inline std::vector<std::pair<int, double>> g_dragMidiOrig;   // MIDI regions that move together with dragged Csound clips

inline std::vector<std::string> g_undoStack, g_redoStack;

inline std::string g_histCommitted, g_histPending;

inline float g_histIdle = 0.f;

inline const size_t kHistoryLimit = 300;

inline float g_ui = 1.f;             // DPI scale

inline const float kDefaultTrackH = 76.f;

inline const float kFixedTrackH = 58.f;   // lanes have a fixed height: name + short description fit exactly

inline const float kMinTrackH = 44.f, kMaxTrackH = 320.f;

inline const float kAutomationLaneH = 42.f;

inline const float kMidiLaneH = 52.f;

inline const int kMidiPitchLow = 48, kMidiPitchHigh = 83;

inline int g_midiPitchLow = kMidiPitchLow;

inline const int kMidiVisiblePitches = 36;

inline const float kMidiRowH = 16.f;

inline bool g_showMonitor = true;

inline bool g_settingsOpen = false;

inline bool g_aboutOpen = false;   // Help > About

inline bool g_welcomeOpen = false; // language selector replaces the old startup welcome

inline int g_settingsGoto = -1;   // menu: which Settings tab to show next (0 Audio, 1 Programs, 2 Render, 3 Editor, 4 Advanced, 5 Files); -1 = none

inline bool g_audioPoolOpen = false;

inline bool g_udoPoolOpen = false;

inline bool g_instrPoolOpen = false;

inline bool g_includePoolOpen = false;

inline bool g_orchestraOpen = false;

inline int g_audioPoolSelected = -1;

inline int g_includeSelected = -1;

inline float g_audioSidebarWidth = 240.f;

inline float g_udoSidebarWidth = 240.f;

inline float g_includeSidebarWidth = 240.f;

inline int g_udoPoolSelected = -1;

inline int g_instrPoolSelected = -1;

inline float g_instrSidebarWidth = 240.f;

inline float g_monitorH = 210.f;

inline const float kMonitorMinH = 90.f, kMonitorMaxH = 520.f, kMonitorSplitterH = 12.f;

enum { kPanelSpectrum = 0, kPanelScope = 1, kPanelConsole = 2, kPanelCount = 3 };

inline int g_panelOrder[kPanelCount] = {kPanelSpectrum, kPanelScope, kPanelConsole};

inline float g_panelW[kPanelCount] = {0.f, 0.f, 0.f};

inline float g_meterW = 0.f;   // width of the meter panel (unscaled px), 0 = from the number of channels; drag its left edge

inline int g_specDetail = 1;    // spectrum drawing: 0 coarse, 1 normal, 2 fine (right-click the spectrum)

inline int g_specFall = 1;      // spectrum fall speed: 0 fast, 1 medium, 2 slow

inline int g_scopeSmooth = 1;   // scope interpolation: 0 linear, 1 cubic, 2 high-resolution cubic

inline int g_scopeDisplay = 1;  // 0 free-running sweep, 1 rising-zero-crossing triggered frame

inline int g_scopeMs = 6;       // scope time window, in milliseconds

inline int g_scopeChannels = 0; // 0 stereo, 1 left, 2 right, 3 mid

inline int g_scopeSample = 0;   // 0 point sample, 1 signed peak over each display bucket

inline bool g_scopeFreeze = false;

inline bool g_scopeGrid = true;

inline bool g_specFreeze = false;

inline int g_specDisplay = 0;   // 0 live response, 1 discrete updates

inline float g_specFrameClock = 0.f;

inline int g_specScale = 0;     // 0 logarithmic, 1 linear

inline int g_specInterpolation = 1; // 0 linear, 1 cubic

inline int g_specStyle = 0;     // tonal peaks by default
inline bool g_specBetter = true;   // Better view: calm, readable (default). Realistic: raw and fast.

inline bool g_specGrid = true;

inline float g_specMinHz = 25.f, g_specMaxHz = 18000.f;

inline float g_specDbFloor = -60.f;

inline bool g_viewMeter = true, g_viewSpectrum = true, g_viewScope = true, g_viewConsole = true;

inline ProcessRunner g_playProc, g_renderProc;

inline CsoundApiRunner g_apiProc;

inline CsoundApiRunner g_offlineProc;

inline CawCodecRunner g_codecProc;

inline TestSound g_testSound;

inline bool g_useCsoundApi = false;

inline uint64_t g_apiSnapshotSeq = 0;

inline ProcessRunner g_testProc;                          // 'Test' button of the code editor: one clip alone, no audio

struct TestRun { std::string name; bool sawError = false; };
inline TestRun g_test;

inline std::vector<OrcSpan> g_orcSpans;                   // where each clip sits in the .csd that is running (for error lines)

inline ProcessRunner g_opcodeProc;                        // 'csound -z' at startup: the opcode list for the editor colors

inline bool g_opcodeScanStarted = false;

inline float g_opcodeScanT = 0.f;

inline bool g_playWaiting = false, g_sectionSeen = false;

inline float g_playWaitT = 0.f;

inline bool g_renderActive = false;

inline std::string g_renderPath;

inline std::string g_renderTemp, g_renderEncodedTemp;

inline bool g_renderConverting = false, g_confirmRenderOverwrite = false;

// The DSP-switch gates (popup asks, user request): offline rendering is blocked while the DSP is
// on, real-time rendering while it is off. The popup offers the switch and continues.
inline bool g_askDspOffExport = false, g_askDspOnExport = false;
inline bool g_askDspOffLock = false; inline int g_askDspOffLockTrack = -1;

inline bool g_renderWillOverwrite = false, g_renderWarnVst = false;

inline bool g_audioExportOpen = false;

inline int g_activeFlacBits = 24, g_activeMp3Kbps = 192;

inline std::string g_exportError;

inline int g_activeRenderFormat = 0;

enum LogLevel { kLogDetail = 0, kLogMessage = 1, kLogImportant = 2, kLogError = 3 };

struct LogLine {
    std::string text; int level = kLogMessage; std::string clip;
    int clipId = -1;                 // the clip the message is about (-1 = none / not found in the project)
    int line = 0;                    // line of the clip's code (as shown in its editor), 0 = unknown
    bool lineApprox = false;         // runtime SuperCollider locations are best-effort; exact parser lines are not
    std::vector<int> lineCandidates; // PD object rows when libpd reports an error but omits the object name
    std::vector<double> times;       // where that clip starts on the timeline (seconds); a shared instrument has several
    double playhead = -1.0;          // playhead when the message came (only while the timeline played), -1 = not playing
};

inline std::vector<LogLine> g_log;

inline bool g_logAuto = true, g_logDirty = false;

inline int g_logDetail = kLogImportant;           // shown: kLogImportant = errors, warnings, what runs | kLogMessage = + all app messages | kLogDetail = + raw engine output

inline std::string g_logClip;                     // empty = all clips

inline char g_logSearch[64] = "";

inline bool g_logProblems = false;                // the console shows only the problems (errors)

inline std::map<int, std::map<int, std::string>> g_diagMarks;   // clip id -> line of the clip code -> error text (red mark in the code editor)

inline std::map<int, int> g_gotoLine;             // clip id -> line the editor puts its cursor on when it is drawn next

inline std::set<int> g_clipWinSeen;   // clip editor windows that are open (to place and focus a newly opened one)

inline int g_focusClipWin = -1;                   // clip editor window that gets the focus

struct ErrCtx { int left = 0; std::string text; int row = -1; std::string rowText; };   // row: index of the error row in g_log that waits for its line

inline ErrCtx g_errCtx;                           // an error line was just printed: the "line N" lines right after it belong to it

inline std::vector<AudioDevice> g_outDevs, g_inDevs;                 // CAW's PortAudio: every driver

inline std::vector<AudioDevice> g_csoundOutDevs, g_csoundInDevs;     // Csound's own numbering (old process engine only)

inline bool g_audioDevicesReady = false;

inline std::vector<float> g_dispDb, g_holdDb, g_holdT;      // master meter state, one entry per output channel (nchnls)

inline int g_engineMaster = 2;                              // master channels the running engine was started with

inline float g_specSm[1024];

inline std::vector<LaneMeterInfo> g_engineMeters;         // meter layout the engine was started with

inline std::vector<LaneMeterInfo> g_idleMeters;           // layout of the current project while Csound is off

inline std::vector<float> g_peakIn(2, 0.f);               // newest peak per meter index (0dbfs-normalized, linear)

inline std::vector<float> g_meterDb;                      // smoothed display value per meter index (dB)

using PdMeterArr = std::array<float,8>;   // one peak per patch channel (up to 8; a stereo patch uses the first two)
inline std::map<int,PdMeterArr> g_pdMeterDb;       // per-track PD output channel peak dB

inline std::map<int,std::array<float,ScEngine::slotStride>> g_scMeterDb;   // SC engine output peak dB per channel, shown on SC track headers

inline std::map<int,PdMeterArr> g_vstMeterDb;      // VST engine output peak dB per plugin output channel (up to 8)

inline uint64_t g_pdSnapshotSeq = 0;

inline std::vector<float> g_meterClipHold;                // per-channel 1.5 second red clip light

inline float g_bandAmp[kMeterBands] = {};

inline float g_rawLeft[monitor_fft::size] = {}, g_rawRight[monitor_fft::size] = {};

inline MonitorFrameAssembler g_rawAssembler;

inline float g_rawFrozenLeft[monitor_fft::size] = {}, g_rawFrozenRight[monitor_fft::size] = {};

inline float g_analyzerFrozenLeft[monitor_fft::size] = {}, g_analyzerFrozenRight[monitor_fft::size] = {};

inline bool g_haveRaw = false;

inline float g_rawAge = 10.f;

inline int g_rawSampleRate = 44100, g_analyzerMode = 0, g_fftChannel = 0;

inline float g_rawFullScale = 1.f;

inline std::vector<float> g_fftDb;

inline std::vector<monitor_bands::Band> g_spectrumBands;

inline std::vector<monitor_bands::Band> g_columnBands;
inline std::vector<float> g_columnHold;   // the static columns' peak-hold caps: jump up instantly, fall slowly
inline std::vector<float> g_columnDisp;   // the static columns' display smoothing: fast attack, slow release
inline std::vector<float> g_fftDbDisp;    // the FFT line's display smoothing: fast attack, release-limited fall

inline std::vector<monitor_peaks::Peak> g_tonePeaks;

inline std::deque<std::vector<float>> g_fftHistory;

inline float g_scopeL[kScopePoints] = {}, g_scopeR[kScopePoints] = {};

inline float g_scopeStagingL[kScopePoints] = {}, g_scopeStagingR[kScopePoints] = {};

inline float g_scopeFrozenL[kScopePoints] = {}, g_scopeFrozenR[kScopePoints] = {};

inline float g_scopeDisplayL[kScopePoints] = {}, g_scopeDisplayR[kScopePoints] = {};

inline bool g_scopeHasFrame = false;

inline bool g_scopeFrozenHasFrame = false;

inline int g_scopeActiveMs = 0;                           // actual engine window, unchanged until restart

inline int g_scopeFrozenMs = 0;

inline float g_monAge = 10.f;                             // seconds since the last monitor line arrived

inline bool g_haveBands = false;

inline bool g_helpOn = true;                 // show / hide the help panel (View menu or the box in the panel)

inline std::string g_helpNext, g_helpShown;  // text of the hovered item (this frame / last frame)

inline const char* kLead = R"csd(; Automate input1 to change the pitch
; the + menu lists input1 as an automation lane (0..1 by default)
kNote cawget "input1"
aEnv linen 1, 0.02, p3, 0.1
aOut oscili 0.2 * aEnv, cpsmidinn(kNote)
out aOut, aOut)csd";

inline constexpr float kFolderHdrH = 24.f;

inline int g_ctxFolder = 0;   // folder of the open folder context menu

inline int g_ctxAutoTrack = -1;        // automation lane the open lane context menu belongs to (persistent: the menu body runs on later frames too)

inline std::string g_ctxAutoChannel;

inline std::set<int> g_collapsedAuto;   // leftover of the removed fold arrow; nothing reads it anymore, old saves just load

inline const char* kVsteClipCode =
    "# the plugin plays what the wired inputs send - pick its parameters in the plugin panel\n"
    "# to drive a picked parameter from the code instead, write e.g.\n"
    "#   P1 = clip(level(in) * 4, 0, 1)\n";

inline const char* kVsteClipCodeOld =   // every VSTe clip made before the fix carries this verbatim
    "# P1 follows the loudness of the sound that comes in\nP1 = clip(level(in) * 4, 0, 1)\n";

inline std::map<int, std::shared_ptr<const vstscript::Program>> g_sampleScripts;

struct PdUiControls {std::vector<std::string> inputs;std::vector<pd_control_syntax::Output> outputs;};

struct RecBuf { int trackId = 0; std::string channel; std::vector<AutoPoint> s; };

inline std::set<std::pair<int, std::string>> g_recArm;   // lanes armed with the R button (track id, channel)

inline std::vector<RecBuf> g_recBufs;                    // dots being recorded while the timeline plays

inline std::vector<CawWriteTarget> g_recNames;

inline ScoreProbeData g_pdScoreData;                     // numeric PD score streams captured during Play

struct WaveUi { int ch = 0; int shape = 0; float rate = 1.f, depth = 50.f, center = 50.f; bool whole = true; double t0 = 0.0, t1 = 4.0; };

inline WaveUi g_waveUi;

struct WavePreview { int frame = -1000; int trackId = -1; std::string channel; std::vector<AutoPoint> pts; };

inline WavePreview g_wavePrev;   // ghost line of the wave that the Wave... box would write

struct WaveHandleDrag {
    bool active = false; int kind = 0;   // 1 depth, 2 time stretch, 3 wave shape
    int trackId = 0; std::string channel; std::vector<int> idx; std::vector<AutoPoint> orig;
    double center = 0.0, lo = 0.0, hi = 1.0; float handleY = 0.f, startY = 0.f;
    // the other selected lanes (one track or several): reshaped by the same handles, each in its own range
    struct Lane { int trackId = 0; std::string channel; std::vector<int> idx; std::vector<AutoPoint> orig; double lo = 0.0, hi = 1.0; };
    std::vector<Lane> others;
    double t0 = 0.0, span = 0.0;   // the common time origin / length of every reshaped lane
};

inline WaveHandleDrag g_wh;

struct WaveHandles { ImVec2 depth, stretch, morph; };

struct StrCtx { std::string* s; ImGuiInputTextCallback chain; };

inline std::map<int, std::string> g_scTimelineCode;

inline bool g_scoreOpen = false;

inline bool g_scoreMapOpen = false;

inline bool g_scoreExportOpen = false;

inline bool g_codeExportOpen = false;

inline int g_scoreExportType = 0, g_scoreExportColor = 0;

inline ScoreSpectrum g_scoreSpec;

inline std::string g_scoreSpecPath, g_scoreSpecMsg;

inline std::filesystem::file_time_type g_scoreSpecTime;

struct MultiCtx { std::string* s; };

inline bool g_mathProbeDone = false;

inline std::string g_mathProbeExe;

inline bool g_mathHasAbs = true, g_mathHasSqrt = true, g_mathHasPow = true;

inline std::string g_mathProbeDetail;

inline bool g_monSeen = false, g_monWarned = false;

inline float g_monRunT = 0.f;

struct MonFrame { char tag = 'M'; int start = 0; std::vector<float> values; };

inline std::deque<std::pair<std::chrono::steady_clock::time_point, MonFrame>> g_monHeld;

inline SOCKET g_monitorSocket = INVALID_SOCKET;

inline bool g_monitorWsa = false;

inline bool g_haveOscRawSeq = false, g_haveOscMeterSeq = false;

inline uint32_t g_oscRawSeq = 0, g_oscMeterSeq = 0;

inline bool g_rtMissingAbs = false, g_rtMissingSqrt = false, g_rtMissingPow = false;

inline std::map<int, std::vector<float>> g_sampleMeterDb;   // sample engine output peak dB per track per output channel

inline std::map<int, int> g_sampleMeterCh;                  // how many meters a sample track shows (from its clips' routing)

inline std::vector<std::string> g_engClips, g_engChans, g_engVstNotes;   // names behind the numbers (fixed when Run is pressed)

inline int g_cmdSerial = 0;                               // number of the last command written

inline int g_cmdShown = 0;                                // number Csound has been told (the serial file)

inline bool g_engineSentShown = false;

inline std::map<int, unsigned long long> g_audioVoiceSig;   // clipId -> the signature the running voice was built from

inline double g_audioLastFrom = -1.0;                       // sampler notes: notes between this and the playhead are triggered

inline int g_audioGainClip = -1, g_audioGainIdx = -1;       // the volume-automation point being dragged (Pen tool)

inline double g_audioClockAnchor = 0.0;

inline double g_sigT = 0.0;               // time since the timeline was last compared with the project

inline unsigned long long g_sig = 0;      // what the running command was made from (timelineSignature)

inline std::set<std::string> g_extraOpcodes;

inline int g_langVersion = 1;

inline std::map<int, std::set<std::string>> g_savedHiddenAuto;   // what was hidden before "hide all"

inline bool g_autoAllHidden = false;

inline std::map<int, bool> g_savedMidiHidden;

inline bool g_midiAllHidden = false;

struct PRect { ImVec2 a, b; };

struct WinMax { bool maxed = false; bool restore = false; ImVec2 pos, size; };

inline std::unordered_map<int, WinMax> g_winMax;

inline int g_tlFollow = 0;   // timeline follow while playing: 0 off, 1 page jump (Cubase), 2 move by timeline (playhead pinned)
inline int g_tlFollowLast = 2;   // the last non-off mode (the F shortcut toggles back to it)

struct UdoPoolEntry {
    std::string name, outputs, inputs, source;
    size_t begin = 0, end = 0;
};

inline std::string g_dropPath;

inline int g_dropX = 0, g_dropY = 0;

inline bool g_dropPending = false;

