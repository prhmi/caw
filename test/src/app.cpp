#include "app.h"
#include "caw_toggles.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
// the few Winsock names used below, mapped to the BSD socket API
using SOCKET = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif
#include "platform.h"
#include "audio_devices.h"
#include "audio_device_match.h"
#include "audio_codec.h"
#include "branding.h"
#include "startup_logo.h"
#include "svg_raster.h"
#include "arrow_widgets.h"
#include "csd_builder.h"
#include "clip_automation_move.h"
#include "clip_timing.h"
#include "code_export.h"
#include "midi_clip_ops.h"
#include "imgui.h"
#include "process_runner.h"
#include "process_quote.h"
#include "csound_api_runner.h"
#include "pd_preview.h"
#include "sc_engine.h"
#include "sc_diagnostics.h"
#include "pd_diagnostics.h"
#include "pd_timeline.h"
#include "pd_daw_engine.h"
#include "pd_test_patch.h"
#include "pd_automation_patch.h"
#include "pd_patch_editor.h"
#include "pd_graph_edit.h"
#include "selected_test_sound.h"
#include "project_io.h"
#include "score_svg.h"
#include "TextEditor.h"
#include "csound_opcodes.h"
#include "marks_gfx.h"
#include "vst_ui.h"
#include "vst_lang.h"
#include "vst_solo.h"
#include "monitor_math.h"
#include "monitor_fft.h"
#include "monitor_bands.h"
#include "monitor_peaks.h"
#include "monitor_frame.h"
#include "monitor_parse.h"
#include "monitor_osc.h"
#include "monitor_work.h"
#include "timeline_zoom.h"
#include "timeline_loop.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cctype>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iomanip>
#include <locale>
#include <limits>
#include <functional>
#include <regex>
#include <sstream>
#include <cstring>
#include <utility>
#include <unordered_map>
#include <memory>

namespace {

// ---------------------------------------------------------------- state
Project g_proj;
// Session-only choices: what this session loads and what the combined Run button starts.
// Unchecked means "do not load / run it" - tracks of every kind can still be added and edited.
bool g_langCsound = true, g_langPd = false, g_langSc = false;
bool g_langVst = true;      // VST3 instruments (VSTi, the VST engine) and effects (VSTe, Csound rack)
bool g_langAudio = false;   // audio tracks (planned: nothing to load yet - kept so the choice round-trips)
bool g_chooseLanguages = true, g_languageChoiceMade = false;
bool g_pdEngineOn = false;
bool g_mixedPd = false; // Csound host owns the one device for both sources
std::string g_pdLibraryPath;
std::string g_scLangPath;
std::string g_scSynthPath;
ScEngine g_scEngine;
std::unique_ptr<CawAudioOutput> g_scSoloOutput;
std::unique_ptr<MonitorCapture> g_scSoloCapture;
uint64_t g_scSoloSnapshotSeq = 0;
// CAW's 4th engine: a dedicated output stream, opened only when no other stream carries audio.
std::unique_ptr<CawAudioOutput> g_vstSoloOutput;
std::unique_ptr<MonitorCapture> g_vstSoloCapture;
uint64_t g_vstSoloSnapshotSeq = 0;
bool g_vstEngineOn = false;            // the "Run VST" button: CAW's own instrument engine is running
bool g_vstHostedInCsound = false;      // true while the Csound engine's rack owns the VST graph (the 4th engine stays silent)
double g_vstRetryT = 0.0;              // seconds until the dedicated output may be retried
std::string g_vstFailedSig;            // the instrument set that failed to load; cleared by a re-pick / a new set
double g_vstFailedRetryT = 0.0;        // seconds until a failed instrument load is tried again
double g_vstJobsT = 0.0;               // 0.5 s timers: the job diff and the clip-code mode check
std::mutex g_vstLogMutex;              // the engine logs from the audio thread: queue, drain on the UI thread
std::vector<std::string> g_vstLogQueue;
bool g_scEngineOn = false, g_scReady = false;
int g_scPendingClipId = 0;
int g_scNextInstanceId = 1;
struct ScActiveInstance { int clipId = 0; std::chrono::steady_clock::time_point until; };
std::map<int, ScActiveInstance> g_scActiveInstances;
std::map<int, std::string> g_scClipFileCache;
int g_scErrorClipId = -1, g_scErrorContextLines = 0, g_scErrorLogIndex = -1;
int g_scOutputClipId = 0, g_scOutputInstanceId = 0;
bool g_scErrorHeadlineSeen = false;
std::string g_scErrorMessage;
unsigned long long g_scTimelineSig = 0;
double g_scPlanT = 0.0;
double g_scLastPDTimelineTime = -1.0;
std::chrono::steady_clock::time_point g_scStartTime{};
CawScRing g_scRing;                        // the SuperCollider mix, captured for VSTe "SuperCollider" inputs
std::atomic<bool> g_scRawSuppress{false};  // true while an active master-mode VSTe replaces the SC mix
void scRealtimeSource(void* context, float* output, unsigned long frames, int channels) noexcept {
    if (context) static_cast<ScEngine*>(context)->process(output, frames, channels);
    if (output && frames > 0 && channels >= 2) {
        g_scRing.push(output, frames);            // capture for the VST rack (engine thread pops it)
        if (g_scRawSuppress.load(std::memory_order_acquire))
            std::fill(output, output + (size_t)frames * (size_t)channels, 0.f);   // a master-mode VSTe replaces the raw mix
    }
    if (!g_vstHostedInCsound)
        vstSolo().renderMix(output, (int)frames, channels);   // the 4th engine: only while it owns the VST graph
}
void attachScRealtimeSource(CawAudioSettings& settings) {
    settings.realtimeSource = &scRealtimeSource;
    settings.realtimeSourceContext = &g_scEngine;
}
// Buffer by resolved patch path so edits survive closing a clip window or
// switching projects; two clips pointing to the same file share one buffer.
std::map<std::string, pd_patch_editor::Buffer> g_pdPatchBuffers;
bool g_askPdOnClose = false;
PdPreview g_pdPreview;
PdDawEngine g_pdTimelineEngine;
CawHub g_cawHub;                         // shared caws/cawr message bus (see collectCawHubNames / pollCawHub)
bool g_pdTimelineMode = false;
bool g_pdResumeAfterReload = false;   // the PD engine is being rebuilt: rejoin the running transport when it is ready
int g_pdPlayingClip = 0;
double g_pdPreviousTime = -1.0;
unsigned long long g_pdTransportEpoch = 0;
std::vector<pd_timeline::Event> g_pdTimelineEvents;
pd_timeline::Event g_pdPlayingEvent{};   // the clip that sounds now (to notice a changed patch)
double g_pdPlanT = 0.0;                  // time since the PD clips were last compared with the project
std::string g_playNotice;                // visible in the transport bar for a few seconds (the console log is easy to miss)
double g_playNoticeT = 0.0;
std::string g_pdLastPlanError;           // so a problem is logged once, not four times a second
bool g_pdNeedsReload = false;
bool g_pdPlanUpdateFailed = false;
double g_play = 0.0;          // playhead (seconds)
std::map<std::string, float> g_scLaneSent;   // lane values already delivered to ~cawr (SuperCollider)
bool g_playing = false;
bool g_liveCsound = false;
float g_pps = 80.f;           // pixels per second (zoom)
// A button that shows a state (on / off): the hover and the pressed colours equal the button colour, so hovering never hides the state.
void pushBtnOn(ImVec4 c) {
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
}
const float kPpsMin = 2.f, kPpsMax = 2000.f;   // zoom range: whole piece on one screen ... single milliseconds
float g_scrollX = 0.f;        // seconds
float g_scrollY = 0.f;        // pixels
int g_selTrack = -1, g_selClip = -1;
int g_nextTrackId = 1;
std::string g_projectPath;
std::string g_savedSnap;               // the project as it was when it was last saved / opened (to know if there are unsaved changes)
std::string g_autoSavedSnap, g_unsavedAutosavePath;
float g_autoSaveElapsed = 0.f;
bool g_askSaveOnClose = false;
bool g_confirmDeleteSelected = false;
ImFont* g_codeFonts[3] = {nullptr, nullptr, nullptr};
ImFont* g_markUiFont = nullptr;
ImFont* g_markCodeFont = nullptr;
ImVec2 g_logoUv0, g_logoUv1;      // fallback: old 512 px bitmap logo (startup_logo.h)
bool g_logoReady = false;
// Vector logo (res/logo.svg) drawn once per size it is shown at, so it is never scaled: About, welcome window, splash.
struct LogoVariant { int px = 0; ImVec2 uv0, uv1; };
constexpr int kLogoVariantCount = 3;
constexpr float kLogoSizes[kLogoVariantCount] = {112.f, 220.f, 240.f};   // before DPI scaling
LogoVariant g_logoVariants[kLogoVariantCount];
int g_logoVariantsReady = 0;
std::unordered_map<int, std::unique_ptr<TextEditor>> g_codeEditors;
int g_autoDragTrackId = -1;
int g_autoDragPoint = -1;
std::string g_autoDragChannel;
enum class Drag { None, Move, Resize, Playhead, ResizeTrack, ResizeAutomation, Marquee, ResizeHeader, ScrollX, ScrollY, ResizeLeft };
Drag g_drag = Drag::None;
double g_dragAnchorT = 0.0;                       // timeline position under the mouse when a clip drag started
std::vector<std::pair<int, double>> g_dragOrig;   // (clip id, start) of every dragged clip
ClipAutomationMove g_clipAutomationMove;
std::set<int> g_selMidiClips;                      // ids of the selected MIDI clips (MIDI lane)
int g_mcDragId = -1;                               // MIDI clip being dragged in the MIDI lane
int g_mcDragMode = 0;                              // 1 move, 2 resize right edge, 3 resize left edge
double g_mcAnchorT = 0.0, g_mcOrigStart = 0.0, g_mcOrigEnd = 0.0;
std::vector<std::pair<int, double>> g_mcOrig;      // (MIDI clip id, start) of everything moved together
std::vector<MidiNote> g_mcOrigNotes;
int g_ctxMidiId = 0;                               // MIDI clip of the right-click menu
std::set<int> g_selClips;                         // ids of all selected clips (g_selTrack/g_selClip = primary one)
ImVec2 g_clipMarqueeStart;
bool g_clipMarqueeMoved = false, g_clipMarqueeAdd = false;
float g_headerW = 370.f;                          // width of the left lane panel (unscaled px), drag its edge to resize
bool g_timelineRun = false;                       // Play: the timeline moves and triggers the instruments of the running Csound
bool g_loopSelecting = false;
double g_loopAnchor = 0.0;
double g_hold = 0.0;                               // after Csound has started, the playhead waits one sound-card buffer so it meets the sound
int g_resync = 0;                                 // not 0 = the timeline has to be (re)started from the playhead (Play pressed, playhead moved)
std::string g_runName;                            // clip started with "Run clip" in the running engine
double g_runLeft = 0.0;                           // seconds until that clip ends by itself
struct LiveCodeEdit { double at = 0.0; bool pending = false; };
std::map<int, LiveCodeEdit> g_liveCodeEdits;      // clip id -> last code edit (recompiled after a short pause)
float g_midiPps = 120.f, g_midiScrollX = 0.f;     // MIDI editor zoom/scroll, independent from the project timeline
struct MidiOrig { int idx; double start; int pitch; };
std::set<int> g_midiSel;                          // selected note indices (clip = g_midiSelectedTrackId)
std::vector<MidiOrig> g_midiMoveOrig;
double g_midiEmptyPressT = -10.0;   // time of the last press on an empty cell of the piano roll (double-click adds a note)
ImVec2 g_midiEmptyPressPos(0.f, 0.f);
bool g_midiMarqueeDbl = false;
bool g_midiMarquee = false, g_midiMarqueeMoved = false, g_midiMarqueeAdd = false;
int g_midiMarqueeClip = -1;
ImVec2 g_midiMarqueeStart;
float g_sbGrab = 0.f;                             // where inside a scrollbar thumb the mouse grabbed it
int g_resizeTrack = -1;
std::string g_resizeAutomationChannel;
float g_resizeStartY = 0.f, g_resizeStartH = 0.f;
int g_midiDragTrackId = -1, g_midiDragNote = -1;
int g_midiSelectedTrackId = -1, g_midiSelectedNote = -1;
int g_midiDragMode = 0; // 1 move, 2 resize
int g_midiDragPitchOffset = 0;
double g_midiDragTimeOffset = 0.0;
int g_trackCounter = 0;
std::vector<int> g_multiMidiIds;       // MIDI clips shown together in ONE piano roll (select them, press Enter)
bool g_multiMidiOpen = false;
inline bool hasClips(const Track& t) { return t.kind == 0 || t.kind == 3 || t.kind == 4 || t.kind == 5 || t.kind == 6; }
int g_ctxTrack = -1;                   // track of the right-click menu in the left panel (-1 = empty panel space)
int g_markTrack = -1, g_markIdx = -1;  // marker being edited
int g_markDrag = -1, g_markDragTrack = -1, g_markDragMode = 0;   // marker being dragged (1 move, 2 resize)
double g_markGrab = 0.0;
int g_nextClipId = 1;
std::unordered_map<int, std::unique_ptr<TextEditor>> g_clipCodeEditors;  // one code editor per clip instrument
bool g_quitRequested = false;             // Ctrl+Q / File > Exit
bool g_codeTyping = false;               // the text cursor is inside a code editor text area (Space types a space)
bool g_codeClickHit = false;
bool g_textEditorFocused = false;     // a code editor window had focus last frame (its own Ctrl+Z wins)
int g_ctxClipId = -1;                 // clip shown in the right-click clip menu

// automation editing
enum class Tool { Select, Pen, Cut, Join, Pen2 };   // one tool at a time: 1 Select, 8 Pen 1 (simplified), 9 Pen 2 (every dot kept), 3 Cut, 4 Join
Tool g_tool = Tool::Select;
bool isPenTool() { return g_tool == Tool::Pen || g_tool == Tool::Pen2; }
enum class AutoMode { None, Move, Marquee, Pen };
double g_trimStart = 0.0, g_trimEnd = 0.0;       // clip edges when a left-edge trim started
std::vector<MidiNote> g_trimNotes;               // the clip's notes when the trim started
AutoMode g_autoMode = AutoMode::None;
int g_autoSelTrackId = -1;            // lane that owns the point selection
std::string g_autoSelChannel;
double g_autoLastPressT = -10.0;      // own double-click detection for the automation lanes (does not depend on the ImGui double-click)
ImVec2 g_autoLastPressPos(0.f, 0.f);
int g_autoLastTrackId = -1;
std::string g_autoLastChannel;
bool g_autoLastAddedValid = false;    // the first click of a double-click dropped a dot (kept here so the double-click can move it)
AutoPoint g_autoLastAdded{0.0, 0.0};
double g_autoPenMinT = 0.0, g_autoPenMaxT = 0.0;   // time span of the pen stroke being drawn (simplified to few dots on release)
bool g_autoPressAdded = false;        // a plain click on an empty lane already dropped its dot on mouse-down (no wait for release)
AutoPoint g_autoPressPoint{0.0, 0.0};
std::set<int> g_autoSel;              // selected point indices in that lane
std::map<std::pair<int, std::string>, std::set<int>> g_autoSelMulti;   // dots picked by a box over several lanes: (track id, channel) -> indices
std::vector<AutoPoint> g_autoDragOrig;
ImVec2 g_autoDragStartMouse;
double g_autoDragStartT = 0.0, g_autoDragStartV = 0.0;
bool g_autoDragMoved = false, g_autoMarqueeAdd = false;
double g_autoPenLastT = 0.0;
float g_autoPenLastX = 0.f;
bool g_autoOpenPointPopup = false;

// box-select of MIDI regions (drag on empty space in a MIDI lane, like the Csound clips)
// One box-select for everything: Csound clips and MIDI regions. A double-click that turns into a drag box-selects;
// a double-click without a drag still creates a clip (kind 1 = Csound clip, 2 = MIDI region).
int g_marqDblKind = 0, g_marqLane = -1;
double g_marqT = 0.0;
std::vector<std::pair<int, double>> g_dragMidiOrig;   // MIDI regions that move together with dragged Csound clips

// undo / redo: whole-project snapshots, committed when an edit is finished
std::vector<std::string> g_undoStack, g_redoStack;
std::string g_histCommitted, g_histPending;
float g_histIdle = 0.f;
const size_t kHistoryLimit = 300;
float g_ui = 1.f;             // DPI scale
const float kDefaultTrackH = 76.f;
const float kFixedTrackH = 58.f;   // lanes have a fixed height: name + short description fit exactly
const float kMinTrackH = 44.f, kMaxTrackH = 320.f;
const float kAutomationLaneH = 42.f;
const float kMidiLaneH = 52.f;
const int kMidiPitchLow = 48, kMidiPitchHigh = 83;
int g_midiPitchLow = kMidiPitchLow;
const int kMidiVisiblePitches = 36;
const float kMidiRowH = 16.f;

bool g_showMonitor = true;
bool g_settingsOpen = false;
bool g_aboutOpen = false;   // Help > About
bool g_welcomeOpen = false; // language selector replaces the old startup welcome
int g_settingsGoto = -1;   // menu: which Settings tab to show next (0 Audio, 1 Programs, 2 Render, 3 Editor, 4 Advanced, 5 Files); -1 = none
bool g_audioPoolOpen = false;
bool g_udoPoolOpen = false;
bool g_instrPoolOpen = false;
bool g_includePoolOpen = false;
bool g_orchestraOpen = false;
int g_audioPoolSelected = -1;
int g_includeSelected = -1;
float g_audioSidebarWidth = 240.f;
float g_udoSidebarWidth = 240.f;
float g_includeSidebarWidth = 240.f;
int g_udoPoolSelected = -1;
int g_instrPoolSelected = -1;
float g_instrSidebarWidth = 240.f;

float g_monitorH = 210.f;
const float kMonitorMinH = 90.f, kMonitorMaxH = 520.f, kMonitorSplitterH = 12.f;
// monitor panels: display order and width (unscaled px, 0 = automatic) of every panel; drag a title to reorder, drag between panels to resize
enum { kPanelSpectrum = 0, kPanelScope = 1, kPanelConsole = 2, kPanelCount = 3 };
int g_panelOrder[kPanelCount] = {kPanelSpectrum, kPanelScope, kPanelConsole};
float g_panelW[kPanelCount] = {0.f, 0.f, 0.f};
float g_meterW = 0.f;   // width of the meter panel (unscaled px), 0 = from the number of channels; drag its left edge
int g_specDetail = 1;    // spectrum drawing: 0 coarse, 1 normal, 2 fine (right-click the spectrum)
int g_specFall = 1;      // spectrum fall speed: 0 fast, 1 medium, 2 slow
int g_scopeSmooth = 1;   // scope interpolation: 0 linear, 1 cubic, 2 high-resolution cubic
int g_scopeDisplay = 1;  // 0 free-running sweep, 1 rising-zero-crossing triggered frame
int g_scopeMs = 6;       // scope time window, in milliseconds
int g_scopeChannels = 0; // 0 stereo, 1 left, 2 right, 3 mid
int g_scopeSample = 0;   // 0 point sample, 1 signed peak over each display bucket
bool g_scopeFreeze = false;
bool g_scopeGrid = true;
bool g_specFreeze = false;
int g_specDisplay = 0;   // 0 live response, 1 discrete updates
float g_specFrameClock = 0.f;
int g_specScale = 0;     // 0 logarithmic, 1 linear
int g_specInterpolation = 1; // 0 linear, 1 cubic
int g_specStyle = 0;     // tonal peaks by default
bool g_specGrid = true;
bool g_specFast = true;    // analyse only the newest 512 samples: faster response, coarser frequency resolution
float g_specMinHz = 25.f, g_specMaxHz = 18000.f;
float g_specDbFloor = -60.f;
// Control periods per scope point for the chosen time window, and the window that really results (whole control periods only).
int scopeDiv() {
    const double period = (double)std::max(1, g_proj.cs.ksmps) / (double)std::max(1, g_proj.cs.sr);
    return std::max(1, (int)std::lround(g_scopeMs / 1000.0 / (kScopePoints * period)));
}
int scopeWindowMs() {
    return (int)std::lround(scopeDiv() * kScopePoints * (double)std::max(1, g_proj.cs.ksmps) / (double)std::max(1, g_proj.cs.sr) * 1000.0);
}



bool g_viewMeter = true, g_viewSpectrum = true, g_viewScope = true, g_viewConsole = true;

ProcessRunner g_playProc, g_renderProc;
CsoundApiRunner g_apiProc;
CsoundApiRunner g_offlineProc;
CawCodecRunner g_codecProc;
TestSound g_testSound;
bool g_useCsoundApi = false;
uint64_t g_apiSnapshotSeq = 0;
ProcessRunner g_testProc;                          // 'Test' button of the code editor: one clip alone, no audio
struct TestRun { std::string name; bool sawError = false; } g_test;
std::string clipLineMessage(const std::string& l);
void logCsoundLine(const std::string& raw);
void startEngine();
void pdTransport(bool play);
void stopScEngine();
void startRender();
void cancelRender();
bool replaceOutputFile(const std::string& temporary, const std::string& final, std::string& error);
std::vector<OrcSpan> g_orcSpans;                   // where each clip sits in the .csd that is running (for error lines)
ProcessRunner g_opcodeProc;                        // 'csound -z' at startup: the opcode list for the editor colors
bool g_opcodeScanStarted = false;
float g_opcodeScanT = 0.f;
bool g_playWaiting = false, g_sectionSeen = false;
float g_playWaitT = 0.f;
bool g_renderActive = false;
std::string g_renderPath;
std::string g_renderTemp, g_renderEncodedTemp;
bool g_renderConverting = false, g_confirmRenderOverwrite = false;
bool g_renderWillOverwrite = false, g_renderWarnVst = false;
bool g_audioExportOpen = false;
int g_activeFlacBits = 24, g_activeMp3Kbps = 192;
std::string g_exportError;
int g_activeRenderFormat = 0;
// Console: every line knows its importance, its language and (when the message names one) its clip.
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
std::vector<LogLine> g_log;
bool g_logAuto = true, g_logDirty = false;
int g_logDetail = kLogImportant;           // shown: kLogImportant = errors, warnings, what runs | kLogMessage = + all app messages | kLogDetail = + raw engine output
std::string g_logClip;                     // empty = all clips
char g_logSearch[64] = "";
bool g_logProblems = false;                // the console shows only the problems (errors)
std::map<int, std::map<int, std::string>> g_diagMarks;   // clip id -> line of the clip code -> error text (red mark in the code editor)
std::map<int, int> g_gotoLine;             // clip id -> line the editor puts its cursor on when it is drawn next
std::set<int> g_clipWinSeen;   // clip editor windows that are open (to place and focus a newly opened one)
int g_focusClipWin = -1;                   // clip editor window that gets the focus
struct ErrCtx { int left = 0; std::string text; int row = -1; std::string rowText; };   // row: index of the error row in g_log that waits for its line
ErrCtx g_errCtx;                           // an error line was just printed: the "line N" lines right after it belong to it

std::vector<AudioDevice> g_outDevs, g_inDevs;                 // CAW's PortAudio: every driver
std::vector<AudioDevice> g_csoundOutDevs, g_csoundInDevs;     // Csound's own numbering (old process engine only)
bool g_audioDevicesReady = false;

std::vector<float> g_dispDb, g_holdDb, g_holdT;      // master meter state, one entry per output channel (nchnls)
int g_engineMaster = 2;                              // master channels the running engine was started with
float g_specSm[1024];
// ---- data printed by the running Csound engine (CawMon instrument): master + lane meters, scope, spectrum bands
std::vector<LaneMeterInfo> g_engineMeters;         // meter layout the engine was started with
std::vector<LaneMeterInfo> g_idleMeters;           // layout of the current project while Csound is off
std::vector<float> g_peakIn(2, 0.f);               // newest peak per meter index (0dbfs-normalized, linear)
std::vector<float> g_meterDb;                      // smoothed display value per meter index (dB)
std::map<int,std::array<float,2>> g_pdMeterDb;       // per-track PD output channel peak dB
std::map<int,std::array<float,2>> g_scMeterDb;       // SC engine output peak dB shown on SC track headers
std::map<int,std::array<float,2>> g_vstMeterDb;      // VST engine output peak dB shown on VSTi track headers
uint64_t g_pdSnapshotSeq = 0;
std::vector<float> g_meterClipHold;                // per-channel 1.5 second red clip light
float g_bandAmp[kMeterBands] = {};
float g_rawLeft[monitor_fft::size] = {}, g_rawRight[monitor_fft::size] = {};
MonitorFrameAssembler g_rawAssembler;
float g_rawFrozenLeft[monitor_fft::size] = {}, g_rawFrozenRight[monitor_fft::size] = {};
float g_analyzerFrozenLeft[monitor_fft::size] = {}, g_analyzerFrozenRight[monitor_fft::size] = {};
bool g_haveRaw = false;
float g_rawAge = 10.f;
int g_rawSampleRate = 44100, g_analyzerMode = 0, g_fftChannel = 0;
float g_rawFullScale = 1.f;
std::vector<float> g_fftDb;
std::vector<monitor_bands::Band> g_spectrumBands;
std::vector<monitor_bands::Band> g_columnBands;
std::vector<monitor_peaks::Peak> g_tonePeaks;
std::deque<std::vector<float>> g_fftHistory;
float g_scopeL[kScopePoints] = {}, g_scopeR[kScopePoints] = {};
float g_scopeStagingL[kScopePoints] = {}, g_scopeStagingR[kScopePoints] = {};
float g_scopeFrozenL[kScopePoints] = {}, g_scopeFrozenR[kScopePoints] = {};
float g_scopeDisplayL[kScopePoints] = {}, g_scopeDisplayR[kScopePoints] = {};
bool g_scopeHasFrame = false;
bool g_scopeFrozenHasFrame = false;
int g_scopeActiveMs = 0;                           // actual engine window, unchanged until restart
int g_scopeFrozenMs = 0;
float g_monAge = 10.f;                             // seconds since the last monitor line arrived
bool g_haveBands = false;


float S(float v) { return v * g_ui; }

// ---- help panel: what used to be pop-up tooltips is shown in a panel at the bottom of the program
bool g_helpOn = true;                 // show / hide the help panel (View menu or the box in the panel)
std::string g_helpNext, g_helpShown;  // text of the hovered item (this frame / last frame)
void helpTip(const char* fmt, ...) {
    if (!g_helpOn || !fmt) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_helpNext = buf;
}


const char* kLead = R"csd(; Automate input1 to change the pitch
; the + menu lists input1 as an automation lane (0..1 by default)
kNote cawget "input1"
aEnv linen 1, 0.02, p3, 0.1
aOut oscili 0.2 * aEnv, cpsmidinn(kNote)
out aOut, aOut)csd";

uint32_t defaultTrackRgb(int index) {
    static const uint32_t palette[] = {
        0xEC783C, 0x3CAAC8, 0x966EDC, 0x6EBE5A, 0xDC5A82, 0xD2B43C,
        0x4D7BE8, 0x2BA88D, 0xC653CE, 0xD16E42, 0x75AAE0, 0xA0A650
    };
    if (index >= 0 && index < (int)(sizeof(palette) / sizeof(palette[0])))
        return palette[index];

    // Keep later tracks visually distinct too, instead of cycling back to a clone color.
    const float hue = std::fmod(std::max(0, index) * 137.507764f, 360.f) / 60.f;
    const float chroma = 0.70f;
    const float x = chroma * (1.f - std::fabs(std::fmod(hue, 2.f) - 1.f));
    const float m = 0.17f;
    float r = 0.f, g = 0.f, b = 0.f;
    if (hue < 1.f) { r = chroma; g = x; }
    else if (hue < 2.f) { r = x; g = chroma; }
    else if (hue < 3.f) { g = chroma; b = x; }
    else if (hue < 4.f) { g = x; b = chroma; }
    else if (hue < 5.f) { r = x; b = chroma; }
    else { r = chroma; b = x; }
    return ((uint32_t)std::lround((r + m) * 255.f) << 16) |
           ((uint32_t)std::lround((g + m) * 255.f) << 8) |
           (uint32_t)std::lround((b + m) * 255.f);
}

ImU32 trackColor(int i, int alpha = 255, uint32_t custom = 0) {
    if (custom != 0)
        return IM_COL32((custom >> 16) & 0xFF, (custom >> 8) & 0xFF, custom & 0xFF, alpha);
    const uint32_t rgb = defaultTrackRgb(i);
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

float automationHeight(const Track& track, const std::string& channel) {
    const auto it = track.automationHeights.find(channel);
    const float h = it == track.automationHeights.end() ? kAutomationLaneH : it->second;
    return track.kind == 2 ? std::max(h, 68.f) : h;   // a graph line has a second row: shape and lane
}

// ---- folder groups of the track panel: the tracks of a folder sit next to each other; a collapsed folder hides them (the sound is not affected)
constexpr float kFolderHdrH = 24.f;
int g_ctxFolder = 0;   // folder of the open folder context menu
int g_ctxAutoTrack = -1;        // automation lane the open lane context menu belongs to (persistent: the menu body runs on later frames too)
std::string g_ctxAutoChannel;
TrackFolder* findFolder(int id) {
    if (id == 0) return nullptr;
    for (TrackFolder& f : g_proj.folders) if (f.id == id) return &f;
    return nullptr;
}
bool trackHiddenByFolder(const Track& t) { const TrackFolder* f = findFolder(t.folderId); return f && f->collapsed; }
void pruneFolders() {   // folders without tracks disappear
    for (size_t k = g_proj.folders.size(); k-- > 0;) {
        bool used = false;
        for (const Track& t : g_proj.tracks) if (t.folderId == g_proj.folders[k].id) used = true;
        if (!used) g_proj.folders.erase(g_proj.folders.begin() + (long)k);
    }
}
int folderMemberCount(int id) { int n = 0; for (const Track& t : g_proj.tracks) if (t.folderId == id) ++n; return n; }
// Put track `idx` into folder `folderId` (0 = out of any folder) and move it next to the other tracks of that folder.
void setTrackFolder(int idx, int folderId) {
    if (idx < 0 || idx >= (int)g_proj.tracks.size()) return;
    int selectedId = -1;
    if (g_selTrack >= 0 && g_selTrack < (int)g_proj.tracks.size()) selectedId = g_proj.tracks[(size_t)g_selTrack].id;
    const int oldFolder = g_proj.tracks[(size_t)idx].folderId;
    Track moved = std::move(g_proj.tracks[(size_t)idx]);
    g_proj.tracks.erase(g_proj.tracks.begin() + idx);
    moved.folderId = folderId;
    int at = std::min(idx, (int)g_proj.tracks.size());
    const int anchor = folderId != 0 ? folderId : oldFolder;
    if (anchor != 0) {   // right after the last remaining track of that folder
        for (int k = 0; k < (int)g_proj.tracks.size(); ++k) if (g_proj.tracks[(size_t)k].folderId == anchor) at = k + 1;
    }
    g_proj.tracks.insert(g_proj.tracks.begin() + at, std::move(moved));
    for (int k = 0; k < (int)g_proj.tracks.size(); ++k) if (g_proj.tracks[(size_t)k].id == selectedId) g_selTrack = k;
    pruneFolders();
}
int newFolderFor(int idx) {
    if (idx < 0 || idx >= (int)g_proj.tracks.size()) return 0;
    int id = 1;
    for (const TrackFolder& f : g_proj.folders) id = std::max(id, f.id + 1);
    TrackFolder f;
    f.id = id;
    f.name = "Folder " + std::to_string(id);
    g_proj.folders.push_back(f);
    g_proj.tracks[(size_t)idx].folderId = id;
    pruneFolders();
    return id;
}

std::set<int> g_collapsedAuto;   // leftover of the removed fold arrow; nothing reads it anymore, old saves just load
bool autoShown(const Track& track, const std::string& channel) { return track.hiddenAutomation.count(channel) == 0 && !trackHiddenByFolder(track); }
// Keep lanes visible while they contain MIDI clips, even if the instrument toggle was switched off.
bool trackHasMidi(const Track& track) {
    if (!track.midiClips.empty()) return true;
    for (const Clip& c : track.clips) if (parseClip(c).midiEnabled) return true;
    return false;
}
bool midiShown(const Track& track) { return !track.midiHidden && !trackHiddenByFolder(track) && trackHasMidi(track); }

float automationHeightSum(const Track& track) {
    float total = 0.f;
    for (const auto& [channel, points] : track.automation) if (autoShown(track, channel)) total += automationHeight(track, channel);
    return total;
}

float automationLaneTop(const Track& track, float baseY, const std::string& channel) {
    float y = baseY;
    for (const auto& [name, points] : track.automation) {
        if (name == channel) break;
        if (autoShown(track, name)) y += S(automationHeight(track, name));
    }
    return y;
}

float midiLaneTop(const Track& track, float baseY) {
    return baseY + S(automationHeightSum(track));
}

uint32_t automationColor(const Track& track, const std::string& channel, int laneIndex) {
    const auto found = track.automationColors.find(channel);
    if (found != track.automationColors.end() && found->second != 0) return found->second;
    static const uint32_t defaults[] = {0x01F2A65Au, 0x015BC0DEu, 0x01B18BF0u, 0x0177C77Du, 0x01E66B87u, 0x01D6C855u};
    return defaults[(size_t)std::max(0, laneIndex) % (sizeof(defaults) / sizeof(defaults[0]))];
}

void fixName(std::string& n);

void ensureUniqueInstrumentName(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)g_proj.tracks.size()) return;
    Track& track = g_proj.tracks[(size_t)trackIndex];
    fixName(track.name);
    const std::string original = track.name;
    int suffix = 2;
    auto duplicate = [&]() {
        for (int i = 0; i < (int)g_proj.tracks.size(); ++i)
            if (i != trackIndex && g_proj.tracks[(size_t)i].name == track.name) return true;
        return false;
    };
    while (duplicate()) track.name = original + "_" + std::to_string(suffix++);
}

double clampTimelineTime(double t) {
    return std::max(0.0, t); // free timing, no hidden snapping
}

uint32_t storedRgb(uint32_t rgb) { return 0x01000000u | (rgb & 0x00FFFFFFu); }

uint32_t clipColorValue(const Track& lane, const Clip& clip) {
    return clip.color ? clip.color : lane.color;
}

// Short explanation shown on a clip: the Info text, else the first comment line of its code.
std::string clipDescription(const Clip& c) {
    if (!c.note.empty()) return c.note;
    const std::string& code = c.code;
    size_t pos = 0;
    while (pos < code.size()) {
        size_t e = code.find('\n', pos);
        if (e == std::string::npos) e = code.size();
        std::string line = code.substr(pos, e - pos);
        pos = e + 1;
        const size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        size_t skip = 0;
        if (line[0] == ';') skip = 1;
        else if (line.compare(0, 2, "//") == 0) skip = 2;
        else continue;
        while (skip < line.size() && (line[skip] == ';' || line[skip] == '/' || line[skip] == ' ' || line[skip] == '\t')) ++skip;
        std::string text = line.substr(skip);
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
        if (!text.empty()) return text;
    }
    return std::string();
}

bool isReservedInstrName(const std::string& n) {
    return n == "CawTail" || n.rfind("CawAuto", 0) == 0;
}

// Csound instrument names must be unique across the whole project: every clip is its own instr.
bool clipNameUsed(const std::string& n, const Clip* except) {
    if (isReservedInstrName(n)) return true;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips)
            if (&c != except && c.name == n) return true;
    return false;
}

// Clips with the same name AND the same code are one shared instrument (duplicates).
// A name only conflicts when a clip with a different instrument already uses it.
bool clipNameConflict(const std::string& n, const Clip* self) {
    if (isReservedInstrName(n)) return true;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips)
            if (&c != self && c.name == n && c.code != self->code) return true;
    return false;
}

// Rename every clip that shares `clip`'s instrument (they carried `oldName` and have the same code).
void renameClipGroup(const Clip& clip, const std::string& oldName) {
    if (oldName == clip.name) return;
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips)
            if (&c != &clip && c.name == oldName && c.code == clip.code) c.name = clip.name;
}

void ensureUniqueClipName(Clip& clip) {
    const std::string old = clip.name;
    fixName(clip.name);
    if (clipNameConflict(clip.name, &clip)) {
        std::string base = clip.name;
        while (!base.empty() && base.back() >= '0' && base.back() <= '9') base.pop_back();
        if (base.empty()) base = "Clip";
        for (int k = 2;; ++k) {
            clip.name = base + std::to_string(k);
            if (!clipNameConflict(clip.name, &clip)) break;
        }
    }
    renameClipGroup(clip, old);
}

// Editing one clip's code updates every clip that shares its instrument.
void syncSharedCode(const Clip& src) {
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips)
            if (&c != &src && c.name == src.name) {
                c.code = src.code; c.lang = src.lang;
                c.midiEnabled = src.midiEnabled; c.pFieldRules = src.pFieldRules;
            }
}

std::string nextClipName(const std::string& laneName) {
    std::string base = laneName;
    while (!base.empty() && base.back() >= '0' && base.back() <= '9') base.pop_back();
    fixName(base);
    for (int k = 1;; ++k) {
        const std::string candidate = base + std::to_string(k);
        if (!clipNameUsed(candidate, nullptr)) return candidate;
    }
}

Clip makeClip(const Track& lane, double start, double dur, const std::string& code) {
    Clip c;
    c.start = clampTimelineTime(start);
    c.dur = std::max(0.001, dur);
    c.id = g_nextClipId++;
    c.name = nextClipName(lane.name);
    c.code = code;
    c.midiEnabled = lane.templateMidiEnabled;
    c.pFieldRules = lane.templatePFieldRules;
    c.lang = 0;
    c.color = storedRgb(defaultTrackRgb(c.id - 1));
    return c;
}

Clip* findClip(int clipId, int* laneIndex = nullptr) {
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti)
        for (Clip& c : g_proj.tracks[ti].clips)
            if (c.id == clipId) { if (laneIndex) *laneIndex = (int)ti; return &c; }
    return nullptr;
}

MidiClip* findMidiClip(int id, int* laneIndex = nullptr) {
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti)
        for (MidiClip& c : g_proj.tracks[ti].midiClips)
            if (c.id == id) { if (laneIndex) *laneIndex = (int)ti; return &c; }
    return nullptr;
}

int newMidiId() {
    int top = 0;
    for (const Track& t : g_proj.tracks) for (const MidiClip& c : t.midiClips) top = std::max(top, c.id);
    return top + 1;
}

// Drop the MIDI clips that played a Csound clip which no longer exists.
void removeMidiClipsOf(int clipId) {
    for (Track& t : g_proj.tracks)
        t.midiClips.erase(std::remove_if(t.midiClips.begin(), t.midiClips.end(),
                                         [clipId](const MidiClip& c) { return c.clipId == clipId; }), t.midiClips.end());
}

// The MIDI editor of a Csound clip: its first MIDI clip, or a new one placed under the Csound clip.
void openMidiEditorFor(int laneIndex, const Clip& clip) {
    if (laneIndex < 0 || laneIndex >= (int)g_proj.tracks.size()) return;
    Track& lane = g_proj.tracks[(size_t)laneIndex];
    for (MidiClip& mc : lane.midiClips) if (mc.clipId == clip.id) { mc.editorOpen = true; return; }
    MidiClip mc;
    mc.id = newMidiId();
    mc.clipId = clip.id;
    mc.start = clip.start;
    mc.dur = clip.dur;
    mc.editorOpen = true;
    lane.midiClips.push_back(mc);
}

// Enabling MIDI creates one region for this clip if it does not already have one.
void ensureMidiClipFor(int laneIndex, const Clip& clip) {
    if (laneIndex < 0 || laneIndex >= (int)g_proj.tracks.size()) return;
    if (midi_clip_ops::ensureForClip(g_proj, (size_t)laneIndex, clip.id))
        g_collapsedAuto.erase(g_proj.tracks[(size_t)laneIndex].id);
}

Clip* selectedClip(int* laneIndex = nullptr) {
    if (g_selTrack < 0 || g_selTrack >= (int)g_proj.tracks.size()) return nullptr;
    Track& t = g_proj.tracks[(size_t)g_selTrack];
    if (g_selClip < 0 || g_selClip >= (int)t.clips.size()) return nullptr;
    if (laneIndex) *laneIndex = g_selTrack;
    return &t.clips[(size_t)g_selClip];
}

// Copy a clip into a brand-new, independent instrument (own name, color and id).
void duplicateClipAsNew(int laneIndex, int clipIndex) {
    if (laneIndex < 0 || laneIndex >= (int)g_proj.tracks.size()) return;
    Track& lane = g_proj.tracks[(size_t)laneIndex];
    if (clipIndex < 0 || clipIndex >= (int)lane.clips.size()) return;
    const Clip source = lane.clips[(size_t)clipIndex];
    Clip copy = makeClip(lane, source.start + source.dur, source.dur, source.code);
    copy.runs = source.runs;
    copy.midiEnabled = source.midiEnabled;
    copy.pFieldRules = source.pFieldRules;
    lane.clips.push_back(copy);
    {
        std::vector<MidiClip> extra;
        for (const MidiClip& mc : lane.midiClips)
            if (mc.clipId == source.id) {
                MidiClip dup = mc;
                dup.id = newMidiId() + (int)extra.size();
                dup.clipId = copy.id;
                dup.start += source.dur;
                dup.editorOpen = false;
                extra.push_back(dup);
            }
        for (MidiClip& dup : extra) lane.midiClips.push_back(dup);
    }
    g_selTrack = laneIndex;
    g_selClip = (int)lane.clips.size() - 1;
    g_selClips.clear();
    g_selClips.insert(copy.id);
}

// Duplicate every selected clip as another use of the SAME instrument (same name, same code).
// The copies are placed right after the selection.
void duplicateSelectedClips() {
    std::vector<std::pair<int, Clip>> made;
    double lo = 1e300, hi = 0.0;
    for (int ti = 0; ti < (int)g_proj.tracks.size(); ++ti)
        for (const Clip& c : g_proj.tracks[(size_t)ti].clips)
            if (g_selClips.count(c.id)) {
                lo = std::min(lo, c.start);
                hi = std::max(hi, c.start + c.dur);
                made.push_back({ti, c});
            }
    if (made.empty()) return;
    const double span = std::max(0.001, hi - lo);
    g_selClips.clear();
    for (auto& item : made) {
        Clip copy = item.second;
        copy.id = g_nextClipId++;
        copy.start += span;
        copy.editorOpen = false;
        Track& lane = g_proj.tracks[(size_t)item.first];
        lane.clips.push_back(copy);
        g_selClips.insert(copy.id);
        g_selTrack = item.first;
        g_selClip = (int)lane.clips.size() - 1;
    }
}

void deleteSelectedMidiClips() {
    for (Track& t : g_proj.tracks)
        t.midiClips.erase(std::remove_if(t.midiClips.begin(), t.midiClips.end(),
                                         [](const MidiClip& c) { return g_selMidiClips.count(c.id) > 0; }), t.midiClips.end());
    g_selMidiClips.clear();
}

void deleteSelectedClips() {
    for (const int id : g_selClips) removeMidiClipsOf(id);
    for (Track& t : g_proj.tracks)
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                     [](const Clip& c) { return g_selClips.count(c.id) > 0; }), t.clips.end());
    g_selClips.clear();
    g_selClip = -1;
}

// Point the primary selection (g_selTrack/g_selClip) at a member of g_selClips.
void setPrimaryFromSet() {
    for (int ti = 0; ti < (int)g_proj.tracks.size(); ++ti) {
        const auto& cl = g_proj.tracks[(size_t)ti].clips;
        for (int k = 0; k < (int)cl.size(); ++k)
            if (g_selClips.count(cl[(size_t)k].id)) { g_selTrack = ti; g_selClip = k; return; }
    }
    g_selClip = -1;
}

int addTrack(const std::string& name, const std::string& templateCode) {
    Track t;
    t.name = name;
    t.id = g_nextTrackId++;
    t.code = templateCode;
    if (templateCode == kLead) t.automationRanges["input1"] = {"input1", 60.0, 80.0, 0.1, 70.0};
    t.color = storedRgb(defaultTrackRgb(t.id - 1));
    g_proj.tracks.push_back(t);
    ensureUniqueInstrumentName((int)g_proj.tracks.size() - 1);
    return (int)g_proj.tracks.size() - 1;
}

int addLangTrack(int /*lang*/) {
    return addTrack("Csound" + std::to_string(++g_trackCounter), kLead);
}

int addNonCsoundTrack(bool superCollider) {
    const std::string scTemplate =
        "// Automate input1: ~cawget.(\"input1\") is live inside a synth - CAW streams the lane into a control bus\n"
        "// outside a synth it returns the lane value once; draw dots on the input1 lane and press play\n"
        "(\n"
        "{ SinOsc.ar(~cawget.(\"input1\") * 660 + 110, 0, 0.1) ! 2 }.play(target: ~cawgroup);\n"
        ")\n";
    const int idx = addTrack(std::string(superCollider ? "SuperCollider" : "PureData") + std::to_string(++g_trackCounter),
                             superCollider ? scTemplate : "");
    g_proj.tracks[(size_t)idx].kind = superCollider ? 5 : 6;
    return idx;
}

// A graph track: one lane in the printed score that holds up to 8 automation lines. It is only drawn in the score, it never makes sound.
int addFreeAutomationTrack() {
    const int idx = addTrack("Graph" + std::to_string(++g_trackCounter), "");
    Track& t = g_proj.tracks[(size_t)idx];
    t.kind = 2;
    t.autoMin = 0.0; t.autoMax = 1.0;
    t.automation["line1"] = std::vector<AutoPoint>{};
    t.automationHeights["line1"] = kAutomationLaneH;
    return idx;
}

// A marker track: text boxes on the timeline for comments and guidance about the project.
int addMarkerTrack() {
    const int idx = addTrack("Notes" + std::to_string(++g_trackCounter), "");
    g_proj.tracks[(size_t)idx].kind = 1;
    return idx;
}

int addVstTrack(bool instrument) {
    const int idx = addTrack(std::string(instrument ? "VSTi" : "VSTe") + std::to_string(++g_trackCounter), "");
    g_proj.tracks[(size_t)idx].kind = instrument ? 3 : 4;
    g_proj.tracks[(size_t)idx].code = instrument   // the code every new clip of this track starts with (CAW code)
        ? "# plays a note every half second, P1 follows a slow lfo\nevery 0.5 {\n  note 48 + int(rnd(12)), 100, 0.4\n}\nP1 = uni(lfo(0.2))\n"
        : "# P1 follows the loudness of the sound that comes in\nP1 = clip(level(in) * 4, 0, 1)\n";
    vstui::openWindow(g_proj.tracks[(size_t)idx].id);
    return idx;
}

void addClip(int laneIndex, double start, double dur, const std::string& code, const std::string& name = std::string()) {
    Track& lane = g_proj.tracks[(size_t)laneIndex];
    if (!hasClips(lane)) return;   // graph and marker tracks take no clips
    Clip c = makeClip(lane, start, dur, code);
    if (!name.empty()) { c.name = name; ensureUniqueClipName(c); }
    lane.clips.push_back(c);
}

// Every project needs stable, unique clip ids/names and an own color per clip.
void normalizeClips() {
    std::set<int> used;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips) g_nextClipId = std::max(g_nextClipId, c.id + 1);
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips) {
            if (c.id <= 0 || used.count(c.id)) c.id = g_nextClipId++;
            used.insert(c.id);
            if (c.color == 0) c.color = storedRgb(defaultTrackRgb(c.id - 1));
            if (c.name.empty()) c.name = nextClipName(t.name);
        }
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips) ensureUniqueClipName(c);
}

// ---------------------------------------------------------------- automation selection helpers
void clearAutoSelection() {
    g_autoSel.clear(); g_autoSelMulti.clear();
    g_autoSelTrackId = -1;
    g_autoSelChannel.clear();
}

Track* findTrackById(int id, int* index = nullptr) {
    for (size_t i = 0; i < g_proj.tracks.size(); ++i)
        if (g_proj.tracks[i].id == id) { if (index) *index = (int)i; return &g_proj.tracks[i]; }
    return nullptr;
}

std::vector<AutoPoint>* selectedAutoLane() {
    Track* t = findTrackById(g_autoSelTrackId);
    if (!t) return nullptr;
    auto it = t->automation.find(g_autoSelChannel);
    return it == t->automation.end() ? nullptr : &it->second;
}

void validateAutoSelection() {
    std::vector<AutoPoint>* lane = selectedAutoLane();
    if (!lane) { clearAutoSelection(); return; }
    for (auto it = g_autoSel.begin(); it != g_autoSel.end();)
        it = (*it < 0 || *it >= (int)lane->size()) ? g_autoSel.erase(it) : std::next(it);
}

// Sort a lane by time while keeping the selected points selected.
// Insert a dot at its time position (after dots with the same time), so a lane that is sorted stays sorted.
void insertAutoPointSorted(std::vector<AutoPoint>& points, const AutoPoint& point) {
    const auto at = std::upper_bound(points.begin(), points.end(), point.t,
                                     [](double t, const AutoPoint& p) { return t < p.t; });
    points.insert(at, point);
}
void sortLaneKeepSelection(std::vector<AutoPoint>& points, std::set<int>& selection) {
    std::vector<std::pair<AutoPoint, bool>> tagged;
    tagged.reserve(points.size());
    for (int i = 0; i < (int)points.size(); ++i) tagged.push_back({points[(size_t)i], selection.count(i) > 0});
    std::stable_sort(tagged.begin(), tagged.end(), [](const auto& a, const auto& b) { return a.first.t < b.first.t; });
    selection.clear();
    for (int i = 0; i < (int)tagged.size(); ++i) {
        points[(size_t)i] = tagged[(size_t)i].first;
        if (tagged[(size_t)i].second) selection.insert(i);
    }
}

struct PdUiControls {std::vector<std::string> inputs;std::vector<pd_control_syntax::Output> outputs;};
PdUiControls pdControls(const Clip& clip) {
    if(clip.code.empty())return {};
    auto file=std::filesystem::u8path(clip.code);
    if(file.is_relative()) {
        if(g_projectPath.empty())return {};
        file=std::filesystem::u8path(g_projectPath).parent_path()/file;
    }
    std::error_code ec;
    file=std::filesystem::absolute(file,ec).lexically_normal();
    if(ec)return {};
    const auto stamp=std::filesystem::last_write_time(file,ec);
    if(ec)return {};
    struct Entry {std::filesystem::file_time_type stamp;PdUiControls controls;};
    static std::map<std::string,Entry> cache;
    const auto key=file.u8string();
    auto found=cache.find(key);
    if(found!=cache.end()&&found->second.stamp==stamp)return found->second.controls;
    std::string source,error;
    if(!pd_patch_editor::read(file,source,error))return {};
    Entry entry{stamp,{pd_control_syntax::inputs(source),pd_control_syntax::outputs(source)}};
    if(cache.size()>256)cache.clear();
    cache[key]=entry;return entry.controls;
}
ParsedTrack pdParsedForUi(const Track& track,const Clip* only=nullptr) {
    if(track.kind!=6)return only?parseClipForTrack(*only,track):parseTrack(track);
    ParsedTrack parsed;
    for(const auto& clip:track.clips) {
        if(only && clip.id!=only->id)continue;
        const auto controls=pdControls(clip);
        auto names=controls.inputs;
        if(!only)for(const auto& output:controls.outputs)if(output.kind=="cawset")names.push_back(output.name);
        for(const auto& name:names) {
            if(std::any_of(parsed.params.begin(),parsed.params.end(),[&](const CawParam& p){return p.chn==name;}))continue;
            CawParam p{name,0.0,1.0,.001,.5};
            auto found=track.automationRanges.find(name);if(found!=track.automationRanges.end())p=found->second;
            parsed.params.push_back(p);
        }
        for(const auto& output:controls.outputs)
            if(output.kind=="cawset")parsed.writeChannels.push_back(output.name);
            else if(output.kind=="cawscore")parsed.scoreStreams[output.name]=1;
    }
    return parsed;
}

void laneValueRange(const Track& track, const std::string& channel, double& lo, double& hi, double& initial) {
    const ParsedTrack parsed = pdParsedForUi(track);
    lo = 0.0; hi = 1.0; initial = 0.5;
    for (const CawParam& p : parsed.params) {
        if (p.chn != channel) continue;
        lo = p.min;
        hi = p.max > lo ? p.max : lo + 1.0;
        initial = p.value;
        return;
    }
}

double laneQuantizeValue(const Track& track, const std::string& channel, double value) {
    if (!hasClips(track)) return value; // free graph tracks have no declared clip step
    const ParsedTrack parsed = pdParsedForUi(track);
    for (const CawParam& param : parsed.params)
        if (param.chn == channel) return quantizeAutomation(value, param);
    return value;
}

const CawParam* laneParameter(const ParsedTrack& parsed, const std::string& channel) {
    for (const CawParam& param : parsed.params) if (param.chn == channel) return &param;
    return nullptr;
}

// ---------------------------------------------------------------- wave generator, drag handles, Capture and live Rec (helpers)
struct RecBuf { int trackId = 0; std::string channel; std::vector<AutoPoint> s; };
std::set<std::pair<int, std::string>> g_recArm;   // lanes armed with the R button (track id, channel)
std::vector<RecBuf> g_recBufs;                    // dots being recorded while the timeline plays
std::vector<CawWriteTarget> g_recNames;
ScoreProbeData g_pdScoreData;                     // numeric PD score streams captured during Play

struct WaveUi { int ch = 0; int shape = 0; float rate = 1.f, depth = 50.f, center = 50.f; bool whole = true; double t0 = 0.0, t1 = 4.0; };
WaveUi g_waveUi;
struct WavePreview { int frame = -1000; int trackId = -1; std::string channel; std::vector<AutoPoint> pts; };
WavePreview g_wavePrev;   // ghost line of the wave that the Wave... box would write

struct WaveHandleDrag {
    bool active = false; int kind = 0;   // 1 depth, 2 time stretch, 3 wave shape
    int trackId = 0; std::string channel; std::vector<int> idx; std::vector<AutoPoint> orig;
    double center = 0.0, lo = 0.0, hi = 1.0; float handleY = 0.f, startY = 0.f;
};
WaveHandleDrag g_wh;

void ensureAutoLane(Track& t, const std::string& channel) {
    if (t.automation.find(channel) == t.automation.end()) {
        t.automation.emplace(channel, std::vector<AutoPoint>{});
        t.automationHeights[channel] = kAutomationLaneH;
    }
    t.hiddenAutomation.erase(channel);
    g_collapsedAuto.erase(t.id);
}

void replaceDotsInRange(std::vector<AutoPoint>& lane, double t0, double t1, const std::vector<AutoPoint>& fresh) {
    lane.erase(std::remove_if(lane.begin(), lane.end(), [&](const AutoPoint& p) { return p.t >= t0 - 1e-9 && p.t <= t1 + 1e-9; }), lane.end());
    lane.insert(lane.end(), fresh.begin(), fresh.end());
    std::stable_sort(lane.begin(), lane.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
}

// the new dots are selected, so the drag handles show up right away
void selectDotsInRange(const Track& t, const std::string& channel, double t0, double t1) {
    const auto it = t.automation.find(channel);
    if (it == t.automation.end()) return;
    g_autoSel.clear(); g_autoSelMulti.clear();
    g_autoSelTrackId = t.id; g_autoSelChannel = channel;
    for (int i = 0; i < (int)it->second.size(); ++i)
        if (it->second[(size_t)i].t >= t0 - 1e-9 && it->second[(size_t)i].t <= t1 + 1e-9) g_autoSel.insert(i);
}

// Ramer-Douglas-Peucker on the value axis: keeps the shape with as few dots as possible.
void thinSamples(std::vector<AutoPoint>& s, double eps) {
    const size_t n = s.size();
    if (n < 3) return;
    std::vector<char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, n - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double maxd = -1.0;
        size_t mi = a;
        const double span = s[b].t - s[a].t;
        for (size_t i = a + 1; i < b; ++i) {
            const double v = span > 1e-12 ? s[a].v + (s[b].v - s[a].v) * ((s[i].t - s[a].t) / span) : s[a].v;
            const double d = std::fabs(s[i].v - v);
            if (d > maxd) { maxd = d; mi = i; }
        }
        if (maxd > eps) { keep[mi] = 1; stack.push_back({a, mi}); stack.push_back({mi, b}); }
    }
    std::vector<AutoPoint> out;
    for (size_t i = 0; i < n; ++i) if (keep[i]) out.push_back(s[i]);
    s.swap(out);
}
void thinToLimit(std::vector<AutoPoint>& s, double eps, size_t maxPoints) {
    std::vector<AutoPoint> work = s;
    thinSamples(work, eps);
    for (int guard = 0; work.size() > maxPoints && guard < 30; ++guard) { eps = std::max(eps * 1.6, 1e-9); work = s; thinSamples(work, eps); }
    s.swap(work);
}

double waveUnit(int shape, double ph) {   // -1 .. 1 at the phase 0 .. 1
    const double kPi = 3.14159265358979323846;
    switch (shape) {
        case 1: return ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0);
        case 2: return ph < 0.5 ? 1.0 : -1.0;
        case 3: return 2.0 * ph - 1.0;
        case 4: return 1.0 - 2.0 * ph;
        default: return std::sin(2.0 * kPi * ph);
    }
}

// shape 0 sine, 1 triangle, 2 square, 3 saw up, 4 saw down. amp = peak as a fraction of the full range.
std::vector<AutoPoint> genWavePoints(int shape, double rate, double amp, double centerValue, double lo, double hi, double t0, double t1) {
    std::vector<AutoPoint> out;
    if (rate <= 1e-6 || t1 <= t0 + 1e-6) return out;
    const double kPi = 3.14159265358979323846;
    const double period = 1.0 / rate, peak = amp * (hi - lo) * 0.5;
    const double cycles = (t1 - t0) / period;
    const double e = std::min(0.004, period * 0.02) / period;   // width of a vertical edge (as a part of the cycle)
    int ppc = 16;
    if (cycles * ppc > 20000.0) ppc = std::max(4, (int)(20000.0 / cycles));
    auto put = [&](double t, double w) {
        AutoPoint p; p.t = t; p.v = std::min(hi, std::max(lo, centerValue + peak * w));
        out.push_back(p);
    };
    for (long c = 0; c < (long)std::ceil(cycles) && out.size() < 40000; ++c) {
        const double base = t0 + (double)c * period;
        std::vector<std::pair<double, double>> bp;
        switch (shape) {
            case 1: bp = {{0.0, 0.0}, {0.25, 1.0}, {0.75, -1.0}}; break;
            case 2: bp = {{0.0, 1.0}, {0.5 - e, 1.0}, {0.5, -1.0}, {1.0 - e, -1.0}}; break;
            case 3: bp = {{0.0, -1.0}, {1.0 - e, 1.0}}; break;
            case 4: bp = {{0.0, 1.0}, {1.0 - e, -1.0}}; break;
            default: for (int k = 0; k < ppc; ++k) bp.push_back({(double)k / ppc, std::sin(2.0 * kPi * k / ppc)}); break;
        }
        for (const auto& b : bp) {
            const double t = base + b.first * period;
            if (t > t1 + 1e-9) break;
            put(t, b.second);
        }
    }
    if (out.empty() || out.back().t < t1 - 1e-6) put(t1, waveUnit(shape, std::fmod(cycles, 1.0)));
    return out;
}

bool autoSelBounds(const std::vector<AutoPoint>& lane, const std::set<int>& sel, double& tMin, double& tMax, double& vMin, double& vMax) {
    bool any = false;
    for (int i : sel) {
        if (i < 0 || i >= (int)lane.size()) continue;
        const AutoPoint& p = lane[(size_t)i];
        if (!any) { tMin = tMax = p.t; vMin = vMax = p.v; any = true; }
        else { tMin = std::min(tMin, p.t); tMax = std::max(tMax, p.t); vMin = std::min(vMin, p.v); vMax = std::max(vMax, p.v); }
    }
    return any;
}

// depth (start -> end, 1 = as drawn), time stretch (1 = as drawn), morph (-1 triangle .. 0 as drawn .. +1 square), all from the original dots
void reshapeDots(std::vector<AutoPoint>& lane, const std::vector<int>& idx, const std::vector<AutoPoint>& orig,
                 double dStart, double dEnd, double stretch, double morph, double lo, double hi) {
    if (orig.empty() || orig.size() != idx.size()) return;
    const double kPi = 3.14159265358979323846;
    double vMin = orig[0].v, vMax = orig[0].v;
    for (const AutoPoint& q : orig) { vMin = std::min(vMin, q.v); vMax = std::max(vMax, q.v); }
    const double center = (vMin + vMax) * 0.5, half = (vMax - vMin) * 0.5;
    const double t0 = orig.front().t, span = orig.back().t - t0;
    const double amt = std::fabs(morph);
    for (size_t k = 0; k < idx.size(); ++k) {
        if (idx[k] < 0 || idx[k] >= (int)lane.size()) continue;
        const double x = span > 1e-9 ? (orig[k].t - t0) / span : 0.0;
        const double depth = dStart + (dEnd - dStart) * x;
        double n = half > 1e-12 ? (orig[k].v - center) / half : 0.0;
        if (amt > 1e-6) {
            const double w = morph > 0.0 ? (n >= 0.0 ? 1.0 : -1.0) : (2.0 / kPi) * std::asin(std::min(1.0, std::max(-1.0, n)));
            n = n + (w - n) * amt;
        }
        AutoPoint& dst = lane[(size_t)idx[k]];
        dst.v = std::min(hi, std::max(lo, center + n * half * depth));
        dst.t = clampTimelineTime(t0 + (orig[k].t - t0) * stretch);
    }
}

struct WaveHandles { ImVec2 depth, stretch, morph; };
WaveHandles waveHandlePos(float x0, float x1, float yTop, float yBot, float yMid, float ly0, float ly1, float xMax) {
    WaveHandles h;
    const float cx = (x0 + x1) * 0.5f;
    h.depth = ImVec2(cx, std::min(ly1 - S(8), std::max(ly0 + S(8), yTop - S(11))));
    h.morph = ImVec2(cx, std::min(ly1 - S(8), std::max(ly0 + S(8), yBot + S(11))));
    h.stretch = ImVec2(std::min(xMax - S(8), x1 + S(13)), std::min(ly1 - S(8), std::max(ly0 + S(8), yMid)));
    return h;
}
void drawWaveHandles(ImDrawList* dl, const WaveHandles& h, int active) {
    const ImU32 fill = IM_COL32(237, 128, 38, 255), line = IM_COL32(255, 255, 255, 255), ink = IM_COL32(20, 20, 24, 255);
    auto knob = [&](ImVec2 c, int kind) {
        dl->AddCircleFilled(c, S(7), active == kind ? IM_COL32(255, 190, 110, 255) : fill);
        dl->AddCircle(c, S(7), line, 0, S(1.2f));
        const float a = S(3);
        if (kind == 1) {        // up and down
            caw_ui::arrow(dl, c, ImGuiDir_Up, a, ink);
            caw_ui::arrow(dl, c, ImGuiDir_Down, a, ink);
        } else if (kind == 2) { // left and right
            caw_ui::arrow(dl, c, ImGuiDir_Left, a, ink);
            caw_ui::arrow(dl, c, ImGuiDir_Right, a, ink);
        } else {                // a small wave
            ImVec2 w[5] = {ImVec2(c.x - a * 1.4f, c.y), ImVec2(c.x - a * 0.7f, c.y - a), ImVec2(c.x, c.y), ImVec2(c.x + a * 0.7f, c.y + a), ImVec2(c.x + a * 1.4f, c.y)};
            dl->AddPolyline(w, 5, ink, 0, S(1.4f));
        }
    };
    knob(h.depth, 1); knob(h.stretch, 2); knob(h.morph, 3);
}

// live Rec: one value of the clip's channel "rec" arrived from the engine monitor
void recFeed(int idx, float v) {
    if (!g_playing || !g_timelineRun || g_recArm.empty() || idx < 0 || idx >= (int)g_recNames.size() || !std::isfinite(v)) return;
    const CawWriteTarget& target = g_recNames[(size_t)idx];
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips) {
            if (c.name != target.clip || g_play < c.start || g_play > c.start + c.dur + 0.05) continue;
            for (const auto& arm : g_recArm) {
                if (arm.first != t.id || (!target.channel.empty() && arm.second != target.channel)) continue;
                RecBuf* buf = nullptr;
                for (RecBuf& b : g_recBufs) if (b.trackId == arm.first && b.channel == arm.second) { buf = &b; break; }
                if (!buf) { g_recBufs.push_back(RecBuf()); buf = &g_recBufs.back(); buf->trackId = arm.first; buf->channel = arm.second; }
                if (!buf->s.empty() && g_play <= buf->s.back().t) continue;
                AutoPoint p; p.t = g_play; p.v = v;
                buf->s.push_back(p);
            }
        }
}

bool hasAutoSelection() {
    if (!g_autoSel.empty()) return true;
    for (const auto& kv : g_autoSelMulti) if (!kv.second.empty()) return true;
    return false;
}

// Deletes the selected dots of EVERY lane (a box can select dots in several lanes at once).
void deleteSelectedAutoPoints() {
    std::map<std::pair<int, std::string>, std::set<int>> all = g_autoSelMulti;
    if (!g_autoSel.empty()) all[{g_autoSelTrackId, g_autoSelChannel}].insert(g_autoSel.begin(), g_autoSel.end());
    for (auto& [key, indices] : all) {
        Track* t = findTrackById(key.first);
        if (!t) continue;
        auto it = t->automation.find(key.second);
        if (it == t->automation.end()) continue;
        std::vector<AutoPoint>& pts = it->second;
        for (auto r = indices.rbegin(); r != indices.rend(); ++r)
            if (*r >= 0 && *r < (int)pts.size()) pts.erase(pts.begin() + *r);
    }
    g_autoSel.clear(); g_autoSelMulti.clear();
}

void endChildTight() {  // EndChild without the extra item spacing below it
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
}

// ------------------------------------------------ std::string <-> ImGui
struct StrCtx { std::string* s; ImGuiInputTextCallback chain; };

int strCb(ImGuiInputTextCallbackData* d) {
    StrCtx* c = (StrCtx*)d->UserData;
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        c->s->resize((size_t)d->BufTextLen);
        d->Buf = c->s->data();
    } else if (c->chain) {
        return c->chain(d);
    }
    return 0;
}

bool inputStr(const char* label, std::string& s, ImGuiInputTextFlags flags, ImGuiInputTextCallback chain = nullptr) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    if (chain) flags |= ImGuiInputTextFlags_CallbackCharFilter;
    StrCtx c{&s, chain};
    return ImGui::InputText(label, s.data(), s.capacity() + 1, flags, strCb, &c);
}

bool inputStrHint(const char* label, const char* hint, std::string& s) {
    StrCtx c{&s, nullptr};
    return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1,
                                    ImGuiInputTextFlags_CallbackResize, strCb, &c);
}

bool inputMulti(const char* label, std::string& s, ImVec2 size, ImGuiInputTextFlags flags) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    StrCtx c{&s, nullptr};
    return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, flags, strCb, &c);
}

int nameFilter(ImGuiInputTextCallbackData* d) {  // track name = valid Csound instrument name
    ImWchar c = d->EventChar;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return 0;
    return 1;
}

void fixName(std::string& n) {
    if (n.empty()) n = "Track";
    if (n[0] >= '0' && n[0] <= '9') n = "T" + n;
}

// ---------------------------------------------------------------- console log + csound processes
LogLine classifyLog(const std::string& text) {
    static const std::regex clipRe(R"re((?:instr|clip)\s+(\w+))re");
    LogLine l;
    l.text = text;
    std::string low = text;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::smatch m;
    if (std::regex_search(text, m, clipRe)) l.clip = m[1].str();
    const bool app = text.rfind("[caw]", 0) == 0;
    l.level = app ? kLogMessage : kLogDetail;
    if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos || low.find("invalid") != std::string::npos) l.level = kLogError;
    else if (!app && (low.find("couldn't create") != std::string::npos || low.find("not understood") != std::string::npos || low.find("no method for") != std::string::npos ||
                      low.find("used before defined") != std::string::npos || low.find("undefined variable") != std::string::npos || low.find("syntax") != std::string::npos)) l.level = kLogError;
    else if (low.find("warning") != std::string::npos || (!app && low.find("extra arguments") != std::string::npos)) l.level = kLogImportant;
    else if (app)
        for (const char* key : {"run ", "rendered", "render", "playback", "stopped", "saved", "opened"})
            if (low.find(key) != std::string::npos) { l.level = kLogImportant; break; }
    return l;
}

void pushLog(LogLine&& l) {
    g_log.push_back(std::move(l));
    if (g_log.size() > 5000) {
        g_log.erase(g_log.begin(), g_log.begin() + 1000);
        if (g_scErrorLogIndex >= 0) {
            if (g_scErrorLogIndex < 1000) g_scErrorLogIndex = -1;
            else g_scErrorLogIndex -= 1000;
        }
    }
    g_logDirty = true;
}

// ---- diagnostics: which clip, which line of its code, which time on the timeline
std::string timeText(double t) {
    const int m = (int)(t / 60.0);
    char b[64];
    std::snprintf(b, sizeof b, "%.2f s (%d:%04.1f)", t, m, t - m * 60.0);
    return b;
}
const Clip* clipByName(const std::string& name) {
    for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) if (c.name == name) return &c;
    return nullptr;
}
// Puts the clip, its line and its start times on a log line. An error with a line also gets a red mark in the code editor of the clip.
void fillLoc(LogLine& l, const std::string& clipName, int line, const std::string& markText) {
    const Clip* first = nullptr;
    std::vector<double> times;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips)
            if (c.name == clipName) { if (!first) first = &c; times.push_back(c.start); }
    if (!first) return;
    std::sort(times.begin(), times.end());
    l.clip = clipName; l.clipId = first->id; l.line = line; l.times = times;
    if (g_playing) l.playhead = g_play;
    if (l.level == kLogError && line > 0) {
        std::string mk = markText;
        while (!mk.empty() && std::isspace((unsigned char)mk.front())) mk.erase(mk.begin());
        while (!mk.empty() && std::isspace((unsigned char)mk.back())) mk.pop_back();
        if (mk.size() > 300) mk.resize(300);
        auto& marks = g_diagMarks[first->id];
        if (!marks.count(line)) marks[line] = mk.empty() ? std::string("error") : mk;
    }
}
void logClipError(const char* language, const Clip& clip, const std::string& message) {
    LogLine entry = classifyLog(std::string("[") + language + "] ERROR: " + message);
    entry.level = kLogError;
    fillLoc(entry, clip.name, 0, message);
    pushLog(std::move(entry));
}
void logClipErrorById(const char* language, int clipId, const std::string& message) {
    for (const Track& track : g_proj.tracks)
        for (const Clip& clip : track.clips)
            if (clip.id == clipId) { logClipError(language, clip, message); return; }
    LogLine entry = classifyLog(std::string("[") + language + "] ERROR: " + message);
    entry.level = kLogError;
    pushLog(std::move(entry));
}
void setScErrorLocation(int line, bool approximate, const std::string& clue) {
    if (line <= 0 || g_scErrorClipId <= 0) return;
    Clip* clip = findClip(g_scErrorClipId);
    if (!clip) return;
    const int lineCount = 1 + (int)std::count(clip->code.begin(), clip->code.end(), '\n');
    if (line > lineCount) return;

    std::string mark = approximate
        ? "Likely runtime-error line, inferred from the SuperCollider message: " + clue
        : clue;
    if (mark.size() > 300) mark.resize(300);
    if (g_scErrorLogIndex >= 0 && (size_t)g_scErrorLogIndex < g_log.size()) {
        LogLine& entry = g_log[(size_t)g_scErrorLogIndex];
        if (entry.lineApprox && !approximate && entry.line > 0 && entry.line != line) {
            auto marks = g_diagMarks.find(clip->id);
            if (marks != g_diagMarks.end()) marks->second.erase(entry.line);
        }
        entry.level = kLogError;
        if (!g_scErrorMessage.empty()) entry.text = "[SuperCollider] " + g_scErrorMessage;
        fillLoc(entry, clip->name, line, mark);
        entry.lineApprox = approximate;
    } else {
        LogLine marker;
        marker.level = kLogError;
        fillLoc(marker, clip->name, line, mark);
    }
    g_gotoLine[clip->id] = line;
}
void startScError(int clipId, const std::string& message) {
    if (clipId <= 0) return;
    g_scErrorClipId = clipId;
    g_scErrorContextLines = 16;
    g_scErrorHeadlineSeen = false;
    g_scErrorMessage = message.empty() ? "SuperCollider clip evaluation failed." : message;
    logClipErrorById("SuperCollider", clipId, g_scErrorMessage);
    g_scErrorLogIndex = g_log.empty() ? -1 : (int)g_log.size() - 1;
}
void updateScErrorHeadline(const std::string& line) {
    g_scErrorHeadlineSeen = true;
    g_scErrorMessage = line;
    if (g_scErrorLogIndex >= 0 && (size_t)g_scErrorLogIndex < g_log.size()) {
        LogLine& primary = g_log[(size_t)g_scErrorLogIndex];
        primary.text = "[SuperCollider] " + line;
        primary.level = kLogError;
    }
}
pd_patch_editor::Buffer* pdEditorBuffer(const Clip& clip) {
    if (clip.code.empty()) return nullptr;
    std::filesystem::path file = std::filesystem::u8path(clip.code);
    if (file.is_relative()) {
        if (g_projectPath.empty()) return nullptr;
        file = std::filesystem::u8path(g_projectPath).parent_path() / file;
    }
    std::error_code ec;
    file = std::filesystem::absolute(file, ec).lexically_normal();
    if (ec || !std::filesystem::is_regular_file(file, ec) || ec) return nullptr;
    auto& buffer = g_pdPatchBuffers[file.u8string()];
    if (!buffer.loaded && !pd_patch_editor::load(file, buffer)) return nullptr;
    return &buffer;
}
pd_diagnostics::SourceLine inferPdEditorLocation(const Clip& clip, const std::string& message) {
    pd_patch_editor::Buffer* buffer = pdEditorBuffer(clip);
    if (!buffer) return {};
    // Do not place a stale mark on unsaved text that no longer matches the
    // patch libpd loaded. Wait for the next saved/reloaded diagnostic.
    if (buffer->dirty()) return {};
    const std::string readable = pd_patch_editor::readableText(buffer->original);
    auto location = pd_diagnostics::inferEditorLine(readable, message);
    if (location.candidates.size() > 8) location.candidates.resize(8);
    return location;
}
void logPdClipMessage(int clipId, const std::string& message) {
    std::string low = message;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const bool warning = low.rfind("warning:", 0) == 0 || low.rfind("notice:", 0) == 0;
    LogLine entry = classifyLog(std::string("[PureData] ") + (warning ? "WARNING: " : "ERROR: ") + message);
    entry.level = warning ? kLogImportant : kLogError;
    if (Clip* clip = findClip(clipId)) {
        const auto location = inferPdEditorLocation(*clip, message);
        fillLoc(entry, clip->name, location.line, message);
        entry.lineApprox = location.approximate;
        entry.lineCandidates = location.candidates;
        if (location.line > 0) {
            entry.lineApprox = location.approximate;
            g_gotoLine[clip->id] = location.line;
        }
    }
    pushLog(std::move(entry));
}
void logPdNote(const std::string& note);
void logPdFailureStatus(const std::string& status) {
    static const std::regex clipError(R"re(PD clip\s+(\d+):\s*(.*))re");
    std::smatch match;
    if (std::regex_search(status, match, clipError)) {
        logPdClipMessage(std::atoi(match[1].str().c_str()), match[2].str());
    } else {
        LogLine entry = classifyLog("[PureData] ERROR: " + status);
        entry.level = kLogError;
        pushLog(std::move(entry));
    }
}
void diagClearAll() { g_diagMarks.clear(); g_errCtx = ErrCtx(); }
void diagClearClip(const std::string& name) { if (const Clip* c = clipByName(name)) g_diagMarks.erase(c->id); g_errCtx = ErrCtx(); }

void logLine(const std::string& s) {
    LogLine l = classifyLog(s);
    if (l.level == kLogError && !l.clip.empty() && l.clipId < 0) fillLoc(l, l.clip, 0, s);
    pushLog(std::move(l));
}

void logPdNote(const std::string& note) {
    static const std::regex clipMessage(R"re(PD clip\s+\d+:)re");
    if (std::regex_search(note, clipMessage)) logPdFailureStatus(note);
    else logLine("[caw] " + note);
}

void drainPdControlOutputs() {
    if(!g_mixedPd && !g_pdEngineOn)return;
    auto values=g_mixedPd?g_apiProc.takePdControlOutputs():g_pdTimelineEngine.takeControlOutputs();
    const unsigned dropped=g_mixedPd?g_apiProc.takePdDroppedControls():g_pdTimelineEngine.takeDroppedControls();
    if(dropped)logLine("[caw] PD control queue dropped "+std::to_string(dropped)+" messages.");
    if(!g_timelineRun)return;
    for(const auto& value:values) {
        if(!std::isfinite(value.value))continue;
        if(value.kind==3) { g_cawHub.send(value.name, value.value); continue; }   // caws: PD -> shared hub
        Track* track=findTrackById(value.trackId);
        if(!track)continue;
        Clip* clip=nullptr;for(auto& c:track->clips)if(c.id==value.clipId){clip=&c;break;}
        if(!clip)continue;
        const double time=static_cast<double>(value.frame)/std::max(1000,g_proj.cs.sr);
        const std::string name=value.name;
        if(value.kind==1 && g_recArm.count({track->id,name})) {
            RecBuf* buf=nullptr;
            for(auto& b:g_recBufs)if(b.trackId==track->id&&b.channel==name){buf=&b;break;}
            if(!buf){g_recBufs.push_back(RecBuf());buf=&g_recBufs.back();buf->trackId=track->id;buf->channel=name;}
            if(buf->s.size()<100000 && (buf->s.empty()||time>buf->s.back().t))
                buf->s.push_back({time,value.value});
        } else if(value.kind==2) {
            auto& samples=g_pdScoreData[clip->name].data;
            bool accept=samples.size()<100000;
            for(auto it=samples.rbegin();accept&&it!=samples.rend();++it)if(it->name==name){
                accept=time>=it->t+.05;break;
            }
            if(accept){ScoreDataSample sample;sample.t=time;sample.v=value.value;sample.name=name;sample.rate=1;samples.push_back(std::move(sample));}
        }
    }
}

bool writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << text;
    return (bool)f;
}

std::string chooseProjectPath(bool saveAs) {
    return plat::chooseFile(saveAs, saveAs ? "Save CAW project" : "Open CAW project", "CAW project", {"caw"},
                            !g_projectPath.empty() ? g_projectPath : std::string("Untitled.caw"));
}

std::string chooseAudioPath() {
    return plat::chooseFile(false, "Choose an audio file", "", {}, std::string());
}

std::string chooseIncludePath() {
    return plat::chooseFile(false, "Choose a Csound include file", "Csound includes", {"udo", "sco", "txt"}, std::string());
}

void saveProject(bool saveAs) {
    std::string path = saveAs || g_projectPath.empty() ? chooseProjectPath(true) : g_projectPath;
    if (path.empty()) return;
    vstui::captureParameters(g_proj);
    if (std::filesystem::path(path).extension().empty()) path += ".caw";
    std::string error;
    if (saveProjectFile(g_proj, path, error)) {
        g_projectPath = std::filesystem::absolute(path).string();
        logLine("[caw] Saved project: " + g_projectPath);
        g_savedSnap = serializeProject(g_proj);
        g_autoSavedSnap = g_savedSnap;
        g_autoSaveElapsed = 0.f;
    } else logLine("[caw] Save failed: " + error);
}

void stopPlayback();
void stopScEngine();
void closeScSoloOutput();
void restoreScSoloOutput();
void ensureVstSoloOutput();
void stopVstEngine(bool unloadPlugins = false);
void startVstEngine();
void vstEngineCheckJobs(float dt);
bool engineIsUp();
std::map<int, unsigned> collectVstInputMasks(const Project& project);
std::set<int> collectVstActiveTracks(const Project& project);
void playNotice(const std::string& text, const std::string& shortText);
void splitCaw(const std::string& code, std::string& patch, std::string& caw);
std::string csoundExePath();
void refreshAudioDevices(bool includeCsound = true);
void validateAudioDeviceSelection();
void historyReset();

// Makes `project` the current one and resets every editor state that belongs to the old project.
void adoptProject(Project&& project, const std::string& path) {
    const auto began = std::chrono::steady_clock::now();
    g_pdPreview.stop();
    g_pdTimelineEngine.stop();
    stopScEngine();
    stopVstEngine(true);   // the instruments belong to the old project: unload them
    g_pdEngineOn = false;
    g_pdNeedsReload = false;
    g_pdScoreData.clear();
    g_pdMeterDb.clear();
    g_pdTransportEpoch = 0;
    g_pdTimelineMode = false; g_pdResumeAfterReload = false; g_pdPlayingClip = 0; g_pdTimelineEvents.clear();
    g_timelineRun = false;
    if (g_playing) stopPlayback();
    vstui::slots().clear();
    g_proj = std::move(project);
    g_audioPoolSelected = -1;
    g_includeSelected = -1;
    g_udoPoolSelected = -1;
    g_instrPoolSelected = -1;
    g_codeEditors.clear();
    g_selMidiClips.clear();
    g_mcDragId = -1;
    g_midiDragTrackId = g_midiDragNote = -1;
    g_midiSelectedTrackId = g_midiSelectedNote = -1;
    g_projectPath = path;
    g_autoSavedSnap.clear();
    g_unsavedAutosavePath.clear();
    g_autoSaveElapsed = 0.f;
    g_nextTrackId = 1;
    std::set<int> usedIds;
    for (Track& t : g_proj.tracks) {
        if (t.id <= 0 || usedIds.find(t.id) != usedIds.end()) t.id = g_nextTrackId;
        usedIds.insert(t.id);
        g_nextTrackId = std::max(g_nextTrackId, t.id + 1);
        if (t.color == 0) t.color = defaultTrackRgb(t.id - 1);
    }
    for (int i = 0; i < (int)g_proj.tracks.size(); ++i) ensureUniqueInstrumentName(i);
    g_nextClipId = 1;
    normalizeClips();
    g_clipCodeEditors.clear();
    clearAutoSelection();
    g_trackCounter = (int)g_proj.tracks.size();
    g_selTrack = g_selClip = -1;
    g_play = 0.0;
    g_loopSelecting = false;
    historyReset();
    if (g_audioDevicesReady) validateAudioDeviceSelection();
    g_savedSnap = serializeProject(g_proj);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    logLine("[caw] Project initialization: " + std::to_string(ms) + " ms.");
}

void openProjectPath(const std::string& path) {
    if (path.empty()) return;
    const auto began = std::chrono::steady_clock::now();
    Project loaded;
    std::string error;
    if (!loadProjectFile(loaded, path, error)) {
        logLine("[caw] Open failed for " + path + ": " + error);
        return;
    }
    const auto readMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    adoptProject(std::move(loaded), std::filesystem::absolute(path).string());
    logLine("[caw] Project file read and parse: " + std::to_string(readMs) + " ms.");
    logLine("[caw] Opened project: " + g_projectPath);
}

void newProject() {
    g_languageChoiceMade = false;
    g_chooseLanguages = true;
    g_opcodeProc.stop(); g_opcodeScanStarted = false; g_opcodeScanT = 0.f;
    g_testProc.stop();
    adoptProject(Project(), std::string());
    g_timelineRun = false;
    logLine("[caw] New empty project. Choose its languages to begin.");
}

void openProject() {
    const std::string path = chooseProjectPath(false);
    if (!path.empty()) openProjectPath(path);
}

bool nameHasTag(const std::string& name, const std::string& tag);   // defined next to devCombo

// Name of the system default playback / recording device (Windows: the one chosen in the Windows sound control panel; elsewhere empty).
std::string windowsDefaultEndpointName(bool capture) { return plat::defaultAudioEndpointName(capture); }

// The device of this host API type (ASIO / WASAPI / ...) that is the Windows default; otherwise the first one of that type. -1 = none found.
int pickDefaultDevice(const std::vector<AudioDevice>& list, const std::string& tag, bool capture) {
    const std::string wanted = tag == "ASIO" ? std::string() : windowsDefaultEndpointName(capture);   // empty on macOS / Linux: first device of that type
    if (!wanted.empty())
        for (const AudioDevice& d : list)
            if (cawDeviceHasHostApi(d.name, tag) && nameHasTag(d.name, wanted)) return d.id;
    for (const AudioDevice& d : list) if (cawDeviceHasHostApi(d.name, tag)) return d.id;
    return -1;
}

void refreshAudioDevices(bool includeCsound) {
    const auto began = std::chrono::steady_clock::now();
    std::string backend = g_proj.cs.rtaudio;
    if (backend == "asio" || backend == "wasapi" || backend == "dsound" || backend == "directsound") backend = "portaudio";
    if (backend == "winmm") backend = "winmme";
    g_proj.cs.rtaudio = backend;
    std::string outError, inError;
    // The list shown in Settings comes from CAW's own PortAudio (it has every driver: ASIO, WASAPI, DirectSound, MME, WDM-KS ...).
    // Csound's own list is only kept to translate a choice when the old (process) engine is used; it can lack some driver types.
    g_csoundOutDevs.clear(); g_csoundInDevs.clear();
    if (includeCsound) {
        g_csoundOutDevs = listCsoundDevices(csoundExePath(), backend, true, outError);
        g_csoundInDevs = listCsoundDevices(csoundExePath(), backend, false, inError);
    }
    std::string paOutError, paInError;
    listPortAudioDevicePairs(g_outDevs, g_inDevs, paOutError, paInError);
    if (g_outDevs.empty()) { g_outDevs = g_csoundOutDevs; if (g_outDevs.empty() && outError.empty()) outError = paOutError; } else outError.clear();
    if (g_inDevs.empty()) { g_inDevs = g_csoundInDevs; if (g_inDevs.empty() && inError.empty()) inError = paInError; } else inError.clear();
    if (!outError.empty()) logLine("[caw] Audio output devices: " + outError);
    else logLine("[caw] Found " + std::to_string(g_outDevs.size()) + " output device(s) in all drivers." +
                 (includeCsound ? " Csound lists " + std::to_string(g_csoundOutDevs.size()) + "." : std::string()));
    if (!inError.empty()) logLine("[caw] Audio input devices: " + inError);
    g_audioDevicesReady = true; // also cache failures; Settings has an explicit Refresh button
    if (!g_outDevs.empty() || !g_inDevs.empty()) validateAudioDeviceSelection();
    // "System default" = Windows default card (WASAPI). On macOS / Linux PortAudio's own default device is used (-odac without a number).
    g_cawDefaultDevice = [](bool capture) { return (capture || !plat::isWindows) ? -1 : pickDefaultDevice(g_outDevs, "WASAPI", false); };
    if (!g_proj.cs.options.empty()) syncOptionsFromSettings(g_proj.cs);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    logLine("[caw] Audio device scan: " + std::to_string(ms) + " ms.");
}

void validateAudioDeviceSelection() {
    // Saved device indices belong to the computer on which they were made.
    auto known = [](const std::vector<AudioDevice>& list, int id) { for (const AudioDevice& d : list) if (d.id == id) return true; return false; };
    if (!g_outDevs.empty() && g_proj.cs.outDev >= 0 && !known(g_outDevs, g_proj.cs.outDev)) {
        logLine("[caw] Audio output device " + std::to_string(g_proj.cs.outDev) + " of this project does not exist on this computer - using the system default.");
        g_proj.cs.outDev = -1;
    }
    if (!g_inDevs.empty() && g_proj.cs.inDev >= 0 && !known(g_inDevs, g_proj.cs.inDev)) {
        logLine("[caw] Audio input device " + std::to_string(g_proj.cs.inDev) + " of this project does not exist on this computer - using the system default.");
        g_proj.cs.inDev = -1;
    }
    if (!g_proj.cs.options.empty()) syncOptionsFromSettings(g_proj.cs);
}

CawAudioSettings pdAudioSettings() {
    CawAudioSettings audio;
    audio.sampleRate = g_proj.cs.sr; audio.channels = 2;
    audio.softwareBuffer = g_proj.cs.bufSW; audio.hardwareBuffer = g_proj.cs.bufHW;
    audio.queueFrames = std::max(512, g_proj.cs.bufSW * 2);
    int selected = g_proj.cs.outDev;
    if (selected == -1 && plat::isWindows) selected = pickDefaultDevice(g_outDevs, "WASAPI", false);
    if (selected >= 0) {
        audio.deviceLabel = "<selected device missing>";
        for (const auto& device : g_outDevs) if (device.id == selected) { audio.deviceLabel = device.name; break; }
    }
    attachScRealtimeSource(audio);
    return audio;
}
bool scHasSharedAudioOwner() {
    return (g_playing && g_useCsoundApi) || g_pdEngineOn || g_pdPreview.active();
}
void closeScSoloOutput() {
    if (g_scSoloOutput) { g_scSoloOutput->close(); g_scSoloOutput.reset(); }
    g_scSoloCapture.reset();
    g_scSoloSnapshotSeq = 0;
    // Every engine that takes the soundcard calls this first: the VST engine's own stream closes
    // too (the engine itself keeps running and is mixed into the stream that opens next).
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
}
bool startScSoloOutput(std::string& error) {
    if (!g_scEngine.embedded() || scHasSharedAudioOwner()) return true;
    if (g_scSoloOutput) return true;
    // The VST engine's own stream (Run VST alone) must let go of the device first: its mix
    // continues in the SC stream through the shared realtime hook.
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) { error = "Select an audio output in Settings > Audio."; return false; }
    auto output = std::make_unique<CawAudioOutput>();
    CawAudioSettings settings = pdAudioSettings();
    auto capture = std::make_unique<MonitorCapture>(settings.sampleRate, settings.channels);
    output->setMonitorCapture(capture.get());
    if (!output->open(settings, error)) return false;
    output->setExpectingAudio(false);
    g_scSoloCapture = std::move(capture);
    g_scSoloOutput = std::move(output);
    return true;
}
void restoreScSoloOutput() {
    if (g_scEngine.embedded() && !scHasSharedAudioOwner() && !g_scSoloOutput) {
        std::string error;
        if (!startScSoloOutput(error)) logLine("[caw] SuperCollider output could not restart: " + error);
    }
    ensureVstSoloOutput();   // no-op unless the VST engine runs and no other stream carries audio
}
// Standalone clip audition remains separate from timeline playback.
bool startPdPatch(const std::string& patch, int clipId, std::string& error) {
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if ((g_playing && !g_useCsoundApi) || g_testSound.running() || g_renderActive || g_pdPreview.active() || g_pdEngineOn) {
        error = "Stop the legacy audio process, test sound, export, or another PD engine before auditioning a PD clip."; return false;
    }
    if (g_pdLibraryPath.empty()) g_pdLibraryPath = plat::bundledLibpd();
    if (g_pdLibraryPath.empty()) { error = "libpd was not found. Select it in Settings > Programs, or copy libpd.dll (+ libwinpthread-1.dll) into <CAW folder>\\PureData\\."; return false; }
    if (g_proj.cs.outDev == -2) { error = "Select an audio output in Settings > Audio."; return false; }
    closeScSoloOutput();
    if (!g_pdPreview.start(g_pdLibraryPath, patch, pdAudioSettings())) { error = g_pdPreview.status(); restoreScSoloOutput(); return false; }
    g_pdPlayingClip = clipId;
    return true;
}
void stopPdPatch() { g_pdPreview.stop(); g_pdPlayingClip = 0; restoreScSoloOutput(); }

// The library is a machine setting, not part of a portable project.
std::filesystem::path pdConfigPath() {
    std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
    return home.empty() ? std::filesystem::path() : std::filesystem::u8path(home) / ".caw_libpd_path";
}
void savePdLibraryPath() {
    const auto file = pdConfigPath();
    if (file.empty()) return;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) out << g_pdLibraryPath;
    else logLine("[caw] Could not save the libpd setting.");
}
std::filesystem::path scConfigPath() {
    const std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
    return home.empty() ? std::filesystem::path() : std::filesystem::u8path(home) / ".caw_sclang_path";
}
void saveScLangPath() {
    const auto file = scConfigPath();
    if (file.empty()) return;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) out << g_scLangPath;
    else logLine("[caw] Could not save the SuperCollider language path.");
}
std::filesystem::path scSynthConfigPath() {
    const std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
    return home.empty() ? std::filesystem::path() : std::filesystem::u8path(home) / ".caw_scsynth_path";
}
void saveScSynthPath() {
    const auto file = scSynthConfigPath();
    if (file.empty()) return;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) out << g_scSynthPath;
    else logLine("[caw] Could not save the libscsynth path.");
}
std::string scLangExePath() {
    std::error_code ec;
    if (!g_scLangPath.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(g_scLangPath), ec) && !ec)
        return g_scLangPath;
    const std::string found = plat::superColliderSclang();
    return found.empty() ? std::string("sclang") : found;
}

// Writes CAW's private SuperCollider configuration into the working directory. sclang is started
// with -l <this file>, so ONLY the SCClassLibrary shipped next to sclang.exe is compiled. Without
// -l, sclang also reads the user's default sclang_conf.yaml (quark include paths): a machine with
// broken or duplicated quark folders then fails CAW's class library boot with "Invalid utf8" /
// "Duplicate identifier" errors even though the user's own SuperCollider IDE works fine. With the
// private config, the user's installation is never read - and never modified. Returns the path,
// or an empty string when the file could not be written (sclang then falls back to the old call).
std::string writePrivateSclangConf() {
    const auto dir = workDir();
    if (dir.empty()) return std::string();
    auto forward = [](std::string value) {
        for (char& c : value) if (c == '\\') c = '/';   // YAML treats '\' as an escape; sclang accepts '/'
        return value;
    };
    std::string conf =
        "# CAW private SuperCollider configuration.\n"
        "# sclang is started with -l <this file>: ONLY the SCClassLibrary shipped with CAW is compiled.\n"
        "# Your own SuperCollider installation, quarks and Extensions are not read and not modified.\n"
        "includePaths: []\n"
        "excludePaths:\n";
    auto addExclude = [&](const std::string& base) {
        if (!base.empty()) conf += "  - \"" + forward(base) + "/SuperCollider/Extensions\"\n";
    };
    if (plat::isWindows) {
        addExclude(plat::getEnv("LOCALAPPDATA"));
        addExclude(plat::getEnv("APPDATA"));
    } else {
        const std::string home = plat::getEnv("HOME");
        if (!home.empty()) {
            conf += "  - \"" + forward(home) + "/.local/share/SuperCollider/Extensions\"\n";
            conf += "  - \"" + forward(home) + "/Library/Application Support/SuperCollider/Extensions\"\n";
        }
    }
    conf += "postfixHead: []\n";
    const auto path = dir / "caw_sclang_conf.yaml";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { logLine("[caw] Could not write the private SuperCollider configuration (working directory not writable)."); return std::string(); }
    out << conf;
    logLine("[caw] SuperCollider private config: " + path.u8string() +
            " - your own SuperCollider install, quarks and Extensions are not read or modified.");
    return path.u8string();
}
std::string scSynthLibraryPath() {
    std::error_code ec;
    auto valid = [&](const std::filesystem::path& path) {
        ec.clear();
        return !path.empty() && std::filesystem::is_regular_file(path, ec) && !ec;
    };
    if (valid(std::filesystem::u8path(g_scSynthPath))) return g_scSynthPath;
    const std::string envPath = plat::getEnv("CAW_SCSYNTH");
    if (valid(std::filesystem::u8path(envPath))) return envPath;
    const std::string name = plat::isWindows ? "libscsynth.dll" : plat::isMac ? "libscsynth.dylib" : "libscsynth.so";
    const auto sibling = std::filesystem::u8path(scLangExePath()).parent_path() / name;
    if (!sibling.parent_path().empty() && valid(sibling)) return sibling.u8string();

    std::vector<std::filesystem::path> roots;
    const std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
    if (!home.empty()) {
        const auto cache = std::filesystem::u8path(home) / ".caw_deps" / "supercollider";
        roots.push_back(cache / "runtime-x64");
        roots.push_back(cache / "build-x64");
    }
    for (const auto& root : roots) {
        ec.clear();
        if (!std::filesystem::is_directory(root, ec) || ec) continue;
        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file(ec) && !ec && it->path().filename().u8string() == name)
                return it->path().u8string();
        }
    }
    return {};
}

std::string scBootstrap(int outputChannels, int serverPort) {
    const int physicalChannels = std::max(1, outputChannels);
    const int serverOutputBuses = physicalChannels + 2 * ScEngine::maxTrackSlots;
    std::ostringstream out;
    out << "(\n"
        << "o = ServerOptions.new; o.maxLogins = 32; o.numOutputBusChannels = " << serverOutputBuses
        << "; o.numInputBusChannels = 0; o.numAudioBusChannels = " << std::max(1024, serverOutputBuses + 256) << ";\n"
        << "~cawserveraddr = NetAddr(\"127.0.0.1\", " << serverPort << ");\n"
        // CAW owns the embedded server's life; sclang's own alive-check must not decide whether it "is running" (it flips
        // to false whenever the audio callback has not pumped the server for a moment, and then Function.play refuses
        // with "Server 'caw' not running"). So: stop the alive thread and keep serverRunning true.
        << "~cawforcerunning = { try { s.stopAliveThread } { |e| e.reportError }; try { s.statusWatcher.serverRunning_(true) } { |e| e.reportError }; };\n"
        << "~cawreadyosc = OSCFunc({ |msg| ~cawreadyosc.free; ~cawforcerunning.value; \"CAW_SC_READY\".postln; }, '/status.reply', ~cawserveraddr);\n"
        << "s = Server.remote(\\caw, ~cawserveraddr, o);\n"
        << "Server.default = s;\n"
        << "s.notify; s.startAliveThread(0.1);\n"
        << "s.sendStatusMsg;\n"
        << "~cawgroups = IdentityDictionary.new; ~cawcancelled = IdentityDictionary.new; ~cawgeneration = 0;\n"
        << "~cawtrackbuses = IdentityDictionary.new;\n"
        << "OSCdef(\\cawStart, { |msg|\n"
        << "    var id = msg[1].asInteger, path = msg[2].asString, trackSlot = msg[7].asInteger, clipId = msg[8].asInteger;\n"
        << "    var delay = msg[3].asFloat.max(0), duration = msg[4].asFloat.max(0.001);\n"
        << "    var p4 = msg[5].asFloat, p5 = msg[6].asFloat, outBus = 0, generation;\n"
        << "    generation = ~cawgeneration;\n"
        << "    if ((trackSlot >= 0) and: { trackSlot < " << ScEngine::maxTrackSlots << " }) {\n"
        << "        outBus = ~cawtrackbuses[trackSlot] ?? { var index = " << physicalChannels << " + (trackSlot * 2); ~cawtrackbuses[trackSlot] = index; index; };\n"
        << "    };\n"
        << "    ~cawcancelled[id] = false;\n"
        << "    SystemClock.sched(delay, {\n"
        << "        var group;\n"
        << "        if ((generation == ~cawgeneration) and: { ~cawcancelled[id] != true }) {\n"
        << "            group = Group.tail(s); ~cawgroups[id] = group; ~cawgroup = group; ~cawoutbus = outBus;\n"
        << "            ~cawp4 = p4; ~cawp5 = p5; ~cawnoteduration = duration;\n"
        << "            if (s.serverRunning.not) { ~cawforcerunning.value; \"CAW_SC_FORCED_RUNNING\".postln; };\n"
        << "            (\"CAW_SC_CLIP_BEGIN \" ++ id.asString ++ \" \" ++ clipId.asString).postln;\n"
        << "            try { thisProcess.interpreter.executeFile(path); } { |error| (\"CAW_SC_CLIP_ERROR \" ++ id.asString ++ \" \" ++ error.asString).postln; error.reportError; };\n"
        << "            (\"CAW_SC_CLIP_END \" ++ id.asString).postln;\n"
        << "            SystemClock.sched(duration, { group.free; ~cawgroups.removeAt(id); ~cawcancelled.removeAt(id); nil; });\n"
        << "        };\n"
        << "        nil;\n"
        << "    });\n"
        << "}, '/caw/start');\n"
        << "OSCdef(\\cawStop, { |msg|\n"
        << "    var id = msg[1].asInteger, group = ~cawgroups[msg[1].asInteger];\n"
        << "    ~cawcancelled[id] = true; if (group.notNil) { group.free; ~cawgroups.removeAt(id); };\n"
        << "}, '/caw/stop');\n"
        << "OSCdef(\\cawStopAll, { |msg| ~cawgeneration = ~cawgeneration + 1; ~cawgroups.values.do { |group| group.free; }; ~cawgroups.clear; ~cawcancelled.clear; }, '/caw/stopAll');\n"
        << "OSCdef(\\cawResume, { |msg| s.sendStatusMsg; ~cawforcerunning.value; \"CAW_SC_READY\".postln; }, '/caw/resume');\n"
        << "~cawr = IdentityDictionary.new;   // CAW hub (caws/cawr): SC clips read ~cawr[\\name]\n"
        << "~cawbuses = IdentityDictionary.new;   // one control bus per automation channel, CAW keeps it current like the lane dots\n"
        << "~cawbus = { |name| var key = name.asSymbol, bus = ~cawbuses[key]; if (bus.isNil) { bus = Bus.control(s, 1); bus.value = ~cawr[key] ? 0; ~cawbuses[key] = bus; }; bus; };   // cawbus: a live control bus for a lane, read it with In.kr(bus.index, 1)\n"
        << "OSCdef(\\cawHub, { |msg| var key = msg[1].asSymbol, value = msg[2], bus = ~cawbuses[key]; ~cawr[key] = value; if (bus.notNil) { bus.value = value; }; }, '/caw/hub');\n"
        << "~cawget = { |name| if (UGen.buildSynthDef.notNil) { In.kr(~cawbus.(name).index, 1) } { ~cawr[name.asSymbol] ? 0 } };   // cawget: inside a synth it is a live k-rate control signal on the lane bus (like Csound cawget); outside it reads the lane value once\n"
        << "OSCdef(\\cawQuit, { |msg|\n"
        << "    ~cawgeneration = ~cawgeneration + 1; ~cawgroups.values.do { |group| group.free; }; ~cawgroups.clear; ~cawcancelled.clear;\n"
        << "    ~cawtrackbuses.clear;\n"
        << "    SystemClock.sched(0.05, { 0.exit; nil; });\n"
        << "}, '/caw/quit');\n"

        << ")\n";
    return out.str();
}

// Csound, SuperCollider and libpd each ship DLLs with the same names (libsndfile-1.dll, libwinpthread-1.dll ...), and a
// process keeps only one copy per name. Load the newest copy of every shared name first, before any engine loads its
// own, so a newer engine (Csound 7) never ends up with an older copy that SuperCollider brought along.
void shareEngineDlls() {
    const std::string csoundExe = csoundExePath();
    std::vector<std::string> dirs;
    auto addFolderOf = [&](const std::string& file) {
        if (!file.empty()) dirs.push_back(std::filesystem::u8path(file).parent_path().u8string());
    };
    addFolderOf(csoundExe);
    addFolderOf(scSynthLibraryPath());
    addFolderOf(g_pdLibraryPath.empty() ? plat::bundledLibpd() : g_pdLibraryPath);
    std::string key;
    for (const std::string& dir : dirs) key += dir + "|";
    static std::string lastKey;
    if (key == lastKey) return;
    lastKey = key;
    for (const std::string& line : plat::preloadNewestSharedDlls(dirs)) logLine(line);
    // A Csound copy inside CAW's own folder is not on PATH. Csound loads its opcode plugins with a plain
    // LoadLibrary, so plugin dependencies can only come from DLLs already in this process (or on PATH).
    // libpd, loaded later for Csound + PD, ships its own copies of several runtime DLL names, and Windows
    // gives every name only to the first engine that asks - that is how Csound + PD could fail with the
    // copied Csound while each engine alone worked fine. Load every DLL of the Csound folder and of its
    // plugin folder by full path, so Csound gets its own copies exactly like in a Csound-only run.
    for (const std::string& line : plat::preloadAllDllsOf(std::filesystem::u8path(csoundExe).parent_path().u8string())) logLine(line);
    for (const std::string& line : plat::preloadAllDllsOf(plat::csoundPluginsDir(csoundExe))) logLine(line);
    // Always visible, so it is immediately obvious WHICH build is running and WHICH Csound was found
    // (a stale exe once made the same error reappear for days).
    logLine("[caw] Engine DLL setup done - Csound: " + (csoundExe.empty() ? std::string("(none found)") : csoundExe));
}

bool startScEngine() {
    auto failed = [](const std::string& text) {
        logLine("[caw] " + text);
        playNotice(text, "SuperCollider failed: see log");
        return false;
    };
    if (!g_langSc) return failed("Enable SuperCollider in Settings > Languages first.");
    if (g_scEngineOn) return true;
    if ((g_playing && !g_useCsoundApi) || g_testSound.running() || g_renderActive) {
        return failed("Stop the legacy Csound process, test sound, or export before starting embedded SuperCollider.");
    }
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    const std::string library = scSynthLibraryPath();
    if (library.empty()) {
        return failed("libscsynth was not found. Choose it under Settings > Programs, or build the CAW SuperCollider runtime.");
    }
    shareEngineDlls();
    std::string error;
    g_scEngine.setGain(g_proj.cs.masterGain);
    const int scOutputChannels = ScEngine::serverOutputChannels;
    const bool resumeLanguage = g_scEngine.canReuseWorld(library, g_proj.cs.sr, scOutputChannels) &&
                                g_scEngine.running();
    if (g_scEngine.active() && !resumeLanguage) g_scEngine.stopLanguage();
    if (!g_scEngine.startEmbedded(library, g_proj.cs.sr, scOutputChannels, error)) {
        return failed("Embedded SuperCollider server failed: " + error);
    }
    if (!resumeLanguage) {
        std::error_code ec;
        const auto bootstrap = workDir() / "caw_supercollider_boot.scd";
        std::filesystem::create_directories(bootstrap.parent_path(), ec);
        if (ec || !writeTextFile(bootstrap.string(), scBootstrap(scOutputChannels, g_scEngine.boundServerPort()))) {
            g_scEngine.setRendering(false);
            return failed("Could not write the SuperCollider startup script.");
        }
            const std::string scConf = writePrivateSclangConf();
            if (!g_scEngine.start(scLangExePath(), bootstrap.string(), error, scConf)) {
            g_scEngine.setRendering(false);
            return failed("sclang did not start: " + error);
        }
    }
    g_scEngineOn = true;
    g_scReady = false;
    g_scActiveInstances.clear();
    g_scTimelineSig = 0;
    g_scPlanT = 0.0;
    g_scLastPDTimelineTime = -1.0;
    g_scStartTime = std::chrono::steady_clock::now();
    if (!scHasSharedAudioOwner() && !startScSoloOutput(error)) {
        g_scEngine.setRendering(false);
        g_scEngineOn = false;
        return failed("SuperCollider could not open CAW's audio output: " + error);
    }
    if (resumeLanguage && !g_scEngine.resumeLanguage()) {
        closeScSoloOutput();
        g_scEngine.setRendering(false);
        g_scEngineOn = false;
        return failed("Could not resume the existing sclang connection.");
    }
    logLine("[caw] Embedded SuperCollider is running inside CAW's shared audio callback; connecting sclang...");
    return true;
}

void stopScEngine() {
    if (!g_scEngineOn) return;
    closeScSoloOutput();
    (void)g_scEngine.stopAll();
    g_scEngine.setRendering(false);
    g_scEngineOn = false; g_scReady = false; g_scPendingClipId = 0;
    g_scActiveInstances.clear();
    g_scTimelineSig = 0;
    if (g_playNotice.find("SuperCollider timed out") != std::string::npos) {
        g_playNotice.clear();
        g_playNoticeT = 0.0;
    }
    if (g_timelineRun && !engineIsUp() && !g_pdTimelineMode && !vstSolo().audible()) g_timelineRun = false;
    ensureVstSoloOutput();   // the VST engine gets its own stream back; PD / Csound keep theirs
    logLine("[caw] SuperCollider stopped.");
}

int newScInstanceId() {
    if (g_scNextInstanceId < 1 || g_scNextInstanceId >= 2000000000) g_scNextInstanceId = 1;
    while (g_scActiveInstances.count(g_scNextInstanceId)) ++g_scNextInstanceId;
    return g_scNextInstanceId++;
}

int scTrackSlot(int trackId) {
    int slot = 0;
    for (const Track& track : g_proj.tracks) if (track.kind == 5) {
        if (track.id == trackId) return slot < ScEngine::maxTrackSlots ? slot : -1;
        ++slot;
    }
    return -1;
}
bool sendScClip(const Clip& clip, double delay, double duration, double p4, double p5, int trackSlot) {
    if (!g_scReady || !g_scEngine.running()) return false;
    if (clip.code.empty()) { logLine("[caw] SuperCollider clip " + clip.name + " has no code."); return false; }
    std::string routedCode = clip.code;
    static const std::regex playTarget(R"re(\.play\s*\(\s*target\s*:\s*~caw[Gg]roup)re");
    static const std::regex directOut(R"re(\bOut\.ar\(\s*0\s*,)re");
    routedCode = std::regex_replace(routedCode, playTarget,
                                    ".play(outbus: ~cawoutbus, target: ~cawgroup");
    routedCode = std::regex_replace(routedCode, directOut, "Out.ar(~cawoutbus,");
    std::error_code ec;
    const auto path = workDir() / ("caw_sc_clip_" + std::to_string(clip.id) + ".scd");
    const auto cached = g_scClipFileCache.find(clip.id);
    ec.clear();
    const bool fileMissing = !std::filesystem::is_regular_file(path, ec) || ec;
    if (fileMissing || cached == g_scClipFileCache.end() || cached->second != routedCode) {
        if (!writeTextFile(path.string(), routedCode)) {
            logLine("[caw] Could not write SuperCollider clip file: " + path.string());
            return false;
        }
        g_scClipFileCache[clip.id] = routedCode;
    }
    std::string oscPath = path.generic_u8string();
    const int instance = newScInstanceId();
    const float delaySec = static_cast<float>(std::max(0.0, delay));
    const float durationSec = static_cast<float>(std::max(0.001, duration));
    if (!g_scEngine.startClip(instance, oscPath, delaySec, durationSec,
                              static_cast<float>(p4), static_cast<float>(p5), trackSlot, clip.id)) {
        logLine("[caw] Could not send the SuperCollider clip to the language process.");
        return false;
    }
    ScActiveInstance active;
    active.clipId = clip.id;
    active.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(
        static_cast<long long>((delaySec + durationSec) * 1000.0f + 500.0f));
    g_scActiveInstances[instance] = active;
    return true;
}

void stopScClip(int clipId) {
    for (auto it = g_scActiveInstances.begin(); it != g_scActiveInstances.end();) {
        if (it->second.clipId == clipId) {
            (void)g_scEngine.stopClip(it->first);
            it = g_scActiveInstances.erase(it);
        } else ++it;
    }
}
void stopAllScClips() {
    if (g_scEngine.active()) (void)g_scEngine.stopAll();
    g_scActiveInstances.clear();
}
bool scClipRunning(int clipId) {
    if (g_scPendingClipId == clipId) return true;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_scActiveInstances.begin(); it != g_scActiveInstances.end();) {
        if (it->second.until <= now) it = g_scActiveInstances.erase(it);
        else { if (it->second.clipId == clipId) return true; ++it; }
    }
    return false;
}
void launchScClip(const Clip& clip) {
    if (g_scReady) {
        const double p4 = clip.midiEnabled ? 60.0 : 0.0;
        const double p5 = clip.midiEnabled ? 100.0 : 0.0;
        int laneIndex = -1;
        findClip(clip.id, &laneIndex);
        const int slot = laneIndex >= 0 ? scTrackSlot(g_proj.tracks[static_cast<size_t>(laneIndex)].id) : -1;
        if (!sendScClip(clip, 0.0, clip.dur, p4, p5, slot)) logLine("[caw] SuperCollider clip could not be launched.");
    } else if (!g_scEngineOn) {
        g_scPendingClipId = clip.id;
        if (!startScEngine()) g_scPendingClipId = 0;
    } else {
        g_scPendingClipId = clip.id;
        playNotice("SuperCollider is still booting. The clip will start when the server is ready.", "SuperCollider is booting...");
    }
}

unsigned long long scTimelineSignature(const Project& project) {
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&h](unsigned long long value) { h ^= value; h *= 1099511628211ULL; };
    auto mixText = [&](const std::string& text) { for (unsigned char c : text) mix(c); mix(0xFF); };
    for (const Track& track : project.tracks) {
        if (track.kind != 5) continue;
        mix(static_cast<unsigned long long>(track.id)); mix(track.muted); mix(track.solo);
        for (const Clip& clip : track.clips) {
            mix(static_cast<unsigned long long>(clip.id));
            mix(static_cast<unsigned long long>(std::llround(clip.start * 1000.0)));
            mix(static_cast<unsigned long long>(std::llround(clip.dur * 1000.0)));
            mix(static_cast<unsigned long long>(std::max(1, clip.runs))); mix(clip.muted); mix(clip.midiEnabled);
            mixText(clip.code);
            mixText(clip.pFieldRules);
            for (const MidiClip& midi : track.midiClips) if (midi.clipId == clip.id) {
                mix(static_cast<unsigned long long>(midi.id));
                mix(static_cast<unsigned long long>(std::llround(midi.start * 1000.0)));
                mix(static_cast<unsigned long long>(std::llround(midi.dur * 1000.0)));
                mix(midi.muted);
                for (const MidiNote& note : midi.notes) {
                    mix(static_cast<unsigned long long>(std::llround(note.start * 1000.0)));
                    mix(static_cast<unsigned long long>(std::llround(note.dur * 1000.0)));
                    mix(static_cast<unsigned long long>(note.pitch));
                    mix(static_cast<unsigned long long>(note.velocity));
                }
            }
        }
    }
    return h;
}

void pollScAutomation();   // defined below pollCawHub: feed the SC lanes (~cawget) while the timeline rolls

void scheduleScTimeline(double from) {
    stopAllScClips();
    g_scLaneSent.clear();   // the lanes restart: ~cawr gets fresh values even if one did not change
    pollScAutomation();
    if (!g_scReady || !g_timelineRun) return;
    bool anySolo = false;
    for (const Track& track : g_proj.tracks) if (track.solo && !track.muted) anySolo = true;
    const bool looping = timeline_loop::valid(g_proj);
    const double loopEnd = looping ? g_proj.loopEnd : std::numeric_limits<double>::infinity();
    const double lead = std::max(0.0, (double)std::max(0, g_proj.cs.bufHW) / std::max(1000, g_proj.cs.sr) + (double)g_proj.cs.syncMs / 1000.0);
    for (const Track& track : g_proj.tracks) {
        if (track.kind != 5 || track.muted || (anySolo && !track.solo)) continue;
        for (const Clip& clip : track.clips) {
            if (clip.muted || clip.code.empty()) continue;
            const int runs = std::min(256, std::max(1, clip.runs));
            for (int run = 0; run < runs; ++run) {
                const double start = clip.start + clip.dur * (double)run / runs;
                const double end = std::min(clip.start + clip.dur * (double)(run + 1) / runs, loopEnd);
                if (end <= from || start >= loopEnd) continue;
                const double actualStart = std::max(start, from);
                const double duration = end - actualStart;
                if (duration <= 0.001) continue;
                const double p4 = clip.midiEnabled ? 60.0 : 0.0;
                const double p5 = clip.midiEnabled ? 100.0 : 0.0;
                const int slot = scTrackSlot(track.id);
                (void)sendScClip(clip, lead + std::max(0.0, start - from), duration, p4, p5, slot);
            }
            for (const MidiNote& note : clipMidiNotes(g_proj, clip.id)) {
                const double start = note.start;
                const double end = start + note.dur;
                if (end <= from || start >= loopEnd) continue;
                const double actualStart = std::max(start, from);
                const double duration = std::min(end, loopEnd) - actualStart;
                if (duration <= 0.001) continue;
                (void)sendScClip(clip, lead + std::max(0.0, start - from), duration,
                                 static_cast<double>(note.pitch), static_cast<double>(note.velocity),
                                 scTrackSlot(track.id));
            }
        }
    }
    g_scTimelineSig = scTimelineSignature(g_proj);
    g_scPlanT = 0.0;
}

void pollScEngine(float /*dt*/) {
    if (!g_scEngine.active()) return;
    for (const std::string& line : g_scEngine.takeOutput()) {
        // CAWS <name> <value> = a SuperCollider clip sends into CAW's shared hub (caws/cawr).
        if (line.rfind("CAWS ", 0) == 0) {
            std::istringstream parsed(line.substr(5));
            std::string name;
            double value = 0;
            if (parsed >> name >> value) g_cawHub.send(name, value);
            continue;
        }
        if (line.rfind("CAW_SC_CLIP_BEGIN ", 0) == 0) {
            std::istringstream parsed(line.substr(18));
            parsed >> g_scOutputInstanceId;
            parsed >> g_scOutputClipId;
            if (g_scOutputClipId <= 0) {
                const auto active = g_scActiveInstances.find(g_scOutputInstanceId);
                if (active != g_scActiveInstances.end()) g_scOutputClipId = active->second.clipId;
            }
            g_scErrorClipId = -1;
            g_scErrorContextLines = 0;
            g_scErrorLogIndex = -1;
            g_scErrorHeadlineSeen = false;
            g_scErrorMessage.clear();
            continue;
        }
        if (line.rfind("CAW_SC_CLIP_END ", 0) == 0) {
            std::istringstream parsed(line.substr(16));
            int instance = 0;
            parsed >> instance;
            if (instance == g_scOutputInstanceId) {
                g_scOutputInstanceId = 0;
                g_scOutputClipId = 0;
                g_scErrorClipId = -1;
                g_scErrorContextLines = 0;
                g_scErrorLogIndex = -1;
                g_scErrorHeadlineSeen = false;
                g_scErrorMessage.clear();
            }
            continue;
        }
        if (line.rfind("CAW_SC_CLIP_ERROR ", 0) == 0) {
            std::istringstream parsed(line.substr(18));
            int instance = 0;
            parsed >> instance;
            std::string message;
            std::getline(parsed, message);
            while (!message.empty() && std::isspace(static_cast<unsigned char>(message.front())))
                message.erase(message.begin());
            const auto active = g_scActiveInstances.find(instance);
            const int clipId = active != g_scActiveInstances.end()
                ? active->second.clipId
                : (instance == g_scOutputInstanceId ? g_scOutputClipId : 0);
            startScError(clipId, message);
            continue;
        }
        if (!line.empty()) {
            const int contextClip = g_scErrorContextLines > 0 ? g_scErrorClipId : 0;
            const int clipId = contextClip > 0 ? contextClip : g_scOutputClipId;
            const int reportedLine = sc_diagnostics::reportedLine(line);
            std::string low = line;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            const bool errorHeadline = low.find("error:") != std::string::npos ||
                                       low.find("exception:") != std::string::npos;
            const bool warningHeadline = low.find("warning:") != std::string::npos;
            if (errorHeadline && clipId > 0) {
                if (contextClip <= 0 || g_scErrorLogIndex < 0) startScError(clipId, line);
                else if (!g_scErrorHeadlineSeen) updateScErrorHeadline(line);
                g_scErrorHeadlineSeen = true;
                g_scErrorContextLines = 16;
                if (reportedLine > 0) {
                    setScErrorLocation(reportedLine, false, line);
                } else if (const Clip* clip = findClip(clipId)) {
                    const auto likely = sc_diagnostics::inferRuntimeLine(clip->code, line);
                    if (likely.line > 0) setScErrorLocation(likely.line, likely.approximate, line);
                }
                continue;
            }
            if (contextClip > 0 && g_scErrorContextLines > 0) {
                --g_scErrorContextLines;
                if (reportedLine > 0) setScErrorLocation(reportedLine, false, line);
                LogLine trace = classifyLog("[SuperCollider] " + line);
                trace.level = kLogMessage;  // keep backtrace detail out of the incident count
                if (const Clip* clip = findClip(contextClip)) {
                    fillLoc(trace, clip->name, reportedLine, line);
                    trace.lineApprox = false;
                }
                pushLog(std::move(trace));
                continue;
            }
            if (warningHeadline && clipId > 0) {
                LogLine warning = classifyLog("[SuperCollider] " + line);
                warning.level = kLogImportant;
                if (const Clip* clip = findClip(clipId)) fillLoc(warning, clip->name, reportedLine, line);
                pushLog(std::move(warning));
                continue;
            }
        }
        if (line.find("CAW_SC_READY") != std::string::npos) {
            if (!g_scReady) {
                g_scReady = true;
                logLine("[caw] sclang connected to CAW's embedded SuperCollider server. Audio shares the CAW output stream.");
                if (g_scPendingClipId > 0) {
                    const int pending = g_scPendingClipId;
                    g_scPendingClipId = 0;
                    if (Clip* clip = findClip(pending)) launchScClip(*clip);
                }
            }
        } else if (!line.empty()) logLine("[SuperCollider] " + line);
    }
    if (g_scEngineOn && !g_scReady &&
        std::chrono::steady_clock::now() - g_scStartTime > std::chrono::seconds(10)) {
        stopScEngine();
        g_scEngine.stopLanguage();
        logLine("[caw] SuperCollider did not answer its startup status request. Check the sclang/libscsynth build and local UDP ports.");
        playNotice("SuperCollider startup timed out. Check the log and SuperCollider build.", "SuperCollider timed out: see log");
        return;
    }
    for (auto it = g_scActiveInstances.begin(); it != g_scActiveInstances.end();) {
        if (it->second.until <= std::chrono::steady_clock::now()) it = g_scActiveInstances.erase(it);
        else ++it;
    }
    if (g_scEngine.finished()) {
        const int code = g_scEngine.exitCode();
        closeScSoloOutput();
        (void)g_scEngine.stopAll();
        g_scEngine.stopLanguage();
        g_scEngine.setRendering(false);
        g_scEngineOn = false; g_scReady = false; g_scPendingClipId = 0;
        g_scActiveInstances.clear();
        if (g_timelineRun && !engineIsUp() && !g_pdTimelineMode && !vstSolo().audible()) g_timelineRun = false;
        ensureVstSoloOutput();   // the VST engine carries the session now
        logLine(code == 0 ? "[caw] SuperCollider ended." : "[caw] SuperCollider ended with exit code " + std::to_string(code) + ". See its messages above.");
    }
}

// CAW hub (caws/cawr): deliver shared bus values to the SuperCollider language.
// Csound channels and the PD instances are handled on their own engine threads;
// sclang is fed from here. SC clips read the values as ~cawr[\name].
void pollCawHub() {
    static std::uint64_t seen = 0;
    std::map<std::string,double> values;
    if (!g_cawHub.take(seen, values)) return;
    if (!g_scEngine.running()) return;
    for (const auto& entry : values)
        g_scEngine.sendHub(entry.first, static_cast<float>(entry.second));
}

// SuperCollider automation: the language has no per-tick engine feed like the PD cawget-<name>
// receivers, so while the timeline rolls CAW evaluates every SuperCollider lane at the playhead
// and mirrors the value into the language (~cawr, read by ~cawget) over the /caw/hub OSC path.
// Lane values stay in the lane's own range (0..1 unless the code declares a range), exactly like
// the PD feed; the shared caws/cawr hub itself is not touched.
void pollScAutomation() {
    if (!g_scReady || !g_scEngine.running() || !g_timelineRun) return;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 5) continue;
        for (const auto& lane : t.automation) {
            const std::string& chn = lane.first;
            const std::vector<AutoPoint>& points = lane.second;
            if (points.empty() || !autoLaneActive(t, chn)) continue;
            double lo = 0.0, hi = 1.0, initial = 0.5;
            const auto range = t.automationRanges.find(chn);
            if (range != t.automationRanges.end()) {
                lo = range->second.min;
                hi = range->second.max > lo ? range->second.max : lo + 1.0;
                initial = range->second.value;
            }
            const float value = static_cast<float>(autoValueAt(points, g_play, initial));
            const bool first = g_scLaneSent.find(chn) == g_scLaneSent.end();
            float& seen = g_scLaneSent[chn];
            if (seen == value) continue;
            seen = value;
            if (first) logLine("[caw] SC automation: lane \"" + chn + "\" streams into the language (~cawget is live)");
            g_scEngine.sendHub(chn, value);
        }
    }
}

// Every hub channel name used by the clip code (caws sends / cawr reads). The runner
// resolves the matching "caws/<name>" Csound channels against this list. PD patches
// need no declaration: their caws outputs arrive through the control queue.
std::vector<std::string> collectCawHubNames() {
    static const std::regex sendRe(R"re((?:^|\n)[ \t]*caws[ \t]+[^,;\r\n]+,[ \t]*"([A-Za-z0-9_/-]+)")re");
    static const std::regex recvRe(R"re((?:^|\n)[ \t]*[ki][A-Za-z0-9_]+[ \t]+cawr[ \t]+"([A-Za-z0-9_/-]+)")re");
    std::set<std::string> names;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips) {
            for (std::sregex_iterator it(c.code.begin(), c.code.end(), sendRe), end; it != end; ++it)
                names.insert((*it)[1].str());
            for (std::sregex_iterator it(c.code.begin(), c.code.end(), recvRe), end; it != end; ++it)
                names.insert((*it)[1].str());
        }
    return {names.begin(), names.end()};
}

bool testPdLibrary(std::string& error) {
    if (g_pdPreview.active()) { error = "Stop PD playback before testing the library."; return false; }
    std::string patch;
    if (!pd_test_patch::create(workDir(), patch, error)) return false;
    PdBackend backend;
    return backend.open(g_pdLibraryPath, patch, g_proj.cs.sr, 2, error);
}

// ---------------------------------------------------------------- undo / redo
bool historyBusy() {
    return ImGui::IsAnyItemActive() || ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) ||
           g_drag != Drag::None || g_autoMode != AutoMode::None || g_midiDragTrackId >= 0;
}

void historyReset() {
    g_undoStack.clear();
    g_redoStack.clear();
    g_histCommitted = serializeProject(g_proj);
    g_histPending.clear();
    g_histIdle = 0.f;
}

void historyCommit(std::string now) {
    g_undoStack.push_back(std::move(g_histCommitted));
    if (g_undoStack.size() > kHistoryLimit) g_undoStack.erase(g_undoStack.begin());
    g_redoStack.clear();
    g_histCommitted = std::move(now);
    g_histPending.clear();
    g_histIdle = 0.f;
}

// Called once per frame: a finished edit (mouse released, field left, typing paused) becomes one undo step.
void historyTick(float dt) {
    if (!cawEnv("CAW_HIST_EVERY_FRAME")) {
        static float acc = 0.f;
        static float interactionGrace = 0.f;
        const ImGuiIO& io = ImGui::GetIO();
        const bool interaction = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) ||
            io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f || io.MouseWheel != 0.f ||
            io.InputQueueCharacters.Size > 0;
        interactionGrace = interaction ? 1.f : std::max(0.f, interactionGrace - dt);
        acc += dt;
        // Continue the existing editing cadence while active; once idle,
        // serialize at most once a second. Undo/close still flush immediately.
        if (acc < (interactionGrace > 0.f ? 0.25f : 1.f)) return;
        dt = acc;
        acc = 0.f;
    }
    if (historyBusy()) { g_histIdle = 0.f; return; }
    std::string now = serializeProject(g_proj);
    if (now == g_histCommitted) { g_histPending.clear(); g_histIdle = 0.f; return; }
    if (ImGui::GetIO().WantTextInput || g_textEditorFocused) {  // group keystrokes into one step
        if (now != g_histPending) { g_histPending = std::move(now); g_histIdle = 0.f; return; }
        g_histIdle += dt;
        if (g_histIdle < 0.8f) return;
    }
    historyCommit(std::move(now));
}

void historyFlush() {
    std::string now = serializeProject(g_proj);
    if (now != g_histCommitted) historyCommit(std::move(now));
}

void restoreSnapshot(const std::string& data) {
    Project restored;
    std::string error;
    if (!deserializeProject(restored, data, error)) { logLine("[caw] Undo/redo failed: " + error); return; }
    std::set<int> laneEditors, laneDelete, clipEditors, clipMidi;
    for (const Track& t : g_proj.tracks) {
        if (t.editorOpen) laneEditors.insert(t.id);
        if (t.deleteSelected) laneDelete.insert(t.id);
        for (const Clip& c : t.clips) {
            if (c.editorOpen) clipEditors.insert(c.id);
        }
        for (const MidiClip& mc : t.midiClips) if (mc.editorOpen) clipMidi.insert(mc.id);
    }
    int selLaneId = -1, selClipId = -1;
    if (g_selTrack >= 0 && g_selTrack < (int)g_proj.tracks.size()) selLaneId = g_proj.tracks[(size_t)g_selTrack].id;
    if (const Clip* c = selectedClip()) selClipId = c->id;

    g_proj = std::move(restored);
    g_nextTrackId = 1;
    g_nextClipId = 1;
    g_selTrack = g_selClip = -1;
    for (int ti = 0; ti < (int)g_proj.tracks.size(); ++ti) {
        Track& t = g_proj.tracks[(size_t)ti];
        t.editorOpen = laneEditors.count(t.id) > 0;
        t.deleteSelected = laneDelete.count(t.id) > 0;
        g_nextTrackId = std::max(g_nextTrackId, t.id + 1);
        if (t.id == selLaneId) g_selTrack = ti;
        for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
            Clip& c = t.clips[(size_t)ci];
            c.editorOpen = clipEditors.count(c.id) > 0;
            g_nextClipId = std::max(g_nextClipId, c.id + 1);
            if (c.id == selClipId) { g_selTrack = ti; g_selClip = ci; }
        }
    }
    for (Track& t : g_proj.tracks) for (MidiClip& mc : t.midiClips) mc.editorOpen = clipMidi.count(mc.id) > 0;
    g_selMidiClips.clear();
    g_mcDragId = -1;
    g_codeEditors.clear();
    g_clipCodeEditors.clear();
    g_drag = Drag::None;
    g_autoMode = AutoMode::None;
    g_autoDragTrackId = -1; g_autoDragPoint = -1; g_autoDragChannel.clear();
    g_midiDragTrackId = g_midiDragNote = -1;
    g_midiSelectedTrackId = g_midiSelectedNote = -1;
    validateAutoSelection();
}

bool canUndo() { return !g_undoStack.empty() || serializeProject(g_proj) != g_histCommitted; }
bool canRedo() { return !g_redoStack.empty(); }

void undo() {
    historyFlush();  // an unfinished edit becomes its own step first
    if (g_undoStack.empty()) return;
    g_redoStack.push_back(std::move(g_histCommitted));
    g_histCommitted = std::move(g_undoStack.back());
    g_undoStack.pop_back();
    restoreSnapshot(g_histCommitted);
    g_histCommitted = serializeProject(g_proj);  // normalized, so it does not look like a new edit
}

void redo() {
    if (g_redoStack.empty()) return;
    historyFlush();
    if (g_redoStack.empty()) return;  // a fresh edit cleared the redo branch
    g_undoStack.push_back(std::move(g_histCommitted));
    g_histCommitted = std::move(g_redoStack.back());
    g_redoStack.pop_back();
    restoreSnapshot(g_histCommitted);
    g_histCommitted = serializeProject(g_proj);
}

void toggleHideAllAutomation();
void showAllAutomation();
void toggleHideAllMidi();
void showAllMidi();
void runJoinTool();


// ---------------------------------------------------------------- score (SVG) : export, editor, 3-minute example
bool g_scoreOpen = false;
bool g_scoreMapOpen = false;
bool g_scoreExportOpen = false;
bool g_codeExportOpen = false;
int g_scoreExportType = 0, g_scoreExportColor = 0;

// ---- spectrogram of the rendered audio file (shown in the score when "Spectrum" is on)
ScoreSpectrum g_scoreSpec;
std::string g_scoreSpecPath, g_scoreSpecMsg;
std::filesystem::file_time_type g_scoreSpecTime;

// The render file: a relative name lives next to the saved project (or in the work folder while the project is unsaved).
const char* renderExtension(int format) {
    return format == 3 ? ".flac" : format == 4 ? ".mp3" : ".wav";
}
std::filesystem::path renderFilePath() {
    std::filesystem::path file = g_proj.cs.renderFile.empty() ? "render.wav" : g_proj.cs.renderFile;
    file.replace_extension(renderExtension(g_proj.cs.renderFormat));
    if (file.is_absolute()) return file;
    return (g_projectPath.empty() ? workDir() : std::filesystem::path(g_projectPath).parent_path()) / file;
}

std::string scoreAudioPath() { return renderFilePath().string(); }

bool refreshScoreSpectrum(bool force) {
    const std::string path = scoreAudioPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        g_scoreSpec = ScoreSpectrum();
        g_scoreSpecPath.clear();
        g_scoreSpecMsg = "No audio file yet: press Render audio first (" + path + ").";
        return false;
    }
    const auto mt = std::filesystem::last_write_time(path, ec);
    if (!force && path == g_scoreSpecPath && mt == g_scoreSpecTime && g_scoreSpec.valid()) return true;
    std::string err;
    if (analyzeScoreWav(path, g_scoreSpec, err)) {
        g_scoreSpecPath = path;
        g_scoreSpecTime = mt;
        g_scoreSpecMsg = "Spectrum of " + path + " (" + std::to_string((int)g_scoreSpec.duration) + " s).";
        return true;
    }
    g_scoreSpec = ScoreSpectrum();
    g_scoreSpecPath.clear();
    g_scoreSpecMsg = "Spectrum failed: " + err + " (render as WAV)";
    return false;
}

void generateScore(bool detailed, bool blackWhite) {
    const std::string fallback = g_projectPath.empty() ? std::string("Untitled") : std::filesystem::path(g_projectPath).stem().string();
    const std::string ext = detailed ? "html" : "svg";
    const std::filesystem::path parent = g_projectPath.empty() ? workDir() : std::filesystem::path(g_projectPath).parent_path();
    const std::string suggested = (parent / (fallback + (detailed ? "_score_full" : "_score_overview") +
                                   (blackWhite ? "_bw." : ".") + ext)).string();
    std::string chosen = plat::chooseFile(true, "Save score", detailed ? "HTML score" : "SVG score", {ext}, suggested);
    if (chosen.empty()) return;
    std::filesystem::path selected(chosen);
    std::string current = selected.extension().string();
    std::transform(current.begin(), current.end(), current.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (current != "." + ext) {
        selected += "." + ext;
        chosen = plat::chooseFile(true, "Confirm score filename", detailed ? "HTML score" : "SVG score", {ext}, selected.string());
        if (chosen.empty()) return;
        selected = chosen;
        current = selected.extension().string();
        std::transform(current.begin(), current.end(), current.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (current != "." + ext) { logLine("[caw] Score export cancelled: use the " + ext + " extension."); return; }
    }
    ScoreRenderOptions opt;
    opt.detailed = detailed;
    opt.blackWhite = blackWhite;
    ScoreProbeData probeData;
    if (g_proj.score.analyze) {   // what the last Render audio measured: shapes of the clips and the data sent to the channel "score"
        std::error_code pec;
        const auto probeTime = std::filesystem::last_write_time(scoreProbePath(), pec);
        if (!pec && loadScoreProbe(scoreProbePath(), probeData)) {
            opt.probe = &probeData;
            bool wantsData = false;
            for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) if (c.scoreDetail && parseClip(c).scoreSig && !probeData.count(c.name)) wantsData = true;
            if (wantsData) logLine("[caw] Score: a Detailed clip has no \"score\" data yet. Press Render audio first.");
            (void)probeTime;
        } else {
            logLine("[caw] Score: no measured clip data yet, shapes are drawn envelopes. Press Render audio to measure the clips.");
        }
    }
    if(!g_pdScoreData.empty()) {
        for(const auto& item:g_pdScoreData) {
            auto& dst=probeData[item.first].data;
            dst.insert(dst.end(),item.second.data.begin(),item.second.data.end());
        }
        opt.probe=&probeData;
    }
    if (g_proj.score.spectrum) {
        if (!refreshScoreSpectrum(false)) logLine("[caw] Score spectrum: " + g_scoreSpecMsg);
        opt.spectrum = g_scoreSpec.valid() ? &g_scoreSpec : nullptr;
    }
    std::vector<std::string> files;
    std::string err;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string temporary = selected.string() + ".tmp-" + std::to_string(stamp);
    if (writeScoreDocument(g_proj, fallback, opt, temporary, files, err, true) && !files.empty() &&
        replaceOutputFile(temporary, selected.string(), err)) {
        logLine("[caw] Score saved: " + selected.string());
        if (detailed) logLine("[caw] The HTML contains all score pages; print it from the browser to make a PDF.");
        plat::openWithDefaultProgram(selected.string());
    } else {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        logLine("[caw] ERROR: score: " + err);
    }
}

void exportCodeClips(code_export::Mode mode, code_export::Filter filter) {
    const std::string parent = plat::chooseFolder("Choose a folder for code export");
    if (parent.empty()) return;
    std::filesystem::path directory;
    int written = 0;
    std::string error;
    if (code_export::write(g_proj, parent, directory, written, error, mode, filter))
        logLine("[caw] Exported " + std::to_string(written) + " code file(s) to " + directory.string());
    else
        logLine("[caw] Code export stopped after " + std::to_string(written) + " file(s): " + error);
}

// ---- small score widgets (shared by the score editor, the track panel, the clip editor and the clip menu)
// ---- score settings of one lane. The same name in two lanes (track, MIDI or automation, of any track) puts them in ONE lane of the score.
void drawLaneName(const char* id, std::string& name, const std::string& fallback) {
    ImGui::SetNextItemWidth(S(220));
    inputStr(id, name, 0);
    ImGui::SameLine();
    if (name.empty()) ImGui::TextDisabled("empty = %s", fallback.c_str());
    else ImGui::TextDisabled("name in the score");
}

// A combo as wide as its longest entry (plus the arrow), never narrower than minW: the text always fits the box, whatever the font size.
float fitWidth(const char* sample, float minW = 0.f) {
    return std::max(minW, ImGui::CalcTextSize(sample).x + ImGui::GetStyle().FramePadding.x * 2.f + S(10));
}
bool comboFit(float minW, const char* id, int* cur, const char* items) {   // items: "a\0b\0c\0"
    float w = 0.f;
    for (const char* q = items; *q; q += std::strlen(q) + 1) w = std::max(w, ImGui::CalcTextSize(q).x);
    ImGui::SetNextItemWidth(std::max(minW, w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f + S(6)));
    return caw_ui::combo(id, cur, items);
}
bool comboFit(float minW, const char* id, int* cur, const char* const items[], int count) {
    float w = 0.f;
    for (int k = 0; k < count; ++k) w = std::max(w, ImGui::CalcTextSize(items[k]).x);
    ImGui::SetNextItemWidth(std::max(minW, w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f + S(6)));
    return caw_ui::combo(id, cur, items, count);
}

void drawTrackScoreMenu(Track& t) {
    ImGui::TextDisabled("Track lane in the score");
    bool shown = t.scoreKind != 3;
    if (ImGui::Checkbox("Show in the score", &shown)) t.scoreKind = shown ? 0 : 3;
    if (!shown) return;
    drawLaneName("##tr_name", t.scoreLabel, t.name);
    {   // every track decides itself: instrument (staff) or electronics (graphic), whether or not it has a Csound clip
        comboFit(S(150), "##tr_kind", &t.scoreKind, "Auto\0Instrument (staff)\0Electronics (graphic)\0");
        if (ImGui::IsItemHovered()) helpTip("Instrument: the sound comes from an instrument (staff, clef). Electronics: the score shows what is inside the audio (shape, grains, FM ...). Free choice per track.");
        if (t.scoreKind != 2) {
            ImGui::SameLine();
            comboFit(S(175), "##tr_clef", &t.scoreClef,
                         "Smart clef\0Treble\0Bass\0No clef\0Treble 8va\0Bass 8vb\0Grand staff\0");
            if (ImGui::IsItemHovered()) helpTip("Smart clef chooses treble or bass to reduce ledger lines. Treble 8va / Bass 8vb shifts the written notes by an octave. Grand staff splits notes between treble and bass staves.");
        }
    }
    comboFit(S(220), "##tr_overlap", &t.scoreOverlap, "Overlap\0Half combine\0");
    if (ImGui::IsItemHovered()) helpTip("Tracks with the same score name share ONE lane.\nOverlap: their shapes are combined into one shape (same colour: one shape, different colours: both colours).\nSeparated: drawn on top of each other at 50% opacity, not combined.");
    ImGui::SetNextItemWidth(S(220));
    inputStr("##tr_sub", t.scoreSub, 0);
    ImGui::SameLine(); ImGui::TextDisabled("small line under the name");
}

void drawMidiScoreMenu(Track& t) {
    ImGui::TextDisabled("MIDI lane in the score");
    drawLaneName("##midi_name", t.scoreMidiLabel, t.scoreLabel.empty() ? t.name : t.scoreLabel);
    comboFit(S(220), "##midi_show", &t.scoreMidiShow,
                 "Every note\0Overall contour\0Not in the score\0Pitch range band\0Every note with its name\0Fast-note clusters\0");
    if (ImGui::IsItemHovered()) helpTip("Fast-note clusters group tightly spaced notes into compact alternative notehead clusters, with a count.");
}

// Stroke width of a line in the score (straight lines, steps, dashed, curve, outlines) and the shape ratio of the mirror / wedge shapes.
// The dots, bars, dashes and break points of the other shapes scale with the stroke width.
void drawAutoStrokeControls(Track& t, const std::string& channel, int shape) {
    const auto sk = t.scoreAutoStroke.find(channel);
    float w = sk == t.scoreAutoStroke.end() ? 1.6f : (float)sk->second;
    ImGui::SetNextItemWidth(fitWidth("stroke-width 8.0", S(200)));
    if (ImGui::SliderFloat("##auto_stroke", &w, 0.3f, 8.f, "stroke-width %.1f")) t.scoreAutoStroke[channel] = std::min(8.0f, std::max(0.3f, w));
    if (ImGui::IsItemHovered()) helpTip("Stroke width of this line in the score (default 1.6).\nStraight lines, steps, dashed line, curve and the outline of the mirror shapes use it directly; the dots, bars, dashes and break points get bigger or smaller by the same ratio.");
    if (sk != t.scoreAutoStroke.end()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset##auto_stroke_reset")) t.scoreAutoStroke.erase(channel);
    }
    if (shape >= 5 && shape <= 7) {
        const auto cv = t.scoreAutoCurve.find(channel);
        float ratio = cv == t.scoreAutoCurve.end() ? 1.f : (float)cv->second;
        ImGui::SetNextItemWidth(fitWidth("shape ratio 3.00 (1 = linear)", S(200)));
        if (ImGui::SliderFloat("##auto_ratio", &ratio, 0.3f, 3.f, "shape ratio %.2f (1 = linear)")) t.scoreAutoCurve[channel] = ratio;
        if (ImGui::IsItemHovered()) helpTip("Below 1: the shape opens fast and then flattens. Above 1: it stays small and opens late.");
    }
}

void drawAutoScoreMenu(Track& t, const std::string& channel) {
    ImGui::TextDisabled("Automation %s in the score", channel.c_str());
    drawLaneName("##auto_name", t.scoreAutoNames[channel], t.scoreLabel.empty() ? t.name : t.scoreLabel);
    const auto it = t.scoreAuto.find(channel);
    int mode = it == t.scoreAuto.end() ? 1 : it->second;
    if (comboFit(S(220), "##auto_mode", &mode, "Not in the score\0Full curve\0Compact contour\0")) t.scoreAuto[channel] = mode;
    if (mode > 0) {
        const auto yz = t.scoreAutoYSize.find(channel);
        int size = yz == t.scoreAutoYSize.end() ? 100 : yz->second;
        ImGui::SetNextItemWidth(fitWidth("Y 400%", S(200)));
        if (ImGui::DragInt("##auto_y_size", &size, 1.f, 50, 400, "Y %d%%"))
            t.scoreAutoYSize[channel] = std::min(400, std::max(50, size));
        if (ImGui::IsItemHovered()) helpTip("Height of this automation in the score.");
    }
    if (mode == 1) {   // how the line looks
        const auto sh = t.scoreAutoShape.find(channel);
        int shape = sh == t.scoreAutoShape.end() ? 0 : sh->second;
        if (comboFit(S(260), "##auto_shape", &shape, "Line looks like: curve (smooth, filled)\0Steps\0Straight lines\0Line (no dots)\0Dashed line\0Mirror, smooth (grows up and down)\0Mirror, boxes (stepped)\0Hairpin (straight wedge, < >)\0Bars\0")) t.scoreAutoShape[channel] = shape;
        if (ImGui::IsItemHovered()) helpTip("How the automation is drawn in the score.\nMirror / boxes / hairpin: the value is the HEIGHT of a shape that is mirrored around a centre line, e.g. 0..127 becomes a shape that slowly gets bigger (like a crescendo).");
        drawAutoStrokeControls(t, channel, shape);
    }
}

// What a clip is in the score. Lanes, notes and automation are settings of the lanes, not of the clip.
void drawClipScoreMenu(Clip& c, Track* lane = nullptr) {
    ImGui::TextDisabled("This clip in the score");
    comboFit(S(240), "Looks like##cl_style", &c.scoreStyle, "amp+frq\0Line\0scattered\0Block\0granular\0FM\0Dots-lines\0");
    if (ImGui::IsItemHovered()) helpTip("The score shows what is INSIDE the audio, so pick the picture of the technique: sliced wave for granular sound, FM graph for FM, dots and lines for played keys / leads.");
    comboFit(S(240), "Shown as##cl_detail", &c.scoreDetail, "Overall\0Detailed\0");
    if (ImGui::IsItemHovered()) helpTip("Overall: the shape the instrument generates (amplitude shape and where its spectrum sits), measured when you press Render audio. It is not the MIDI notes.\n\nDetailed: also everything the code sends to the channel \"score\", of any type (numbers, signals, strings):\n  chnset kFreq, \"score\"   (aSig for a signal, Sname for text)\nRender audio records it, the score draws it.");
    const ParsedTrack streams = lane && lane->kind==6 ? pdParsedForUi(*lane,&c) : parseClip(c);
    if (ImGui::BeginMenu("Edit cawscore")) {
        const bool pd=lane && lane->kind==6;
        ImGui::TextDisabled(pd ? "Send numbers to [s $0-cawscore-name]" : "cawscore \"frq\", kFrq");
        if (streams.scoreStreams.empty()) ImGui::TextDisabled(pd ? "Add a score send to the saved PD patch." : "Add a cawscore line to this clip's code.");
        for (const auto& stream : streams.scoreStreams) {
            ImGui::PushID(stream.first.c_str());
            const char* kind = stream.second == 4 ? "i-rate" : stream.second == 2 ? "a-rate" : stream.second == 3 ? "S-rate" : "k-rate";
            ImGui::Text("%s  (%s)", stream.first.c_str(), kind);
            if (stream.second == 2) {
                const auto it = c.scoreDataStyle.find(stream.first);
                const int style = it == c.scoreDataStyle.end() ? 0 : it->second;
                int choice = style == 4 ? 1 : style == 3 ? 2 : 0;
                if (comboFit(S(220), "Display", &choice, "Waveform envelope\0Combined spectrum\0Hidden\0"))
                    c.scoreDataStyle[stream.first] = choice == 1 ? 4 : choice == 2 ? 3 : 0;
                ImGui::TextDisabled("Spectrum is a relative average over the rendered clip.");
            } else if (stream.second != 1) {
                ImGui::TextDisabled("%s", stream.second == 4 ? "Value labels" :
                    stream.second == 2 ? "Audio min/max envelope (50 ms buckets)" : "Text labels");
                int style = c.scoreDataStyle.count(stream.first) ? c.scoreDataStyle[stream.first] : 0;
                bool show = style != 3;
                if (ImGui::Checkbox("Show", &show)) c.scoreDataStyle[stream.first] = show ? 0 : 3;
            } else {
                int style = c.scoreDataStyle.count(stream.first) ? c.scoreDataStyle[stream.first] : 0;
                if (comboFit(S(200), "Display", &style, "Line chart\0Steps\0Dots\0Hidden\0"))
                    c.scoreDataStyle[stream.first] = style;
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled(pd ? "Play the timeline to capture numeric score values; capture resets on Play." : "Render audio again after editing code.");
        ImGui::EndMenu();
    }
    comboFit(S(200), "Role##cl_role", &c.scoreRole, "Generator\0Effect\0both\0");
    ImGui::Checkbox("Write the coding language", &c.scoreShowLang);
    if (lane) {   // automation of this clip: how it appears in the score (set here, next to the code)
        const ParsedTrack pc = lane->kind==6 ? pdParsedForUi(*lane,&c) : parseClip(c);
        if (!pc.params.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Automation of this clip in the score");
            for (const CawParam& prm : pc.params) {
                ImGui::PushID(prm.chn.c_str());
                drawAutoScoreMenu(*lane, prm.chn);
                ImGui::PopID();
            }
        }
    }
}

void drawScoreMarkList(const char* idPrefix, std::vector<ScoreMark>& list, bool section) {
    int erase = -1;
    for (int i = 0; i < (int)list.size(); ++i) {
        ScoreMark& mk = list[(size_t)i];
        ImGui::PushID(i);
        float t = (float)mk.t;
        ImGui::SetNextItemWidth(S(90));
        if (ImGui::DragFloat("##t", &t, 0.1f, 0.f, 36000.f, "%.1f s")) mk.t = std::max(0.f, t);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(section ? 44 : 150));
        inputStr("##label", mk.label, 0);
        if (section) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(200));
            inputStr("##text", mk.text, 0);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) erase = i;
        ImGui::PopID();
    }
    if (erase >= 0) list.erase(list.begin() + erase);
    (void)idPrefix;
}

// Read-only patch-board overview. Static code matches show potential routes,
// not proof that a conditional statement actually executes.
void drawScoreRoutingMap() {
    struct Route { std::string from, to; bool toMaster; };
    std::vector<Route> routes;
    std::set<std::pair<std::string, std::string>> codeRoutes;
    const std::regex sendRe(R"re(\bcawsa\s+"([^"]+)")re");
    const std::regex receiveRe(R"re(\bcawra\s+"([^"]+)")re");
    auto destination = [&](const std::string& bus) {
        for (const Track& other : g_proj.tracks)
            if (other.kind == 4 && vstlang::busName(other) == bus) return std::string("VSTe: ") + other.name;
        return std::string("BUS: ") + bus;
    };
    for (const Track& t : g_proj.tracks) {
        if (t.kind == 0) {
            for (const Clip& clip : t.clips) {
                std::istringstream lines(clip.code);
                std::string line;
                while (std::getline(lines, line)) {
                    const size_t semicolon = line.find(';');
                    if (semicolon != std::string::npos) line.erase(semicolon);
                    std::smatch match;
                    const std::string clipName = "Csound: " + clip.name;
                    if (std::regex_search(line, match, sendRe))
                        codeRoutes.insert({clipName, destination(match[1].str())});
                    if (std::regex_search(line, match, receiveRe))
                        codeRoutes.insert({"BUS: " + match[1].str(), clipName});
                }
            }
        }
        if (t.kind != 3 && t.kind != 4) continue;
        // VSTe output always goes to the master now (master = replace the source, mix = alongside it).
        routes.push_back({t.name, "MASTER", true});
        // Each enabled track / clips input feeds the VSTe; master inputs have no named source node.
        if (t.kind == 4) for (const auto& in : t.vstInputs)
            if (in.on && in.mode != 2 && !in.name.empty())
                routes.push_back({in.name, t.name, false});
    }
    for (const auto& route : codeRoutes) routes.push_back({route.first, route.second, false});
    ImGui::TextUnformatted("SCORE PATCH BOARD");
    ImGui::TextDisabled("VST outputs and literal cawsa/cawra in Csound clips. Dynamic code and other routes are not shown.");
    if (routes.empty()) { ImGui::TextDisabled("No recognized routes yet."); return; }
    ImGui::BeginChild("##score_patchboard", ImVec2(0, S(400)), true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(S(570), ImGui::GetContentRegionAvail().x);
    const float rowH = S(64), left = origin.x + S(20), right = origin.x + width - S(210);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(width, rowH * routes.size() + S(20)));
    for (size_t i = 0; i < routes.size(); ++i) {
        const float y = origin.y + S(12) + i * rowH;
        const ImU32 wire = routes[i].toMaster ? IM_COL32(246, 151, 58, 255) : IM_COL32(67, 193, 200, 255);
        dl->AddRectFilled(ImVec2(left, y), ImVec2(left + S(180), y + S(45)), IM_COL32(39, 42, 46, 255), S(4));
        dl->AddRect(ImVec2(left, y), ImVec2(left + S(180), y + S(45)), IM_COL32(111, 105, 91, 255), S(4));
        dl->PushClipRect(ImVec2(left + S(8), y), ImVec2(left + S(174), y + S(45)), true);
        dl->AddText(ImVec2(left + S(10), y + S(13)), IM_COL32(235, 232, 219, 255), routes[i].from.c_str());
        dl->PopClipRect();
        dl->AddBezierCubic(ImVec2(left + S(180), y + S(22)), ImVec2(left + S(230), y + S(22)),
                           ImVec2(right - S(45), y + S(22)), ImVec2(right, y + S(22)), wire, S(3));
        dl->AddCircleFilled(ImVec2(left + S(180), y + S(22)), S(5), wire);
        dl->AddRectFilled(ImVec2(right, y), ImVec2(right + S(180), y + S(45)), IM_COL32(40, 43, 45, 255), S(4));
        dl->AddRect(ImVec2(right, y), ImVec2(right + S(180), y + S(45)), IM_COL32(111, 105, 91, 255), S(4));
        dl->AddCircleFilled(ImVec2(right, y + S(22)), S(5), wire);
        dl->PushClipRect(ImVec2(right + S(10), y), ImVec2(right + S(178), y + S(45)), true);
        dl->AddText(ImVec2(right + S(12), y + S(13)), IM_COL32(235, 232, 219, 255), routes[i].to.c_str());
        dl->PopClipRect();
    }
    ImGui::EndChild();
}

void drawScoreMapWindow() {
    if (!g_scoreMapOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(700), S(500)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Map score", &g_scoreMapOpen))
        drawScoreRoutingMap();
    ImGui::End();
}

void drawScoreExportControls() {
    ImGui::TextUnformatted("Score");
    ImGui::SetNextItemWidth(S(260));
    caw_ui::combo("##score_export_type", &g_scoreExportType, "Overview (SVG)\0Full score (HTML)\0");
    ImGui::TextUnformatted("Color");
    ImGui::SetNextItemWidth(S(260));
    caw_ui::combo("##score_export_color", &g_scoreExportColor, "Colour\0Black and white\0");
    if (ImGui::Button("Export score")) generateScore(g_scoreExportType == 1, g_scoreExportColor == 1);
}

void drawScoreExportWindow() {
    if (!g_scoreExportOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(360), S(210)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Export score", &g_scoreExportOpen)) {
        drawScoreExportControls();
    }
    ImGui::End();
}

void drawCodeExportWindow() {
    if (!g_codeExportOpen) return;
    static int mode = 0, filter = 0;
    ImGui::SetNextWindowSize(ImVec2(S(570), S(330)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Export code", &g_codeExportOpen)) {
        ImGui::TextUnformatted("Format");
        ImGui::SetNextItemWidth(-1);
        caw_ui::combo("##code_export_format", &mode,
            "Separate raw clip files\0All raw clip code in one TXT\0Csound clip code in one ORC\0Csound instruments in one ORC\0Whole project as CSD\0");
        if (mode <= 1) {
            ImGui::TextUnformatted("Include");
            ImGui::SetNextItemWidth(-1);
            caw_ui::combo("##code_export_filter", &filter, "All clip code\0Csound clips only\0VST CAW clips only\0");
        }
        if (mode == 0) ImGui::TextWrapped("Writes one original source file per clip. VST CAW code stays separate from Csound.");
        else if (mode == 1) ImGui::TextWrapped("Combines the original clip text into one labelled TXT file.");
        else if (mode == 2) ImGui::TextWrapped("Combines raw Csound clip bodies. The file is not a standalone orchestra.");
        else if (mode == 3) ImGui::TextWrapped("Wraps Csound clips as instruments. Some code may still depend on project opcodes, files or CAW helpers.");
        else ImGui::TextWrapped("Generates a Csound project and score. SuperCollider and Pure Data clips are not included in Csound export. External files may be needed; CAW-hosted VST plugins do not run standalone in Csound.");
        ImGui::Spacing();
        ImGui::TextDisabled("SuperCollider and Pure Data audio export is not implemented yet.");
        ImGui::TextDisabled("A new folder is created, so existing exports are not overwritten.");
        if (ImGui::Button("Export code")) {
            exportCodeClips(static_cast<code_export::Mode>(mode), static_cast<code_export::Filter>(filter));
        }
    }
    ImGui::End();
}

void drawLogo(ImDrawList* dl, ImVec2 origin, float size) {
    const LogoVariant* best = nullptr;
    for (int i = 0; i < g_logoVariantsReady; ++i)
        if (!best || std::fabs(g_logoVariants[i].px - size) < std::fabs(best->px - size)) best = &g_logoVariants[i];
    if (best && std::fabs(best->px - size) < 2.f) {
        const ImVec2 p(std::floor(origin.x + .5f), std::floor(origin.y + .5f));   // whole pixels: 1:1, no blur
        dl->AddImage(ImGui::GetIO().Fonts->TexID, p, ImVec2(p.x + best->px, p.y + best->px), best->uv0, best->uv1);
        return;
    }
    if (g_logoReady)
        dl->AddImage(ImGui::GetIO().Fonts->TexID, origin, ImVec2(origin.x + size, origin.y + size), g_logoUv0, g_logoUv1);
}

struct MultiCtx { std::string* s; };
int multiCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = ((MultiCtx*)d->UserData)->s;
        str->resize((size_t)d->BufTextLen);
        d->Buf = str->data();
    }
    return 0;
}
bool inputMulti(const char* label, std::string& s, ImVec2 size) {
    MultiCtx ctx{&s};
    return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, multiCb, &ctx);
}

void drawScoreEditor() {
    if (!g_scoreOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(680), S(720)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(200), S(70)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Score", &g_scoreOpen)) {
        ScoreInfo& sc = g_proj.score;
        const float labelW = S(120);
        auto row = [&](const char* name) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(name); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(S(400)); };
        ImGui::TextDisabled("Score for contemporary music. Muted tracks and clips are not drawn, like in an audio render.");
        if (ImGui::BeginTabBar("##score_tabs")) {
            if (ImGui::BeginTabItem("Piece")) {
                row("Title");     inputStr("##sc_title", sc.title, 0);
                row("Subtitle");  inputStr("##sc_sub", sc.subtitle, 0);
                row("Composer");  inputStr("##sc_comp", sc.composer, 0);
                row("Copyright"); inputStr("##sc_copy", sc.copyright, 0);
                row("Cover notes");
                inputMulti("##sc_notes", sc.notes, ImVec2(S(400), S(90)));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Layout")) {
                row("Clip analysis");
                ImGui::Checkbox("Render audio measures every clip##sc_an", &sc.analyze);
                if (ImGui::IsItemHovered()) helpTip("While rendering, every clip is measured (loudness, where its spectrum sits) and the data sent to the channel \"score\" is recorded. The score then draws the real shape of each sound. Press Render audio after changing sounds.");
                row("Detailed labels");
                int labelMode = sc.detailedDataLabels ? 1 : 0;
                if (comboFit(S(220), "##sc_detail_labels", &labelMode, "Start and duration\0Data sent / received\0"))
                    sc.detailedDataLabels = labelMode == 1;
                if (ImGui::IsItemHovered()) helpTip("Full score only. Data sent shows captured values from the clip's \"score\" channel; received shows automation inputs and their ranges. Render audio first to capture outgoing values.");
                row("Line weight");
                float lw = (float)sc.lineWeight;
                if (ImGui::SliderFloat("##sc_lw", &lw, 0.4f, 3.f, "%.2f x")) sc.lineWeight = lw;
                row("Seconds / system");
                float v = (float)sc.sysSec;
                if (ImGui::SliderFloat("##sc_sys", &v, 10.f, 300.f, "%.0f s")) sc.sysSec = v;
                ImGui::TextDisabled("How many seconds fit on one line of the score: more seconds = a denser line.");
                row("Systems / page");
                ImGui::InputInt("##sc_spp", &sc.systemsPerPage, 1, 1);
                sc.systemsPerPage = std::min(12, std::max(0, sc.systemsPerPage));
                ImGui::SameLine(); ImGui::TextDisabled("0 = as many as fit");
                ImGui::Checkbox("Cover page (lanes, sections, notes, key)", &sc.coverPage);
                ImGui::Checkbox("Cue list pages (all clip texts)", &sc.eventList);
                ImGui::Checkbox("Spectrum lane (spectrogram of the rendered audio)", &sc.spectrum);
                if (sc.spectrum) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Analyse now")) refreshScoreSpectrum(true);
                    if (g_scoreSpecMsg.empty()) refreshScoreSpectrum(false);
                    ImGui::TextDisabled("%s", g_scoreSpecMsg.c_str());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sections and markers")) {
                ImGui::TextUnformatted("Sections (boxed letters on the ruler)");
                drawScoreMarkList("sec", sc.sections, true);
                if (ImGui::Button("+ Section at playhead")) {
                    ScoreMark mk;
                    mk.t = g_play;
                    mk.label = std::string(1, (char)('A' + (int)(sc.sections.size() % 26)));
                    sc.sections.push_back(mk);
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Time markers (red vertical lines through all lanes)");
                drawScoreMarkList("mark", sc.markers, false);
                if (ImGui::Button("+ Marker at playhead")) {
                    ScoreMark mk;
                    mk.t = g_play;
                    mk.label = "marker";
                    sc.markers.push_back(mk);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Map")) {
                ImGui::Checkbox("Lanes with the same name are ONE lane", &sc.mergeSameName);
                ImGui::TextDisabled("Resulting lanes of the score:");
                for (const ScoreLaneInfo& li : scoreLaneSummary(g_proj)) {
                    std::string from;
                    for (size_t i = 0; i < li.tracks.size(); ++i) from += (i ? ", " : "") + li.tracks[i];
                    ImGui::BulletText("%s  [%s%s]  <- %s", li.name.c_str(), li.staff ? "staff" : "graphic", li.merged ? ", merged" : "", from.c_str());
                    if (!li.automation.empty()) {
                        std::string am;
                        for (size_t i = 0; i < li.automation.size(); ++i) am += (i ? ", " : "") + li.automation[i];
                        ImGui::Indent(S(24)); ImGui::TextDisabled("automation: %s", am.c_str()); ImGui::Unindent(S(24));
                    }
                }
                ImGui::Separator();
                for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti) {
                    Track& t = g_proj.tracks[ti];
                    ImGui::PushID((int)ti + 1000);
                    if (caw_ui::treeNode(t.name.c_str())) {
                        drawTrackScoreMenu(t);
                        if (trackHasMidi(t)) { ImGui::Separator(); drawMidiScoreMenu(t); }
                        for (const auto& lane : t.automation) {
                            if (lane.second.empty()) continue;
                            ImGui::Separator();
                            ImGui::PushID(lane.first.c_str());
                            drawAutoScoreMenu(t, lane.first);
                            ImGui::PopID();
                        }
                        for (Clip& c : t.clips) {
                            ImGui::Separator();
                            ImGui::PushID(c.id);
                            ImGui::TextUnformatted(c.name.c_str());
                            drawClipScoreMenu(c, &t);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Export score");
        ImGui::PushID("score_edit_export");
        drawScoreExportControls();
        ImGui::PopID();
    }
    ImGui::End();
}

void drawMainMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New project", "Ctrl+N")) newProject();
        if (ImGui::MenuItem("Open project...", "Ctrl+O")) openProject();
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveProject(false);
        if (ImGui::MenuItem("Save As...")) saveProject(true);
        // A regular selectable opens the side popup on click, not hover.
        // Unlike BeginMenu, it never draws ImGui's arrow underneath the plus.
        const bool exportClicked = ImGui::Selectable("Export", false, ImGuiSelectableFlags_DontClosePopups);
        const ImVec2 exportA = ImGui::GetItemRectMin(), exportB = ImGui::GetItemRectMax();
        caw_ui::disclosure(ImGui::GetWindowDrawList(),
                           ImVec2(exportB.x - S(12), (exportA.y + exportB.y) * .5f),
                           ImGui::GetFontSize() * .19f, ImGui::GetColorU32(ImGuiCol_Text), false);
        if (exportClicked && !ImGui::IsPopupOpen("##export_flyout")) ImGui::OpenPopup("##export_flyout");
        ImGui::SetNextWindowPos(ImVec2(exportB.x, exportA.y), ImGuiCond_Appearing);
        bool exportAction = false;
        if (ImGui::BeginPopup("##export_flyout")) {
            if (ImGui::MenuItem("Audio export (Csound)...", nullptr, false, g_langCsound)) { g_audioExportOpen = true; exportAction = true; }
            if (g_renderActive && ImGui::MenuItem("Cancel audio export")) { cancelRender(); exportAction = true; }
            if (ImGui::MenuItem("Code clips...")) { g_codeExportOpen = true; exportAction = true; }
            if (ImGui::MenuItem("Score...")) { g_scoreExportOpen = true; exportAction = true; }
            ImGui::EndPopup();
        }
        if (exportAction) ImGui::CloseCurrentPopup();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Ctrl+Q")) appRequestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Select tool", "1", g_tool == Tool::Select)) g_tool = Tool::Select;
        if (ImGui::MenuItem("Pen 1 (draw automation, simplified to few dots)", "8", g_tool == Tool::Pen)) g_tool = Tool::Pen;
        if (ImGui::MenuItem("Pen 2 (draw automation, keeps every dot)", "9", g_tool == Tool::Pen2)) g_tool = Tool::Pen2;
        if (ImGui::MenuItem("Cut tool (split MIDI regions)", "3", g_tool == Tool::Cut)) g_tool = Tool::Cut;
        if (ImGui::MenuItem("Join MIDI regions", "4", g_tool == Tool::Join)) runJoinTool();
        ImGui::Separator();
        int laneIndex = -1;
        Clip* clip = selectedClip(&laneIndex);
        if (ImGui::MenuItem("Duplicate selected clips (same instrument)", "Ctrl+D", false, !g_selClips.empty())) duplicateSelectedClips();
        if (ImGui::MenuItem("Duplicate clip as new instrument", nullptr, false, clip != nullptr)) duplicateClipAsNew(laneIndex, g_selClip);
        if (ImGui::MenuItem("Edit selected clip code", nullptr, false, clip != nullptr) && clip) clip->editorOpen = true;
        validateAutoSelection();
        if (ImGui::MenuItem("Delete selected automation points", "Del", false, hasAutoSelection())) deleteSelectedAutoPoints();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Pool")) {
        if (ImGui::MenuItem("File")) g_audioPoolOpen = true;
        if (g_langCsound) {
            if (ImGui::MenuItem("UDO")) g_udoPoolOpen = true;
            if (ImGui::MenuItem("Instrument")) g_instrPoolOpen = true;
            if (ImGui::MenuItem("Orchestra")) g_orchestraOpen = true;
            if (ImGui::MenuItem("Includes")) g_includePoolOpen = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Monitor panel", nullptr, &g_showMonitor);
        ImGui::MenuItem("Help panel (bottom)", nullptr, &g_helpOn);
        ImGui::Separator();
        ImGui::MenuItem("Master meter and gain (right panel)", nullptr, &g_viewMeter);
        ImGui::MenuItem("Spectrum", nullptr, &g_viewSpectrum);
        ImGui::MenuItem("Signal scope", nullptr, &g_viewScope);
        ImGui::MenuItem("Console", nullptr, &g_viewConsole);
        ImGui::Separator();
        ImGui::TextDisabled("Timeline");
        if (ImGui::MenuItem("Hide / show all automation lanes", "Alt+H")) toggleHideAllAutomation();
        if (ImGui::MenuItem("Show all automation lanes", "Alt+J")) showAllAutomation();
        if (ImGui::MenuItem("Hide / show all MIDI lanes", "Alt+M")) toggleHideAllMidi();
        if (ImGui::MenuItem("Show all MIDI lanes", "Alt+N")) showAllMidi();
        ImGui::Separator();
        ImGui::TextDisabled("Keyboard shortcuts");
        ImGui::TextDisabled("1 Select   8 Pen   3 Cut   4 Join   Esc = Select");
        ImGui::TextDisabled("M mute / S solo the selected track");
        ImGui::TextDisabled("Space play / stop timeline. MIDI editor: - / + move playhead, Shift = fast");
        ImGui::TextDisabled("Ctrl+D duplicate clips   Del delete");
        ImGui::TextDisabled("Ctrl+A select all dots of a lane");
        ImGui::TextDisabled("Ctrl+Z undo   Ctrl+Y redo   Ctrl+S save");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Score")) {
        if (ImGui::MenuItem("Edit score")) g_scoreOpen = true;
        if (ImGui::MenuItem("Map score")) g_scoreMapOpen = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Languages...")) g_chooseLanguages = true;
        ImGui::Separator();
        static const char* tabs[] = {"General", "Audio", "Programs", "Editor", "Advanced", "Files"};
        static const int ids[] = {6, 0, 1, 3, 4, 5};
        for (int i = 0; i < 6; ++i)
            if ((ids[i] != 4 || g_langCsound) && ImGui::MenuItem(tabs[i])) { g_settingsOpen = true; g_settingsGoto = ids[i]; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About CAW")) g_aboutOpen = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
    if (g_chooseLanguages) { ImGui::OpenPopup("Choose languages###caw_languages"); g_chooseLanguages = false; }
    if (ImGui::BeginPopupModal("Choose languages###caw_languages", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.93f, 0.50f, 0.15f, 1.f), "What does this session use?");
        ImGui::Separator();
        ImGui::TextDisabled("Programming languages");
        ImGui::Checkbox("Csound", &g_langCsound);
        ImGui::Checkbox("Pure Data", &g_langPd);
        ImGui::Checkbox("SuperCollider", &g_langSc);
        ImGui::Spacing();
        ImGui::TextDisabled("Engines and media");
        ImGui::Checkbox("VST", &g_langVst);
        ImGui::Checkbox("Audio", &g_langAudio);
        ImGui::Spacing();
        if (!g_langCsound && !g_langPd && !g_langSc && !g_langVst && !g_langAudio)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.f), "Nothing selected: score only.");
        if (ImGui::Button("Continue", ImVec2(S(110), 0))) {
            if (!g_langSc && g_scEngineOn) stopScEngine();
            if (!g_langVst && g_vstEngineOn) stopVstEngine();   // unchecked: stop the sound, keep the plugins loaded
            g_languageChoiceMade = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (g_welcomeOpen) { ImGui::OpenPopup("Welcome to CAW###welcome_caw"); g_welcomeOpen = false; }
    if (ImGui::BeginPopupModal("Welcome to CAW###welcome_caw", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float logoSize = S(220), bodyWidth = S(300);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        drawLogo(ImGui::GetWindowDrawList(), ImVec2(start.x + (bodyWidth - logoSize) * .5f, start.y), logoSize);
        ImGui::Dummy(ImVec2(bodyWidth, logoSize));
        const char* title = "CAW - Code Audio Workstation";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (bodyWidth - ImGui::CalcTextSize(title).x) * .5f));
        ImGui::TextUnformatted(title);
        const char* version = "0.0.34 beta";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (bodyWidth - ImGui::CalcTextSize(version).x) * .5f));
        ImGui::TextUnformatted(version);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (bodyWidth - S(100)) * .5f);
        if (ImGui::Button("Close##welcome", ImVec2(S(100), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_aboutOpen) { ImGui::OpenPopup("About CAW###about_caw"); g_aboutOpen = false; }
    if (ImGui::BeginPopupModal("About CAW###about_caw", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // logo on the left, name / version / author on its right
        const float aboutLogo = S(112);
        drawLogo(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), aboutLogo);
        ImGui::Dummy(ImVec2(aboutLogo, aboutLogo));
        ImGui::SameLine(0.f, S(18));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (aboutLogo - ImGui::GetTextLineHeightWithSpacing() * 3.f) * .5f));
        ImGui::TextColored(ImVec4(0.93f, 0.50f, 0.15f, 1.f), "CAW - Code Audio Workstation");
        ImGui::TextDisabled("Version 0.0.34 beta");
        ImGui::TextDisabled("Parham Izadyar  -  parhamizadyar.net");
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::TextUnformatted("simple audio workstation, driven by code: Csound, SuperCollider, Pure Data");
        ImGui::TextUnformatted("Use any one, two, or all three engines in the same project, with data passing between them.");
        ImGui::TextUnformatted("Project score export, with VST effects and instruments also working alongside CAW’s own code");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(S(100), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_confirmDeleteSelected) {
        ImGui::OpenPopup("Confirm track deletion");
        g_confirmDeleteSelected = false;
    }
    if (ImGui::BeginPopupModal("Confirm track deletion", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int selectedCount = 0;
        for (const Track& t : g_proj.tracks) if (t.deleteSelected) ++selectedCount;
        ImGui::Text("Delete %d selected track(s)? Clips and automation on those tracks will be removed.", selectedCount);
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(100, 0))) {
            const int draggedId = g_autoDragTrackId;
            const int selectedMidiTrack = g_midiSelectedTrackId;
            const bool removesDragged = std::any_of(g_proj.tracks.begin(), g_proj.tracks.end(),
                [draggedId](const Track& t) { return t.id == draggedId && t.deleteSelected; });
            const bool removesMidiTrack = std::any_of(g_proj.tracks.begin(), g_proj.tracks.end(),
                [selectedMidiTrack](const Track& t) { return t.id == selectedMidiTrack && t.deleteSelected; });
            g_proj.tracks.erase(std::remove_if(g_proj.tracks.begin(), g_proj.tracks.end(),
                [](const Track& t) { return t.deleteSelected; }), g_proj.tracks.end());
            pruneFolders();
            if (removesDragged) { g_autoDragTrackId = -1; g_autoDragPoint = -1; g_autoDragChannel.clear(); }
            if (removesMidiTrack) { g_midiSelectedTrackId = -1; g_midiSelectedNote = -1; g_midiDragTrackId = -1; g_midiDragNote = -1; }
            g_selTrack = g_selClip = -1;
            g_confirmDeleteSelected = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            g_confirmDeleteSelected = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---- Finding the programs (Csound) on this PC ----
// A path typed in Settings is used only if that file exists on THIS computer (a project made on another PC carries that PC's paths).
// Otherwise: the usual install folders are scanned (any folder whose name starts with the program's name, the newest name first),
// then PATH. Nothing found -> the bare name is returned and the console says what to do.
bool looksLikeFilePath(const std::string& t) { return t.find('/') != std::string::npos || t.find('\\') != std::string::npos; }

std::string findInstalledExe(const char* folderPrefix, const std::vector<std::string>& relPaths, const char* exeName) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::vector<std::string> roots = plat::csoundInstallRoots();
    auto lower = [](std::string t) { for (char& c : t) c = (char)std::tolower((unsigned char)c); return t; };
    const std::string prefix = lower(folderPrefix);
    std::vector<std::pair<std::string, fs::path>> hits;   // folder name, exe path
    std::set<std::string> seen;
    for (const std::string& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string folder = entry.path().filename().string();
            if (lower(folder).rfind(prefix, 0) != 0) continue;   // "Csound7", "Csound6", "Csound.app" ...
            for (const std::string& rel : relPaths) {
                const fs::path exe = entry.path() / rel;
                if (fs::exists(exe, ec) && seen.insert(lower(exe.string())).second) { hits.push_back({folder, exe}); break; }
            }
        }
    }
    if (!hits.empty()) {
        std::sort(hits.begin(), hits.end(), [&](const auto& a, const auto& b) { return lower(a.first) > lower(b.first); });   // Csound7 before Csound6
        return hits.front().second.string();
    }
    return plat::findOnPath(exeName);   // PATH (and the usual folders on macOS / Linux)
}

// The program to start: typed path (if the file exists here), else the search above, else the bare name.
std::string resolveProgram(const std::string& typed, const char* bareName, const char* folderPrefix, const std::vector<std::string>& relPaths, const char* exeName) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!typed.empty() && typed != bareName && looksLikeFilePath(typed) && fs::exists(typed, ec)) return typed;
    const std::string found = findInstalledExe(folderPrefix, relPaths, exeName);
    if (!found.empty()) return found;
    return typed.empty() ? std::string(bareName) : typed;   // not found anywhere: the start error tells the user what to do
}

std::string csoundExePath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string& typed = g_proj.cs.csoundExe;
    // A path typed in the project settings wins; otherwise the Csound copy shipped inside CAW's folder
    // (<caw.exe folder>\Csound), then an installed Csound, then PATH.
    const bool typedOk = !typed.empty() && typed != "csound" && looksLikeFilePath(typed) && fs::exists(typed, ec);
    std::string exe = typedOk ? std::string() : plat::bundledCsound();
    if (exe.empty())
        exe = resolveProgram(typed, "csound", "Csound", plat::csoundRelativeExePaths(), plat::csoundProgramName());
    plat::prepareCsoundEnvironment(exe);
    return exe;
}

// Ground truth about the current Csound's basic math opcodes: compile tiny abs / sqrt / pow probe
// orchestras through the same API library the engine uses. The 'csound -z' opcode list is formatted
// for humans (columns, argument variants) and cannot be parsed by first-word matching: on an installed
// Csound 7 that compiles abs and sqrt fine, the parsed list contained neither - which made CAW refuse
// to start with a false "provides neither abs nor sqrt" error. A real compile does not guess.
bool g_mathProbeDone = false;
std::string g_mathProbeExe;
bool g_mathHasAbs = true, g_mathHasSqrt = true, g_mathHasPow = true;
std::string g_mathProbeDetail;
// Runs (or reuses) the probe for the resolved Csound. Returns true when abs OR sqrt compiles.
static bool ensureCsoundMathProbe(bool force = false) {
    const std::string exe = csoundExePath();
    if (g_mathProbeDone && !force && g_mathProbeExe == exe) return g_mathHasAbs || g_mathHasSqrt;
    g_mathProbeDone = true; g_mathProbeExe = exe; g_mathProbeDetail.clear();
    std::string error;
    const bool probed = g_apiProc.probeMath(exe, g_mathHasAbs, g_mathHasSqrt, g_mathHasPow, error);
    if (!probed) { g_mathHasAbs = g_mathHasSqrt = g_mathHasPow = false; g_mathProbeDetail = error; }
    else if (!g_mathHasAbs || !g_mathHasSqrt || !g_mathHasPow) g_mathProbeDetail = error;
    setCsoundMathOps(g_mathHasAbs, g_mathHasSqrt, g_mathHasPow);
    logLine("[caw] Csound math check (" + (exe.empty() ? std::string("(none found)") : exe) + "): "
            + (g_mathHasAbs ? "abs ok" : "abs MISSING")
            + (g_mathHasSqrt ? ", sqrt ok" : ", sqrt MISSING")
            + (g_mathHasPow ? ", pow ok" : ", pow missing"));
    if (!g_mathProbeDetail.empty()) logLine("[caw] Csound math check: " + g_mathProbeDetail);
    return g_mathHasAbs || g_mathHasSqrt;
}

// Meter / scope / spectrum frames: the CawMon instrument prints text lines (CAWM / CAWS / CAWL / CAWR / CAWC <first index> <values>)
// on Csound's output. They are held back by the output latency (plus the sync offset from Settings), so the meters move with the sound.
bool g_monSeen = false, g_monWarned = false;
float g_monRunT = 0.f;
double engineOutputLatency();
struct MonFrame { char tag = 'M'; int start = 0; std::vector<float> values; };
std::deque<std::pair<std::chrono::steady_clock::time_point, MonFrame>> g_monHeld;
SOCKET g_monitorSocket = INVALID_SOCKET;
#ifdef _WIN32
bool g_monitorWsa = false;
using SockLen = int;
#else
using SockLen = socklen_t;
#endif
bool g_haveOscRawSeq = false, g_haveOscMeterSeq = false;
uint32_t g_oscRawSeq = 0, g_oscMeterSeq = 0;

void closeMonitorSocket() {
    if (g_monitorSocket != INVALID_SOCKET) { closesocket(g_monitorSocket); g_monitorSocket = INVALID_SOCKET; }
#ifdef _WIN32
    if (g_monitorWsa) { WSACleanup(); g_monitorWsa = false; }
#endif
    g_cawMonitorPort = 0;
    g_haveOscRawSeq = g_haveOscMeterSeq = false;
}
bool openMonitorSocket() {
    closeMonitorSocket();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_monitorWsa = true;
#endif
    g_monitorSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_monitorSocket == INVALID_SOCKET) { closeMonitorSocket(); return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int receiveSize = 2 * 1024 * 1024;
    setsockopt(g_monitorSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveSize), sizeof(receiveSize));
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool nonblockOk = ioctlsocket(g_monitorSocket, FIONBIO, &nonblocking) == 0;
#else
    const int flags = fcntl(g_monitorSocket, F_GETFL, 0);
    const bool nonblockOk = flags >= 0 && fcntl(g_monitorSocket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (bind(g_monitorSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || !nonblockOk) {
        closeMonitorSocket(); return false;
    }
    SockLen addrLen = sizeof(addr);
    if (getsockname(g_monitorSocket, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        closeMonitorSocket(); return false;
    }
    g_cawMonitorPort = ntohs(addr.sin_port);
    return g_cawMonitorPort > 0;
}

void updateSpectrumDerived() {
    if (g_fftDb.size() != (size_t)monitor_fft::bins) return;
    if (g_specStyle == 1)
        g_spectrumBands = monitor_bands::analyze(g_fftDb, g_rawSampleRate, 3, g_specFast ? monitor_fft::fastSize : monitor_fft::size);
    if (g_specStyle == 3)
        g_columnBands = monitor_bands::analyze(g_fftDb, g_rawSampleRate, monitor_bands::kColumnBandsPerOctave, g_specFast ? monitor_fft::fastSize : monitor_fft::size);
    if (g_specStyle == 0) {
        g_tonePeaks = monitor_peaks::find(g_fftDb, g_rawSampleRate, g_specFast ? monitor_fft::fastSize : monitor_fft::size);
    }
}

void analyzeCurrentRaw() {
    float input[monitor_fft::size];
    for (int i = 0; i < monitor_fft::size; ++i)
        input[i] = g_fftChannel == 1 ? g_rawRight[i] : g_fftChannel == 2 ? (g_rawLeft[i] + g_rawRight[i]) * 0.5f : g_rawLeft[i];
    g_fftDb = monitor_fft::spectrum(input, g_specFast ? monitor_fft::fastSize : monitor_fft::size);
}

void applyRawSamples(const float* left, const float* right) {
    const float inv = 1.f / g_rawFullScale;
    for (int i = 0; i < monitor_fft::size; ++i) {
        g_rawLeft[i] = left[i] * inv;
        g_rawRight[i] = right[i] * inv;
    }
    g_haveRaw = true;
    g_rawAge = 0.f;
    const auto work = monitor_work::select(g_showMonitor && g_viewSpectrum, g_specFreeze, g_analyzerMode, g_specStyle);
    if (work.fft) {
        analyzeCurrentRaw();
        if (work.thirdBands || work.columnBands || work.tonalPeaks) updateSpectrumDerived();
        if (work.history) {
            g_fftHistory.push_back(g_fftDb);
            if (g_fftHistory.size() > 120) g_fftHistory.pop_front();
        }
    }
}

void applyMonitor(char tag, int start, const std::vector<float>& values) {
    if (start < 0) return;
    if (tag == 'B') {
        if (values.size() == (size_t)monitor_fft::size * 2)
            applyRawSamples(values.data(), values.data() + monitor_fft::size);
        return;
    }
    if (tag == 'U' || tag == 'V') {
        if (g_rawAssembler.push(tag, start, values, 1.f))
            applyRawSamples(g_rawAssembler.left, g_rawAssembler.right);
        return;
    }
    const float inv = 1.f / (float)std::max(0.0001, g_proj.cs.zdbfs);
    for (size_t i = 0; i < values.size(); ++i) {
        float v = values[i];
        if (tag == 'C') { recFeed(start + (int)i, v); continue; }   // live Rec value of a clip: used as it is
        if (!std::isfinite(v)) v = 0.f;
        v *= inv;
        const int idx = start + (int)i;
        if (tag == 'M') {
            if ((size_t)idx >= g_peakIn.size()) continue;
            g_peakIn[(size_t)idx] = v;
            if (v >= 0.99426f && (size_t)idx < g_meterClipHold.size())
                g_meterClipHold[(size_t)idx] = 1.5f;
        } else if (tag == 'S' && idx < kMeterBands) { g_bandAmp[idx] = v; g_haveBands = true; }
        else if (tag == 'L' && idx < kScopePoints) g_scopeStagingL[idx] = std::min(1.f, std::max(-1.f, v));
        else if (tag == 'R' && idx < kScopePoints) g_scopeStagingR[idx] = std::min(1.f, std::max(-1.f, v));
    }
    if (tag == 'R' && start + (int)values.size() >= kScopePoints) {
        std::copy(g_scopeStagingL, g_scopeStagingL + kScopePoints, g_scopeL);
        std::copy(g_scopeStagingR, g_scopeStagingR + kScopePoints, g_scopeR);
        g_scopeHasFrame = true;
    }
    g_monAge = 0.f;
    g_monSeen = true;
}

// A monitor line of Csound's output; the tag may sit anywhere in the line. Returns false for every other line.
bool handleMonitorLine(const std::string& l) {
    size_t at = std::string::npos;
    for (const char* tagText : {"CAWM ", "CAWS ", "CAWL ", "CAWR ", "CAWC ", "CAWU ", "CAWV "}) {
        const size_t f = l.find(tagText);
        if (f != std::string::npos) { at = f; break; }
    }
    if (at == std::string::npos) return false;
    MonFrame fr;
    fr.tag = l[at + 3];
    if (fr.tag == 'U' || fr.tag == 'V') {
        if (!parseRawMonitorValues(l.data() + at + 5, l.data() + l.size(), fr.start, fr.values)) {
            // Queue an invalid chunk so the assembler discards the incomplete frame.
            fr.start = 0;
            fr.values.clear();
        }
    } else {
        std::istringstream in(l.substr(at + 5));
        if (!(in >> fr.start)) return true;
        float v = 0.f;
        while (in >> v) fr.values.push_back(v);
    }
    const auto due = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(engineOutputLatency() * 1e6));
    g_monHeld.emplace_back(due, std::move(fr));
    if (g_monHeld.size() > 100000) g_monHeld.clear();
    return true;
}

void pollMonitor() {
    if (g_monitorSocket != INVALID_SOCKET) {
        unsigned char data[65536];
        for (int n = 0; n < 256; ++n) {
            sockaddr_in from{};
            SockLen fromLen = sizeof(from);
            const int got = (int)recvfrom(g_monitorSocket, reinterpret_cast<char*>(data), sizeof(data), 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (got == SOCKET_ERROR) break;
            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
            monitor_osc::Packet packet;
            if (!monitor_osc::decode(data, (size_t)got, packet)) continue;
            g_sectionSeen = true;
            bool& have = packet.kind == 'B' ? g_haveOscRawSeq : g_haveOscMeterSeq;
            uint32_t& last = packet.kind == 'B' ? g_oscRawSeq : g_oscMeterSeq;
            if (have && packet.sequence != last && !monitor_osc::newer(packet.sequence, last)) continue;
            if (have && packet.kind == 'B' && packet.sequence == last) continue;
            have = true; last = packet.sequence;
            MonFrame fr;
            fr.tag = packet.kind; fr.start = packet.start; fr.values = std::move(packet.values);
            const auto due = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(engineOutputLatency() * 1e6));
            g_monHeld.emplace_back(due, std::move(fr));
        }
    }
    const auto now = std::chrono::steady_clock::now();
    MonFrame newestRaw;
    bool haveNewestRaw = false;
    while (!g_monHeld.empty() && g_monHeld.front().first <= now) {
        MonFrame& frame = g_monHeld.front().second;
        if (frame.tag == 'B') { newestRaw = std::move(frame); haveNewestRaw = true; }
        else applyMonitor(frame.tag, frame.start, frame.values);
        g_monHeld.pop_front();
    }
    // If the UI paused, don't calculate FFTs for stale snapshots.
    if (haveNewestRaw) applyMonitor(newestRaw.tag, newestRaw.start, newestRaw.values);
}

// Csound rejected a math opcode at parse time ("Unable to find opcode entry for 'abs'" / "unknown
// opcode 'abs'"). Remember WHICH name so the failed run can heal the generator: the finish handler
// flips the matching flag and the next build writes |x| via sqrt(x*x) or plain sums instead.
bool g_rtMissingAbs = false, g_rtMissingSqrt = false, g_rtMissingPow = false;
void noteRealtimeOpcodeError(const std::string& line) {
    static const std::regex missing(R"re((?:opcode entry for|unknown opcode)\s+['\"]?([A-Za-z_]\w*)['\"]?)re");
    std::smatch m;
    if (!std::regex_search(line, m, missing)) return;
    const std::string name = m[1].str();
    if (name == "abs") g_rtMissingAbs = true;
    else if (name == "sqrt") g_rtMissingSqrt = true;
    else if (name == "pow") g_rtMissingPow = true;
}

void drainPlayOutput() {
    const auto lines = g_useCsoundApi ? g_apiProc.takeOutput() : g_playProc.takeOutput();
    for (const std::string& l : lines) {
        if (handleMonitorLine(l)) continue;
        noteRealtimeOpcodeError(l);
        logCsoundLine(l);
    }
    pollMonitor();
}

// ---- live Rec: when the timeline stops, the recorded values become dots
void updateRecording() {
    if (g_timelineRun) return;
    for (RecBuf& b : g_recBufs) {
        if (b.s.size() < 2) continue;
        Track* t = findTrackById(b.trackId);
        if (!t) continue;
        double lo, hi, initial;
        laneValueRange(*t, b.channel, lo, hi, initial);
        const ParsedTrack parsed = pdParsedForUi(*t);
        const CawParam* param = hasClips(*t) ? laneParameter(parsed, b.channel) : nullptr;
        for (AutoPoint& p : b.s) {
            p.v = std::min(hi, std::max(lo, p.v));
            if (param) p.v = quantizeAutomation(p.v, *param);
        }
        const double from = b.s.front().t, to = b.s.back().t;
        thinToLimit(b.s, 0.003 * (hi - lo), 1500);
        ensureAutoLane(*t, b.channel);
        replaceDotsInRange(t->automation[b.channel], from, to, b.s);
        selectDotsInRange(*t, b.channel, from, to);
        logLine("[caw] Rec: " + std::to_string(b.s.size()) + " dots written into lane " + b.channel + ".");
    }
    g_recBufs.clear();
}

bool trackHasRec(const Track& t) {
    if (!hasClips(t)) return false;
    for (const Clip& c : t.clips) if (parseClip(c).hasRec) return true;
    return false;
}

// ---- clip editor buttons: Wave... (write a wave as dots) and Capture... (record the code's own signal)
void drawWavePopup(Track& lane, Clip& clip) {
    const ParsedTrack pc = pdParsedForUi(lane,&clip);
    if (pc.params.empty()) { ImGui::TextDisabled(lane.kind==5 ? "Add ~cawget.(\"name\") to the SuperCollider code first." : lane.kind==6 ? "Add [r $0-cawget-name] to the saved PD patch first." : "Add a cawget channel in the code first."); return; }
    WaveUi& w = g_waveUi;
    if (ImGui::IsWindowAppearing()) { w.ch = 0; w.t0 = clip.start; w.t1 = clip.start + clip.dur; }
    w.ch = std::min((int)pc.params.size() - 1, std::max(0, w.ch));
    ImGui::TextDisabled("AUTOMATION SHAPE");
    {   // the lane combo is as wide as the longest channel name
        float laneW = 0.f;
        for (const CawParam& q : pc.params) laneW = std::max(laneW, ImGui::CalcTextSize(q.chn.c_str()).x);
        ImGui::SetNextItemWidth(std::max(S(190), laneW + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f + S(6)));
    }
    if (caw_ui::beginCombo("Lane##wave_ch", pc.params[(size_t)w.ch].chn.c_str())) {
        for (int k = 0; k < (int)pc.params.size(); ++k)
            if (ImGui::Selectable(pc.params[(size_t)k].chn.c_str(), k == w.ch)) w.ch = k;
        ImGui::EndCombo();
    }
    comboFit(S(190), "Shape##wave_shape", &w.shape, "Sine\0Triangle\0Square\0Saw up\0Saw down\0");
    ImGui::SetNextItemWidth(fitWidth("60.000 Hz", S(190)));
    ImGui::DragFloat("Rate##wave_rate", &w.rate, 0.02f, 0.01f, 60.f, "%.3f Hz");
    w.rate = std::max(0.01f, w.rate);
    ImGui::SetNextItemWidth(fitWidth("100 % of range", S(190)));
    ImGui::SliderFloat("Depth##wave_depth", &w.depth, 0.f, 100.f, "%.0f %% of range");
    ImGui::SetNextItemWidth(fitWidth("100 % of range", S(190)));
    ImGui::SliderFloat("Centre##wave_center", &w.center, 0.f, 100.f, "%.0f %% of range");
    ImGui::Checkbox("Whole clip##wave_whole", &w.whole);
    if (w.whole) { w.t0 = clip.start; w.t1 = clip.start + clip.dur; }
    else {
        ImGui::SetNextItemWidth(fitWidth("36000.000 s", S(110))); ImGui::InputDouble("From##wave_t0", &w.t0, 0.0, 0.0, "%.3f s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fitWidth("36000.000 s", S(110))); ImGui::InputDouble("To##wave_t1", &w.t1, 0.0, 0.0, "%.3f s");
        w.t0 = clampTimelineTime(w.t0); w.t1 = std::max(w.t0 + 0.01, clampTimelineTime(w.t1));
    }
    const CawParam& prm = pc.params[(size_t)w.ch];
    const double lo = prm.min, hi = prm.max > prm.min ? prm.max : prm.min + 1.0;
    std::vector<AutoPoint> pts = genWavePoints(w.shape, w.rate, w.depth / 100.0, lo + (hi - lo) * w.center / 100.0, lo, hi, w.t0, w.t1);
    for (AutoPoint& point : pts) point.v = quantizeAutomation(point.v, prm);
    g_wavePrev.frame = ImGui::GetFrameCount(); g_wavePrev.trackId = lane.id; g_wavePrev.channel = prm.chn; g_wavePrev.pts = pts;
    ImGui::TextDisabled("%d points. Existing points in the range will be replaced.", (int)pts.size());
    if (ImGui::Button("Write dots##wave_go") && pts.size() >= 2) {
        ensureAutoLane(lane, prm.chn);
        replaceDotsInRange(lane.automation[prm.chn], w.t0, w.t1, pts);
        selectDotsInRange(lane, prm.chn, w.t0, w.t1);
        g_wavePrev.frame = -1000;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##wave_cancel")) { g_wavePrev.frame = -1000; ImGui::CloseCurrentPopup(); }
}

// Display smoothing of every meter (called once per frame).
void updateMeters(float dt) {
    for (auto& entry : g_scMeterDb)
        for (float& db : entry.second) db = std::max(-100.f, db - 36.f * dt);
    if (g_scEngineOn) {
        for (const Track& track : g_proj.tracks) if (track.kind == 5) {
            const int slot = scTrackSlot(track.id);
            if (slot < 0) continue;
            auto& meter = g_scMeterDb[track.id];
            for (int channel = 0; channel < 2; ++channel) {
                const float peak = g_scEngine.takeTrackPeak(slot, channel);
                const float db = 20.f * std::log10(std::max(peak, 1e-5f));
                meter[static_cast<size_t>(channel)] = std::max(meter[static_cast<size_t>(channel)], db);
            }
        }
    }
    if (g_scSoloOutput && g_scSoloCapture) {
        MonitorCapture::Snapshot snapshot;
        if (g_scSoloCapture->take(snapshot, g_scSoloSnapshotSeq)) {
            g_scSoloSnapshotSeq = snapshot.sequence;
            g_rawFullScale = 1.f;
            g_rawSampleRate = std::max(1, g_proj.cs.sr);
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    if (g_vstSoloOutput && g_vstSoloCapture) {   // Run VST alone: its own stream feeds the master meter and the scope
        MonitorCapture::Snapshot snapshot;
        if (g_vstSoloCapture->take(snapshot, g_vstSoloSnapshotSeq)) {
            g_vstSoloSnapshotSeq = snapshot.sequence;
            g_rawFullScale = 1.f;
            g_rawSampleRate = std::max(1, g_proj.cs.sr);
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    if (g_pdEngineOn) {
        MonitorCapture::Snapshot snapshot;
        if (g_pdTimelineEngine.takeSnapshot(snapshot, g_pdSnapshotSeq)) {
            g_pdSnapshotSeq = snapshot.sequence;
            // PD output is normalized and captured at the device callback.
            g_rawFullScale = 1.f;
            g_rawSampleRate = std::max(1, g_proj.cs.sr);
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    for(auto& entry:g_pdMeterDb)
        for(float& db:entry.second)db=std::max(-100.f,db-36.f*dt);
    for(auto& entry:g_vstMeterDb)
        for(float& db:entry.second)db=std::max(-100.f,db-36.f*dt);
    if (g_vstEngineOn || g_vstHostedInCsound) {   // the rack of whichever engine hosts the VST graph
        std::vector<VstRack::SoloPeak> peaks;
        vstSolo().takePeaks(peaks);
        for (const auto& p : g_apiProc.takeVstPeaks()) peaks.push_back({p.trackId, p.l, p.r});
        for(const auto& track:peaks) {
            auto it=g_vstMeterDb.find(track.trackId);
            if(it==g_vstMeterDb.end())it=g_vstMeterDb.emplace(track.trackId,std::array<float,2>{-100.f,-100.f}).first;
            const float channels[2] = {track.l, track.r};
            for(int ch=0;ch<2;++ch) {
                const float db=20.f*std::log10(std::max(channels[ch],1e-5f));
                it->second[ch]=std::max(it->second[ch],db);
            }
        }
    }
    if(g_pdEngineOn)for(const auto& track:g_pdTimelineEngine.takePeaks()) {
        auto it=g_pdMeterDb.find(track.trackId);
        if(it==g_pdMeterDb.end())it=g_pdMeterDb.emplace(track.trackId,std::array<float,2>{-100.f,-100.f}).first;
        for(int ch=0;ch<2;++ch) {
            const float db=20.f*std::log10(std::max(track.value[ch],1e-5f));
            it->second[ch]=std::max(it->second[ch],db);
        }
    }
    if(g_mixedPd)for(const auto& track:g_apiProc.takePdPeaks()) {
        auto it=g_pdMeterDb.find(track.trackId);
        if(it==g_pdMeterDb.end())it=g_pdMeterDb.emplace(track.trackId,std::array<float,2>{-100.f,-100.f}).first;
        for(int ch=0;ch<2;++ch) {
            const float db=20.f*std::log10(std::max(track.value[ch],1e-5f));
            it->second[ch]=std::max(it->second[ch],db);
        }
    }
    g_monAge += dt;
    g_rawAge += dt;
    if (g_monAge > 0.5f) std::fill(g_peakIn.begin(), g_peakIn.end(), 0.f);
    if (g_meterDb.size() != g_peakIn.size()) g_meterDb.assign(g_peakIn.size(), -90.f);
    if (g_meterClipHold.size() != g_peakIn.size()) g_meterClipHold.assign(g_peakIn.size(), 0.f);
    for (size_t i = 0; i < g_peakIn.size(); ++i) {
        const float d = 20.f * std::log10(std::max(g_peakIn[i], 1e-5f));
        g_meterDb[i] = std::max(d, g_meterDb[i] - 36.f * dt);
        g_meterClipHold[i] = std::max(0.f, g_meterClipHold[i] - dt);
    }
}

// ---- Run: the project is the Csound engine (one CSD, every clip is an instrument). It stays alive until Stop, like CsoundQt's Run.
// ---- Play: every clip has a start time on the timeline = p2 of its score line. Play sends those lines (from the playhead on) to the running Csound.

bool engineIsUp() { return g_liveCsound && !g_playWaiting && (g_useCsoundApi ? g_apiProc.running() : g_playProc.running()); }

// ---- App -> running Csound: numbered command files (no ports, no stdin, no tables).
// A command = "mode rows" (mode 1 = replaces the running timeline, 0 = plays next to it) and one row of numbers per event (delay in seconds from now).
// Csound reads command n when the serial file says n.
std::vector<std::string> g_engClips, g_engChans, g_engVstNotes;   // names behind the numbers (fixed when Run is pressed)
int g_cmdSerial = 0;                               // number of the last command written
int g_cmdShown = 0;                                // number Csound has been told (the serial file)
bool g_engineSentShown = false;

void writeSerial() {
    if (g_cmdShown == g_cmdSerial) return;
    if (g_useCsoundApi) {
        // The command file is closed before publishing its serial number.
        g_apiProc.setCommandSerial(g_cmdSerial);
        g_cmdShown = g_cmdSerial;
        return;
    }
    // The serial file is changed IN PLACE (8 digits, never deleted or replaced): Csound reads it 100 times a second and can always open it.
    const std::string path = engineFilePrefix() + "serial.txt";
    char text[16];
    std::snprintf(text, sizeof text, "%08d\n", g_cmdSerial);
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!f) { std::ofstream create(path, std::ios::binary); create << text; if (!create) return; }
        else { f.seekp(0); f.write(text, 9); f.flush(); if (!f) return; }
    }
    g_cmdShown = g_cmdSerial;
}

// Writes command n (the file first, then the serial number): mode, number of rows, then the rows.
void sendCommand(bool replace, const std::string& rows, int count) {
    ++g_cmdSerial;
    if (!writeTextFile(engineFilePrefix() + std::to_string(g_cmdSerial) + ".txt", std::string(replace ? "1 " : "0 ") + std::to_string(count) + "\n" + rows)) { logLine("[caw] Could not write the command file for Csound."); return; }
    std::error_code ec;
    if (g_cmdSerial > 100) std::filesystem::remove(engineFilePrefix() + std::to_string(g_cmdSerial - 100) + ".txt", ec);   // old commands are not needed any more
    writeSerial();
}

// Run: a fresh engine has seen no command yet.
void resetCommands() {
    g_cmdSerial = 0;
    g_cmdShown = -1;
    g_engClips = engineClipNames(g_proj);
    g_engChans = engineChannelNames(g_proj);
    g_engVstNotes = engineVstNoteNames(g_proj);
    writeSerial();
}

int numberOf(const std::vector<std::string>& v, const std::string& name) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == name) return (int)i + 1;
    return 0;
}

void warnAddedAfterRun() {
    static bool warned = false;
    if (!warned) { warned = true; logLine("[caw] A clip or channel was added after Run: press Run Csound again to hear it."); }
}

std::vector<VstJob> collectVstJobs(const Project& project, const std::vector<LaneMeterInfo>& meters, bool ignoreMuteSolo = false) {
    std::vector<VstJob> jobs;
    // Same mute/solo rule as the Csound, PD and SC engines: a muted track is silent, and when any
    // track is soloed only the soloed tracks sound.
    bool anySolo = false;
    for (const Track& t : project.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : project.tracks) {
        if ((t.kind != 3 && t.kind != 4) || t.vstPath.empty()) continue;
        if (!ignoreMuteSolo && (t.muted || (anySolo && !t.solo))) continue;
        VstJob j; j.id = t.id; j.name = vstBusName(t); j.path = t.vstPath;
        j.parameterState = t.vstParameterState;
        j.instrument = t.kind == 3; j.pick = t.vstPick; j.init = t.vstInit;
        j.outMode = t.vstOutMode == 1 ? 1 : 0;
        bool anyIn = false;
        for (int s = 0; s < 8; ++s) {
            j.inputs[s].on = t.vstInputs[s].on; j.inputs[s].mode = t.vstInputs[s].mode; j.inputs[s].name = t.vstInputs[s].name;
            const bool configured = t.vstInputs[s].mode == 2 || ((t.vstInputs[s].mode == 0 || t.vstInputs[s].mode == 1) && !t.vstInputs[s].name.empty());
            if (configured) anyIn = true;
        }
        j.routedIn = !j.instrument && anyIn;
        // PD / SuperCollider clips are rendered outside Csound, so their audio never reaches the
        // cawvstin channels. VSTi / VSTe tracks are the same: the rack renders their plugins and
        // adds the result straight to the master. Those sources feed the WHOLE source mix into
        // the slot instead (stereo - the same channels the source sends to the master). Same
        // lookup order as resolveVstInputRoutes: PD / SC first, then VST, then Csound lines.
        const auto engineOf = [&project](const std::string& trackName, const std::string& clipName, int mode, int& srcId) {
            const auto owner = [&](int kindA, int kindB) -> const Track* {
                for (const Track& o : project.tracks) {
                    if (o.kind != kindA && o.kind != kindB) continue;
                    const bool match = mode == 0 ? o.name == trackName
                        : std::any_of(o.clips.begin(), o.clips.end(),
                            [&clipName](const Clip& c) { return c.name == clipName; });
                    if (match) return &o;
                }
                return nullptr;
            };
            if (const Track* o = owner(5, 6)) { srcId = o->id; return o->kind == 6 ? 1 : 2; }   // Pure Data / SuperCollider mix
            if (const Track* o = owner(3, 4)) { srcId = o->id; return 3; }                      // VSTi / VSTe output, wired by the rack
            return 0;                                                                           // a Csound track: the CSD routing writes the lines
        };
        // Carry the resolved line ranges into the job so the rack can pack the ENABLED slots from
        // line 1 (live on/off never leaves silent gaps in front of a later input).
        if (j.routedIn) {
            const auto routes = resolveVstInputRoutes(project, t);
            for (const VstInputRoute& r : routes) {
                if (r.slot < 0 || r.slot >= 8) continue;
                j.inputs[r.slot].lineStart = r.lineStart;
                j.inputs[r.slot].lineCount = r.lineCount;
                if (r.mode != 2) j.inputs[r.slot].engine = engineOf(r.name, r.name, r.mode, j.inputs[r.slot].srcId);
                if (j.inputs[r.slot].engine == 0) j.inputs[r.slot].csChans = r.chans;   // the per-clip tap channels the CSD publishes
            }
        }
        for (const LaneMeterInfo& lm : meters) if (lm.trackId == t.id) { j.meterFirst = lm.first; break; }
        jobs.push_back(j);
    }
    return jobs;
}

// ---------------------------------------------------------------- CAW's 4th engine: the independent VST rack
// The instrument tracks render in the app (see vst_solo.h) - like a DAW playing a VST while
// sharing the soundcard. Notes from MIDI clips are scheduled by CAW itself (no Csound needed);
// when Csound runs, its note ring (clip code) and parameter channels are forwarded in.
// What the VST engine loads: instruments AND effects, whatever Csound is doing - so a Csound
// start / stop never changes the plugin set (destroying and re-creating instances is what left a
// plugin mute).
std::vector<VstJob> collectVstEngineJobs() {
    return collectVstJobs(g_proj, {}, true);   // muted or not: M / S are applied live
}

// The MIDI-clip notes of the instrument tracks as one absolute-time schedule. A VSTi track's
// own MIDI clips go straight to its plugin; MIDI clips on any other track still route through
// their target clip (clipId) - when that clip lives on a VSTi track, its notes drive that plugin.
std::vector<VstSoloNote> buildVstSoloNotes() {
    std::vector<VstSoloNote> out;
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : g_proj.tracks) {
        if (t.muted || (anySolo && !t.solo) || t.midiClips.empty()) continue;
        for (const MidiClip& mc : t.midiClips) {
            if (mc.muted) continue;
            int targetTrackId = -1;
            if (t.kind == 3) {
                targetTrackId = t.id;
            } else {
                int laneIdx = -1;
                const Clip* target = findClip(mc.clipId, &laneIdx);
                if (!target || target->muted || laneIdx < 0) continue;
                const Track& vt = g_proj.tracks[(size_t)laneIdx];
                if (vt.kind != 3 || vt.muted || (anySolo && !vt.solo)) continue;   // not a VSTi: the Csound path plays it
                targetTrackId = vt.id;
            }
            for (const MidiNote& n : mc.notes) {
                if (n.start >= mc.dur) continue;
                VstSoloNote sn;
                sn.start = mc.start + n.start;
                sn.dur = std::max(0.005, std::min(mc.start + mc.dur, sn.start + n.dur) - sn.start);
                sn.pitch = std::min(127, std::max(0, n.pitch));
                sn.velocity = std::min(127, std::max(1, n.velocity));
                sn.trackId = targetTrackId;
                if (targetTrackId >= 0) out.push_back(sn);
            }
        }
    }
    return out;
}

// The clip code of the VSTi tracks for the VST-only path (no Csound engine): the VST engine
// interprets the CAW language itself, so `note / cc / msg` lines play without Csound. While
// Csound runs, it translates and forwards the same code, so this list stays empty there.
std::vector<VstScript> buildVstScripts() {
    std::vector<VstScript> out;
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 3 || t.muted || (anySolo && !t.solo)) continue;
        for (const Clip& c : t.clips) {
            if (c.muted || c.code.empty()) continue;
            VstScript s;
            s.trackId = t.id;
            s.clipId = c.id;
            s.start = c.start;
            s.dur = std::max(0.01, c.dur);
            s.code = c.code;
            for (int k = 0; k < 8; ++k) {
                double v = k < (int)t.vstInit.size() ? t.vstInit[(size_t)k] : 0.5;
                s.params[k] = std::isfinite(v) ? std::min(1.0, std::max(0.0, v)) : 0.5;
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

// A dedicated output stream only when NO other stream is carrying audio (the engine's, PD's,
// SC's all get the mix through the realtime hook instead). Only while the VST engine runs.
void ensureVstSoloOutput() {
    if (!g_vstEngineOn || !vstSolo().ready() || g_vstSoloOutput || g_vstRetryT > 0.0) return;   // no plugin: no stream
    if (scHasSharedAudioOwner() || g_scSoloOutput || g_playing || g_testSound.running() || g_renderActive) return;
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) return;   // no output chosen: Settings already asks for one
    auto output = std::make_unique<CawAudioOutput>();
    CawAudioSettings settings = pdAudioSettings();
    auto capture = std::make_unique<MonitorCapture>(settings.sampleRate, settings.channels);
    output->setMonitorCapture(capture.get());
    std::string error;
    if (!output->open(settings, error)) {
        logLine("[caw] VST engine output: " + error);
        g_vstRetryT = 20.0;   // do not retry (and log) every frame
        return;
    }
    output->setExpectingAudio(false);
    g_vstSoloCapture = std::move(capture);
    g_vstSoloOutput = std::move(output);
    g_vstSoloSnapshotSeq = 0;
    logLine("[caw] VST engine: playing through " + g_vstSoloOutput->description());
}

bool trackIsVsti(int trackId) {
    for (const Track& t : g_proj.tracks) if (t.id == trackId) return t.kind == 3;
    return false;
}

// The set of VST track ids that should keep sounding right now (live M / S). Recomputed whenever a
// mute or solo flag changes and pushed to the running engine so the VSTe reacts without a restart.
std::set<int> collectVstActiveTracks(const Project& project) {
    std::set<int> ids;
    bool anySolo = false;
    for (const Track& t : project.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : project.tracks)
        if ((t.kind == 3 || t.kind == 4) && !t.vstPath.empty() && !t.muted && !(anySolo && !t.solo))
            ids.insert(t.id);
    return ids;
}

// What the 4th engine processes: live M / S. Effects take part too - it is the effect rack while
// Csound is off (with Csound on, g_vstHostedInCsound keeps this engine silent).
std::set<int> vstSoloActiveIds() {
    return collectVstActiveTracks(g_proj);
}

// The live "may this Csound clip reach the master" gates of the CSD: one channel per (clip,
// VSTe track), cawvstmix_<clip>_<vstId>. 0 = audible, 1 = that effect replaces the source. A
// master-mode effect REPLACES every clip its enabled inputs name, and only while it is active;
// everything else stays 0, so an unused channel can never mute anything. Called at Run, for the
// render, and live whenever the wiring / AUDIO OUTPUT / M / S / input on/off changes.
std::vector<std::pair<std::string, double>> vstGateValues() {
    std::vector<std::pair<std::string, double>> out;
    if (!g_langVst) return out;   // VST off for the session: nothing processes, so nothing may replace a source
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& vt : g_proj.tracks) {
        if (vt.kind != 4 || vt.vstPath.empty()) continue;
        const bool vtActive = !vt.muted && !(anySolo && !vt.solo);
        for (const Track& t : g_proj.tracks) {
            if (t.kind == 5 || t.kind == 6) continue;
            for (const Clip& c : t.clips) {
                bool mute = false;
                if (vtActive && vt.vstOutMode == 0)
                    for (const auto& in : vt.vstInputs) {
                        if (!in.on) continue;
                        if ((in.mode == 0 && in.name == t.name) || (in.mode == 1 && in.name == c.name)) { mute = true; break; }
                    }
                out.push_back({"cawvstmix_" + c.name + "_" + std::to_string(vt.id), mute ? 1.0 : 0.0});
            }
        }
    }
    return out;
}

// Live M / S: the rack of whichever engine hosts the VST graph follows it (both get the same set;
// the idle engine does nothing with it).
void pushVstActiveLive() {
    if (g_liveCsound && g_useCsoundApi) g_apiProc.setVstActiveTracks(collectVstActiveTracks(g_proj));
    if (g_vstEngineOn) vstSolo().setActive(vstSoloActiveIds());
}

bool anyVstiTrack() {
    for (const Track& t : g_proj.tracks) if (t.kind == 3 && !t.vstPath.empty() && !t.muted) return true;
    return false;
}
bool anyVsteTrack() {
    for (const Track& t : g_proj.tracks) if (t.kind == 4 && !t.vstPath.empty() && !t.muted) return true;
    return false;
}
bool anyVstTrack() { return anyVstiTrack() || anyVsteTrack(); }

// The panel's live-edit hooks (parameters, M / S, picks). Wired once; they serve whichever rack
// hosts the VST graph - Csound's while it runs, the 4th engine while it does (both get the calls;
// the idle engine simply has no such track loaded).
void wireVstUiHooks() {
    if (vstui::pushControlChannels) return;
    vstui::pushControlChannels = [](const std::vector<std::pair<std::string, double>>& values) { g_apiProc.setControlChannels(values); };
    vstui::pushVstActive = [](const std::set<int>& ids) {
        g_apiProc.setVstActiveTracks(ids);
        if (g_vstEngineOn) vstSolo().setActive(ids);
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());   // an inactive effect no longer replaces its source
    };
    vstui::pushVstParamOverride = [](int trackId, int slot, double value) {
        g_apiProc.pushVstParamOverride(trackId, slot, value);
        if (g_vstEngineOn && trackIsVsti(trackId)) vstSolo().pushParamOverride(trackId, slot, value);   // the 4th engine's own copy of the instrument
    };
    vstui::pushVstPickRefresh = [](int trackId, const std::vector<int>& pick) {
        g_apiProc.refreshVstPicks(trackId, pick);
        if (g_vstEngineOn && trackIsVsti(trackId)) vstSolo().pushPickUpdate(trackId, pick);
    };
    vstui::pushVstParamLive = [](int trackId, int paramIndex, double value) {
        g_apiProc.pushVstParamValue(trackId, paramIndex, value);
        if (g_vstEngineOn && trackIsVsti(trackId)) vstSolo().pushParamValue(trackId, paramIndex, value);
    };
    vstui::pushVstOutModeLive = [](int trackId, int mode) {
        g_apiProc.setVstOutModeLive(trackId, mode);
        if (g_vstEngineOn) vstSolo().setOutMode(trackId, mode);   // the effect may run in the VST engine
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());   // replace <-> mix, now
    };
    vstui::pushVstInputMasks = [](const std::map<int, unsigned>& masks) {
        g_apiProc.setVstInputMasks(masks);
        if (g_vstEngineOn) vstSolo().setInputMasks(masks);
        // The checkbox also changes the slot's configured state, so the jobs are re-pointed too:
        // an input that was off at Run can be switched ON live and start feeding at once.
        if (g_langVst && g_liveCsound && g_useCsoundApi) g_apiProc.setVstWiring(collectVstJobs(g_proj, g_engineMeters, true));
        if (g_vstEngineOn) vstSolo().setWiring(collectVstEngineJobs());
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());   // an input switched off stops replacing
    };
    vstui::pushVstWiring = []() {   // an input mode / name changed: re-route the effect now, no Run
        if (g_langVst && g_liveCsound && g_useCsoundApi) g_apiProc.setVstWiring(collectVstJobs(g_proj, g_engineMeters, true));
        if (g_vstEngineOn) vstSolo().setWiring(collectVstEngineJobs());
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());
    };
    // A .vst3 finished loading into the editor: the engine applies it NOW (not up to 0.5 s later),
    // and when the transport is already playing it comes up by itself - a VSTi track plays in
    // every situation, also when Run VST was never pressed. A re-picked plugin also gets a new
    // attempt after a failed load.
    vstui::notifyPluginLoaded = [](int trackId) {
        const Track* t = nullptr;
        for (const Track& o : g_proj.tracks) if (o.id == trackId && (o.kind == 3 || o.kind == 4)) { t = &o; break; }
        if (!t) return;
        g_vstFailedSig.clear(); g_vstFailedRetryT = 0.0;
        if (g_langVst && g_liveCsound && g_useCsoundApi)
            g_apiProc.setVstWiring(collectVstJobs(g_proj, g_engineMeters, true));   // the Csound rack reloads the new .vst3 live, no Run and no delete / re-create
        if (t->kind == 3 && !g_vstEngineOn) { if (g_timelineRun && g_langVst) startVstEngine(); return; }   // VST not enabled for the session: do not auto-run
        vstEngineCheckJobs(10.0f);   // past the 0.5 s gate: the changed plugin set is applied on this frame
    };
    // Values the engine's plugins are actually using: the Csound rack first, then the 4th engine
    // (clip code writes P1..P8 there too, so the P sliders and the plugin GUI must follow it).
    vstui::takeVstEngineParams = [](int trackId, double out[8]) {
        if (g_apiProc.vstEngineParams(trackId, out)) return true;
        return g_vstEngineOn && vstSolo().params(trackId, out);
    };
    g_apiProc.setScAudioRing(&g_scRing);            // VSTe "SuperCollider" inputs read this
    g_apiProc.setScRawSuppress(&g_scRawSuppress);   // master-mode VSTe replaces the raw SC mix
}

// ---------------------------------------------------------------- the VST engine (own Run / Stop button)
// Loads the instrument plugins, schedules the MIDI clips from the playhead and mixes the sound into
// whatever stream is open (or opens its own). It does not need Csound; when Csound runs too, the
// notes written by clip code (note / cc / msg) and the P1..P8 channels are forwarded into it.
void vstEngineJoinTransport() {   // notes + loop + position, from the current playhead
    // The Csound rack needs nothing here: its own score plays the MIDI clips into every VSTi's note
    // ring on the Csound clock. Only CAW's own VST engine (Csound off) gets the schedule pushed.
    const std::vector<VstSoloNote> notes = buildVstSoloNotes();
    if (!g_vstEngineOn) return;
    vstSolo().setNotes(notes);
    vstSolo().setLoop(timeline_loop::valid(g_proj), g_proj.loopStart, g_proj.loopEnd);
    vstSolo().play(g_play);
}

bool applyVstInstrumentJobs(const std::vector<VstJob>& jobs) {   // (re)load the plugins when the set changed
    vstui::captureParameters(g_proj);
#if defined(CAW_VST3) && defined(_WIN32)
    static bool com = false; if (!com) { OleInitialize(nullptr); com = true; }   // plugins expect COM on the thread that loads them
#endif
    const CawAudioSettings audio = pdAudioSettings();
    static VstEngineAudio soloAudio;   // the SuperCollider mix is captured by CAW's callback (g_scRing)
    soloAudio.scRing = &g_scRing;
    soloAudio.pdMix = nullptr;         // the Pure Data mix has no app-side ring: a PD input needs Csound
    if (!vstSolo().start(jobs, audio.sampleRate, 2, 512, &soloAudio)) return false;
    vstSolo().setActive(vstSoloActiveIds());
    vstSolo().setInputMasks(collectVstInputMasks(g_proj));
    if (!g_liveCsound && !g_playing) {   // the effects run in THIS engine: say what each one can hear here
        for (const VstJob& j : jobs) {
            if (j.instrument) continue;
            std::string sum;
            bool csInput = false;
            for (int s = 0; s < 8; ++s) {
                const auto& src = j.inputs[s];
                if (!src.on || src.lineCount <= 0) continue;
                if (!sum.empty()) sum += ", ";
                sum += "in" + std::to_string(s + 1) + ": ";
                if (src.mode == 2) sum += "master";
                else if (src.engine == 1) { sum += "\"" + src.name + "\" (PureData - needs Csound)"; csInput = true; }
                else if (src.engine == 2) sum += "\"" + src.name + "\" (SuperCollider mix)";
                else if (src.engine == 3) sum += "\"" + src.name + "\" (VST output)";
                else { sum += "\"" + src.name + "\" (Csound track - needs Csound)"; csInput = true; }
            }
            logLine("[caw] VSTe \"" + j.name + "\" runs in the VST engine; inputs: " +
                    (sum.empty() ? std::string("(none enabled - nothing to process)") : sum) + ".");
            if (csInput)
                logLine("[caw] VSTe \"" + j.name + "\": a Csound / PD track only sounds while Csound runs. "
                        "With the VST engine alone, wire the effect to a VSTi track, the master, or the SuperCollider mix.");
        }
    }
    return true;
}

// Everything that must follow a successful load: the clip code, the schedule and the output
// stream. ONE function for every path (Run VST, a plugin picked later, a retry), so the order in
// which the user does things can never leave the engine half-started.
void vstEngineAfterLoad() {
    vstSolo().setScripts(engineIsUp() ? std::vector<VstScript>() : buildVstScripts());   // clip code runs in this engine when Csound is off
    vstSolo().setWiring(collectVstEngineJobs());   // the effect inputs / output modes as configured now
    if (g_timelineRun) vstEngineJoinTransport();   // Play was already running: join it from the playhead
    else { vstSolo().setNotes(buildVstSoloNotes()); vstSolo().setLoop(timeline_loop::valid(g_proj), g_proj.loopStart, g_proj.loopEnd); }
    ensureVstSoloOutput();   // its own stream, unless Csound / PD / SC already carry the audio
}

// Load / reload the instruments and make them play. A failed load is retried every 5 s (and at
// once when the plugin is picked again) instead of leaving the engine silent until Run VST is
// pressed once more.
bool vstEngineLoadJobs(const std::vector<VstJob>& jobs) {
    if (applyVstInstrumentJobs(jobs)) {
        g_vstFailedSig.clear(); g_vstFailedRetryT = 0.0;
        vstEngineAfterLoad();
        return true;
    }
    g_vstFailedSig = VstSoloEngine::signature(jobs);
    g_vstFailedRetryT = 5.0;
    return false;
}

void startVstEngine() {
    if (g_vstEngineOn) return;
    if (!g_langVst) {   // the session list says "do not load VST": tracks and their code still work
        playNotice("VST is not enabled for this session - enable it in Settings > Languages.", "VST is disabled");
        logLine("[caw] VST is not enabled (Settings > Languages): Run VST does nothing, the plugins are not loaded.");
        return;
    }
    if (g_renderActive) { playNotice("An export is running - wait for it or cancel it first.", "Export running"); return; }
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) { playNotice("Select an audio output in Settings > Audio.", "Select an audio output"); return; }
    const std::vector<VstJob> jobs = collectVstEngineJobs();
    wireVstUiHooks();
    vstSolo().setLog([](const std::string& line) {   // called from the audio thread too
        std::lock_guard<std::mutex> lock(g_vstLogMutex);
        if (g_vstLogQueue.size() < 200) g_vstLogQueue.push_back(line);
    });
    // The engine runs even with no plugin yet: choosing / changing a .vst3 later loads it
    // automatically (notifyPluginLoaded + vstEngineCheckJobs) without a Stop / Run VST cycle -
    // and the P1..P8 mapping is carried over to the new plugin.
    g_vstEngineOn = true;
    g_vstRetryT = 0.0;
    g_vstFailedSig.clear();
    g_vstFailedRetryT = 0.0;
    g_vstJobsT = 0.0;
    g_vstMeterDb.clear();
    const bool kept = vstSolo().ready();   // Stop VST keeps the plugins loaded: this Run resumes them
    if (kept && jobs.empty()) vstSolo().unload();   // the plugin was removed while stopped: free its instances
    vstSolo().setEngineOn(true);
    if (jobs.empty()) {
        playNotice("The VST engine is on. Load a .vst3 into a VST track (right-click the track) - it starts by itself.",
                   "Run VST: waiting for a plugin");
        vstEngineAfterLoad();
    } else if (!vstEngineLoadJobs(jobs)) {
        playNotice("The VST engine is on, but the plugin did not load - see the log. It is retried every 5 s (picking the plugin again retries at once).",
                   "VST engine: no plugin loaded - see the log");
    } else if (kept) {
        logLine("[caw] VST engine: the plugins stayed loaded - the same instances play again (no reload).");
    }
    logLine("[caw] VST engine on: VSTi play from their MIDI clips and clip code; VSTe effects run here while Csound is off. Press Play.");
}

void stopVstEngine(bool unloadPlugins) {
    if (!g_vstEngineOn) {
        if (unloadPlugins && vstSolo().ready()) {   // stopped earlier with the plugins kept: free them now
            vstSolo().unload();
            logLine("[caw] VST engine stopped, plugins unloaded.");
        }
        return;
    }
    g_vstEngineOn = false;
    g_vstFailedSig.clear();
    g_vstFailedRetryT = 0.0;
    g_vstJobsT = 0.0;
    vstSolo().setEngineOn(false);
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
    g_vstMeterDb.clear();
    if (unloadPlugins) {
        vstSolo().unload();   // a new project / exit: the plugins go away with it
        logLine("[caw] VST engine stopped, plugins unloaded.");
    } else {
        // Stop VST only stops the sound: the plugin instances stay loaded so Run VST resumes
        // exactly what was playing. Unloading and re-creating them here is what left an
        // instrument silent until its .vst3 was picked again.
        vstSolo().stop();
        logLine("[caw] VST engine stopped (the plugins stay loaded - Run VST resumes them).");
    }
}

// The 0.5 s check: the clip-code mode and the instrument set. It runs from vstEngineTick (per
// frame, gated) and at once from notifyPluginLoaded (dt >= 0.5), so a plugin picked in the panel
// is loaded on the very frame - and a plugin picked BEFORE Run VST behaves exactly like one
// picked after it.
void vstEngineCheckJobs(float dt) {
    if (!g_vstEngineOn) return;
    g_vstJobsT += dt;
    if (g_vstJobsT < 0.5) return;
    g_vstJobsT = 0.0;
    {   // Clip code: this engine interprets it only while Csound is off. A running Csound translates
        // the same code and forwards its notes, so the scripts are empty then (no double notes).
        static int scriptsMode = -1;
        const bool csUp = engineIsUp();
        vstSolo().setScripts(csUp ? std::vector<VstScript>() : buildVstScripts());
        const int mode = csUp ? 0 : 1;
        if (mode != scriptsMode) {
            const bool hadMode = scriptsMode >= 0;
            scriptsMode = mode;
            if (!hadMode) { /* first check after Run VST: quiet */ }
            else if (mode == 1) logLine("[caw] VST engine: Csound is off - the clip code (note / cc / msg, P1..P8) runs in the VST engine.");
            else logLine("[caw] VST engine: Csound is on again - it translates the clip code and forwards its notes.");
        }
    }
    pushVstActiveLive();   // whichever engine hosts the VST graph follows the live M / S state
    const std::vector<VstJob> jobs = collectVstEngineJobs();
    vstSolo().setWiring(jobs);   // effect inputs / output modes as configured now (no plugin reload)
    if (vstSolo().sameJobs(jobs)) { g_vstFailedSig.clear(); return; }
    if (jobs.empty()) {
        g_vstFailedSig.clear();
        if (vstSolo().ready()) {   // the last VST track / plugin went away: unload, keep the button on and wait
            vstSolo().unload();
            vstSolo().setEngineOn(true);   // Run VST stays on: a plugin loaded later starts playing by itself
            g_vstMeterDb.clear();
            logLine("[caw] VST engine: no plugin now - waiting (Run VST stays on).");
        }
        return;
    }
    const std::string sig = VstSoloEngine::signature(jobs);
    if (sig == g_vstFailedSig && g_vstFailedRetryT > 0.0) return;   // this set already failed: retried when the timer runs out
    logLine("[caw] VST engine: the plugins changed - reloading.");
    if (!vstEngineLoadJobs(jobs))
        logLine("[caw] VST engine: the plugin could not be loaded - retrying every 5 s (see the lines above).");
}

// Every frame: the engine's log, the dedicated stream, and a reload when a plugin was loaded / removed.
void vstEngineTick(float dt) {
    {
        std::vector<std::string> queued;
        { std::lock_guard<std::mutex> lock(g_vstLogMutex); queued.swap(g_vstLogQueue); }
        for (std::string& line : queued) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) logLine(line);
        }
    }
    if (!g_vstEngineOn) return;
    if (g_vstRetryT > 0.0) g_vstRetryT -= dt;
    if (g_vstFailedRetryT > 0.0) g_vstFailedRetryT -= dt;
    if (!g_vstSoloOutput) ensureVstSoloOutput();
    vstEngineCheckJobs(dt);
}

// Live VSTE input on/off: one bitmask per VSTe track, bit i = input slot i feeds the plugin.
std::map<int, unsigned> collectVstInputMasks(const Project& project) {
    std::map<int, unsigned> masks;
    for (const Track& t : project.tracks) {
        if (t.kind != 4) continue;
        unsigned mask = 0;
        for (int s = 0; s < 8; ++s) if (t.vstInputs[s].on) mask |= (1u << s);
        masks[t.id] = mask;
    }
    return masks;
}

std::string num(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(9) << v;
    return o.str();
}

// A row of a command:  type  number  delay  dur  a  b   (10 clip start, 11 ramp, 12 silence, 13 stop clip)
std::string row(int type, int ix, double delay, double dur, double a, double b) {
    return std::to_string(type) + " " + std::to_string(ix) + " " + num(delay) + " " + num(dur) + " " + num(a) + " " + num(b) + "\n";
}

void commandStopAll() { sendCommand(true, row(12, 0, 0, 0, 0, 0), 1); }

// Play from time t: the timeline events (clip start times) from the playhead on. stopFirst: silence what sounds now (play, seek).
// Without it (live edit) what sounds keeps sounding and only the events still to come are replaced.
void commandPlayFrom(double t, bool stopFirst) {
    std::string rows;
    int count = 0, sent = 0;
    if (stopFirst) { rows += row(12, 0, 0, 0, 0, 0); ++count; }
    const bool looping = timeline_loop::valid(g_proj);
    const double remaining = looping ? std::max(0.0, g_proj.loopEnd - t) : 0.0;
    for (TimelineEvent ev : timelineEvents(g_proj, t, stopFirst)) {
        if (looping && !timeline_loop::trim(ev, remaining)) continue;
        int type = ev.automation ? 11 : 10;
        int ix = 0;
        if (!ev.automation && ev.name.rfind("CawVstNote_", 0) == 0) {
            // MIDI-clip notes of the VSTi tracks. While Csound hosts the VST graph, its score fires
            // the note helper (CawVstNote_<id>), which writes the note into the plugin's ring - the
            // same transport and clock as the Csound clips. Without Csound hosting, CAW's 4th
            // engine schedules them itself (audio-rate, loop-aware), so they are not sent here.
            if (!g_vstHostedInCsound) continue;
            type = 14;
            ix = numberOf(g_engVstNotes, ev.name);
        } else {
            ix = ev.automation ? numberOf(g_engChans, ev.name) : numberOf(g_engClips, ev.name);
        }
        if (!ix) { warnAddedAfterRun(); continue; }
        rows += row(type, ix, ev.start, ev.dur, ev.a, ev.b);
        ++count; ++sent;
    }
    if (looping) {
        rows += row(12, 0, remaining, 0, 0, 0);
        ++count;
    }
    sendCommand(true, rows, count);
    if (!g_engineSentShown && sent) { g_engineSentShown = true; logLine("[caw] timeline -> Csound: " + std::to_string(sent) + " clip / note / ramp starts from " + num(t) + " s"); }
}

// The Run button of a clip editor: the clip plays next to the timeline.
void commandStartClip(const Clip& clip, double dur, double p4, double p5) {
    const int ix = numberOf(g_engClips, clip.name);
    if (!ix) { warnAddedAfterRun(); return; }
    sendCommand(false, row(10, ix, 0, dur, p4, p5), 1);
}

void commandStopClip(const Clip& clip) {
    const int ix = numberOf(g_engClips, clip.name);
    if (ix) sendCommand(false, row(13, ix, 0, 0, 0, 0), 1);
}

void stopPlayback() {
    if (g_useCsoundApi) g_apiProc.stop();
    else g_playProc.stop();
    drainPlayOutput();
    closeMonitorSocket();
    g_useCsoundApi = false;
    g_cawMonitorApi = false;
    g_monHeld.clear();
    g_playing = false;
    g_liveCsound = false;
    g_vstHostedInCsound = false;   // the Csound rack is gone: the 4th engine is the VST host again
    g_playWaiting = false;
    g_runLeft = 0.0;
    g_mixedPd = false;
    restoreScSoloOutput();   // SC and the VST engine get the soundcard back
}

void startEngine() {
    if (g_pdEngineOn || g_pdTimelineMode || g_pdPreview.active()) {
        // Csound + PD share one engine: stop the separate PD engine (and SC running on it) automatically, then continue.
        logLine("[caw] Stopping the separate PD engine; Csound + PD will run together on one engine.");
        if (g_scEngineOn) stopScEngine();
        if (g_pdTimelineMode && g_timelineRun) { pdTransport(false); g_timelineRun = false; }
        g_pdResumeAfterReload = false;
        g_pdPreview.stop();
        g_pdTimelineEngine.stop();
        g_pdPlayingClip = 0; g_pdEngineOn = false; g_pdTimelineMode = false; g_pdTimelineEvents.clear();
        restoreScSoloOutput();
    }
    if (g_testSound.running()) { logLine("[caw] Wait for the test sound to finish before starting Csound."); return; }
    if (g_renderActive) { logLine("[caw] A render is running - wait for it or cancel it first."); return; }
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_playing) stopPlayback();
    vstui::captureParameters(g_proj);
    diagClearAll();
    g_cawScopeDiv = scopeDiv();
    g_cawScopePeak = g_scopeSample;
    g_scopeActiveMs = scopeWindowMs();
    g_scopeHasFrame = false;
    shareEngineDlls();
    const std::string exe = csoundExePath();
    if (exe.empty()) {
        logLine("[caw] Csound was not found. Either install Csound (https://csound.com/download.html - Csound 6 and 7 both work) or copy a full portable Csound folder into CAW's folder as <CAW folder>\\Csound\\bin\\csound.exe (1-PREPARE.ps1 -CsoundDir <your Csound folder> stages it for you). CAW can also browse for it in Settings > Programs.");
        playNotice("Csound was not found - see the log for how to fix it.", "Csound missing");
        return;
    }
    if (!ensureCsoundMathProbe()) {
        // The probe result (and Csound's own message, if any) was just logged. Refuse only on real
        // evidence: a Csound that cannot compile abs OR sqrt in a tiny test cannot build CAW's meters.
        logLine("[caw] ERROR: " + exe + " cannot compile abs or sqrt (CAW compiled tiny test orchestras through the same Csound library the engine uses" + (g_mathProbeDetail.empty() ? ")." : ": " + g_mathProbeDetail + ").") + " CAW's meters need one of them; this Csound installation or copy is incomplete. Pick a full csound.exe in Settings > Programs (Browse opens CAW's own folder) or re-copy it with 1-PREPARE, then try again.");
        return;
    }
    std::string apiError;
    wireVstUiHooks();   // the panel's live-edit hooks (once is enough)
    g_useCsoundApi =!cawEnv("CAW_MON_TEXT") && !cawEnv("CAW_MON_OSC") && g_apiProc.prepare(exe, apiError);
    if (g_scEngineOn && !g_useCsoundApi) {
        logLine("[caw] SuperCollider can share output only with CAW's Csound API host. Clear CAW_MON_TEXT/CAW_MON_OSC or fix the Csound API setup.");
        g_useCsoundApi = false;
        return;
    }
    const bool mixedRequested = g_langCsound && g_langPd;
    std::vector<pd_timeline::Event> mixedPlan;
    if (mixedRequested) {
        if (!g_useCsoundApi) {
            logLine("[caw] Shared Csound/PD audio requires the Csound API host. " + apiError);
            return;
        }
        if (g_pdLibraryPath.empty()) g_pdLibraryPath = plat::bundledLibpd();
        if (g_pdLibraryPath.empty()) { logLine("[caw] libpd was not found. Select it in Settings > Programs, or copy libpd.dll (+ libwinpthread-1.dll) into <CAW folder>\\PureData\\."); return; }
        int bad=0;
        // Include muted clips, flagged: M / S inside the shared engine is a live replan, never
        // a "clips changed" restart of Csound + PD + the device.
        if (!pd_timeline::makePlan(g_proj,g_projectPath,mixedPlan,apiError,false,&bad,true)) {
            logLine("[caw] PD clip problem: "+apiError);
            if(bad>0)for(Track& t:g_proj.tracks)for(Clip& c:t.clips)if(c.id==bad)c.editorOpen=true;
            return;
        }
    }
    if (!g_useCsoundApi && std::fabs(g_proj.cs.masterGain - 1.f) > 0.0001f) {
        logLine("[caw] The master gain requires CAW host audio. Restore Master to 100% or enable the Csound API.");
        return;
    }
    g_cawMonitorApi = g_useCsoundApi;
    if (g_useCsoundApi) logLine("[caw] Using the Csound API and in-memory monitor.");
    else if (g_proj.cs.monitorFeed && !cawEnv("CAW_MON_TEXT")) {
        if (!apiError.empty()) logLine("[caw] " + apiError + " Trying the OSC monitor instead.");
        if (!openMonitorSocket()) logLine("[caw] OSC monitor unavailable, using the text monitor.");
        else logLine("[caw] Binary OSC monitor active (local only).");
    }
    const std::string csdPath = (workDir() / "caw_play.csd").string();
    Project engineProject = g_proj;
    if (!g_useCsoundApi) {
        // The legacy process engine uses Csound's own device indices.
        // Enumerate them only when that engine is actually needed.
        if (g_csoundOutDevs.empty() && g_csoundInDevs.empty()) refreshAudioDevices(true);
        // The old process engine opens the card by CSOUND's device number, which differs from the PortAudio list shown in Settings:
        // translate by device label (a device Csound's PortAudio cannot see falls back to the default).
        auto translate = [](int id, const std::vector<AudioDevice>& shown, const std::vector<AudioDevice>& csound, const char* what) {
            if (id < 0) return id;
            for (const AudioDevice& d : shown) if (d.id == id)
                for (const AudioDevice& c : csound)
                    if (normalizeCawDeviceLabel(c.name) == normalizeCawDeviceLabel(d.name)) return c.id;
            logLine(std::string("[caw] The selected ") + what + " device is not available in Csound's own audio system - using the default.");
            return -1;
        };
        engineProject.cs.outDev = translate(g_proj.cs.outDev, g_outDevs, g_csoundOutDevs, "output");
        engineProject.cs.inDev = translate(g_proj.cs.inDev, g_inDevs, g_csoundInDevs, "input");
        if (!engineProject.cs.options.empty()) syncOptionsFromSettings(engineProject.cs);
    }
    if (!writeTextFile(csdPath, buildCsd(engineProject, "engine", 0.0, &g_orcSpans))) {
        logLine("[caw] ERROR: could not write " + csdPath);
        closeMonitorSocket();
        g_cawMonitorApi = false;
        g_useCsoundApi = false;
        return;
    }
    const std::string cmd = "\"" + exe + "\" \"" + csdPath + "\"";
    if (!g_useCsoundApi) logLine("[caw] Run (Csound runs until Stop; Play sends the clips at their start times): " + cmd);
    resetCommands();   // the command files exist before Csound reads them
    g_engineMeters = meterLayout(g_proj);
    g_recNames = cawWriteTargets(g_proj);
    int top = masterChannels(g_proj.cs);
    for (const LaneMeterInfo& lm : g_engineMeters) top = std::max(top, lm.first + lm.channels);
    {
        g_engineMaster = masterChannels(g_proj.cs);
        g_peakIn.assign((size_t)top, 0.f);
        g_meterDb.assign((size_t)top, -90.f);
        g_meterClipHold.assign((size_t)top, 0.f);
    }
    g_haveBands = false;
    g_haveRaw = false;
    g_scopeFreeze = false;
    g_scopeFrozenHasFrame = false;
    g_specFreeze = false;
    g_rawAge = 10.f;
    g_rawAssembler.reset();
    g_fftDb.clear();
    g_spectrumBands.clear();
    g_columnBands.clear();
    g_tonePeaks.clear();
    g_fftHistory.clear();
    g_rawSampleRate = std::max(1, g_proj.cs.sr);
    g_rawFullScale = (float)std::max(0.0001, g_proj.cs.zdbfs);
    g_monHeld.clear();
    g_monSeen = false;
    g_monWarned = false;
    g_monRunT = 0.f;
    g_engineSentShown = false;
    g_apiSnapshotSeq = 0;
    if (g_useCsoundApi) {
        // First host-output version is output-only. Never silently drop a
        // configured recording input; the legacy OSC engine supports it.
        if (g_proj.cs.inDev >= 0 || g_proj.cs.options.find("-iadc") != std::string::npos) {
            logLine("[caw] CAW-owned audio input is not available yet. Use run_osc_monitor.bat for audio input.");
            g_cawMonitorApi = false; g_useCsoundApi = false;
            return;
        }
        if (g_proj.cs.outDev == -2) {
            logLine("[caw] No output selected. Choose a soundcard in Settings or use the OSC launcher for silent output.");
            g_cawMonitorApi = false; g_useCsoundApi = false;
            return;
        }
        CawAudioSettings audio;
        audio.sampleRate = g_proj.cs.sr;
        audio.channels = g_proj.cs.nchnls;
        audio.softwareBuffer = g_proj.cs.bufSW;
        audio.hardwareBuffer = g_proj.cs.bufHW;
        audio.fullScale = g_rawFullScale;
        attachScRealtimeSource(audio);
        int selected = g_proj.cs.outDev;
        if (selected == -1 && plat::isWindows) selected = pickDefaultDevice(g_outDevs, "WASAPI", false);
        if (selected >= 0) {
            audio.deviceLabel = "<selected device missing>";
            for (const auto& device : g_outDevs)
                if (device.id == selected) { audio.deviceLabel = device.name; break; }
        }
        {   // The whole VST graph lives in the engine while Csound runs: instruments AND effects in
            // ONE rack, on Csound's block clock. The rack reads each track's note ring
            // (cawvst_<id>_wr/tab - the score's CawVstNote_<id> writes the MIDI clip notes there)
            // and parameter channels for clip code, renders instruments first so a VSTi feeds a
            // VSTe in the same block, and mixes straight into the Csound output - the 4th engine's
            // rules, on this engine. It goes silent while this one runs (g_vstHostedInCsound).
            wireVstUiHooks();   // the panel edits reach THIS rack too (wiring, output mode, plugin change)
            vstui::captureAllParameters(g_proj);   // Run bakes exactly what the plugin GUIs show right now
            // Every VST track is loaded whatever its M / S state: mute / solo are applied live, so a
            // muted track must still be in the rack to come back without a Run.
            std::vector<VstJob> rackJobs = collectVstJobs(g_proj, g_engineMeters, true);
            if (!g_langVst) rackJobs.clear();   // VST unchecked for the session: no VST plugin is loaded either
            g_apiProc.setVstJobs(rackJobs);
            g_apiProc.setVstActiveTracks(collectVstActiveTracks(g_proj));   // initial live M / S state
            g_apiProc.setVstInputMasks(collectVstInputMasks(g_proj));       // initial live input on/off state
            // Source gates (cawvstmix_<clip>_<id>): 1 = that master-mode effect replaces the clip,
            // 0 = the clip reaches the master. The app keeps them current while the engine runs.
            g_apiProc.setControlChannels(vstGateValues());
            g_apiProc.setMasterGain(g_proj.cs.masterGain);
            g_apiProc.setPdSource(mixedRequested ? g_pdLibraryPath : std::string(),mixedPlan);
            // CAW hub (caws/cawr): the shared bus + the hub channel names found in the clip code.
            g_apiProc.setHub(&g_cawHub);
            g_apiProc.setHubNames(collectCawHubNames());
        }
        closeScSoloOutput();
        if (!g_apiProc.start(csdPath, g_rawSampleRate, top,
                             g_proj.cs.monitorFeed && !cawEnv("CAW_NO_MON"), apiError, &audio)) {
            restoreScSoloOutput();
            logLine("[caw] ERROR: " + apiError);
            g_cawMonitorApi = false; g_useCsoundApi = false;
            return;
        }
    } else {
        g_playProc.setHighPriority(true);
        if (!g_playProc.start(cmd, true)) {
            logLine("[caw] ERROR: " + g_playProc.lastError());
            closeMonitorSocket();
            return;
        }
    }
    g_playing = true;
    g_liveCsound = true;
    g_vstHostedInCsound = g_useCsoundApi;   // the engine's rack owns the VST graph now; the 4th engine must not render
    g_mixedPd = mixedRequested;
    if(g_mixedPd){g_pdTimelineEvents=std::move(mixedPlan);g_pdNeedsReload=false;g_pdPlanUpdateFailed=false;g_pdPlanT=0.0;}
    g_playWaiting = true;
    g_playWaitT = 0.f;
    g_sectionSeen = false;
    g_resync = 1;   // if the timeline is already playing, bring the new engine in sync with the playhead
}

bool replaceOutputFile(const std::string& temporary, const std::string& final, std::string& error) {
#ifdef _WIN32
    if (MoveFileExA(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    error = "Could not replace output file (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, final, ec);
    if (!ec) return true;
    error = "Could not replace output file: " + ec.message();
    return false;
#endif
}

void cleanupRenderTemps() {
    std::error_code ec;
    if (!g_renderTemp.empty()) std::filesystem::remove(g_renderTemp, ec);
    if (!g_renderEncodedTemp.empty()) std::filesystem::remove(g_renderEncodedTemp, ec);
    g_renderTemp.clear(); g_renderEncodedTemp.clear();
}

void startRenderNow() {
    if (!g_langCsound) { logLine("[caw] PD audio export is not available yet. Enable Csound to export Csound projects."); return; }
    if (g_pdEngineOn || g_pdPreview.active()) { logLine("[caw] Stop Pure Data playback before exporting."); return; }
    if (g_renderActive) return;
    vstui::captureParameters(g_proj);
    g_exportError.clear();
    const int format = std::min(4, std::max(0, g_proj.cs.renderFormat));
    const int channels = g_proj.cs.exportChannels;
    if ((format == 4 && channels > 2) || (format == 3 && channels > 8)) {
        g_exportError = format == 4 ? "MP3 supports mono or stereo only." : "FLAC supports up to 8 channels.";
        logLine("[caw] " + g_exportError);
        return;
    }
    if (format == 4 && !cawMp3BitrateSupported(g_proj.cs.sr, g_proj.cs.exportMp3Kbps)) {
        g_exportError = "MP3 needs a supported sample rate (8 to 48 kHz) and bitrate. Check project audio settings.";
        logLine("[caw] " + g_exportError);
        return;
    }
    for (const Track& track : g_proj.tracks)
        if (!track.muted && (track.kind == 5 || track.kind == 6) &&
            std::any_of(track.clips.begin(), track.clips.end(), [](const Clip& c) { return !c.muted; })) {
            g_exportError = "Audio export cannot include PD or SuperCollider clips yet.";
            logLine("[caw] " + g_exportError);
            return;
        }
    for (const Track& track : g_proj.tracks)
        if (!track.muted && (track.kind == 3 || track.kind == 4) && !track.clips.empty() && track.vstPath.empty()) {
            g_exportError = "VST track " + track.name + " has clips but no plugin selected.";
            logLine("[caw] " + g_exportError);
            return;
        }
    diagClearAll();
    if (g_playing) stopPlayback();
    const std::string csdPath = (workDir() / "caw_render.csd").string();
    g_renderPath = renderFilePath().string();
    g_activeRenderFormat = format;
    g_activeFlacBits = g_proj.cs.exportFlacBits;
    g_activeMp3Kbps = g_proj.cs.exportMp3Kbps;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir = std::filesystem::path(g_renderPath).parent_path();
    const std::string stem = ".caw_render_" + std::to_string(stamp);
    g_renderTemp = (dir / (stem + ".wav")).string();
    g_renderEncodedTemp = format >= 3 ? (dir / (stem + renderExtension(format))).string() : std::string();
    g_renderConverting = false;
    { std::error_code rec; std::filesystem::remove(scoreProbePath(), rec); }   // the score probe writes a fresh file
    if (!writeTextFile(csdPath, buildCsd(g_proj, "render", 0.0, &g_orcSpans))) {
        g_exportError = "Could not write the temporary render.";
        logLine("[caw] ERROR: could not write " + csdPath);
        cleanupRenderTemps();
        return;
    }
    if (!ensureCsoundMathProbe()) {
        g_exportError = "This Csound cannot compile abs or sqrt - the generated project cannot compile. Pick a full csound.exe in Settings > Programs (Browse opens CAW's own folder).";
        logLine("[caw] ERROR: " + g_exportError);
        cleanupRenderTemps();
        return;
    }
    std::string error;
    shareEngineDlls();
    if (!g_offlineProc.prepare(csoundExePath(), error)) {
        g_exportError = "Audio engine unavailable: " + error;
        logLine("[caw] " + g_exportError);
        cleanupRenderTemps();
        return;
    }
    vstui::captureAllParameters(g_proj);   // the export renders exactly what the plugin GUIs show
    g_offlineProc.setVstJobs(g_langVst ? collectVstJobs(g_proj, {}) : std::vector<VstJob>{});   // VST unchecked: not loaded in the render either
    g_offlineProc.setControlChannels(vstGateValues());   // the source gates for the render too
    const int wavFormat = format >= 3 ? 2 : format;
    if (!g_offlineProc.startOffline(csdPath, g_proj.cs.sr, masterChannels(g_proj.cs),
                                    (float)g_proj.cs.zdbfs, g_proj.cs.masterGain,
                                    wavFormat, g_renderTemp, error, channels)) {
        g_exportError = "Audio export failed: " + error;
        logLine("[caw] " + g_exportError);
        cleanupRenderTemps();
        return;
    }
    logLine("[caw] Rendering through CAW's C++ audio and VST host.");
    g_renderActive = true;
}

void startRender() {
    if (g_renderActive) return;
    std::error_code ec;
    g_renderWillOverwrite = std::filesystem::exists(renderFilePath(), ec);
    if (ec) { g_exportError = "Could not check output: " + ec.message(); logLine("[caw] " + g_exportError); return; }
    g_renderWarnVst = false;
    for (const Track& track : g_proj.tracks)
        if ((track.kind == 3 || track.kind == 4) && !track.muted && !track.vstPath.empty())
            g_renderWarnVst = true;
    if (g_renderWillOverwrite || g_renderWarnVst) g_confirmRenderOverwrite = true;
    else startRenderNow();
}

void drawExportOptions(CsoundSettings& s) {
    s.renderFormat = std::min(4, std::max(0, s.renderFormat));
    const int kind = s.renderFormat < 3 ? 0 : s.renderFormat == 3 ? 1 : 2;
    const char* formats[] = {"WAV", "FLAC", "MP3"};
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Format", formats[kind])) {
        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(formats[i], i == kind)) {
            s.renderFormat = i == 0 ? 1 : i == 1 ? 3 : 4;
            std::filesystem::path p = s.renderFile.empty() ? "render.wav" : s.renderFile;
            p.replace_extension(renderExtension(s.renderFormat));
            s.renderFile = p.string();
        }
        ImGui::EndCombo();
    }
    if (s.renderFormat < 3) {
        const char* bits[] = {"16-bit PCM", "24-bit PCM", "32-bit float"};
        ImGui::SetNextItemWidth(S(180));
        if (caw_ui::beginCombo("Bit depth", bits[s.renderFormat])) {
            for (int i = 0; i < 3; ++i) if (ImGui::Selectable(bits[i], i == s.renderFormat)) s.renderFormat = i;
            ImGui::EndCombo();
        }
    } else if (s.renderFormat == 3) {
        ImGui::SetNextItemWidth(S(180));
        if (caw_ui::beginCombo("Bit depth", s.exportFlacBits == 16 ? "16-bit lossless" : "24-bit lossless")) {
            if (ImGui::Selectable("16-bit lossless", s.exportFlacBits == 16)) s.exportFlacBits = 16;
            if (ImGui::Selectable("24-bit lossless", s.exportFlacBits == 24)) s.exportFlacBits = 24;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("FLAC is lossless: its bitrate varies with the audio. Bit depth sets its precision.");
    } else {
        static const int rates[] = {8,16,24,32,40,48,56,64,80,96,112,128,144,160,192,224,256,320};
        if (cawMp3RateSupported(s.sr) && !cawMp3BitrateSupported(s.sr, s.exportMp3Kbps)) {
            int best = 0, delta = 100000;
            for (int rate : rates) if (cawMp3BitrateSupported(s.sr, rate) && std::abs(rate - s.exportMp3Kbps) < delta) {
                best = rate; delta = std::abs(rate - s.exportMp3Kbps);
            }
            if (best) s.exportMp3Kbps = best;
        }
        const std::string current = std::to_string(s.exportMp3Kbps) + " kbps";
        ImGui::SetNextItemWidth(S(180));
        if (caw_ui::beginCombo("Bitrate", current.c_str())) {
            for (int rate : rates) if (cawMp3BitrateSupported(s.sr, rate)) {
                const std::string label = std::to_string(rate) + " kbps";
                if (ImGui::Selectable(label.c_str(), s.exportMp3Kbps == rate)) s.exportMp3Kbps = rate;
            }
            ImGui::EndCombo();
        }
    }
    const int channelChoice = s.exportChannels == 1 ? 0 : s.exportChannels == 2 ? 1 : s.exportChannels == 4 ? 2 : 3;
    const char* channels[] = {"Mono", "Stereo", "Quadro", "Other"};
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Channels", channels[channelChoice])) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(channels[i], i == channelChoice))
            s.exportChannels = i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 4 : 6;
        ImGui::EndCombo();
    }
    if (channelChoice == 3) {
        ImGui::SetNextItemWidth(S(100));
        if (ImGui::InputInt("Channel count", &s.exportChannels, 0, 0))
            s.exportChannels = std::min(64, std::max(1, s.exportChannels));
    }
    if (ImGui::IsItemHovered()) helpTip("Mono/stereo downmix the project; higher counts preserve channel order and add silence for missing channels.");
    if (s.renderFormat == 4 && s.exportChannels > 2) ImGui::TextDisabled("MP3 supports mono or stereo.");
    if (s.renderFormat == 3 && s.exportChannels > 8) ImGui::TextDisabled("FLAC supports up to 8 channels.");
}

void drawAudioExport() {
    if (!g_audioExportOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(560), S(410)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Audio export", &g_audioExportOpen)) {
        CsoundSettings& s = g_proj.cs;
        ImGui::TextUnformatted("Output file");
        ImGui::SetNextItemWidth(-S(105));
        inputStr("##export_file", s.renderFile, 0);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string ext = renderExtension(s.renderFormat) + 1;
            const std::string chosen = plat::chooseFile(true, "Choose audio export file", "Audio file", {ext}, renderFilePath().string());
            if (!chosen.empty()) {
                std::filesystem::path path(chosen);
                path.replace_extension(renderExtension(s.renderFormat));
                s.renderFile = path.string();
            }
        }
        ImGui::PushID("audio_export_options");
        drawExportOptions(s);
        ImGui::PopID();
        ImGui::SetNextItemWidth(S(120));
        if (ImGui::InputDouble("Tail (seconds)", &s.tail, .5, 2, "%.1f"))
            s.tail = std::min(60.0, std::max(0.0, s.tail));
        ImGui::TextDisabled("Destination: %s", renderFilePath().string().c_str());
        if (!g_exportError.empty()) ImGui::TextColored(ImVec4(.98f, .43f, .34f, 1.f), "%s", g_exportError.c_str());
        if (g_renderActive) {
            if (ImGui::Button("Cancel export")) cancelRender();
        } else if (ImGui::Button("Export audio")) startRender();
    }
    ImGui::End();
}

void cancelRender() {
    g_offlineProc.stop();
    g_codecProc.stop();
    g_renderProc.stop();
    for (const std::string& l : g_offlineProc.takeOutput()) logLine(l);
    for (const std::string& l : g_renderProc.takeOutput()) logLine(l);
    g_renderActive = false;
    cleanupRenderTemps();
    logLine("[caw] Render cancelled.");
}

bool startFallbackEncoder() {
    std::error_code ec;
    std::filesystem::remove(g_renderEncodedTemp, ec);
    std::string ffmpeg = plat::bundledFfmpeg();
    if (ffmpeg.empty() && !g_proj.cs.ffmpegExe.empty()) {
        std::error_code pathError;
        if (std::filesystem::is_regular_file(g_proj.cs.ffmpegExe, pathError))
            ffmpeg = g_proj.cs.ffmpegExe;
        else
            logLine("[caw] Configured FFmpeg was not found: " + g_proj.cs.ffmpegExe);
    }
    if (ffmpeg.empty()) ffmpeg = plat::findOnPath("ffmpeg");
    if (ffmpeg.empty()) return false;
    const std::string codec = g_activeRenderFormat == 3
        ? " -codec:a flac -sample_fmt " + std::string(g_activeFlacBits == 16 ? "s16" : "s32") +
          " -bits_per_raw_sample " + std::to_string(g_activeFlacBits) + " "
        : " -codec:a libmp3lame -b:a " + std::to_string(g_activeMp3Kbps) + "k ";
    const std::string cmd = processArg(ffmpeg) + " -nostdin -n -i " + processArg(g_renderTemp)
        + codec + processArg(g_renderEncodedTemp);
    return g_renderProc.start(cmd);
}

void tickAutosave(float dt) {
    if (!g_proj.cs.autosaveEnabled) { g_autoSaveElapsed = 0.f; return; }
    g_autoSaveElapsed += std::max(0.f, dt);
    const int minutes = std::min(120, std::max(1, g_proj.cs.autosaveMinutes));
    if (g_autoSaveElapsed < minutes * 60.f || historyBusy()) return;
    g_autoSaveElapsed = 0.f;
    vstui::captureParameters(g_proj);
    const std::string snapshot = serializeProject(g_proj);
    if (snapshot == g_savedSnap || snapshot == g_autoSavedSnap) return;
    std::filesystem::path target;
    if (!g_projectPath.empty()) {
        target = std::filesystem::path(g_projectPath);
        target += ".autosave.caw";
    } else {
        if (g_unsavedAutosavePath.empty()) {
            std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
            if (home.empty()) home = workDir().string();
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            g_unsavedAutosavePath = (std::filesystem::path(home) / "CAW Autosaves" /
                ("Untitled-" + std::to_string(stamp) + ".autosave.caw")).string();
        }
        target = g_unsavedAutosavePath;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) { logLine("[caw] Autosave folder failed: " + ec.message()); return; }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string tmp = target.string() + "." + std::to_string(stamp) + ".tmp";
    std::string error;
    if (!saveProjectFile(g_proj, tmp, error)) {
        logLine("[caw] Autosave failed: " + error);
        std::filesystem::remove(tmp, ec);
        return;
    }
    if (!replaceOutputFile(tmp, target.string(), error)) {
        logLine("[caw] Autosave failed: " + error);
        std::filesystem::remove(tmp, ec);
        return;
    }
    g_autoSavedSnap = snapshot;
    logLine("[caw] Recovery autosave: " + target.string());
}

// How long a sound needs from the moment the engine starts it until it comes out of the speakers (hardware buffer -B plus the sync offset from Settings).
// The timeline sends events this much EARLIER, so the sound is heard when the playhead is at the event, not one buffer later.
double engineOutputLatency() {
    const double sync = (double)g_proj.cs.syncMs / 1000.0;   // Settings > Audio > Sound sync: tuned by ear / eye until the sound meets the playhead
    return std::max(0.0, std::min(1.0, (double)std::max(0, g_proj.cs.bufHW) / (double)std::max(1000, g_proj.cs.sr) + sync));
}

// The timeline: the playhead moves; when Csound is running (Run) it gets the clips of the timeline from the playhead on, in ONE command,
// and starts each clip at its start time. It never starts Csound by itself: that is the job of the "Run" button.
double g_sigT = 0.0;               // time since the timeline was last compared with the project
unsigned long long g_sig = 0;      // what the running command was made from (timelineSignature)

void mixedPdTransport(bool playing) {
    if(g_mixedPd)
        g_apiProc.pdTransport(playing,g_play,g_proj.loopStart,g_proj.loopEnd,timeline_loop::valid(g_proj),g_proj.cs.sr,g_cmdSerial);
}

void tickTimeline(float dt) {
    if (g_drag == Drag::Playhead) { g_resync = 1; return; }  // wait until the playhead is released
    if (timeline_loop::valid(g_proj) && (g_play < g_proj.loopStart || g_play >= g_proj.loopEnd)) {
        g_play = timeline_loop::wrap(g_proj, g_play);
        g_resync = 1;
    }
    const bool scUp = g_scReady && g_scEngine.running();
    // The 4th engine (the VSTi rack) owns the playhead clock when nothing else does.
    vstSolo().setDrivesTransport(!engineIsUp() && !scUp);
    const bool vstUp = vstSolo().audible();
    const bool audioUp = engineIsUp() || scUp || vstUp;
    if (!audioUp) {
        g_play = timeline_loop::wrap(g_proj, g_play + dt);  // silent playhead move
        g_resync = 1;
        return;
    }
    if (g_resync) {              // started, or the playhead jumped: silence, then play from here
        g_resync = 0;
        vstEngineJoinTransport();   // the VST engine: fresh schedule from the MIDI clips, play from the playhead
        if (engineIsUp()) commandPlayFrom(g_play, true);
        if (scUp) scheduleScTimeline(g_play);
        mixedPdTransport(true);
        g_sig = timelineSignature(g_proj);
        g_scTimelineSig = scTimelineSignature(g_proj);
        g_sigT = 0.0; g_scPlanT = 0.0;
        g_hold = engineOutputLatency();
        return;
    }
    if (g_hold > 0.0) { g_hold -= dt; return; }   // the playhead meets the sound (one sound-card buffer)
    // The soundcard is the clock when the 4th engine drives the transport (audio-rate accurate);
    // otherwise the frame clock advances the playhead as before.
    if (vstSolo().drivesTransport()) g_play = vstSolo().timeSeconds();
    else {
        g_play += dt;
        if (g_vstEngineOn) vstSolo().followClock(g_play);   // stay in time with the Csound / SC transport
    }
    pollScAutomation();   // SuperCollider lanes follow the playhead (~cawget in the language)
    if (timeline_loop::valid(g_proj) && g_play >= g_proj.loopEnd) {
        g_play = timeline_loop::wrap(g_proj, g_play);
        // This host reissues transport commands at the boundary. It is not a
        // sample-accurate or gapless loop, but nothing beyond the end is queued.
        if (engineIsUp()) commandPlayFrom(g_play, true);
        if (scUp) scheduleScTimeline(g_play);
        mixedPdTransport(true);
        g_sig = timelineSignature(g_proj);
        g_scTimelineSig = scTimelineSignature(g_proj);
        g_sigT = 0.0; g_scPlanT = 0.0;
        return;
    }
    // Re-plan edits occasionally. SC code edits restart its groups from the current playhead.
    g_sigT += dt;
    if (g_sigT >= 0.25) {
        g_sigT = 0.0;
        const unsigned long long sig = timelineSignature(g_proj);
        if (sig != g_sig) {
            g_sig = sig;
            if (engineIsUp()) commandPlayFrom(g_play, false);
            const std::vector<VstSoloNote> notes = buildVstSoloNotes();   // MIDI clips / mute / solo edited while playing
            if (g_vstEngineOn) vstSolo().setNotes(notes);
        }
    }
    if (scUp) {
        g_scPlanT += dt;
        if (g_scPlanT >= 0.25) {
            g_scPlanT = 0.0;
            const unsigned long long sig = scTimelineSignature(g_proj);
            if (sig != g_scTimelineSig) scheduleScTimeline(g_play);
        }
    }
}

// First PD timeline milestone: one non-overlapping patch at a time. The UI
// clock triggers starts/stops, so timing and loops are not sample accurate.
// The full text goes to the console log; the transport bar has room for a short one.
void playNotice(const std::string& text, const std::string& shortText = std::string()) {
    logLine("[caw] " + text);
    g_playNotice = shortText.empty() ? text : shortText; g_playNoticeT = 10.0;
}
// One plan for Play and for live editing. Csound clips only count while the Csound engine runs.
// includeMuted builds the plan the PD engine loads: every clip, with the muted ones flagged, so
// M / S is applied live instead of rebuilding the engine (and its device stream) on every mute.
bool makePdPlan(std::vector<pd_timeline::Event>& events, std::string& error, bool includeMuted = false) {
    int bad = 0;
    const bool ok = pd_timeline::makePlan(g_proj, g_projectPath, events, error, g_playing || engineIsUp(), &bad, includeMuted);
    if (!ok && bad > 0)   // open the clip that needs attention
        for (Track& t : g_proj.tracks) for (Clip& c : t.clips) if (c.id == bad) c.editorOpen = true;
    return ok;
}

// Retiming an already-loaded clip must not stop the engine or the transport.
void refreshMixedPdPlan(float dt) {
    if(!g_mixedPd || !g_apiProc.pdReady())return;
    for(const auto& note:g_apiProc.takePdPlanNotes())logPdNote(note);
    const auto updateError=g_apiProc.takePdPlanError();
    if(!updateError.empty()) {
        g_pdPlanUpdateFailed=true;g_pdNeedsReload=true;
        playNotice("PD timing update failed: "+updateError,"PD timing update failed: see log");
    }
    if(g_pdPlanUpdateFailed)return;
    g_pdPlanT+=dt;
    if(g_pdPlanT<.25)return;
    g_pdPlanT=0;
    std::vector<pd_timeline::Event> next;std::string error;int bad=0;
    // Same plan shape as the engine holds (muted clips flagged), so M / S is a live replan.
    const bool valid=pd_timeline::makePlan(g_proj,g_projectPath,next,error,false,&bad,true);
    if(valid && !pd_timeline::samePlan(next,g_pdTimelineEvents) &&
       pd_timeline::sameInstances(next,g_pdTimelineEvents) && g_apiProc.replanPd(next)) {
        g_pdTimelineEvents=std::move(next);g_pdNeedsReload=false;return;
    }
    if(!valid || !pd_timeline::samePlan(next,g_pdTimelineEvents)) {
        if(!g_pdNeedsReload) {
            if(g_timelineRun){g_timelineRun=false;commandStopAll();mixedPdTransport(false);stopAllScClips();}
            playNotice(valid ? "PD clips changed. Restart the shared engines before playing." :
                "PD clip problem: "+error,"PD changed: restart engines");
        }
        g_pdNeedsReload=true;
    } else g_pdNeedsReload=false;
}

void pdTransport(bool play) {
    g_pdTimelineEngine.transport(play,g_play,g_proj.loopStart,g_proj.loopEnd,timeline_loop::valid(g_proj),g_proj.cs.sr);
}
// Rebuild the isolated runtime on edit. If playing, stop the transport first
// rather than silently continuing with stale or mismatched clips.
void refreshPdPlan(float dt) {
    if (!g_langPd || !g_pdEngineOn || !g_pdTimelineEngine.ready()) return;
    for(const auto& note:g_pdTimelineEngine.takeNotes())logPdNote(note);
    const auto updateError=g_pdTimelineEngine.takePlanError();
    if(!updateError.empty()) {
        g_pdPlanUpdateFailed=true;g_pdNeedsReload=true;
        playNotice("PD timing update failed: "+updateError,"PD timing update failed: see log");
    }
    if(g_pdPlanUpdateFailed)return;
    g_pdPlanT += dt;
    if (g_pdPlanT < 0.25) return;
    g_pdPlanT = 0.0;
    std::vector<pd_timeline::Event> next; std::string error;
    if (!makePdPlan(next,error,true)) {
        // A PD clip cannot be planned: PD goes quiet, nothing else does. The transport keeps
        // running on the VST / SC clock and the other engines are not touched.
        if (g_pdTimelineMode) { pdTransport(false); g_pdTimelineMode=false; g_pdPlayingClip=0; g_pdResumeAfterReload=false; g_resync=1; }
        if(error!=g_pdLastPlanError){g_pdLastPlanError=error;playNotice("PD clip problem: "+error,"PD clip problem: see the log");}
        g_pdNeedsReload=true;
        return;
    }
    g_pdLastPlanError.clear();
    if (!pd_timeline::samePlan(next,g_pdTimelineEvents)) {
        if(pd_timeline::sameInstances(next,g_pdTimelineEvents) && g_pdTimelineEngine.replan(next)) {
            // Timing, notes, x-fields or a mute changed: applied live. No restart, no device
            // close, no transport stop - muting PD must not touch the rest of the project.
            g_pdTimelineEvents=std::move(next);g_pdNeedsReload=false;return;
        }
        // A different set of PD clips: rebuild the PD engine only. The transport keeps going
        // (the VST engine takes the clock) and PD rejoins as soon as its new stream is ready.
        g_pdResumeAfterReload = g_timelineRun;
        if (g_pdTimelineMode) { pdTransport(false); g_pdTimelineMode=false; g_pdPlayingClip=0; g_resync=1; }
        g_pdTimelineEngine.stop();
        g_pdTimelineEngine.setHub(&g_cawHub);   // CAW hub: PD-only mode shares the same caws/cawr bus
        if(!g_pdTimelineEngine.start(g_pdLibraryPath,next,pdAudioSettings())){
            g_pdEngineOn=false;g_pdNeedsReload=true;g_pdResumeAfterReload=false;
            playNotice("PD reload failed: "+g_pdTimelineEngine.status());
            restoreScSoloOutput();
            return;
        }
        g_pdSnapshotSeq=0;
        g_pdTimelineEvents=std::move(next);
        if (g_pdResumeAfterReload)
            playNotice("PD clips reloaded - PD rejoins when it is ready; the other engines keep playing.",
                       "PD reloading: the other engines keep playing");
    }
    g_pdNeedsReload=false;
}

void tickPdTimeline(float dt = 0.f) {
    if (dt > 0.f) refreshPdPlan(dt);
    if (!g_pdTimelineMode || !g_timelineRun) return;
    static std::uint64_t lastUnderruns=0;
    const auto underruns=g_pdTimelineEngine.underflows();
    if(underruns<lastUnderruns)lastUnderruns=0;
    if(underruns>lastUnderruns) {
        lastUnderruns=underruns;
        playNotice("PD audio underrun. Increase the audio buffer in Settings > Audio if glitches continue.",
                   "PD audio underrun: try a larger buffer");
    }
    const auto pos=g_pdTimelineEngine.position();
    if(pos>=0)g_play=static_cast<double>(pos)/std::max(1000,g_proj.cs.sr);
    if (g_vstEngineOn) vstSolo().followClock(g_play);   // the VST engine stays in time with PD
    const auto* event = pd_timeline::at(g_pdTimelineEvents, g_play);
    g_pdPlayingClip=(event && !event->muted)?event->clipId:0;   // a muted clip is loaded, not playing
    // MIDI edits / live M / S of the instrument tracks while PD drives the clock: the VST
    // engine gets its new schedule here instead of on the Csound path.
    g_sigT += dt;
    if (g_sigT >= 0.25) {
        g_sigT = 0.0;
        const unsigned long long sig = timelineSignature(g_proj);
        if (sig != g_sig) {
            g_sig = sig;
            if (g_vstEngineOn) vstSolo().setNotes(buildVstSoloNotes());
        }
    }
    if (g_scReady) {
        const auto sig = scTimelineSignature(g_proj);
        if (g_scTimelineSig != sig || (g_scLastPDTimelineTime >= 0.0 && g_play + 0.05 < g_scLastPDTimelineTime))
            scheduleScTimeline(g_play);
        pollScAutomation();   // SC automation lanes follow the PD playhead too
    }
    g_scLastPDTimelineTime = g_play;
}

// Opcodes found in the code (opcode MyUdo, ...) or reported by "csound -z"; the editors refresh their colors when this grows.
std::set<std::string> g_extraOpcodes;
int g_langVersion = 1;

void addExtraOpcode(const std::string& name) {
    if (name.size() >= 2 && g_extraOpcodes.insert(name).second) ++g_langVersion;
}

// user-defined opcodes:  opcode Name, aa, kk
void scanUserOpcodes(const std::string& code) {
    static const std::regex udo(R"re((?:^|\n)[ \t]*opcode[ \t]+([A-Za-z_]\w*))re");
    for (std::sregex_iterator it(code.begin(), code.end(), udo), end; it != end; ++it) addExtraOpcode((*it)[1].str());
}

// "Run clip": run only this clip (no audio), so syntax / init errors show in the console with the line number of the clip.
void runClipNow(const Clip& clip);
bool pushClipCodeToEngine(const Clip& clip);   // live code: recompile the current text into the running engine
void runClip(const Clip& clip) {
    if (!g_langCsound) { logLine("[caw] Enable Csound in Settings > Languages to run Csound clips."); return; }
    Clip copy = clip;
    logLine("[caw] Run Csound clip " + clip.name);
    runClipNow(copy);
}

void runClipNow(const Clip& clip) {
    if (g_renderActive) { logLine("[caw] A render is running - wait for it or cancel it first."); return; }
    if (engineIsUp()) {   // "Run" is running: play the whole clip in that engine
        const ParsedTrack pc = parseClip(clip);
        pushClipCodeToEngine(clip);   // the text may have changed since this engine was compiled
        if (auto it = g_liveCodeEdits.find(clip.id); it != g_liveCodeEdits.end()) it->second.pending = false;
        if (numberOf(g_engClips, clip.name)) { commandStartClip(clip, clip.dur, pc.midiEnabled ? 60.0 : 0.0, pc.midiEnabled ? 100.0 : 0.0); g_runName = clip.name; g_runLeft = clip.dur; }
        if (g_runLeft > 0.0 && g_runName == clip.name) logLine("[caw] Run clip " + clip.name + ": playing the whole clip (" + std::to_string(clip.dur) + " s) in the running Csound.");
        else logLine("[caw] Could not send the clip to Csound.");
        return;
    }
    diagClearClip(clip.name);
    g_testProc.stop();
    g_test = TestRun();
    g_test.name = clip.name;
    const std::string csdPath = (workDir() / "caw_test.csd").string();
    OrcSpan span;
    const std::string csd = buildClipTestCsd(g_proj, clip, span);
    g_orcSpans.assign(1, span);
    if (!writeTextFile(csdPath, csd)) {
        logLine("[caw] ERROR: could not write " + csdPath);
        return;
    }
    logLine("[caw] Run clip " + clip.name + " (this clip alone, no audio)...");
    if (!g_testProc.start("\"" + csoundExePath() + "\" \"" + csdPath + "\"", false))
        logLine("[caw] ERROR: " + g_testProc.lastError());
}

// "Run" of a clip turns into "Stop" while the clip plays in the engine or its silent test runs.
bool clipRunning(const Clip& clip) { return (g_runLeft > 0.0 && g_runName == clip.name) || (g_testProc.active() && g_test.name == clip.name); }

void stopClipRun(const Clip& clip) {
    if (g_runLeft > 0.0 && g_runName == clip.name) {
        commandStopClip(clip);
        g_runLeft = 0.0;
    }
    if (g_testProc.active() && g_test.name == clip.name) g_testProc.stop();
    logLine("[caw] Run clip " + clip.name + ": stopped.");
}

// ---- LIVE CODE (VSTi / VSTe and Csound clips) -------------------------------------------------
// The clip editor used to only change the text: the running instruments kept the code Csound
// compiled at Run, so every edit needed a full restart of every engine. Now the edited clip's
// instrument is rebuilt from the CURRENT project and compiled into the running Csound between two
// audio blocks (csoundCompileOrc). Csound keeps already-playing instances on the old definition,
// so a clip that is playing is restarted at its current position - only that clip, nothing else.

// Compiles this clip's new instrument into the running engine. Returns false when there is no
// engine to receive it (no API engine, or the instrument could not be built).
bool pushClipCodeToEngine(const Clip& clip) {
    if (!engineIsUp()) return false;
    if (!g_useCsoundApi) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logLine("[caw] Live code needs CAW's Csound API engine (the external Csound process cannot recompile while it runs). Code edits are applied by the next Run.");
        }
        return false;
    }
    const std::string orc = buildClipInstrumentCode(g_proj, clip);
    if (orc.empty()) {
        logLine("[caw] live code \"" + clip.name + "\": this clip has no instrument in the running engine - press Run once.");
        return false;
    }
    g_apiProc.pushLiveInstrument(clip.name, orc);
    return true;
}

// A code change is heard at once: a playing clip is stopped and started again at the playhead, so
// the new definition runs from here. Other clips, lanes and engines keep going untouched.
void restartClipForLiveCode(const Clip& clip, const Track& track) {
    if (!engineIsUp()) return;
    const bool manual = g_runLeft > 0.0 && g_runName == clip.name;   // started with the clip's Run button
    double dur = clip.dur;
    if (!manual) {
        if (!g_playing || !g_timelineRun || clip.muted || track.muted) return;
        bool anySolo = false;
        for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
        if (anySolo && !track.solo) return;
        const double local = g_play - clip.start;
        if (local < 0.0 || local >= clip.dur) return;
        const ParsedTrack pc = parseClipForTrack(clip, track);
        const int runs = pc.launcher ? 1 : std::min(256, std::max(1, clip.runs));
        const double slot = clip.dur / (double)runs;
        dur = slot - std::fmod(local, slot);
        if (dur <= 0.05) return;
    }
    commandStopClip(clip);
    commandStartClip(clip, dur, 0, 0);
    if (manual) g_runLeft = dur;
}

void applyLiveClipCode(const Clip& clip, const Track& track) {
    if (pushClipCodeToEngine(clip)) restartClipForLiveCode(clip, track);
}

// Called by the clip editors on every text change; the recompile runs once the typing pauses.
void markLiveCodeEdit(const Clip& clip) {
    g_liveCodeEdits[clip.id] = LiveCodeEdit{ImGui::GetTime(), true};
}

void tickLiveCodeEdits() {
    const double now = ImGui::GetTime();
    for (auto& entry : g_liveCodeEdits) {
        if (!entry.second.pending || now - entry.second.at < 0.5) continue;
        entry.second.pending = false;
        bool vstOnly = false;
        for (const Track& t : g_proj.tracks) {
            if (t.kind == 5 || t.kind == 6) continue;   // PD / SC live in their own editors
            for (const Clip& c : t.clips) {
                if (c.id != entry.first) continue;
                if (engineIsUp()) applyLiveClipCode(c, t);
                else if (g_vstEngineOn && t.kind == 3) vstOnly = true;
                break;
            }
        }
        if (vstOnly) {   // no Csound: the VST engine interprets the edited clip code itself
            vstSolo().setScripts(buildVstScripts());
            logLine("[caw] live code: the VST engine is using the edited clip code.");
        }
    }
}

// "line 12" in a Csound message -> "instr Lead1, line 5 of the clip" (lines outside any clip stay as they are)
bool resolveOrcLine(const std::string& l, std::string& text, std::string& name, int& src) {
    static const std::regex lineRe(R"re(\bline\s+(\d+))re", std::regex_constants::icase);
    std::smatch m;
    if (g_orcSpans.empty() || l.find("score") != std::string::npos || !std::regex_search(l, m, lineRe)) return false;
    const int orcLine = std::atoi(m[1].str().c_str());
    for (const OrcSpan& sp : g_orcSpans) {
        const int idx = orcLine - sp.first;
        if (idx < 0 || idx >= (int)sp.lineMap.size()) continue;
        src = sp.lineMap[(size_t)idx];
        name = sp.name;
        const std::string where = "instr " + sp.name + ", " + (src > 0 ? "clip line " + std::to_string(src) : std::string("generated line (<caw> / cawRun)"));
        text = l.substr(0, (size_t)m.position(0)) + where + l.substr((size_t)(m.position(0) + m.length(0)));
        return true;
    }
    return false;
}

// One output line of Csound (play, render, run clip): the line number of the orchestra becomes clip + clip line, and the clip's start time is added.
// The line number often comes on its own line right after the error ("line 12:"), so a line that follows an error belongs to it.
void logCsoundLine(const std::string& raw) {
    // Live-code results: the line numbers of a runtime-compiled snippet are not the CSD's line
    // numbers, so they must not be mapped onto clips / editor markers.
    if (raw.rfind("[caw] live code", 0) == 0) { logLine(raw); return; }
    static const std::regex pdClipError(R"re(PD clip\s+(\d+):\s*(.*))re");
    std::smatch pdMatch;
    if (std::regex_search(raw, pdMatch, pdClipError)) {
        logPdClipMessage(std::atoi(pdMatch[1].str().c_str()), pdMatch[2].str());
        return;
    }
    std::string text, name;
    int src = 0;
    const bool mapped = resolveOrcLine(raw, text, name, src);
    LogLine l = classifyLog(mapped ? text : raw);
    const bool errLine = l.level == kLogError;
    if (mapped && g_errCtx.left > 0) l.level = kLogError;
    const std::string mark = errLine ? raw : g_errCtx.text;
    if (errLine) { g_errCtx.left = 6; g_errCtx.text = raw; }
    else if (g_errCtx.left > 0) --g_errCtx.left;
    if (l.level == kLogError) {
        if (mapped) fillLoc(l, name, src, mark);
        else if (!l.clip.empty()) fillLoc(l, l.clip, 0, raw);
    }
    pushLog(std::move(l));
}

std::string clipLineMessage(const std::string& l) {
    std::string text, name;
    int src = 0;
    return resolveOrcLine(l, text, name, src) ? text : l;
}

void pollTestAndScan(float dt) {
    if (g_runLeft > 0.0) g_runLeft -= dt;
    if (g_testProc.active()) {
        for (const std::string& l : g_testProc.takeOutput()) {
            std::string low = l;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos) g_test.sawError = true;
            logCsoundLine(l);
        }
        if (g_testProc.finished()) {
            const int code = g_testProc.exitCode();
            g_testProc.stop();
            logLine(g_test.sawError || code != 0 ? "[caw] Run clip " + g_test.name + ": errors found, see above."
                                                  : "[caw] Run clip " + g_test.name + ": no errors.");
        }
    }
    if (g_opcodeProc.active()) {
        g_opcodeScanT += dt;
        static const std::regex word(R"re(^\s*([A-Za-z_]\w*)\b)re");
        for (const std::string& l : g_opcodeProc.takeOutput()) {
            std::smatch m;
            if (std::regex_search(l, m, word)) addExtraOpcode(m[1].str());
        }
        if (g_opcodeProc.finished() || g_opcodeScanT > 10.f) g_opcodeProc.stop();
    } else if (g_langCsound && !g_opcodeScanStarted && (g_textEditorFocused || g_playing)) {
        g_opcodeScanStarted = true;
        g_opcodeProc.start("\"" + csoundExePath() + "\" -z", false);   // only when code/engine needs it
    }
}

void pollProcesses(float dt) {
    // Load the engines' shared runtime DLLs on the first frame, before any PD preview, audition or engine
    // start can bring its own copies in: Windows keeps one copy per DLL name and whoever loads first decides
    // for the whole process. This is what made Csound + PD fail after PD clips had been previewed earlier in
    // the same session - Csound's plugins then got libpd's copies, or found no dependencies at all.
    static bool sharedOnce = false;
    if (!sharedOnce) { sharedOnce = true; shareEngineDlls(); }
    pollScEngine(dt);
    drainPdControlOutputs();
    pollCawHub();   // deliver caws/cawr hub changes to the SuperCollider language
    // ---- Play (Csound runs the whole project in real time)
    const bool usingApi = g_useCsoundApi && g_apiProc.active();
    if (usingApi || g_playProc.active()) {
        if (g_playing && g_proj.cs.monitorFeed && !g_monSeen && !g_monWarned) {
            g_monRunT += dt;
            if (g_monRunT > 5.f) {
                g_monWarned = true;
                logLine("[caw] No monitor data from Csound after 5 s. Look for a Csound error above (CawMon); you can untick 'Send meter data' in Settings.");
            }
        }
        const auto playLines = usingApi ? g_apiProc.takeOutput() : g_playProc.takeOutput();
        for (const std::string& l : playLines) {
            if (handleMonitorLine(l)) { g_sectionSeen = true; continue; }
            logCsoundLine(l);
            // "Live" = Csound is really performing. API engine: its first audio block reached the soundcard ("SECTION 1" is printed
            // earlier, before CAW opened the soundcard, so it must not count there). Process engine: Csound's own "SECTION 1" line,
            // which it prints after it has opened the soundcard.
            if (usingApi) { if (l.find("first audio block delivered") != std::string::npos) g_sectionSeen = true; }
            else if (l.find("SECTION 1") != std::string::npos) g_sectionSeen = true;
            {   // the sound card could not be opened: Csound never starts playing
                std::string low = l;
                std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                static bool hinted = false;
                if (!g_playWaiting && !g_sectionSeen) hinted = false;
                if (!hinted && (low.find("unable to open") != std::string::npos || low.find("error opening") != std::string::npos || low.find("portaudio error") != std::string::npos ||
                                low.find("failed to initialise") != std::string::npos || low.find("failed to initialize") != std::string::npos || low.find("audio device") != std::string::npos)) {
                    hinted = true;
                    logLine("[caw] Csound could not open the sound card at " + std::to_string(g_proj.cs.sr) + " Hz. Set the sample rate in Settings to the rate of your output device (usually 48000 or 44100) and the right output device, then press Run again.");
                }
            }
        }
        if (usingApi) {
            MonitorCapture::Snapshot snapshot;
            if (g_apiProc.takeSnapshot(snapshot, g_apiSnapshotSeq)) {
                g_apiSnapshotSeq = snapshot.sequence;
                // The API host captures after CAW mixes SC/PD, so samples are
                // already normalized to the device range.
                g_rawFullScale = 1.f;
                const auto produced = std::chrono::steady_clock::time_point(
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(snapshot.timestampNs)));
                const auto due = produced + std::chrono::microseconds((long long)(engineOutputLatency() * 1e6));
                MonFrame raw;
                raw.tag = 'B';
                raw.values.reserve(monitor_fft::size * 2);
                raw.values.insert(raw.values.end(), snapshot.left.begin(), snapshot.left.end());
                raw.values.insert(raw.values.end(), snapshot.right.begin(), snapshot.right.end());
                g_monHeld.emplace_back(due, std::move(raw));
                MonFrame meters;
                meters.tag = 'M';
                meters.values = std::move(snapshot.meters);
                g_monHeld.emplace_back(due, std::move(meters));
            }
        }
        pollMonitor();
        writeSerial();
        if (g_playWaiting) {
            g_playWaitT += dt;
            // Live only when Csound really started performing (its "SECTION 1" line, the API start message or the first meter data).
            // Opening the sound card (ASIO / WDM-KS ...) can take several seconds, so there is no short timer any more.
            // Fallback after 15 s for a Csound with silenced messages (-m0): go live, but only while the process is still running.
            if (g_sectionSeen) g_playWaiting = false;
            else if (g_playWaitT > 15.f && (usingApi ? g_apiProc.running() : g_playProc.running())) {
                g_playWaiting = false;
                logLine("[caw] Csound did not report that it started (messages off?). Assuming it is running.");
            }
        }
        const bool finished = usingApi ? g_apiProc.finished() : g_playProc.finished();
        if (finished) {
            const int code = usingApi ? g_apiProc.exitCode() : g_playProc.exitCode();
            if (usingApi) g_apiProc.stop(); else g_playProc.stop();
            drainPlayOutput();
            closeMonitorSocket();
            g_cawMonitorApi = false;
            g_useCsoundApi = false;
            g_playing = false;
            g_liveCsound = false;
            g_mixedPd = false;
            g_playWaiting = false;
            restoreScSoloOutput();   // the VST engine (and SC) get the soundcard back
            logLine(code == 0 ? "[caw] Csound stopped." : "[caw] Csound stopped with exit code " + std::to_string(code) + " - see messages above.");
            if (code != 0 && (g_rtMissingAbs || g_rtMissingSqrt || g_rtMissingPow)) {
                // Self-heal: learn ONLY from Csound's own message, then adapt the generator so the
                // next Run compiles (abs -> sqrt(x*x), sqrt -> raw sums, pow -> sqrt).
                if (g_rtMissingAbs && g_mathHasAbs) {
                    g_mathHasAbs = false;
                    logLine("[caw] This Csound has no usable abs opcode - meters will be built with sqrt(x*x). Press Run again.");
                }
                if (g_rtMissingSqrt && g_mathHasSqrt) {
                    g_mathHasSqrt = false;
                    logLine("[caw] This Csound has no usable sqrt opcode - spectrum bands will use raw sums. Press Run again.");
                }
                if (g_rtMissingPow && g_mathHasPow) {
                    g_mathHasPow = false;
                    logLine("[caw] This Csound has no usable pow opcode - spectrum bands will use sqrt. Press Run again.");
                }
                setCsoundMathOps(g_mathHasAbs, g_mathHasSqrt, g_mathHasPow);
            }
            if (usingApi && code != 0)
                logLine("[caw] If the API engine is incompatible with your Csound installation, use run_osc_monitor.bat to try the previous engine.");
        }
    }

    if (g_pdEngineOn && !g_pdTimelineEngine.active()) {
        for(const auto& note:g_pdTimelineEngine.takeNotes())logPdNote(note);
        const std::string status = g_pdTimelineEngine.status();
        if (status != "PD stopped.") logPdFailureStatus(status);
        playNotice("PD engine stopped: " + status, "PD stopped: see the log");
        if (g_scReady) stopAllScClips();
        g_pdEngineOn = false; g_pdTimelineMode = false; g_pdPlayingClip = 0;
        g_pdResumeAfterReload = false;
        if (g_timelineRun) g_resync = 1;   // the VST / SC clock takes over from this position
        g_pdTimelineEvents.clear();
        restoreScSoloOutput();
    }
    vstEngineTick(dt);   // VST engine: log, own stream, plugin changes
    // The PD engine was rebuilt while the transport kept running (a different set of PD
    // clips). Rejoin as soon as its new stream is ready; PD then drives the clock again
    // and the VST engine re-anchors on it.
    if (g_timelineRun && g_pdResumeAfterReload && !g_pdTimelineMode && g_pdEngineOn &&
        g_pdTimelineEngine.ready() && !g_renderActive) {
        std::vector<pd_timeline::Event> plan; std::string err;
        if (!makePdPlan(plan, err, true) || !pd_timeline::samePlan(plan, g_pdTimelineEvents)) {
            g_pdResumeAfterReload = false;
        } else {
            g_pdResumeAfterReload = false;
            g_pdTimelineMode = true;
            g_pdPreviousTime = -1.0;
            vstEngineJoinTransport();
            pdTransport(true);
            logLine("[caw] PD rejoined the running transport.");
        }
    }
    // ---- timeline (independent of the Csound engine)
    if (g_timelineRun) {
        if (g_pdTimelineMode) {
            if(g_drag!=Drag::Playhead)tickPdTimeline(dt);
        } else tickTimeline(dt);
    }
    else refreshPdPlan(dt);
    refreshMixedPdPlan(dt);

    // ---- CAW offline renderer, followed by a codec only for FLAC/MP3
    auto finishExport = [&](const std::string& file) {
        std::string error;
        std::error_code ec;
        const bool appeared = !g_renderWillOverwrite && std::filesystem::exists(g_renderPath, ec);
        if (ec) error = "Could not check the destination: " + ec.message();
        else if (appeared) error = "The destination appeared during rendering; it was not replaced.";
        else if (replaceOutputFile(file, g_renderPath, error)) logLine("[caw] Audio export finished: " + g_renderPath);
        if (!error.empty()) { g_exportError = error; logLine("[caw] Audio export failed: " + error); }
        g_renderActive = false;
        cleanupRenderTemps();
    };
    if (g_offlineProc.active()) {
        for (const std::string& l : g_offlineProc.takeOutput()) logCsoundLine(l);
        if (g_offlineProc.finished()) {
            const int code = g_offlineProc.exitCode();
            g_offlineProc.stop();
            for (const std::string& l : g_offlineProc.takeOutput()) logLine(l);
            if (code != 0) {
                g_exportError = "Audio render failed. Check the console for details.";
                logLine("[caw] Offline audio export failed (code " + std::to_string(code) + ").");
                g_renderActive = false; cleanupRenderTemps();
            } else if (g_activeRenderFormat >= 3) {
                g_renderConverting = true;
                logLine(g_activeRenderFormat == 3 ? "[caw] Encoding FLAC..." : "[caw] Encoding MP3...");
                if (!g_codecProc.start(g_renderTemp, g_renderEncodedTemp, csoundExePath(),
                                       g_activeRenderFormat, g_activeFlacBits, g_activeMp3Kbps) &&
                    !startFallbackEncoder()) {
                    g_exportError = "Audio encoder could not start.";
                    logLine("[caw] " + g_exportError);
                    g_renderActive = false; cleanupRenderTemps();
                }
            } else finishExport(g_renderTemp);
        }
    }
    if (g_codecProc.active() && g_codecProc.finished()) {
        const bool success = g_codecProc.success();
        const std::string error = g_codecProc.error();
        g_codecProc.stop();
        if (success) finishExport(g_renderEncodedTemp);
        else {
            logLine("[caw] Native encoder: " + error);
            if (!startFallbackEncoder()) {
                g_exportError = "MP3/FLAC encoder unavailable. On Windows, keep the bundled ffmpeg.exe beside CAW; otherwise install FFmpeg on PATH. Native encoder: " + error;
                logLine("[caw] " + g_exportError);
                g_renderActive = false; cleanupRenderTemps();
            }
        }
    }
    if (g_renderProc.active()) {
        for (const std::string& l : g_renderProc.takeOutput()) logLine(l);
        if (g_renderProc.finished()) {
            const int code = g_renderProc.exitCode();
            g_renderProc.stop();
            for (const std::string& l : g_renderProc.takeOutput()) logLine(l);
            if (code != 0) {
                g_exportError = "Audio encoding failed. Check the console for details.";
                logLine("[caw] Audio encoding failed (code " + std::to_string(code) + ").");
                g_renderActive = false; cleanupRenderTemps();
            } else finishExport(g_renderEncodedTemp);
        }
    }
}

// ---- MIDI clip tools (the Csound clips are never touched)
// Split a MIDI clip in two at timeline time t. Notes crossing the cut are split too.
void cutMidiClipAt(int laneIndex, int midiIndex, double t) {
    if (laneIndex < 0 || laneIndex >= (int)g_proj.tracks.size()) return;
    Track& lane = g_proj.tracks[(size_t)laneIndex];
    if (midiIndex < 0 || midiIndex >= (int)lane.midiClips.size()) return;
    MidiClip left = lane.midiClips[(size_t)midiIndex];
    if (t <= left.start + 0.01 || t >= left.start + left.dur - 0.01) return;
    const double cut = t - left.start;
    MidiClip right = left;
    right.id = newMidiId();
    right.start = t;
    right.dur = left.dur - cut;
    right.editorOpen = false;
    right.notes.clear();
    std::vector<MidiNote> keep;
    for (MidiNote n : left.notes) {
        if (n.start >= cut) {
            n.start -= cut;
            right.notes.push_back(n);
        } else if (n.start + n.dur > cut) {
            MidiNote r = n;
            r.start = 0.0;
            r.dur = n.start + n.dur - cut;
            right.notes.push_back(r);
            n.dur = cut - n.start;
            keep.push_back(n);
        } else {
            keep.push_back(n);
        }
    }
    lane.midiClips[(size_t)midiIndex].notes = keep;
    lane.midiClips[(size_t)midiIndex].dur = cut;
    lane.midiClips.push_back(right);
    g_selMidiClips.clear();
    g_selMidiClips.insert(right.id);
}

// Merge the selected MIDI clips of a lane into one (a single selected clip joins with the next one of the same instrument).
void joinSelectedClips() {
    bool any = false;
    for (int li = 0; li < (int)g_proj.tracks.size(); ++li) {
        Track& lane = g_proj.tracks[(size_t)li];
        std::vector<int> ids;
        for (const MidiClip& c : lane.midiClips) if (g_selMidiClips.count(c.id)) ids.push_back(c.id);
        if (ids.empty()) continue;
        auto byStart = [&](int a, int b) { return findMidiClip(a)->start < findMidiClip(b)->start; };
        std::sort(ids.begin(), ids.end(), byStart);
        if (ids.size() == 1) {
            const MidiClip* first = findMidiClip(ids[0]);
            const MidiClip* next = nullptr;
            for (const MidiClip& c : lane.midiClips)
                if (c.id != first->id && c.clipId == first->clipId && c.start >= first->start &&
                    (!next || c.start < next->start)) next = &c;
            if (!next) continue;
            ids.push_back(next->id);
        }
        const int targetClip = findMidiClip(ids[0])->clipId;
        MidiClip merged = *findMidiClip(ids[0]);
        double end = merged.start + merged.dur;
        std::vector<int> used{ids[0]};
        for (size_t k = 1; k < ids.size(); ++k) {
            const MidiClip& other = *findMidiClip(ids[k]);
            if (other.clipId != targetClip) { logLine("[caw] Join: skipped " + std::string("a MIDI clip that belongs to another instrument.")); continue; }
            if (other.muted != merged.muted) { logLine("[caw] Join: skipped a MIDI clip with a different mute state."); continue; }
            end = std::max(end, other.start + other.dur);
            used.push_back(ids[k]);
        }
        if (used.size() < 2) continue;
        for (size_t k = 1; k < used.size(); ++k) {
            const MidiClip& other = *findMidiClip(used[k]);
            for (MidiNote n : other.notes) {
                n.start += other.start - merged.start;
                merged.notes.push_back(n);
            }
        }
        merged.dur = end - merged.start;
        std::stable_sort(merged.notes.begin(), merged.notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
        lane.midiClips.erase(std::remove_if(lane.midiClips.begin(), lane.midiClips.end(), [&](const MidiClip& c) {
            return std::find(used.begin(), used.end(), c.id) != used.end(); }), lane.midiClips.end());
        lane.midiClips.push_back(merged);
        g_selMidiClips.clear();
        g_selMidiClips.insert(merged.id);
        any = true;
    }
    if (!any) logLine("[caw] Join: click a MIDI clip in the MIDI lane first (Ctrl+click adds more). Csound clips are never joined.");
}

// Join tool: with MIDI regions selected it joins them right away; with nothing selected it arms the tool,
// and the next click on a MIDI region joins that region with the next one.
void runJoinTool() {
    if (!g_selMidiClips.empty()) {
        joinSelectedClips();
        if (g_tool == Tool::Join) g_tool = Tool::Select;
    } else {
        g_tool = Tool::Join;
    }
}

// Play button: only plays the timeline clips (moves the playhead and sends the clips to the RUNNING Csound).
// It never starts Csound. Use "Run" for that. Without a running engine the playhead just moves, silently.
bool timelinePlaying() { return g_timelineRun; }

void toggleTimelinePlay() {
    if (g_timelineRun) {
        g_timelineRun = false;
        g_pdResumeAfterReload = false;
        if (g_pdTimelineMode) {
            pdTransport(false);g_pdPlayingClip = 0; g_pdTimelineMode = false;
        }
        if (engineIsUp()) commandStopAll();
        if (g_scReady) stopAllScClips();
        if (g_vstEngineOn) vstSolo().halt();   // the VST engine stops scheduling and releases its notes
        g_scTimelineSig = 0; g_scPlanT = 0.0; g_scLastPDTimelineTime = -1.0;
        mixedPdTransport(false);
    } else {
        // VST tracks play in every situation: while Csound runs, its rack is the VST graph (nothing
        // else to start); without it, Play brings the VST engine up. Csound / PD / SC are never
        // started by Play.
        if (!g_vstEngineOn && !g_vstHostedInCsound && anyVstTrack()) {
            if (g_langVst) startVstEngine();
            else playNotice("Enable VST in Settings > Languages - the VSTi tracks are skipped, the other engines play.", "Enable VST first");
        }
        bool hasAudibleScClip = false;
        bool anyTrackSolo = false;
        for (const Track& track : g_proj.tracks) if (track.solo && !track.muted) anyTrackSolo = true;
        for (const Track& track : g_proj.tracks) {
            if (track.kind != 5 || track.muted || (anyTrackSolo && !track.solo)) continue;
            for (const Clip& clip : track.clips) if (!clip.muted && !clip.code.empty()) hasAudibleScClip = true;
        }
        if (hasAudibleScClip && !g_langSc) {
            // The SC clips are skipped, not the whole play: the engines that are up still play.
            playNotice("Enable SuperCollider in Settings > Languages - the SC clips are skipped, the other engines play.", "Enable SuperCollider first");
        } else if (hasAudibleScClip && !g_scReady) {
            const std::string runSc = g_langCsound
                ? (g_langPd ? "Run Csound + PD + SC" : "Run Csound + SC")
                : (g_langPd ? "Run PD + SC" : "Run SuperCollider");
            const std::string message = g_scEngineOn
                ? std::string("SuperCollider is still preparing - the SC clips are skipped for now.")
                : "The SC clips are skipped - press " + runSc + " to hear them.";
            playNotice(message, g_scEngineOn ? "SuperCollider is preparing" : runSc);
            if (!engineIsUp() && !g_vstEngineOn && !g_pdEngineOn) return;   // nothing else could play: keep the old stop
        }
        std::vector<pd_timeline::Event> events; std::string error;
        // Load every PD clip, muted ones flagged: a mute is then live and never rebuilds the
        // engine. Only clips that are audible actually start playing.
        if (g_pdEngineOn && !makePdPlan(events, error, true)) {
            events.clear();
            playNotice(error, error.find("no patch") != std::string::npos ? "PD clip has no patch: choose a .pd file" : "PD clip problem: see the log");
            // The PD clips are skipped; the VST / Csound / SC engines still play.
        }
        bool audiblePdEvents = false;
        for (const auto& e : events) if (!e.muted) { audiblePdEvents = true; break; }
        if(g_mixedPd) {
            int bad=0;
            if(!g_apiProc.pdReady() || !engineIsUp()) {playNotice("Shared engines are preparing. Wait for Csound and PD ready.");return;}
            if(!pd_timeline::makePlan(g_proj,g_projectPath,events,error,false,&bad,true)) {playNotice("PD clip problem: "+error);return;}
            if(g_pdNeedsReload || !pd_timeline::samePlan(events,g_pdTimelineEvents)) {
                playNotice("PD clips changed. Restart the shared engines before playing.");return;
            }
            audiblePdEvents = !events.empty();
        }
        if (!audiblePdEvents) {
            // A PD track without any usable clip would otherwise just move the playhead without a word.
            bool pdTrack = false, pdClip = false;
            for (const Track& t : g_proj.tracks) if (t.kind == 6 && !t.muted) { pdTrack = true; for (const Clip& c : t.clips) if (!c.muted) pdClip = true; }
            if (pdTrack && !g_pdEngineOn && !g_mixedPd) playNotice("Press Run PD to enable Pure Data clips.");
            else if (pdTrack && !pdClip) playNotice("The PD track has no clips. Double-click its lane to add one, then choose a .pd file.", "PD track has no clip: double-click its lane");
            else if (pdTrack) playNotice("All PD clips or PD tracks are muted (or another track is solo).");
        }
        if (audiblePdEvents && !g_mixedPd) {
            // A PD problem skips the PD timeline only: VSTi / Csound / SC still play. Play again
            // once PD is ready (Run PD preloads its clips; the VST engine keeps working meanwhile).
            if (g_pdLibraryPath.empty()) g_pdLibraryPath = plat::bundledLibpd();
            if (!g_langPd) {
                playNotice("Enable Pure Data in Settings > Languages - the PD clips are skipped, the other engines play.");
            } else if (g_playing || g_pdPreview.active() || g_testSound.running() || g_renderActive) {
                playNotice(g_pdPreview.active() ? "A PD clip is already running from its window (Stop clip first)." : "Stop other audio playback before playing PD clips.", "PD skipped: another playback runs"); return;
            } else if (!g_pdTimelineEngine.ready()) {
                playNotice("PD is still preparing - the other engines play now. Press Play again when PD is ready.", "PD skipped: PD is preparing");
            } else if (g_pdNeedsReload || !pd_timeline::samePlan(events,g_pdTimelineEvents)) {
                // An edit made just before Play: a live replan needs no reload at all.
                if (!g_pdNeedsReload && pd_timeline::sameInstances(events,g_pdTimelineEvents) && g_pdTimelineEngine.replan(events)) {
                    g_pdTimelineEvents=events;
                    g_pdTimelineMode = true; g_pdPreviousTime = -1.0;
                } else {
                    g_pdNeedsReload=true;playNotice("PD clips changed - they are skipped. Press Run PD to preload them again.", "PD skipped: Run PD again");
                }
            } else if (g_pdLibraryPath.empty()) {
                playNotice("libpd was not found - Settings > Programs, or copy libpd.dll into <CAW folder>\\PureData\\.", "PD skipped: libpd was not found");
            } else {
                g_pdTimelineMode = true; g_pdPreviousTime = -1.0;
                logLine("[caw] PD timeline uses preloaded clip instances and the audio frame clock.");
            }
        }        if (timeline_loop::valid(g_proj) && (g_play < g_proj.loopStart || g_play >= g_proj.loopEnd)) g_play = g_proj.loopStart;
        g_pdScoreData.clear();
        g_timelineRun = true; g_resync = 1;
        if (g_pdTimelineMode) {
            vstEngineJoinTransport();   // PD runs its own clock: the VST engine joins it from here
            pdTransport(true);
            if (g_scReady) scheduleScTimeline(g_play);
        }
        else if (!engineIsUp() && !g_scReady && !g_vstEngineOn)
            logLine(g_langCsound ? "[caw] Press Run Csound for Csound clips."
                    : (!g_langPd && !g_langSc && !g_langVst) ? "[caw] Score only: no engine is enabled (Settings > Languages). The timeline and editors work."
                    : "[caw] No SuperCollider or Pure Data audio engine is running.");
    }
}

void seekTimeline(double t) {
    g_play = std::max(0.0, t);
    if (g_timelineRun) {
        g_resync = 2;
        if (g_pdTimelineMode) { g_pdPlayingClip = 0; vstEngineJoinTransport(); pdTransport(true); if (g_scReady) scheduleScTimeline(g_play); }
    }
}

// ---- show / hide all automation and MIDI lanes (Alt+H / Alt+J / Alt+M / Alt+N)
std::map<int, std::set<std::string>> g_savedHiddenAuto;   // what was hidden before "hide all"
bool g_autoAllHidden = false;
std::map<int, bool> g_savedMidiHidden;
bool g_midiAllHidden = false;

void toggleHideAllAutomation() {
    if (!g_autoAllHidden) {
        g_savedHiddenAuto.clear();
        for (Track& t : g_proj.tracks) {
            g_savedHiddenAuto[t.id] = t.hiddenAutomation;
            for (const auto& lane : t.automation) t.hiddenAutomation.insert(lane.first);
        }
        clearAutoSelection();
        g_autoAllHidden = true;
    } else {
        for (Track& t : g_proj.tracks) {
            const auto it = g_savedHiddenAuto.find(t.id);
            t.hiddenAutomation = it != g_savedHiddenAuto.end() ? it->second : std::set<std::string>();
        }
        g_autoAllHidden = false;
    }
}

void showAllAutomation() {
    for (Track& t : g_proj.tracks) {
        t.hiddenAutomation.clear();
        for (const CawParam& p : pdParsedForUi(t).params)
            if (!t.automation.count(p.chn)) {
                t.automation.emplace(p.chn, std::vector<AutoPoint>{});
                t.automationHeights[p.chn] = kAutomationLaneH;
            }
    }
    g_autoAllHidden = false;
}

void toggleHideAllMidi() {
    if (!g_midiAllHidden) {
        g_savedMidiHidden.clear();
        for (Track& t : g_proj.tracks) { g_savedMidiHidden[t.id] = t.midiHidden; t.midiHidden = true; }
        g_midiAllHidden = true;
    } else {
        for (Track& t : g_proj.tracks) {
            const auto it = g_savedMidiHidden.find(t.id);
            t.midiHidden = it != g_savedMidiHidden.end() ? it->second : false;
        }
        g_midiAllHidden = false;
    }
}

void showAllMidi() {
    for (Track& t : g_proj.tracks) t.midiHidden = false;
    g_midiAllHidden = false;
}

// Global mute/solo (Alt+M / Alt+S, and the M / S buttons in the transport bar):
// they only touch the TRACK mute/solo flags - clip mutes and automation lane
// mutes (scoreAutoMuted) stay as they are.
int unmuteAllTracks() {
    int count = 0;
    for (Track& t : g_proj.tracks) if (t.muted) { t.muted = false; ++count; }
    pushVstActiveLive();
    return count;
}
int unsoloAllTracks() {
    int count = 0;
    for (Track& t : g_proj.tracks) if (t.solo) { t.solo = false; ++count; }
    pushVstActiveLive();
    return count;
}

// ---------------------------------------------------------------- transport
// One row: transport, editing tools and Score, with engine status at right.
void drawTransport() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.f));
    ImGui::BeginChild("transport", ImVec2(0, S(52)), false, ImGuiWindowFlags_NoScrollbar);
    const float totalW = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPos(ImVec2(S(12), S(10)));

    // ---- status pill, drawn first at the right edge so nothing can push it out of the window
    const CsoundSettings& cs = g_proj.cs;
    char statusText[96];
    ImVec4 statusCol(0.55f, 0.55f, 0.60f, 1.f);
    const bool csStarting = g_playing && g_playWaiting;
    const bool pdStarting = (g_pdEngineOn && !g_pdTimelineEngine.ready()) ||
                            (g_mixedPd && g_playing && !g_apiProc.pdReady());
    const bool scStarting = g_scEngineOn && !g_scReady;
    const bool vstStarting = g_vstEngineOn && !vstSolo().ready();
    const bool anyStarting = csStarting || pdStarting || scStarting || vstStarting;
    const bool csReady = g_liveCsound && !g_playWaiting;
    const bool pdReady = (g_pdEngineOn && g_pdTimelineEngine.ready()) ||
                         (g_pdPreview.active() && g_pdPreview.ready()) ||
                         (g_mixedPd && g_apiProc.pdReady());
    const bool scReady = g_scEngineOn && g_scReady;
    const bool vstReady = g_vstEngineOn && vstSolo().ready();
    const char csStatus = csStarting ? '~' : (csReady ? '1' : '0');
    const char scStatus = scStarting ? '~' : (scReady ? '1' : '0');
    const char pdStatus = pdStarting ? '~' : (pdReady ? '1' : '0');
    const char vstStatus = vstStarting ? '~' : (vstReady ? '1' : '0');
    if (g_renderActive) {
        std::snprintf(statusText, sizeof statusText, "Rendering...");
        statusCol = ImVec4(0.95f, 0.60f, 0.20f, 1.f);
    } else {
        // The pill lists exactly what this session is set to use (Settings > Languages).
        std::string parts;
        if (g_langCsound || g_liveCsound) parts += std::string("CS:") + csStatus;
        if (g_langPd || g_pdEngineOn) { if (!parts.empty()) parts += "  "; parts += std::string("PD:") + pdStatus; }
        if (g_langSc || g_scEngineOn) { if (!parts.empty()) parts += "  "; parts += std::string("SC:") + scStatus; }
        if (g_langVst || g_vstEngineOn) { if (!parts.empty()) parts += "  "; parts += std::string("VST:") + vstStatus; }
        if (parts.empty()) parts = "score only";
        std::snprintf(statusText, sizeof statusText, "%s", parts.c_str());
        if (anyStarting) statusCol = ImVec4(0.95f, 0.72f, 0.30f, 1.f);
        else if (csReady || scReady || pdReady || vstReady) statusCol = ImVec4(0.40f, 0.85f, 0.50f, 1.f);
    }
    if (g_playNoticeT > 0.0) {   // a reason why Play did nothing, shown where it cannot be missed
        g_playNoticeT -= ImGui::GetIO().DeltaTime;
        std::snprintf(statusText, sizeof statusText, "%.64s", g_playNotice.c_str());
        statusCol = ImVec4(0.95f, 0.60f, 0.20f, 1.f);
    }
    char engineText[96];
    if (g_langCsound) std::snprintf(engineText, sizeof engineText, "%d Hz | ksmps %d | %d ch", cs.sr, cs.ksmps, cs.nchnls);
    else {
        std::string eng;
        if (g_langPd) eng = "Pure Data";
        if (g_langSc) { if (!eng.empty()) eng += " + "; eng += "SuperCollider"; }
        if (g_langVst) { if (!eng.empty()) eng += " + "; eng += "VST"; }
        if (eng.empty()) eng = "score only";
        std::snprintf(engineText, sizeof engineText, "%d Hz | %s", cs.sr, eng.c_str());
    }
    const float dotW = S(14);
    const float statusW = dotW + ImGui::CalcTextSize(statusText).x + S(12);
    const float engineW = ImGui::CalcTextSize(engineText).x + S(16);
    const bool showEngine = totalW > S(1550);
    const float rightW = statusW + (showEngine ? engineW : 0.f) + S(12);
    if (totalW > S(1050)) {
        const ImVec2 base = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = base.x + totalW - rightW + S(4);
        const float yMid = base.y + S(26);
        if (showEngine) {
            const ImVec2 sz = ImGui::CalcTextSize(engineText);
            dl->AddText(ImVec2(x, yMid - sz.y * 0.5f), IM_COL32(120, 120, 130, 255), engineText);
            x += engineW;
        }
        const ImVec2 sz = ImGui::CalcTextSize(statusText);
        dl->AddRectFilled(ImVec2(x - S(6), yMid - S(13)), ImVec2(x + statusW, yMid + S(13)), IM_COL32(28, 29, 34, 255), S(13));
        dl->AddCircleFilled(ImVec2(x + S(5), yMid), S(4), ImGui::ColorConvertFloat4ToU32(statusCol));
        dl->AddText(ImVec2(x + dotW, yMid - sz.y * 0.5f), ImGui::ColorConvertFloat4ToU32(statusCol), statusText);
    }

    // ---- always visible: go to start, Play, Run, time
    if (ImGui::Button("|<")) seekTimeline(0.0);
    ImGui::SameLine();
    const bool tlPlaying = timelinePlaying();
    if (tlPlaying) pushBtnOn(ImVec4(0.85f, 0.35f, 0.10f, 1.f));
    if (ImGui::Button(tlPlaying ? " Stop " : " Play ")) toggleTimelinePlay();
    if (tlPlaying) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        helpTip("Play the timeline. Start the engines for the enabled languages with the combined Run button, or with the single-language Run button.");
    ImGui::SameLine();
    const bool hasLoop = g_proj.loopEnd - g_proj.loopStart >= 0.01;
    const bool loopOn = g_proj.loopEnabled && hasLoop;
    if (!hasLoop) ImGui::BeginDisabled();
    if (loopOn) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
    if (ImGui::Button("Loop")) {
        g_proj.loopEnabled = !g_proj.loopEnabled;
        if (g_timelineRun) g_resync = 2;
        if (g_timelineRun && g_pdTimelineMode) pdTransport(true);
    }
    if (loopOn) ImGui::PopStyleColor(3);
    if (!hasLoop) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        helpTip("Right-click the timeline ruler twice to set loop start and end. Toggle Loop here.");
    ImGui::SameLine();
    if (ImGui::SmallButton("M##global_unmute")) {
        const int count = unmuteAllTracks();
        logLine("[caw] Unmuted all tracks" + std::string(count ? " (" + std::to_string(count) + (count == 1 ? " track)" : " tracks)") : " (none were muted)") + ".");
    }
    if (ImGui::IsItemHovered())
        helpTip("Global mute: unmute EVERY muted track (Alt+M). Only the track mutes are cleared - clips and automation lanes keep their own mute. Solo stays.");
    ImGui::SameLine();
    if (ImGui::SmallButton("S##global_unsolo")) {
        const int count = unsoloAllTracks();
        logLine("[caw] Cleared solo on all tracks" + std::string(count ? " (" + std::to_string(count) + (count == 1 ? " track)" : " tracks)") : " (none were solo)") + ".");
    }
    if (ImGui::IsItemHovered())
        helpTip("Global solo off: clear solo on EVERY track (Alt+S). Mutes stay as they are.");
    ImGui::SameLine();
    if (g_langCsound || g_liveCsound) {
        const bool csOn = g_liveCsound;
        const bool scOn = g_scEngineOn;
        const bool vstOn = g_vstEngineOn;
        const bool csPd = g_mixedPd || (g_langCsound && g_langPd);
        const bool vstWanted = g_langVst && anyVstTrack();   // instruments or effects - nothing to run without a VST track
        const bool on = csOn;
        // The label is the list of engines this button starts, exactly as checked in Settings >
        // Languages ("Run Csound + VST", "Run Csound + PD + SC + VST", ...).
        std::string label = g_langSc ? (csPd ? "Run Csound + PD + SC" : "Run Csound + SC")
                                     : (csPd ? "Run Csound + PD" : "Run Csound");
        if (g_langVst) label += " + VST";
        if (csOn) {
            // Checked engines that are not up yet: pressing ADDS them; when all are up, it stops them.
            std::string missing;
            if (g_langSc && !scOn) missing = "SC";
            if (vstWanted && !vstOn) { if (!missing.empty()) missing += " + "; missing += "VST"; }
            if (!missing.empty()) label = "Add " + missing;
            else label = (csPd || scOn || vstOn) ? "Stop engines" : "Stop Csound";
        }
        if (on) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(label.c_str())) {
            if (csOn) {
                bool added = false;
                if (g_langSc && !scOn) { startScEngine(); added = true; }
                if (vstWanted && !vstOn) { startVstEngine(); added = true; }
                if (!added) {
                    if (scOn) stopScEngine();
                    if (vstOn) stopVstEngine();
                    stopPlayback();
                }
            } else {
                const bool startScAfterHost = g_langSc && !g_scEngineOn;
                // Open the Csound/PD-owned device stream first, then start SC
                // into that callback instead of opening and replacing the card twice.
                startEngine();
                if (startScAfterHost && g_liveCsound && !startScEngine()) stopPlayback();
                if (vstWanted && !g_vstEngineOn) startVstEngine();
            }
        }
        if (on) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) helpTip("%s", (g_langSc || g_langVst)
            ? "Run the checked engines together (Csound with PD when enabled, SuperCollider, VST). If Csound is already running, this button adds the missing ones or stops them all."
            : (csPd ? "Run Csound and PD through one CAW audio output." : "Run Csound for Csound timeline clips."));
        ImGui::SameLine();
    }
    if (g_langPd && !g_langCsound && !g_mixedPd) {
        const bool on = g_pdEngineOn;
        const bool scOn = g_scEngineOn;
        const bool vstOn = g_vstEngineOn;
        const bool vstWanted = g_langVst && anyVstTrack();   // instruments or effects - nothing to run without a VST track
        std::string label = g_langSc ? "Run PD + SC" : "Run PD";
        if (g_langVst) label += " + VST";
        if (on) {
            std::string missing;
            if (g_langSc && !scOn) missing = "SC";
            if (vstWanted && !vstOn) { if (!missing.empty()) missing += " + "; missing += "VST"; }
            if (!missing.empty()) label = "Add " + missing;
            else label = (scOn || vstOn) ? "Stop engines" : "Stop PD";
        }
        if (on) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(label.c_str())) {
            if (on) {
                bool added = false;
                if (g_langSc && !scOn) { startScEngine(); added = true; }
                if (vstWanted && !vstOn) { startVstEngine(); added = true; }
                if (!added) {
                    const bool hadSc = scOn;
                    if (hadSc) stopScEngine();
                    if (vstOn) stopVstEngine();
                    if (g_pdTimelineMode && g_timelineRun) { pdTransport(false); g_timelineRun = false; }
                    g_pdTimelineEngine.stop(); g_pdPlayingClip = 0; g_pdEngineOn = false; g_pdTimelineMode = false; g_pdTimelineEvents.clear();
                    g_pdResumeAfterReload = false;
                    if (!hadSc) restoreScSoloOutput();
                }
            } else if ((g_playing && !g_useCsoundApi) || engineIsUp() || g_testSound.running() || g_renderActive || g_pdPreview.active()) {
                playNotice("Stop the legacy audio process, test sound, or export before starting PD.");
            } else {
                if (g_pdLibraryPath.empty()) g_pdLibraryPath = plat::bundledLibpd();
                std::string error;
                std::vector<pd_timeline::Event> plan;
                if (g_pdLibraryPath.empty()) playNotice("Choose libpd in Settings > Programs.");
                else if (!makePdPlan(plan, error, true)) playNotice(error, "PD clip problem: see the log");
                else {
                    if (!g_audioDevicesReady) refreshAudioDevices(false);
                    if (g_proj.cs.outDev == -2) playNotice("Select an audio output in Settings > Audio.");
                    else {
                        closeScSoloOutput();
                        g_pdTimelineEngine.setHub(&g_cawHub);   // CAW hub: PD-only mode shares the same caws/cawr bus
                        if (!g_pdTimelineEngine.start(g_pdLibraryPath, plan, pdAudioSettings())) {
                            playNotice("PD preparation failed: " + g_pdTimelineEngine.status());
                            restoreScSoloOutput();
                        } else {
                            g_pdTimelineEvents=std::move(plan);g_pdNeedsReload=false;
                            g_pdPlanUpdateFailed=false;
                            g_idleMeters=meterLayout(g_proj);
                            g_pdSnapshotSeq=0;
                            g_peakIn.assign((size_t)std::max(1, g_proj.cs.nchnls), 0.f);
                            g_meterClipHold.assign(g_peakIn.size(), 0.f);
                            g_haveRaw=false;g_rawAge=10.f;g_monAge=10.f;
                            g_pdEngineOn = true; g_pdPlanT = 0.0;
                            logLine("[caw] Preloading all PD clips. Play is available when PD is ready.");
                            if (g_langSc && !g_scEngineOn && !startScEngine()) {
                                g_pdTimelineEngine.stop();
                                g_pdEngineOn = false; g_pdTimelineMode = false; g_pdTimelineEvents.clear();
                                restoreScSoloOutput();
                            }
                            if (vstWanted && !g_vstEngineOn && g_pdEngineOn) startVstEngine();
                        }
                    }
                }
            }
        }
        if (on) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) helpTip("%s", g_langSc || g_langVst
            ? "Run the checked engines together on one CAW output (PD, SuperCollider, VST), or add/stop them while PD keeps running."
            : "Preload all PD clips. Audio output stays open until Stop PD.");
        ImGui::SameLine();
    }
    if (g_langSc && !g_langCsound && !g_langPd) {
        const bool on = g_scEngineOn;
        const bool vstOn = g_vstEngineOn;
        const bool vstWanted = g_langVst && anyVstTrack();   // instruments or effects - nothing to run without a VST track
        std::string label;
        if (!on) label = g_langVst ? "Run SuperCollider + VST" : "Run SuperCollider";
        else if (vstWanted && !vstOn) label = "Add VST";
        else label = vstOn ? "Stop engines" : "Stop SuperCollider";
        if (on || vstOn) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(label.c_str())) {
            if (!on) {
                if (startScEngine() && vstWanted && !g_vstEngineOn) startVstEngine();
            } else if (vstWanted && !vstOn) {
                startVstEngine();
            } else {
                if (vstOn) stopVstEngine();
                stopScEngine();
            }
        }
        if (on || vstOn) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) helpTip("Start or stop the checked engines inside CAW. Their audio is mixed into the same output stream.");
        ImGui::SameLine();
    }
    // The VST-only session: the same combined Run button, in its shortest form ("Run VST").
    // With any other engine checked the VST engine is part of that engine's button instead.
    if (g_langVst && !g_langCsound && !g_liveCsound && !g_langPd && !g_langSc) {
        const bool on = g_vstEngineOn;
        if (on) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(on ? "Stop VST" : "Run VST")) {
            if (on) stopVstEngine();
            else startVstEngine();
        }
        if (on) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            helpTip("Run the VSTi (instrument) tracks. Starts CAW's own VST engine: it loads the instrument plugins and plays the MIDI clips and the notes from clip code. It does not need Csound. Press Play afterwards.");
        ImGui::SameLine();
    }
    int ms = (int)(g_play * 1000.0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%02d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    ImGui::SameLine(0, S(18));

    auto toolButton = [&](const char* label, Tool tool, const char* tip, bool join = false) {
        const bool active = g_tool == tool;
        if (active) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
        if (ImGui::Button(label)) { if (join) runJoinTool(); else g_tool = tool; }
        if (active) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) helpTip("%s", tip);
    };
    toolButton("Select", Tool::Select, "Select tool (1): select, box-select, move and resize clips.");
    ImGui::SameLine(0, S(3));
    // One pen button. Click = use the pen; right-click = choose Pen 1 / Pen 2 (keys 8 / 9 also work).
    static Tool lastPen = Tool::Pen;
    const bool penActive = g_tool == Tool::Pen || g_tool == Tool::Pen2;
    if (penActive) lastPen = g_tool;
    if (penActive) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
    if (ImGui::Button(lastPen == Tool::Pen2 ? "Pen 2" : "Pen 1")) g_tool = lastPen;
    if (penActive) ImGui::PopStyleColor(3);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##pen_menu");
    if (ImGui::IsItemHovered()) helpTip("Pen: draw automation. Right-click to choose Pen 1 (simplified, 8) or Pen 2 (detailed, 9).");
    if (ImGui::BeginPopup("##pen_menu")) {
        if (ImGui::MenuItem("Pen 1: simplified", "8", g_tool == Tool::Pen)) g_tool = lastPen = Tool::Pen;
        if (ImGui::MenuItem("Pen 2: detailed", "9", g_tool == Tool::Pen2)) g_tool = lastPen = Tool::Pen2;
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, S(3));
    toolButton("Cut", Tool::Cut, "Cut MIDI clips (3).");
    ImGui::SameLine(0, S(3));
    toolButton("Join", Tool::Join, "Join MIDI clips (4).", true);
    ImGui::SameLine(0, S(10));
    if (ImGui::Button("Score")) g_scoreOpen = true;
    if (ImGui::IsItemHovered()) helpTip("Edit and export the score.");
    endChildTight();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- arrange view
// ---- marker / graph track items on the timeline
bool primsConvex(const std::vector<ImVec2>& v) {
    if (v.size() < 3) return false;
    int sign = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const ImVec2 &a = v[i], &b = v[(i + 1) % v.size()], &c = v[(i + 2) % v.size()];
        const float cr = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (std::fabs(cr) < 1e-4f) continue;
        const int sg = cr > 0 ? 1 : -1;
        if (sign == 0) sign = sg; else if (sign != sg) return false;
    }
    return true;
}
void drawShapePrims(ImDrawList* dl, int shape, float x0, float y0, float x1, float y1, ImU32 line, ImU32 fillSoft, double ratio = 1.0) {
    for (const markgfx::Prim& p : markgfx::shapePrims(shape, x0, y0, x1, y1, ratio)) {
        std::vector<ImVec2> v;
        for (const auto& q : p.pts) v.push_back(ImVec2((float)q.first, (float)q.second));
        if (p.fill && (p.solid || primsConvex(v))) dl->AddConvexPolyFilled(v.data(), (int)v.size(), p.solid ? line : fillSoft);
        if (v.size() >= 2) dl->AddPolyline(v.data(), (int)v.size(), line, p.closed ? ImDrawFlags_Closed : 0, S((float)p.sw));
    }
}
ImU32 codeTokColor(int kind) {
    static const ImU32 cols[] = { IM_COL32(225, 226, 232, 255), IM_COL32(197, 134, 192, 255), IM_COL32(181, 206, 168, 255),
                                  IM_COL32(206, 145, 120, 255), IM_COL32(106, 153, 85, 255), IM_COL32(86, 156, 214, 255) };
    return cols[std::min(5, std::max(0, kind))];
}
// draws one item in the box (x0,top)-(x1,bot); `rgb` is the item colour. Size (percent) shrinks shapes and boxes inside the row (the score shows the full size),
// Font size changes the text.
void drawMarkItem(ImDrawList* dl, int trackIdx, const Track& track, const TrackMark& mk, float x0, float x1, float top, float bot, uint32_t rgb) {
    const ImU32 line = trackColor(trackIdx, 235, rgb), soft = trackColor(trackIdx, 80, rgb);
    const float rowH = bot - top;
    const float scale = (float)std::min(400, std::max(40, mk.size)) / 100.f;
    // on the timeline a shape or box is at most as big as its row; the score shows the full size
    const float h = std::min(rowH, rowH * scale);
    const float sTop = top + (rowH - h) * 0.5f, sBot = sTop + h;
    ImFont* font = g_markUiFont ? g_markUiFont : ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * (mk.font > 0 ? (float)mk.font / 9.f : 1.f);
    auto centered = [&](float cx, float cy, const char* t) {
        const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, t);
        dl->AddText(font, fs, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), IM_COL32(240, 241, 245, 255), t);
    };
    if (track.kind == 2) {   // graph track: graphic-score shapes only
        const float tw = mk.text.empty() ? 0.f : font->CalcTextSizeA(fs, FLT_MAX, 0.f, mk.text.c_str()).x + S(8);
        const float textX = (float)markgfx::shapeRight(mk.shape, x0, sTop, x1, sBot, mk.ratio / 100.0) + S(4);
        dl->PushClipRect(ImVec2(x0, std::min(top, sTop)), ImVec2(std::max(std::max(x1, x0 + h) + S(2), textX + tw), std::max(bot, sBot)), true);
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), trackColor(trackIdx, 26, rgb), S(3));   // always shown on the timeline: light background + thin border
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bot), trackColor(trackIdx, 110, rgb), S(3), 0, S(1.f));
        drawShapePrims(dl, mk.shape, x0, sTop, x1, sBot, line, soft, mk.ratio / 100.0);
        if (!mk.text.empty()) dl->AddText(font, fs, ImVec2(textX, (top + bot) * 0.5f - fs * 0.5f), IM_COL32(235, 236, 240, 255), mk.text.c_str());
        dl->PopClipRect();
        return;
    }
    const std::string label = mk.text;
    if (mk.type == 1) {   // letter box
        const float w = std::max(h, font->CalcTextSizeA(fs, FLT_MAX, 0.f, label.c_str()).x + S(12));
        dl->AddRectFilled(ImVec2(x0, sTop), ImVec2(x0 + w, sBot), soft, S(2));
        dl->AddRect(ImVec2(x0, sTop), ImVec2(x0 + w, sBot), line, S(2), 0, S(1.8f));
        centered(x0 + w * 0.5f, (sTop + sBot) * 0.5f, label.c_str());
    } else if (mk.type == 2) {   // cue
        const float cx = x0 + h * 0.5f, cy = (sTop + sBot) * 0.5f;
        if (mk.shape == 0) { dl->AddCircleFilled(ImVec2(cx, cy), h * 0.5f, soft); dl->AddCircle(ImVec2(cx, cy), h * 0.5f, line, 0, S(1.8f)); }
        else if (mk.shape == 1) { dl->AddRectFilled(ImVec2(x0, sTop), ImVec2(x0 + h, sBot), soft, S(2)); dl->AddRect(ImVec2(x0, sTop), ImVec2(x0 + h, sBot), line, S(2), 0, S(1.8f)); }
        else if (mk.shape == 2) {
            const ImVec2 d[4] = { ImVec2(x0, cy), ImVec2(cx, sTop), ImVec2(x0 + h, cy), ImVec2(cx, sBot) };
            dl->AddConvexPolyFilled(d, 4, soft); dl->AddPolyline(d, 4, line, ImDrawFlags_Closed, S(1.8f));
        } else if (mk.shape == 4) {   // [ Q ]
            const float t = std::min(S(5), h * 0.3f), xl = x0 + S(1), xr = x0 + h - S(1);
            const ImVec2 lb[4] = { ImVec2(xl + t, sTop), ImVec2(xl, sTop), ImVec2(xl, sBot), ImVec2(xl + t, sBot) };
            const ImVec2 rb[4] = { ImVec2(xr - t, sTop), ImVec2(xr, sTop), ImVec2(xr, sBot), ImVec2(xr - t, sBot) };
            dl->AddPolyline(lb, 4, line, 0, S(1.8f)); dl->AddPolyline(rb, 4, line, 0, S(1.8f));
        } else dl->AddLine(ImVec2(x0 + S(2), sBot - S(1)), ImVec2(x0 + h - S(2), sBot - S(1)), line, S(2));
        centered(cx, cy - (mk.shape == 3 ? S(1) : 0.f), "Q");
        if (!label.empty()) dl->AddText(font, fs, ImVec2(x0 + h + S(5), cy - fs * 0.5f), IM_COL32(235, 236, 240, 255), label.c_str());
    } else if (mk.type == 4) {   // span bracket: horizontal line, 90 degree ends, comment centred above (ends down) or below (ends up)
        const float tk = std::min(rowH * 0.35f, S(7.f) * scale), up = mk.shape == 1 ? 1.f : 0.f;
        const float yl = up > 0.5f ? top + tk + S(1) : bot - tk - S(1), ye = up > 0.5f ? top + S(1) : bot - S(1);
        const float xe = std::max(x1, x0 + S(24));
        const ImVec2 br[4] = { ImVec2(x0, ye), ImVec2(x0, yl), ImVec2(xe, yl), ImVec2(xe, ye) };
        dl->AddPolyline(br, 4, line, 0, S(1.8f));
        if (!label.empty()) {
            const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label.c_str());
            dl->AddText(font, fs, ImVec2((x0 + xe) * 0.5f - sz.x * 0.5f, up > 0.5f ? yl + S(2) : yl - sz.y - S(2)), IM_COL32(235, 236, 240, 255), label.c_str());
        }
    } else if (mk.type == 3) {   // code, coding-style font, colors of the chosen language
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), IM_COL32(24, 25, 30, 255), S(3));
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bot), line, S(3), 0, S(1.2f));
        ImFont* mono = g_markCodeFont ? g_markCodeFont : (g_codeFonts[0] ? g_codeFonts[0] : font);
        const float cfs = fs * 0.85f;
        dl->PushClipRect(ImVec2(x0 + S(3), top), ImVec2(x1 - S(2), bot), true);
        float y = top + S(2);
        std::istringstream lines(label);
        std::string ln;
        while (std::getline(lines, ln) && y + cfs <= bot + 1.f) {
            float x = x0 + S(6);
            for (const markgfx::Tok& tk : markgfx::highlightLine(ln, mk.lang)) {
                dl->AddText(mono, cfs, ImVec2(x, y), codeTokColor(tk.kind), tk.text.c_str());
                x += mono->CalcTextSizeA(cfs, FLT_MAX, 0.f, tk.text.c_str()).x;
            }
            y += cfs + S(1);
        }
        dl->PopClipRect();
    } else {   // comment
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), soft, S(3));
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bot), line, S(3), 0, S(1.5f));
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + S(4), bot), trackColor(trackIdx, 255, rgb), S(2));
        dl->PushClipRect(ImVec2(x0 + S(6), top), ImVec2(x1 - S(2), bot), true);
        dl->AddText(font, fs, ImVec2(x0 + S(8), top + S(3)), IM_COL32(235, 236, 240, 255), label.c_str(), nullptr, std::max(S(20), x1 - x0 - S(12)));
        dl->PopClipRect();
    }
}

// Tracks use the original 6 px LEDs; only the master uses taller LEDs.
void drawLedMeter(ImDrawList* dl, float x0, float x1, float top, float bottom, float db, bool clip, bool gray = false, bool master = false) {
    if (x1 <= x0 || bottom <= top) return;
    const float pitch = S(master ? 12.f : 7.8f), ledH = S(master ? 10.f : 6.f), pad = (pitch - ledH) * 0.5f;
    const int n = std::max(2, (int)std::floor((bottom - top) / pitch));
    const float fraction = std::min(1.f, std::max(0.f, (db + 60.f) / 60.f));
    const int lit = (int)std::lround(std::pow(fraction, 1.8f) * (n - 1));
    dl->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
    for (int i = 0; i < n; ++i) {
        const bool isTop = i == n - 1;
        const bool on = isTop ? clip : i < lit;
        const float y = bottom - (i + 1) * pitch + pad;
        const ImU32 color = gray ? (on ? IM_COL32(175, 180, 186, 255) : IM_COL32(38, 41, 45, 255))
                                 : isTop ? (on ? IM_COL32(255, 45, 45, 255) : IM_COL32(33, 6, 6, 255))
                                         : (on ? IM_COL32(50, 212, 255, 255) : IM_COL32(7, 28, 33, 255));
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + ledH), color, S(0.06f));
    }
    dl->PopClipRect();
}

// Q aligns selected starts, without a grid or changes to clip durations.
void qSelectedClips() {
    if (g_selClips.size() + g_selMidiClips.size() < 2) return;
    historyFlush();
    clip_timing::sync(g_proj, g_selClips, g_selMidiClips);
    historyFlush();
}

void drawClipQMenu() {
    if (ImGui::MenuItem("Q", "Q", false, g_selClips.size() + g_selMidiClips.size() >= 2))
        qSelectedClips();
    if (ImGui::IsItemHovered()) helpTip("Align selected clip starts to the earliest selected clip.");
}

void drawChevron(ImDrawList* dl, float x, float y, float radius, bool right, ImU32 color) {
    caw_ui::disclosure(dl, ImVec2(x, y), radius, color, !right);
}

void drawArrange(float belowH, float rightW) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::BeginChild("arrange", ImVec2(-rightW, -belowH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sb = S(12);  // scrollbar thickness
    if (g_drag == Drag::ResizeHeader && ImGui::IsMouseDown(0))
        g_headerW = std::min(900.f, std::max(120.f, (io.MousePos.x - org.x) / g_ui));
    const float headerW = std::min(std::max(S(g_headerW), S(120)), std::max(S(120), avail.x - S(160))), rulerH = S(26);
    const ImVec2 bodyMin(org.x + headerW, org.y + rulerH);
    const ImVec2 bodyMax(org.x + avail.x - sb, org.y + avail.y - sb);
    const ImVec2 rulerMin(bodyMin.x, org.y), rulerMax(bodyMax.x, bodyMin.y);
    const ImVec2 hdrMin(org.x, bodyMin.y), hdrMax(bodyMin.x, bodyMax.y);
    const ImVec2 m = io.MousePos;
    for (Track& tr : g_proj.tracks) tr.height = tr.kind == 2 ? 58.f : kFixedTrackH;  // lanes have a fixed height  // lanes have a fixed height
    const int nT = (int)g_proj.tracks.size();

    {   // keep the multi-selection consistent with the primary selection and the existing clips
        std::set<int> existing;
        for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) existing.insert(c.id);
        for (auto it = g_selClips.begin(); it != g_selClips.end();)
            it = existing.count(*it) ? std::next(it) : g_selClips.erase(it);
        const Clip* primary = selectedClip();
        if (g_selClips.empty() && primary) g_selClips.insert(primary->id);
        else if (!primary || !g_selClips.count(primary->id)) setPrimaryFromSet();
    }

    auto th = [&](int i) {
        const Track& t = g_proj.tracks[(size_t)i];
        if (trackHiddenByFolder(t)) return 0.f;   // inside a collapsed folder
        const bool midiEnabled = midiShown(t);
        return S(t.height + automationHeightSum(t) + (midiEnabled ? kMidiLaneH : 0.f));
    };
    auto hiddenTrack = [&](int i) { return trackHiddenByFolder(g_proj.tracks[(size_t)i]); };
    std::vector<float> yTop((size_t)nT + 1);
    std::vector<float> yEnd((size_t)nT + 1);       // bottom of the track (with its automation and MIDI lanes)
    std::vector<float> folderHdrH((size_t)nT, 0.f);   // > 0: the header strip of a folder group sits right above this track (its first member)
    {
        std::set<int> seen;
        for (int i = 0; i < nT; ++i) {
            const int fid = g_proj.tracks[(size_t)i].folderId;
            if (fid != 0 && findFolder(fid) && seen.insert(fid).second) folderHdrH[(size_t)i] = S(kFolderHdrH);
        }
    }
    std::vector<float> baseBottom((size_t)nT);
    auto layout = [&]() {
        float y = bodyMin.y - g_scrollY;
        for (int i = 0; i < nT; ++i) {
            y += folderHdrH[(size_t)i];
            yTop[(size_t)i] = y;
            baseBottom[(size_t)i] = hiddenTrack(i) ? y : y + S(g_proj.tracks[(size_t)i].height);
            y += th(i);
            yEnd[(size_t)i] = y;
        }
        yTop[(size_t)nT] = y;
        yEnd[(size_t)nT] = y;
    };

    auto tx = [&](double t) { return bodyMin.x + (float)((t - g_scrollX) * g_pps); };
    auto xt = [&](float x) { return (double)(x - bodyMin.x) / g_pps + g_scrollX; };
    auto laneOf = [&](float y) -> int {
        for (int i = 0; i < nT; ++i)
            if (y >= yTop[(size_t)i] && y < yEnd[(size_t)i]) return i;
        return -1;
    };
    auto clipAt = [&](int ti, float x) -> int {
        if (ti < 0) return -1;
        auto& cl = g_proj.tracks[(size_t)ti].clips;
        for (int k = (int)cl.size() - 1; k >= 0; --k)
            if (x >= tx(cl[(size_t)k].start) && x <= tx(cl[(size_t)k].start + cl[(size_t)k].dur)) return k;
        return -1;
    };

    const bool winHover = ImGui::IsWindowHovered();
    const bool inBody = winHover && ImGui::IsMouseHoveringRect(bodyMin, bodyMax);
    const bool inRuler = winHover && ImGui::IsMouseHoveringRect(rulerMin, rulerMax);

    // ---- scroll / zoom
    const bool inHdr = winHover && ImGui::IsMouseHoveringRect(hdrMin, hdrMax);
    if ((inBody || inRuler || inHdr) && io.MouseWheel != 0.f) {
        if (inHdr) {
            g_scrollY -= io.MouseWheel * S(kDefaultTrackH) * 0.6f;
        } else if (io.KeyCtrl) {
            g_pps = std::min(kPpsMax, std::max(kPpsMin, g_pps * (1.f + 0.12f * io.MouseWheel)));
            g_scrollX = zoomAroundPlayhead(g_pps, g_play, std::max(1.f, bodyMax.x - bodyMin.x));
        } else if (io.KeyShift) {
            g_scrollX -= io.MouseWheel * 60.f / g_pps;
        } else {
            g_scrollY -= io.MouseWheel * S(kDefaultTrackH) * 0.6f;
        }
    }
    if (io.MouseWheelH != 0.f && (inBody || inRuler)) g_scrollX -= io.MouseWheelH * 60.f / g_pps;
    const double viewSec = std::max(0.001, (double)(bodyMax.x - bodyMin.x) / g_pps);
    const double minX = 0.0;
    g_scrollX = (float)std::max(minX, (double)g_scrollX);
    float contentH = S(kDefaultTrackH);
    for (int i = 0; i < nT; ++i) contentH += th(i) + folderHdrH[(size_t)i];
    const float viewH = std::max(1.f, bodyMax.y - bodyMin.y);
    const float maxY = std::max(0.f, contentH - viewH);
    g_scrollY = std::min(std::max(g_scrollY, 0.f), maxY);
    double projectEnd = 0.0;
    for (const Track& t : g_proj.tracks) {
        for (const Clip& c : t.clips) projectEnd = std::max(projectEnd, c.start + c.dur);
        for (const auto& lane : t.automation)
            for (const AutoPoint& p : lane.second) projectEnd = std::max(projectEnd, p.t);
    }
    const double maxX = std::max({0.0, projectEnd - viewSec * 0.5, 10.0 - viewSec, (double)g_scrollX});
    const double scrollSpan = maxX - minX;
    const double totalSec = scrollSpan + viewSec;
    const float vTrackLen = viewH, hTrackLen = std::max(1.f, bodyMax.x - bodyMin.x);
    const float vThumbLen = std::min(vTrackLen, std::max(S(28), vTrackLen * viewH / std::max(viewH, contentH)));
    const float hThumbLen = std::min(hTrackLen, std::max(S(28), hTrackLen * (float)(viewSec / totalSec)));
    const float vRange = std::max(1.f, vTrackLen - vThumbLen), hRange = std::max(1.f, hTrackLen - hThumbLen);
    layout();

    // ---- resize edges: instrument lane and each automation lane can size independently
    int edgeTrack = -1;
    int edgeAutomationTrack = -1;
    std::string edgeAutomationChannel;
    float edgeY = 0.f;
    const bool headerEdge = winHover && g_drag == Drag::None && std::fabs(m.x - bodyMin.x) <= S(3) &&
                            m.y >= org.y && m.y <= bodyMax.y + sb;
    if (!headerEdge && winHover && g_drag == Drag::None && m.x >= org.x && m.x <= bodyMax.x && m.y >= bodyMin.y && m.y <= bodyMax.y) {
        for (int i = 0; i < nT; ++i) {
            if (hiddenTrack(i)) continue;
            const Track& track = g_proj.tracks[(size_t)i];
            const float baseEdge = baseBottom[(size_t)i];
            float laneTop = baseEdge;
            for (const auto& [channel, points] : track.automation) {
            if (!autoShown(track, channel)) continue;
                const float laneBottom = laneTop + S(automationHeight(track, channel));
                if (std::fabs(m.y - laneBottom) <= S(4)) {
                    edgeAutomationTrack = i;
                    edgeAutomationChannel = channel;
                    edgeY = laneBottom;
                    break;
                }
                laneTop = laneBottom;
            }
            if (edgeAutomationTrack >= 0) break;
        }
    }
    const bool resizingAuto = g_drag == Drag::ResizeAutomation;
    if (edgeTrack >= 0 || edgeAutomationTrack >= 0 || g_drag == Drag::ResizeTrack || resizingAuto)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    bool consumed = false;

    // ---- drag the edge of the left panel to resize it
    if (headerEdge || g_drag == Drag::ResizeHeader) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (headerEdge && ImGui::IsMouseClicked(0)) {
        g_drag = Drag::ResizeHeader;
        consumed = true;
    }

    // ---- scrollbars (X: seconds, Y: pixels)
    const ImVec2 vBarA(bodyMax.x, bodyMin.y), vBarB(bodyMax.x + sb, bodyMax.y);
    const ImVec2 hBarA(bodyMin.x, bodyMax.y), hBarB(bodyMax.x, bodyMax.y + sb);
    if (g_drag == Drag::None && winHover && ImGui::IsMouseClicked(0)) {
        if (ImGui::IsMouseHoveringRect(vBarA, vBarB)) {
            const float top = bodyMin.y + (maxY > 0.f ? g_scrollY / maxY * vRange : 0.f);
            g_sbGrab = (m.y >= top && m.y <= top + vThumbLen) ? m.y - top : vThumbLen * 0.5f;
            g_drag = Drag::ScrollY;
            consumed = true;
        } else if (ImGui::IsMouseHoveringRect(hBarA, hBarB)) {
            const float left = bodyMin.x + (scrollSpan > 0.0 ? (float)std::min(1.0, std::max(0.0, (g_scrollX - minX) / scrollSpan)) * hRange : 0.f);
            g_sbGrab = (m.x >= left && m.x <= left + hThumbLen) ? m.x - left : hThumbLen * 0.5f;
            g_drag = Drag::ScrollX;
            consumed = true;
        }
    }
    if (g_drag == Drag::ScrollY && ImGui::IsMouseDown(0))
        g_scrollY = std::min(maxY, std::max(0.f, (m.y - bodyMin.y - g_sbGrab) / vRange * maxY));
    if (g_drag == Drag::ScrollX && ImGui::IsMouseDown(0))
        g_scrollX = (float)std::min(maxX, std::max(minX, minX + (double)(m.x - bodyMin.x - g_sbGrab) / hRange * scrollSpan));
    if (g_drag == Drag::ScrollX || g_drag == Drag::ScrollY) { consumed = true; layout(); }

    if (edgeTrack >= 0) {
        if (ImGui::IsMouseDoubleClicked(0)) {
            g_proj.tracks[(size_t)edgeTrack].height = kDefaultTrackH;
            consumed = true;
        } else if (ImGui::IsMouseClicked(0)) {
            g_drag = Drag::ResizeTrack;
            g_resizeTrack = edgeTrack;
            g_resizeStartY = m.y;
            g_resizeStartH = S(g_proj.tracks[(size_t)edgeTrack].height);
            consumed = true;
        }
    } else if (edgeAutomationTrack >= 0) {
        Track& track = g_proj.tracks[(size_t)edgeAutomationTrack];
        if (ImGui::IsMouseDoubleClicked(0)) {
            track.automationHeights[edgeAutomationChannel] = kAutomationLaneH;
            consumed = true;
        } else if (ImGui::IsMouseClicked(0)) {
            g_drag = Drag::ResizeAutomation;
            g_resizeTrack = edgeAutomationTrack;
            g_resizeAutomationChannel = edgeAutomationChannel;
            g_resizeStartY = m.y;
            g_resizeStartH = S(automationHeight(track, edgeAutomationChannel));
            consumed = true;
        }
    }

    // ---- automation editing
    // Select tool: click a dot to select it, Ctrl+click to add/remove, drag empty space to box-select,
    //              drag selected dots to move them, click empty space (no drag) to add a dot,
    //              double-click or right-click a dot to type its exact value, Del deletes the selection.
    // Pen tool:    press and drag across a lane to write automation (replaces the dots under the stroke).
    const float autoPadY = S(7);
    auto autoLaneGeometry = [&](const Track& track, int ti, const std::string& channel, float& y0, float& y1) {
        y0 = automationLaneTop(track, baseBottom[(size_t)ti], channel);
        y1 = y0 + S(automationHeight(track, channel));
    };
    auto valueAtY = [&](float y, float y0, float y1, double lo, double hi) {
        const float usable = std::max(1.f, y1 - y0 - 2.f * autoPadY);
        const double v = hi - std::min(1.f, std::max(0.f, (y - y0 - autoPadY) / usable)) * (hi - lo);
        return std::min(hi, std::max(lo, v));
    };
    auto yOfValue = [&](double v, float y0, float y1, double lo, double hi) {
        const float usable = std::max(1.f, y1 - y0 - 2.f * autoPadY);
        const double norm = (std::min(hi, std::max(lo, v)) - lo) / (hi - lo);
        return y1 - autoPadY - (float)norm * usable;
    };
    validateAutoSelection();
    {   // ---- drag handles of the selected dots: depth (top), time stretch (right), square / triangle morph (bottom). Shift on depth = ramp (wave slowly widens)
        if (g_wh.active) {
            int whTi = -1;
            Track* whTrack = findTrackById(g_wh.trackId, &whTi);
            auto whLane = whTrack ? whTrack->automation.find(g_wh.channel) : std::map<std::string, std::vector<AutoPoint>>::iterator();
            if (!whTrack || whLane == whTrack->automation.end() || !ImGui::IsMouseDown(0)) {
                if (whTrack && whLane != whTrack->automation.end()) sortLaneKeepSelection(whLane->second, g_autoSel);
                g_wh.active = false;
            } else {
                consumed = true;
                float ly0 = 0.f, ly1 = 0.f;
                autoLaneGeometry(*whTrack, whTi, g_wh.channel, ly0, ly1);
                const float yC = yOfValue(g_wh.center, ly0, ly1, g_wh.lo, g_wh.hi);
                const float x0px = tx(g_wh.orig.front().t), x1px = tx(g_wh.orig.back().t);
                double dS = 1.0, dE = 1.0, st = 1.0, mo = 0.0;
                if (g_wh.kind == 1) {
                    const float denom = std::max(S(12), yC - g_wh.handleY);
                    dE = std::min(4.0, std::max(0.0, (double)((yC - m.y) / denom)));
                    dS = io.KeyShift ? 1.0 : dE;
                    ImGui::SetTooltip(io.KeyShift ? "Depth ramp: 100 %% -> %.0f %%" : "Depth %.0f %%", dE * 100.0);
                } else if (g_wh.kind == 2) {
                    st = std::min(20.0, std::max(0.05, (double)((m.x - S(13) - x0px) / std::max(S(4), x1px - x0px))));
                    ImGui::SetTooltip("Time %.0f %%", st * 100.0);
                } else {
                    mo = std::min(1.0, std::max(-1.0, (double)((g_wh.startY - m.y) / S(60))));
                    if (std::fabs(mo) < 0.02) ImGui::SetTooltip("Wave as drawn");
                    else ImGui::SetTooltip(mo > 0 ? "Towards square %.0f %%" : "Towards triangle %.0f %%", std::fabs(mo) * 100.0);
                }
                reshapeDots(whLane->second, g_wh.idx, g_wh.orig, dS, dE, st, mo, g_wh.lo, g_wh.hi);
                const ParsedTrack whParsed = pdParsedForUi(*whTrack);
                const CawParam* whParam = hasClips(*whTrack) ? laneParameter(whParsed, g_wh.channel) : nullptr;
                if (whParam) for (int index : g_wh.idx)
                    if (index >= 0 && index < (int)whLane->second.size())
                        whLane->second[(size_t)index].v = quantizeAutomation(whLane->second[(size_t)index].v, *whParam);
            }
        } else if (inBody && !consumed && ImGui::IsMouseClicked(0) && g_autoSel.size() >= 2 && g_autoMode == AutoMode::None) {
            int whTi = -1;
            Track* whTrack = findTrackById(g_autoSelTrackId, &whTi);
            auto whLane = whTrack ? whTrack->automation.find(g_autoSelChannel) : std::map<std::string, std::vector<AutoPoint>>::iterator();
            double tMin, tMax, vMin, vMax;
            if (whTrack && whLane != whTrack->automation.end() && autoShown(*whTrack, g_autoSelChannel) && autoSelBounds(whLane->second, g_autoSel, tMin, tMax, vMin, vMax)) {
                float ly0 = 0.f, ly1 = 0.f;
                autoLaneGeometry(*whTrack, whTi, g_autoSelChannel, ly0, ly1);
                double lo, hi, initial;
                laneValueRange(*whTrack, g_autoSelChannel, lo, hi, initial);
                const WaveHandles wh = waveHandlePos(tx(tMin), tx(tMax), yOfValue(vMax, ly0, ly1, lo, hi), yOfValue(vMin, ly0, ly1, lo, hi),
                                                     yOfValue((vMin + vMax) * 0.5, ly0, ly1, lo, hi), ly0, ly1, bodyMax.x);
                auto onKnob = [&](ImVec2 c) { return std::hypot(m.x - c.x, m.y - c.y) <= S(9); };
                int kind = onKnob(wh.depth) ? 1 : onKnob(wh.stretch) ? 2 : onKnob(wh.morph) ? 3 : 0;
                if (kind) {
                    g_wh = WaveHandleDrag();
                    g_wh.active = true; g_wh.kind = kind; g_wh.trackId = whTrack->id; g_wh.channel = g_autoSelChannel;
                    for (int idx : g_autoSel) if (idx >= 0 && idx < (int)whLane->second.size()) { g_wh.idx.push_back(idx); g_wh.orig.push_back(whLane->second[(size_t)idx]); }
                    g_wh.center = (vMin + vMax) * 0.5; g_wh.lo = lo; g_wh.hi = hi;
                    g_wh.handleY = kind == 1 ? wh.depth.y : wh.morph.y; g_wh.startY = m.y;
                    consumed = true;
                }
            }
        }
    }
    if (inBody && !consumed && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) {
        g_autoSelMulti.clear();
        for (int ti = 0; ti < nT && !consumed; ++ti) {
            if (hiddenTrack(ti)) continue;
            Track& track = g_proj.tracks[(size_t)ti];
            for (auto& [channel, points] : track.automation) {
            if (!autoShown(track, channel)) continue;
                float laneY0 = 0.f, laneY1 = 0.f;
                autoLaneGeometry(track, ti, channel, laneY0, laneY1);
                if (!ImGui::IsMouseHoveringRect(ImVec2(bodyMin.x, laneY0), ImVec2(bodyMax.x, laneY1))) continue;
                double lo, hi, initial;
                laneValueRange(track, channel, lo, hi, initial);
                int hit = -1;
                float hitDist = S(8);
                for (int pi = 0; pi < (int)points.size(); ++pi) {
                    const float dx = tx(points[(size_t)pi].t) - m.x;
                    const float dy = yOfValue(points[(size_t)pi].v, laneY0, laneY1, lo, hi) - m.y;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < hitDist) { hitDist = dist; hit = pi; }
                }
                const bool sameLane = g_autoSelTrackId == track.id && g_autoSelChannel == channel;
                if (!sameLane) { g_autoSel.clear(); g_autoSelMulti.clear(); g_autoSelTrackId = track.id; g_autoSelChannel = channel; }
                g_selTrack = ti;
                g_selClip = -1;
                consumed = true;
                if (!io.KeyCtrl && !io.KeyShift) { g_selClips.clear(); g_selMidiClips.clear(); }

                if (ImGui::IsMouseClicked(1)) {  // right-click a dot: edit its value
                    if (hit >= 0) {
                        if (!g_autoSel.count(hit)) { g_autoSel.clear(); g_autoSelMulti.clear(); g_autoSel.insert(hit); }
                        g_autoOpenPointPopup = true;
                    }
                    break;
                }
                g_autoDragTrackId = track.id;
                g_autoDragChannel = channel;
                g_autoDragStartMouse = m;
                g_autoDragMoved = false;
                const double t = clampTimelineTime(xt(m.x));
                const double v = laneQuantizeValue(track, channel, valueAtY(m.y, laneY0, laneY1, lo, hi));
                // double-click = create the dot AND drag it, all with the same press (Cubase style)
                const double nowT = ImGui::GetTime();
                const bool dbl = !isPenTool() && !io.KeyCtrl && !io.KeyShift &&
                    (ImGui::IsMouseDoubleClicked(0) ||
                     (nowT - g_autoLastPressT < 0.5 && g_autoLastTrackId == track.id && g_autoLastChannel == channel &&
                      std::fabs(m.x - g_autoLastPressPos.x) < S(16) && std::fabs(m.y - g_autoLastPressPos.y) < S(16)));
                const bool lastAddedValid = g_autoLastAddedValid;
                const AutoPoint lastAdded = g_autoLastAdded;
                g_autoLastPressT = dbl ? -10.0 : nowT;
                g_autoLastPressPos = m;
                g_autoLastTrackId = track.id;
                g_autoLastChannel = channel;
                g_autoLastAddedValid = false;
                if (dbl) {
                    int idx = hit;
                    if (idx < 0) {
                        if (lastAddedValid)   // drop the dot the first click made: the double-click makes the one that is dragged
                            for (size_t pi = 0; pi < points.size(); ++pi)
                                if (points[pi].t == lastAdded.t && points[pi].v == lastAdded.v) { points.erase(points.begin() + (long)pi); break; }
                        points.push_back({t, v});
                        std::stable_sort(points.begin(), points.end(), [](const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
                        for (int pi = 0; pi < (int)points.size(); ++pi)
                            if (points[(size_t)pi].t == t && points[(size_t)pi].v == v) { idx = pi; break; }
                    }
                    g_autoSel.clear(); g_autoSelMulti.clear();
                    if (idx >= 0) g_autoSel.insert(idx);
                    g_autoPressAdded = false;
                    g_autoDragOrig = points;
                    g_autoDragStartT = xt(m.x);
                    g_autoDragStartV = valueAtY(m.y, laneY0, laneY1, lo, hi);
                    g_autoMode = AutoMode::Move;
                    break;
                }
                if (isPenTool()) {
                    g_autoSel.clear(); g_autoSelMulti.clear();
                    // start the stroke: drop dots that sit right under the pen, then write the first one
                    const double eps = 2.0 / std::max(1.f, g_pps);
                    points.erase(std::remove_if(points.begin(), points.end(),
                        [&](const AutoPoint& p) { return std::fabs(p.t - t) <= eps; }), points.end());
                    // The lane stays sorted by time during the whole stroke: the line is drawn in the order of the dots,
                    // so an unsorted lane (new dots appended at the end) zig-zags while drawing.
                    std::stable_sort(points.begin(), points.end(), [](const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
                    insertAutoPointSorted(points, {t, v, false});
                    g_autoPenLastT = t;
                    g_autoPenLastX = m.x;
                    g_autoPenMinT = g_autoPenMaxT = t;
                    g_autoMode = AutoMode::Pen;
                } else if (hit >= 0) {
                    if (io.KeyCtrl) {
                        if (g_autoSel.count(hit)) g_autoSel.erase(hit); else g_autoSel.insert(hit);
                    } else if (!g_autoSel.count(hit)) {
                        g_autoSel.clear(); g_autoSelMulti.clear();
                        g_autoSel.insert(hit);
                    }
                    // double-click (like Cubase): the first click already dropped a dot, the second press grabs it
                    // and drags it at once (no selection-tool switch). Right-click a dot to type its exact value.
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        g_autoSel.clear(); g_autoSelMulti.clear();
                        g_autoSel.insert(hit);
                        g_autoDragOrig = points;
                        g_autoDragStartT = xt(m.x);
                        g_autoDragStartV = valueAtY(m.y, laneY0, laneY1, lo, hi);
                        g_autoMode = AutoMode::Move;
                    } else if (g_autoSel.count(hit)) {
                        g_autoDragOrig = points;
                        g_autoDragStartT = xt(m.x);
                        g_autoDragStartV = valueAtY(m.y, laneY0, laneY1, lo, hi);
                        g_autoMode = AutoMode::Move;
                    }
                } else {
                    g_autoMarqueeAdd = io.KeyCtrl || io.KeyShift;
                    if (!g_autoMarqueeAdd) {
                        g_autoSel.clear(); g_autoSelMulti.clear();
                        // instant feedback: the dot appears on mouse-down. If the mouse then drags, it turns into a box-select instead.
                        g_autoPressPoint = {t, v};
                        points.push_back(g_autoPressPoint);
                        std::stable_sort(points.begin(), points.end(), [](const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
                        for (int pi = 0; pi < (int)points.size(); ++pi)
                            if (points[(size_t)pi].t == t && points[(size_t)pi].v == v) { g_autoSel.insert(pi); break; }
                        g_autoLastAddedValid = true; g_autoLastAdded = g_autoPressPoint;
                        // create + drag with the SAME press (Cubase style): the new dot follows the mouse while the button is held.
                        // (Box-select = Shift/Ctrl + drag on empty lane space.)
                        g_autoPressAdded = false;
                        g_autoDragOrig = points;
                        g_autoDragStartT = t;
                        g_autoDragStartV = valueAtY(m.y, laneY0, laneY1, lo, hi);
                        g_autoMode = AutoMode::Move;
                    } else {
                        g_autoMode = AutoMode::Marquee;
                    }
                }
                break;
            }
        }
    }
    if (g_autoMode != AutoMode::None) {
        int ti = -1;
        Track* track = findTrackById(g_autoDragTrackId, &ti);
        auto laneIt = track ? track->automation.find(g_autoDragChannel) : std::map<std::string, std::vector<AutoPoint>>::iterator();
        if (!track || laneIt == track->automation.end()) {
            g_autoMode = AutoMode::None;
            g_autoDragTrackId = -1;
        } else {
            std::vector<AutoPoint>& points = laneIt->second;
            float laneY0 = 0.f, laneY1 = 0.f;
            autoLaneGeometry(*track, ti, g_autoDragChannel, laneY0, laneY1);
            double lo, hi, initial;
            laneValueRange(*track, g_autoDragChannel, lo, hi, initial);
            const float movedPx = std::fabs(m.x - g_autoDragStartMouse.x) + std::fabs(m.y - g_autoDragStartMouse.y);
            if (movedPx > S(3)) g_autoDragMoved = true;
            const bool down = ImGui::IsMouseDown(0);
            consumed = true;
            if (g_autoMode == AutoMode::Move) {
                if (down && g_autoDragMoved && g_autoDragOrig.size() == points.size()) {
                    const double dt = io.KeyShift ? 0.0 : xt(m.x) - g_autoDragStartT;  // Shift = value only
                    const double dv = valueAtY(m.y, laneY0, laneY1, lo, hi) - g_autoDragStartV;
                    for (int idx : g_autoSel) {
                        if (idx < 0 || idx >= (int)points.size()) continue;
                        points[(size_t)idx].t = clampTimelineTime(g_autoDragOrig[(size_t)idx].t + dt);
                        points[(size_t)idx].v = laneQuantizeValue(*track, g_autoDragChannel,
                            std::min(hi, std::max(lo, g_autoDragOrig[(size_t)idx].v + dv)));
                    }
                }
                if (!down) {
                    sortLaneKeepSelection(points, g_autoSel);
                    g_autoMode = AutoMode::None;
                }
            } else if (g_autoMode == AutoMode::Marquee) {
                if (g_autoPressAdded && g_autoDragMoved) {   // it was a drag, not a click: take the dot back and box-select
                    for (size_t pi = 0; pi < points.size(); ++pi)
                        if (points[pi].t == g_autoPressPoint.t && points[pi].v == g_autoPressPoint.v) { points.erase(points.begin() + (long)pi); break; }
                    g_autoPressAdded = false;
                    g_autoSel.clear(); g_autoSelMulti.clear();
                }
                if (!down) {
                    if (g_autoPressAdded) {
                        g_autoPressAdded = false;   // plain click: the dot is already there and selected
                    } else if (!g_autoDragMoved) {
                        // plain click on empty lane: add a dot there and select it
                        if (!g_autoMarqueeAdd) { g_autoSel.clear(); g_autoSelMulti.clear(); }
                        points.push_back({clampTimelineTime(xt(g_autoDragStartMouse.x)),
                                          laneQuantizeValue(*track, g_autoDragChannel,
                                              valueAtY(g_autoDragStartMouse.y, laneY0, laneY1, lo, hi))});
                        g_autoSel.insert((int)points.size() - 1);
                    } else {
                        const float x0 = std::min(m.x, g_autoDragStartMouse.x), x1 = std::max(m.x, g_autoDragStartMouse.x);
                        const float y0 = std::min(m.y, g_autoDragStartMouse.y), y1 = std::max(m.y, g_autoDragStartMouse.y);
                        for (int pi = 0; pi < (int)points.size(); ++pi) {
                            const float px = tx(points[(size_t)pi].t);
                            const float py = yOfValue(points[(size_t)pi].v, laneY0, laneY1, lo, hi);
                            if (px >= x0 && px <= x1 && py >= y0 - S(4) && py <= y1 + S(4)) g_autoSel.insert(pi);
                        }
                    }
                    sortLaneKeepSelection(points, g_autoSel);
                    g_autoMode = AutoMode::None;
                }
            } else if (g_autoMode == AutoMode::Pen) {
                if (down && std::fabs(m.x - g_autoPenLastX) >= S(3)) {
                    const double t = clampTimelineTime(xt(m.x));
                    const double v = laneQuantizeValue(*track, g_autoDragChannel, valueAtY(m.y, laneY0, laneY1, lo, hi));
                    const double a = std::min(t, g_autoPenLastT), b = std::max(t, g_autoPenLastT);
                    // The stroke overwrites everything it passes over, left to right or right to left: old dots AND dots of
                    // this stroke between the last pen position and the new one. A dot where the pen lands is replaced.
                    points.erase(std::remove_if(points.begin(), points.end(), [&](const AutoPoint& p) {
                        return (p.t > a && p.t < b) || p.t == t;
                    }), points.end());
                    // Dots of the stroke have no dot symbol while drawing (only the line), the lane stays time-sorted.
                    insertAutoPointSorted(points, {t, v, true});
                    g_autoPenLastT = t;
                    g_autoPenLastX = m.x;
                    g_autoPenMinT = std::min(g_autoPenMinT, t);
                    g_autoPenMaxT = std::max(g_autoPenMaxT, t);
                }
                if (!down) {
                    // the stroke becomes a few straight segments: Ramer-Douglas-Peucker on the dots of the stroke (a sine wave -> a triangle wave)
                    if (g_tool == Tool::Pen) {   // Pen 1 only
                        std::stable_sort(points.begin(), points.end(), [](const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
                        size_t i0 = points.size(), i1 = 0;
                        for (size_t pi = 0; pi < points.size(); ++pi)
                            if (points[pi].t >= g_autoPenMinT - 1e-9 && points[pi].t <= g_autoPenMaxT + 1e-9) { i0 = std::min(i0, pi); i1 = pi; }
                        if (i0 < points.size()) for (size_t pi = i0; pi <= i1; ++pi) points[pi].line = false;   // Pen 1: the few kept dots are shown
                        if (i0 < points.size() && i1 > i0 + 1) {
                            const float usable = std::max(1.f, laneY1 - laneY0 - 2.f * autoPadY);
                            const float tol = std::max(S(8), usable * 0.18f);
                            std::vector<ImVec2> px;
                            for (size_t pi = i0; pi <= i1; ++pi) px.push_back(ImVec2(tx(points[pi].t), yOfValue(points[pi].v, laneY0, laneY1, lo, hi)));
                            std::vector<char> keep(px.size(), 0);
                            keep.front() = keep.back() = 1;
                            std::function<void(size_t, size_t)> rdp = [&](size_t a, size_t b) {
                                if (b <= a + 1) return;
                                const float dx = px[b].x - px[a].x, dy = px[b].y - px[a].y;
                                const float len = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
                                float best = 0.f; size_t bi = a;
                                for (size_t k = a + 1; k < b; ++k) {
                                    const float d = std::fabs(dy * (px[k].x - px[a].x) - dx * (px[k].y - px[a].y)) / len;
                                    if (d > best) { best = d; bi = k; }
                                }
                                if (best > tol) { keep[bi] = 1; rdp(a, bi); rdp(bi, b); }
                            };
                            rdp(0, px.size() - 1);
                            std::vector<AutoPoint> out(points.begin(), points.begin() + (long)i0);
                            for (size_t k = 0; k < px.size(); ++k) if (keep[k]) out.push_back(points[i0 + k]);
                            out.insert(out.end(), points.begin() + (long)i1 + 1, points.end());
                            points = out;
                        }
                    }
                    if (g_tool == Tool::Pen2) {   // Pen 2 keeps every point, but only the first and the last point of the stroke show a dot
                        std::stable_sort(points.begin(), points.end(), [](const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
                        size_t i0 = points.size(), i1 = 0;
                        for (size_t pi = 0; pi < points.size(); ++pi)
                            if (points[pi].t >= g_autoPenMinT - 1e-9 && points[pi].t <= g_autoPenMaxT + 1e-9) { i0 = std::min(i0, pi); i1 = pi; }
                        if (i0 < points.size()) for (size_t pi = i0; pi <= i1; ++pi) points[pi].line = false;
                        if (i0 < points.size() && i1 > i0 + 1) {
                            points[i0].line = false; points[i1].line = false;
                            for (size_t pi = i0 + 1; pi < i1; ++pi) points[pi].line = true;
                        }
                    }
                    std::set<int> none;
                    sortLaneKeepSelection(points, none);
                    g_autoMode = AutoMode::None;
                }
            }
            if (g_autoMode == AutoMode::None) g_autoDragTrackId = -1;
        }
    }
    if (isPenTool() && inBody) {
        for (int ti = 0; ti < nT; ++ti) {
            if (hiddenTrack(ti)) continue;
            const Track& track = g_proj.tracks[(size_t)ti];
            for (const auto& lane : track.automation) {
                if (!autoShown(track, lane.first)) continue;
                float y0 = 0.f, y1 = 0.f;
                autoLaneGeometry(track, ti, lane.first, y0, y1);
                if (m.y >= y0 && m.y < y1) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
    }

    // ---- automation keyboard: Del deletes dots, Ctrl+A selects the whole lane, Esc clears
    const bool arrangeKeys = !io.WantTextInput && !g_textEditorFocused &&
                             (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                              ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
    bool deletedPoints = false;
    (void)deletedPoints;
    if (arrangeKeys && hasAutoSelection() &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        deleteSelectedAutoPoints();
        deletedPoints = true;
    }
    if (arrangeKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        if (std::vector<AutoPoint>* lane = selectedAutoLane()) {
            g_autoSel.clear(); g_autoSelMulti.clear();
            for (int i = 0; i < (int)lane->size(); ++i) g_autoSel.insert(i);
        }
    }
    if (arrangeKeys && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_autoSel.clear(); g_autoSelMulti.clear(); g_selMidiClips.clear(); g_tool = Tool::Select; }
    if (arrangeKeys && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) g_tool = Tool::Select;   // 1 = select
        if (ImGui::IsKeyPressed(ImGuiKey_8, false)) g_tool = Tool::Pen;      // 8 = pen 1
        if (ImGui::IsKeyPressed(ImGuiKey_9, false)) g_tool = Tool::Pen2;     // 9 = pen 2
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) g_tool = Tool::Cut;      // 3 = cut
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) g_tool = g_tool == Tool::Cut ? Tool::Select : Tool::Cut;   // C toggles cut
        if (ImGui::IsKeyPressed(ImGuiKey_4, false)) runJoinTool();           // 4 = join
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) qSelectedClips();
        if (g_selTrack >= 0 && g_selTrack < nT) {                            // M / S = mute / solo the selected track
            Track& selected = g_proj.tracks[(size_t)g_selTrack];
            if (ImGui::IsKeyPressed(ImGuiKey_M, false)) { selected.muted = !selected.muted; if (selected.muted) selected.solo = false; }
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) { selected.solo = !selected.solo; if (selected.solo) selected.muted = false; }
        }
        if (io.KeyShift && !io.KeyAlt && !io.KeyCtrl && !io.WantTextInput) {  // Shift+M / Shift+S = global
            if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
                const int count = unmuteAllTracks();
                logLine("[caw] Unmuted all tracks" + std::string(count ? " (" + std::to_string(count) + (count == 1 ? " track)" : " tracks)") : " (none were muted)") + ".");
            }
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                const int count = unsoloAllTracks();
                logLine("[caw] Cleared solo on all tracks" + std::string(count ? " (" + std::to_string(count) + (count == 1 ? " track)" : " tracks)") : " (none were solo)") + ".");
            }
        }
    }
    if (arrangeKeys && io.KeyAlt && !io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) toggleHideAllAutomation();
        if (ImGui::IsKeyPressed(ImGuiKey_J, false)) showAllAutomation();
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) toggleHideAllMidi();
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) showAllMidi();
    }

    // MIDI lane: MIDI clips are free regions. Click = select, drag = move (any position), drag an edge = resize,
    // Cut tool = split, double-click = piano roll, double-click on empty space = new MIDI clip.
    if (inBody && !consumed) {
        for (int ti = 0; ti < nT; ++ti) {
            Track& track = g_proj.tracks[(size_t)ti];
            if (!midiShown(track)) continue;
            const float laneTop = midiLaneTop(track, baseBottom[(size_t)ti]);
            const float laneBottom = laneTop + S(kMidiLaneH);
            if (m.y >= laneTop && m.y < laneBottom) {
                int target = -1;
                for (int k = (int)track.midiClips.size() - 1; k >= 0; --k) {
                    const MidiClip& c = track.midiClips[(size_t)k];
                    if (xt(m.x) >= c.start && xt(m.x) <= c.start + c.dur) { target = k; break; }
                }
                if (target >= 0 && g_mcDragId < 0) {
                    const MidiClip& hc = track.midiClips[(size_t)target];
                    if (g_tool == Tool::Cut || tx(hc.start + hc.dur) - m.x < S(8) || m.x - tx(hc.start) < S(8))
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) {
                    g_selTrack = ti;
                    if (target >= 0) {
                        MidiClip& hit = track.midiClips[(size_t)target];
                        if (ImGui::IsMouseClicked(0) &&
                            tx(hit.start + hit.dur) - tx(hit.start) > S(46) &&
                            m.x >= tx(hit.start + hit.dur) - S(21) && m.x <= tx(hit.start + hit.dur) - S(5) &&
                            m.y >= laneTop + S(5) && m.y <= laneTop + S(21)) {
                            hit.muted = !hit.muted;
                        } else if (ImGui::IsMouseClicked(1)) {
                            g_ctxMidiId = hit.id;
                            if (!g_selMidiClips.count(hit.id)) {
                                g_selMidiClips.clear(); g_selMidiClips.insert(hit.id);
                                g_selClips.clear(); g_selClip = -1;
                            }
                            ImGui::OpenPopup("##midi_context");
                        } else if (g_tool == Tool::Cut) {
                            cutMidiClipAt(ti, target, xt(m.x));
                        } else if (g_tool == Tool::Join) {
                            g_selClips.clear();
                            g_selClip = -1;
                            g_selMidiClips.clear();
                            g_selMidiClips.insert(hit.id);
                            runJoinTool();   // joins this region with the next one, then back to Select
                            g_tool = Tool::Select;
                        } else if (ImGui::IsMouseDoubleClicked(0)) {
                            hit.editorOpen = true;
                        } else {
                            const bool additive = io.KeyCtrl || io.KeyShift;
                            if (!additive && !g_selMidiClips.count(hit.id)) { g_selClips.clear(); g_selClip = -1; }
                            bool picked = true;
                            if (additive) {
                                if (g_selMidiClips.count(hit.id)) { g_selMidiClips.erase(hit.id); picked = false; }
                                else g_selMidiClips.insert(hit.id);
                            } else if (!g_selMidiClips.count(hit.id)) {
                                g_selMidiClips.clear();
                                g_selMidiClips.insert(hit.id);
                            }
                            if (picked) {
                                g_mcDragId = hit.id;
                                g_mcAnchorT = xt(m.x);
                                g_mcOrigStart = hit.start;
                                g_mcOrigEnd = hit.start + hit.dur;
                                g_mcOrigNotes = hit.notes;
                                if (tx(hit.start + hit.dur) - m.x < S(8)) { g_mcDragMode = 2; g_selMidiClips.clear(); g_selMidiClips.insert(hit.id); g_selClips.clear(); g_selClip = -1; }
                                else if (m.x - tx(hit.start) < S(8)) { g_mcDragMode = 3; g_selMidiClips.clear(); g_selMidiClips.insert(hit.id); g_selClips.clear(); g_selClip = -1; }
                                else g_mcDragMode = 1;
                                g_dragOrig.clear();   // selected Csound clips travel with the MIDI regions
                                if (g_mcDragMode == 1)
                                    for (const Track& tr : g_proj.tracks)
                                        for (const Clip& sc : tr.clips)
                                            if (g_selClips.count(sc.id)) g_dragOrig.push_back({sc.id, sc.start});
                                g_clipAutomationMove.capture(g_proj, g_selClips);
                                g_mcOrig.clear();
                                for (const Track& tr : g_proj.tracks)
                                    for (const MidiClip& mc : tr.midiClips)
                                        if (g_selMidiClips.count(mc.id)) g_mcOrig.push_back({mc.id, mc.start});
                            }
                        }
                    } else {
                        // empty MIDI lane space: drag (also after a double-click) = box-select Csound clips and MIDI regions;
                        // a double-click that is not dragged adds a MIDI clip (see the release code)
                        g_clipMarqueeAdd = io.KeyCtrl || io.KeyShift;
                        if (!g_clipMarqueeAdd) { g_selMidiClips.clear(); g_selClips.clear(); g_selClip = -1; }
                        g_drag = Drag::Marquee;
                        g_clipMarqueeStart = m;
                        g_clipMarqueeMoved = false;
                        g_marqDblKind = ImGui::IsMouseDoubleClicked(0) ? 2 : 0;
                        g_marqLane = ti;
                        g_marqT = clampTimelineTime(xt(m.x));
                    }
                    consumed = true;
                    break;
                }
            }
        }
    }
    if (g_mcDragId >= 0) {
        MidiClip* dragged = findMidiClip(g_mcDragId);
        if (!dragged || !ImGui::IsMouseDown(0)) {
            g_mcDragId = -1;
        } else if (g_mcDragMode == 1) {
            double dt = xt(m.x) - g_mcAnchorT;
            double minStart = 1e300;
            for (const auto& o : g_mcOrig) minStart = std::min(minStart, o.second);
            for (const auto& o : g_dragOrig) minStart = std::min(minStart, o.second);
            if ((!g_mcOrig.empty() || !g_dragOrig.empty()) && minStart + dt < 0.0) dt = -minStart;
            for (const auto& o : g_mcOrig)
                if (MidiClip* c = findMidiClip(o.first)) c->start = o.second + dt;   // completely free position
            for (Track& tr : g_proj.tracks)
                for (Clip& c : tr.clips)
                    for (const auto& o : g_dragOrig)
                        if (o.first == c.id) c.start = o.second + dt;
            if (!g_dragOrig.empty()) g_clipAutomationMove.apply(g_proj, dt);
        } else if (g_mcDragMode == 2) {
            dragged->dur = std::max(0.01, clampTimelineTime(xt(m.x) - dragged->start));
        } else if (g_mcDragMode == 3) {
            const double ns = std::min(g_mcOrigEnd - 0.01, std::max(0.0, xt(m.x)));
            const double delta = ns - g_mcOrigStart;
            dragged->start = ns;
            dragged->dur = g_mcOrigEnd - ns;
            dragged->notes.clear();
            for (MidiNote n : g_mcOrigNotes) {   // notes keep their place on the timeline
                n.start -= delta;
                if (n.start + n.dur <= 0.0) continue;
                if (n.start < 0.0) { n.dur += n.start; n.start = 0.0; }
                dragged->notes.push_back(n);
            }
        }
    }

    // ---- marker tracks: double-click adds a note, drag moves / resizes it, right-click edits
    bool markerHandled = false;
    if (inBody && !consumed) {
        const int mti = laneOf(m.y);
        if (mti >= 0 && (g_proj.tracks[(size_t)mti].kind == 1 || (g_proj.tracks[(size_t)mti].kind == 2 && m.y <= baseBottom[(size_t)mti]))) {
            markerHandled = true;
            Track& mt = g_proj.tracks[(size_t)mti];
            const float top = yTop[(size_t)mti] + S(6), bot = baseBottom[(size_t)mti] - S(6);
            int hit = -1;
            if (m.y >= top && m.y <= bot)
                for (int k = (int)mt.marks.size() - 1; k >= 0; --k) {
                    const TrackMark& mk = mt.marks[(size_t)k];
                    if (m.x >= tx(mk.t) && m.x <= std::max(tx(mk.t + mk.dur), tx(mk.t) + S(24))) { hit = k; break; }
                }
            if (hit >= 0 && g_markDrag < 0) {
                const TrackMark& hm = mt.marks[(size_t)hit];
                if (std::max(tx(hm.t + hm.dur), tx(hm.t) + S(24)) - m.x < S(8)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsMouseDoubleClicked(0)) {
                if (hit < 0) {
                    TrackMark nm;
                    nm.t = std::max(0.0, xt(m.x));
                    nm.dur = 4.0;
                    nm.text = mt.kind == 2 ? "" : "Note";
                    nm.scorePos = mt.kind == 2 ? 1 : 0;   // graph track items are made for the score; marker notes only when you choose
                    mt.marks.push_back(nm);
                    hit = (int)mt.marks.size() - 1;
                }
                g_markTrack = mti; g_markIdx = hit; g_selTrack = mti;
                ImGui::OpenPopup("##mark_edit");
            } else if (ImGui::IsMouseClicked(0)) {
                g_selTrack = mti;
                if (hit >= 0) {
                    const TrackMark& hm = mt.marks[(size_t)hit];
                    g_markDrag = hit; g_markDragTrack = mti;
                    g_markDragMode = std::max(tx(hm.t + hm.dur), tx(hm.t) + S(24)) - m.x < S(8) ? 2 : 1;
                    g_markGrab = xt(m.x) - hm.t;
                }
            } else if (ImGui::IsMouseClicked(1) && hit >= 0) {
                g_markTrack = mti; g_markIdx = hit;
                ImGui::OpenPopup("##mark_edit");
            }
        }
    }
    if (g_markDrag >= 0 && g_markDragTrack >= 0 && g_markDragTrack < (int)g_proj.tracks.size()) {
        Track& mt = g_proj.tracks[(size_t)g_markDragTrack];
        if (g_markDrag < (int)mt.marks.size() && ImGui::IsMouseDown(0)) {
            TrackMark& mk = mt.marks[(size_t)g_markDrag];
            if (g_markDragMode == 2) mk.dur = std::max(0.1, xt(m.x) - mk.t);
            else mk.t = std::max(0.0, xt(m.x) - g_markGrab);
        } else g_markDrag = -1;
    } else g_markDrag = -1;
    if (ImGui::BeginPopup("##mark_edit")) {
        if (g_markTrack >= 0 && g_markTrack < nT && g_markIdx >= 0 && g_markIdx < (int)g_proj.tracks[(size_t)g_markTrack].marks.size()) {
            TrackMark& mk = g_proj.tracks[(size_t)g_markTrack].marks[(size_t)g_markIdx];
            const bool graphItem = g_proj.tracks[(size_t)g_markTrack].kind == 2;
            ImGui::TextDisabled(graphItem ? "Graph item (drawn in the score)" : "Marker (timeline, optionally in the score)");
            if (graphItem) {
                // graph: only graphic-score shapes (grain cloud, glissando, swell...). Start and length: drag the item on the timeline.
                comboFit(S(190), "Shape##mark_shape", &mk.shape, markgfx::kShapeNames, markgfx::kShapeCount);
                mk.shape = std::min(markgfx::kShapeCount - 1, std::max(0, mk.shape));
            } else {
                comboFit(S(190), "Type##mark_type", &mk.type, markgfx::kMarkTypeNames, markgfx::kMarkTypeCount);
                mk.type = std::min(markgfx::kMarkTypeCount - 1, std::max(0, mk.type));
                if (mk.type == 2) {
                    ImGui::SetNextItemWidth(S(190));
                    mk.shape = std::min(markgfx::kCueCount - 1, std::max(0, mk.shape));
                    comboFit(0.f, "Q style##mark_cue", &mk.shape, markgfx::kCueNames, markgfx::kCueCount);
                } else if (mk.type == 3) {
                } else if (mk.type == 4) {
                    ImGui::SetNextItemWidth(S(190));
                    mk.shape = std::min(1, std::max(0, mk.shape));
                    comboFit(0.f, "Direction##mark_bracket", &mk.shape, markgfx::kBracketNames, 2);
                }
            }
            {
                const char* poss[] = {"Not in the score", "Above the lane", "Below the lane"};
                comboFit(S(190), "In score##mark_pos", &mk.scorePos, poss, 3);
                if (ImGui::IsItemHovered()) helpTip("Shows this item in the score, above or below its lane. Marker track: the lane that has the same name as the marker track.");
            }
            {   // size: shape / box size in percent, font size of the text
                int sz = mk.size;
                ImGui::SetNextItemWidth(S(190));
                if (ImGui::SliderInt("Size##mark_size", &sz, 40, 400, "%d %%")) mk.size = sz;
                if (ImGui::IsItemHovered()) helpTip(graphItem ? "Size of the shape in the score. The timeline row shows it at most as big as the row (100 %)." : "Size of the box, letter box or cue in the score.");
                {
                    int fnt = mk.font > 0 ? mk.font : 9;
                    ImGui::SetNextItemWidth(S(190));
                    if (ImGui::SliderInt("Font size##mark_font", &fnt, 6, 40, "%d pt")) mk.font = fnt;
                    if (ImGui::IsItemHovered()) helpTip("Size of the text, on the timeline and in the score.");
                }
                if (graphItem) {
                    ImGui::Checkbox("Border in score##mark_border", &mk.border);
                    if (ImGui::IsItemHovered()) helpTip("Draws a thin border around this item in the score. On the timeline the border and a light background are always shown.");
                }
                if (graphItem && mk.shape >= 3 && mk.shape <= 5) {
                    int rt = mk.ratio;
                    ImGui::SetNextItemWidth(S(190));
                    if (ImGui::SliderInt("Ratio##mark_ratio", &rt, 30, 300, "%d %%")) mk.ratio = rt;
                    if (ImGui::IsItemHovered()) helpTip("Curve of the swell / fade. 100 % = straight edges, above 100 % = slow start (curves in), below 100 % = fast start (curves out).");
                }
            }
            if (graphItem) {   // preview of the shape
                const ImVec2 pp = ImGui::GetCursorScreenPos();
                const float pw = S(190), ph = S(44);
                ImGui::Dummy(ImVec2(pw, ph));
                const uint32_t pc = mk.color ? mk.color : g_proj.tracks[(size_t)g_markTrack].color;
                ImDrawList* pdl = ImGui::GetWindowDrawList();
                pdl->AddRectFilled(pp, ImVec2(pp.x + pw, pp.y + ph), IM_COL32(20, 21, 26, 255), S(3));
                drawShapePrims(pdl, mk.shape, pp.x + S(4), pp.y + S(4), pp.x + pw - S(4), pp.y + ph - S(4), trackColor(g_markTrack, 235, pc), trackColor(g_markTrack, 80, pc), mk.ratio / 100.0);
                ImGui::TextDisabled("Text beside the shape (a few letters)");
                ImGui::SetNextItemWidth(S(190));
                inputStr("##mark_gtext", mk.text, 0, nullptr);
            }
            if (!graphItem) {
                const char* what = mk.type == 1 ? "Letters (A, B, C...)" : mk.type == 2 ? "Cue label (1, intro...)" : mk.type == 3 ? "Code" : mk.type == 4 ? "Comment (centred on the bracket)" : "Comment";
                ImGui::TextDisabled("%s", what);
                if (mk.type == 1 || mk.type == 2 || mk.type == 4) {
                    ImGui::SetNextItemWidth(S(190));
                    inputStr("##mark_short", mk.text, 0, nullptr);
                } else if (mk.type == 3) {
                    ImFont* cf = g_codeFonts[0];
                    if (cf) ImGui::PushFont(cf);
                    inputMulti("##mark_text", mk.text, ImVec2(S(320), S(110)));
                    if (cf) ImGui::PopFont();
                } else inputMulti("##mark_text", mk.text, ImVec2(S(280), S(90)));
            }
            float rgb[3] = {0.9f, 0.6f, 0.2f};
            const uint32_t cur = mk.color ? mk.color : g_proj.tracks[(size_t)g_markTrack].color;
            if (cur) { rgb[0] = ((cur >> 16) & 0xFF) / 255.f; rgb[1] = ((cur >> 8) & 0xFF) / 255.f; rgb[2] = (cur & 0xFF) / 255.f; }
            if (ImGui::ColorEdit3("Color##mark_color", rgb, ImGuiColorEditFlags_NoInputs))
                mk.color = ((uint32_t)std::lround(rgb[0] * 255.f) << 16) | ((uint32_t)std::lround(rgb[1] * 255.f) << 8) | (uint32_t)std::lround(rgb[2] * 255.f);
            if (ImGui::Button("Delete note")) {
                g_proj.tracks[(size_t)g_markTrack].marks.erase(g_proj.tracks[(size_t)g_markTrack].marks.begin() + g_markIdx);
                g_markIdx = -1;
                ImGui::CloseCurrentPopup();
            }
        } else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- mouse interaction
    if (inBody && !consumed && !markerHandled) {
        int ti = laneOf(m.y);
        int ci = clipAt(ti, m.x);
        if (ci >= 0 && g_drag == Drag::None) {
            const Clip& hc = g_proj.tracks[(size_t)ti].clips[(size_t)ci];
            if (tx(hc.start + hc.dur) - m.x < S(8) || m.x - tx(hc.start) < S(8))
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsMouseDoubleClicked(0)) {
            if (ci >= 0) {
                g_selTrack = ti; g_selClip = ci;
                g_proj.tracks[(size_t)ti].clips[(size_t)ci].editorOpen = true;  // this clip's own code
            } else if (ti >= 0) {
                // double-click on empty lane space: a drag box-selects, no drag adds a new clip (made on release)
                g_clipMarqueeAdd = io.KeyCtrl || io.KeyShift;
                if (!g_clipMarqueeAdd) { g_selClips.clear(); g_selClip = -1; g_selMidiClips.clear(); }
                g_selTrack = ti;
                g_drag = Drag::Marquee;
                g_clipMarqueeStart = m;
                g_clipMarqueeMoved = false;
                g_marqDblKind = 1;
                g_marqLane = ti;
                g_marqT = xt(m.x);
            }
        } else if (ImGui::IsMouseClicked(1) && ci >= 0) {
            g_selTrack = ti; g_selClip = ci;
            g_ctxClipId = g_proj.tracks[(size_t)ti].clips[(size_t)ci].id;
            if (!g_selClips.count(g_ctxClipId)) {
                g_selClips.clear(); g_selClips.insert(g_ctxClipId); g_selMidiClips.clear();
            }
            ImGui::OpenPopup("##clip_context");
        } else if (ImGui::IsMouseClicked(0) && ci >= 0 &&
                   tx(g_proj.tracks[(size_t)ti].clips[(size_t)ci].start + g_proj.tracks[(size_t)ti].clips[(size_t)ci].dur) -
                       tx(g_proj.tracks[(size_t)ti].clips[(size_t)ci].start) > S(46) &&
                   m.x >= tx(g_proj.tracks[(size_t)ti].clips[(size_t)ci].start + g_proj.tracks[(size_t)ti].clips[(size_t)ci].dur) - S(21) &&
                   m.x <= tx(g_proj.tracks[(size_t)ti].clips[(size_t)ci].start + g_proj.tracks[(size_t)ti].clips[(size_t)ci].dur) - S(5) &&
                   m.y >= yTop[(size_t)ti] + S(5) + S(3) && m.y <= yTop[(size_t)ti] + S(5) + S(19)) {
            Clip& mc = g_proj.tracks[(size_t)ti].clips[(size_t)ci];
            mc.muted = !mc.muted;
        } else if (ImGui::IsMouseClicked(0)) {
            g_autoSel.clear(); g_autoSelMulti.clear();
            if (!(io.KeyCtrl || io.KeyShift) && !(ci >= 0 && g_selClips.count(g_proj.tracks[(size_t)ti].clips[(size_t)ci].id)))
                g_selMidiClips.clear();   // grabbing an already selected clip keeps the MIDI selection so both move together
            g_marqDblKind = 0;
            if (ci >= 0) {
                Clip& c = g_proj.tracks[(size_t)ti].clips[(size_t)ci];
                const bool additive = io.KeyCtrl || io.KeyShift;
                bool picked = true;
                if (additive) {
                    if (g_selClips.count(c.id)) { g_selClips.erase(c.id); picked = false; }
                    else g_selClips.insert(c.id);
                } else if (!g_selClips.count(c.id)) {
                    g_selClips.clear();
                    g_selClips.insert(c.id);
                }
                g_selTrack = ti;
                g_selClip = ci;
                if (!picked) setPrimaryFromSet();
                else if (tx(c.start + c.dur) - m.x < S(8)) {
                    g_selClips.clear();
                    g_selClips.insert(c.id);
                    g_drag = Drag::Resize;
                } else if (m.x - tx(c.start) < S(8)) {
                    g_selClips.clear();
                    g_selClips.insert(c.id);
                    g_drag = Drag::ResizeLeft;
                    g_trimStart = c.start;
                    g_trimEnd = c.start + c.dur;
                } else {
                    g_drag = Drag::Move;
                    g_dragAnchorT = xt(m.x);
                    g_dragOrig.clear();
                    for (const Track& tr : g_proj.tracks)
                        for (const Clip& sc : tr.clips)
                            if (g_selClips.count(sc.id)) g_dragOrig.push_back({sc.id, sc.start});
                    g_clipAutomationMove.capture(g_proj, g_selClips);
                    g_dragMidiOrig.clear();
                    for (const Track& tr : g_proj.tracks)
                        for (const MidiClip& mc2 : tr.midiClips)
                            if (g_selMidiClips.count(mc2.id)) g_dragMidiOrig.push_back({mc2.id, mc2.start});
                }
            } else {
                // empty lane space: drag = box-select clips (Ctrl/Shift adds to the selection)
                g_clipMarqueeAdd = io.KeyCtrl || io.KeyShift;
                if (!g_clipMarqueeAdd) { g_selClips.clear(); g_selClip = -1; }
                g_selTrack = ti;
                g_drag = Drag::Marquee;
                g_clipMarqueeStart = m;
                g_clipMarqueeMoved = false;
            }
        }
    }
    if (inRuler && !consumed && ImGui::IsMouseClicked(0)) {
        g_drag = Drag::Playhead;
    }
    if (inRuler && !consumed && ImGui::IsMouseClicked(1)) {
        const double at = clampTimelineTime(xt(m.x));
        if (!g_loopSelecting) {
            g_loopAnchor = at;
            g_loopSelecting = true;
            g_proj.loopStart = g_proj.loopEnd = at;
            g_proj.loopEnabled = false;
        } else if (std::fabs(at - g_loopAnchor) >= 0.01) {
            g_proj.loopStart = std::min(g_loopAnchor, at);
            g_proj.loopEnd = std::max(g_loopAnchor, at);
            g_proj.loopEnabled = true;
            g_loopSelecting = false;
        }
        if (g_timelineRun) g_resync = 2;
        if (g_timelineRun && g_pdTimelineMode) pdTransport(true);
    }
    if (inRuler && !consumed && g_drag == Drag::None)
        helpTip(g_loopSelecting ? "Right-click the ruler to set the other loop boundary." :
                "Right-click twice to set a loop. Left-click to move the playhead.");

    if (g_drag != Drag::None) {
        if (!ImGui::IsMouseDown(0)) {
            if (g_drag == Drag::Marquee && g_clipMarqueeMoved) {
                const ImVec2 m(std::min(bodyMax.x, std::max(bodyMin.x, ImGui::GetIO().MousePos.x)),
                               std::min(bodyMax.y, std::max(bodyMin.y, ImGui::GetIO().MousePos.y)));   // same box wherever the mouse is released
                const float x0 = std::min(m.x, g_clipMarqueeStart.x), x1 = std::max(m.x, g_clipMarqueeStart.x);
                const float y0 = std::min(m.y, g_clipMarqueeStart.y), y1 = std::max(m.y, g_clipMarqueeStart.y);
                const float pad = S(5);
                for (int ti2 = 0; ti2 < nT; ++ti2)
                    for (const Clip& c : g_proj.tracks[(size_t)ti2].clips) {
                        const float cx0 = tx(c.start), cx1 = tx(c.start + c.dur);
                        const float cy0 = yTop[(size_t)ti2] + pad, cy1 = baseBottom[(size_t)ti2] - pad;
                        if (cx1 >= x0 && cx0 <= x1 && cy1 >= y0 && cy0 <= y1) g_selClips.insert(c.id);
                    }
                for (int ti2 = 0; ti2 < nT; ++ti2) {   // MIDI regions inside the same box
                    const Track& tr2 = g_proj.tracks[(size_t)ti2];
                    if (!midiShown(tr2)) continue;
                    const float lt = midiLaneTop(tr2, baseBottom[(size_t)ti2]);
                    if (lt + S(kMidiLaneH) < y0 || lt > y1) continue;
                    for (const MidiClip& mc : tr2.midiClips)
                        if (tx(mc.start + mc.dur) >= x0 && tx(mc.start) <= x1) g_selMidiClips.insert(mc.id);
                }
                {   // automation dots of visible lanes inside the box (the lane holding the most dots becomes the dot selection)
                    int bestTrack = -1;
                    std::string bestChannel;
                    std::set<int> bestSel;
                    std::map<std::pair<int, std::string>, std::set<int>> foundLanes;
                    for (int ti2 = 0; ti2 < nT; ++ti2) {
                        const Track& tr2 = g_proj.tracks[(size_t)ti2];
                        for (const auto& [channel, points] : tr2.automation) {
                            if (!autoShown(tr2, channel)) continue;
                            float ly0 = 0.f, ly1 = 0.f;
                            autoLaneGeometry(tr2, ti2, channel, ly0, ly1);
                            if (ly1 < y0 || ly0 > y1) continue;
                            double lo, hi, initial;
                            laneValueRange(tr2, channel, lo, hi, initial);
                            std::set<int> found;
                            for (int pi = 0; pi < (int)points.size(); ++pi) {
                                const float px = tx(points[(size_t)pi].t);
                                const float py = yOfValue(points[(size_t)pi].v, ly0, ly1, lo, hi);
                                if (px >= x0 && px <= x1 && py >= y0 - S(4) && py <= y1 + S(4)) found.insert(pi);
                            }
                            if (!found.empty()) foundLanes[{tr2.id, channel}] = found;
                            if (found.size() > bestSel.size()) { bestSel = found; bestTrack = ti2; bestChannel = channel; }
                        }
                    }
                    if (!g_clipMarqueeAdd) clearAutoSelection();
                    if (bestTrack >= 0) {
                        const int bestId = g_proj.tracks[(size_t)bestTrack].id;
                        if (g_autoSelTrackId != bestId || g_autoSelChannel != bestChannel) { g_autoSel.clear(); g_autoSelMulti.clear(); g_autoSelTrackId = bestId; g_autoSelChannel = bestChannel; }
                        g_autoSel.insert(bestSel.begin(), bestSel.end());
                        for (const auto& [key, idxs] : foundLanes)   // the other lanes keep their dots too, so Del removes all of them
                            if (!(key.first == bestId && key.second == bestChannel)) g_autoSelMulti[key].insert(idxs.begin(), idxs.end());
                    }
                }
                setPrimaryFromSet();
            } else if (g_drag == Drag::Marquee && g_marqDblKind != 0 && g_marqLane >= 0 && g_marqLane < nT) {
                Track& lane = g_proj.tracks[(size_t)g_marqLane];
                if (g_marqDblKind == 1 && hasClips(lane)) {
                    // new clip = new, independent instrument (own name, code copy, color, MIDI)
                    std::string code = lane.code;
                    if (g_selTrack == g_marqLane && g_selClip >= 0 && g_selClip < (int)lane.clips.size()) code = lane.clips[(size_t)g_selClip].code;
                    lane.clips.push_back(makeClip(lane, g_marqT, 2.0, code));
                    g_selTrack = g_marqLane; g_selClip = (int)lane.clips.size() - 1;
                    g_selClips.clear();
                    g_selClips.insert(lane.clips.back().id);
                } else {
                    // new MIDI clip for the selected (or first) MIDI-capable Csound clip of this lane
                    int owner = 0;
                    if (g_selTrack == g_marqLane && g_selClip >= 0 && g_selClip < (int)lane.clips.size() &&
                        parseClip(lane.clips[(size_t)g_selClip]).midiEnabled) owner = lane.clips[(size_t)g_selClip].id;
                    for (const Clip& c : lane.clips) if (!owner && parseClip(c).midiEnabled) owner = c.id;
                    if (owner) {
                        MidiClip mc;
                        mc.id = newMidiId();
                        mc.clipId = owner;
                        if (const Clip* target = findClip(owner)) mc.name = target->name;
                        mc.start = g_marqT;
                        mc.dur = 2.0;
                        lane.midiClips.push_back(mc);
                        g_selMidiClips.clear();
                        g_selMidiClips.insert(mc.id);
                    }
                }
            }
            if (g_drag == Drag::Playhead && g_timelineRun && g_pdTimelineMode) pdTransport(true);
            if (g_drag == Drag::Marquee) g_marqDblKind = 0;
            g_drag = Drag::None;
        } else if (g_drag == Drag::Playhead) {
            g_play = std::max(0.0, xt(m.x));
        } else if (g_drag == Drag::ResizeTrack) {
            if (g_resizeTrack >= 0 && g_resizeTrack < nT) {
                float h = (g_resizeStartH + (m.y - g_resizeStartY)) / g_ui;
                g_proj.tracks[(size_t)g_resizeTrack].height = std::min(kMaxTrackH, std::max(kMinTrackH, h));
            }
        } else if (g_drag == Drag::ResizeAutomation) {
            if (g_resizeTrack >= 0 && g_resizeTrack < nT) {
                Track& track = g_proj.tracks[(size_t)g_resizeTrack];
                float h = (g_resizeStartH + (m.y - g_resizeStartY)) / g_ui;
                track.automationHeights[g_resizeAutomationChannel] = std::min(320.f, std::max(28.f, h));
            }
        } else if (g_drag == Drag::Marquee) {
            if (std::fabs(m.x - g_clipMarqueeStart.x) + std::fabs(m.y - g_clipMarqueeStart.y) > S(3)) g_clipMarqueeMoved = true;
        } else if (g_drag == Drag::ResizeHeader || g_drag == Drag::ScrollX || g_drag == Drag::ScrollY) {
            // handled above
        } else if (g_drag == Drag::Move) {
            double dt = xt(m.x) - g_dragAnchorT;
            double minStart = 1e300;
            for (const auto& o : g_dragOrig) minStart = std::min(minStart, o.second);
            for (const auto& o : g_dragMidiOrig) minStart = std::min(minStart, o.second);
            if ((!g_dragOrig.empty() || !g_dragMidiOrig.empty()) && minStart + dt < 0.0) dt = -minStart;
            for (Track& tr : g_proj.tracks)
                for (Clip& c : tr.clips)
                    for (const auto& o : g_dragOrig)
                        if (o.first == c.id) c.start = o.second + dt;
            for (const auto& o : g_dragMidiOrig)
                if (MidiClip* mcv = findMidiClip(o.first)) mcv->start = o.second + dt;
            if (g_dragOrig.size() == 1 && g_dragMidiOrig.empty() && g_selTrack >= 0 && g_selTrack < nT && g_selClip >= 0 &&
                g_selClip < (int)g_proj.tracks[(size_t)g_selTrack].clips.size()) {
                int nt = laneOf(m.y);
                if (nt >= 0 && nt != g_selTrack && hasClips(g_proj.tracks[(size_t)nt]) &&
                    ((g_proj.tracks[(size_t)nt].kind == 6) == (g_proj.tracks[(size_t)g_selTrack].kind == 6)) &&
                    ((g_proj.tracks[(size_t)nt].kind == 5) == (g_proj.tracks[(size_t)g_selTrack].kind == 5))) {
                    auto& from = g_proj.tracks[(size_t)g_selTrack].clips;
                    Clip tmp = from[(size_t)g_selClip];
                    from.erase(from.begin() + g_selClip);
                    {
                        auto& fromMidi = g_proj.tracks[(size_t)g_selTrack].midiClips;
                        auto& toMidi = g_proj.tracks[(size_t)nt].midiClips;
                        for (size_t k = 0; k < fromMidi.size();) {
                            if (fromMidi[k].clipId == tmp.id) { toMidi.push_back(fromMidi[k]); fromMidi.erase(fromMidi.begin() + (long)k); }
                            else ++k;
                        }
                    }
                    g_proj.tracks[(size_t)nt].clips.push_back(tmp);
                    g_selTrack = nt;
                    g_selClip = (int)g_proj.tracks[(size_t)nt].clips.size() - 1;
                }
            }
            if (!g_dragOrig.empty()) g_clipAutomationMove.apply(g_proj, dt);
        } else if (g_drag == Drag::ResizeLeft && g_selTrack >= 0 && g_selTrack < nT && g_selClip >= 0 &&
                   g_selClip < (int)g_proj.tracks[(size_t)g_selTrack].clips.size()) {
            Clip& c = g_proj.tracks[(size_t)g_selTrack].clips[(size_t)g_selClip];
            const double ns = std::min(g_trimEnd - 0.001, std::max(0.0, xt(m.x)));
            const double delta = ns - g_trimStart;
            c.start = ns;
            c.dur = g_trimEnd - ns;
            (void)delta;
        } else if (g_drag == Drag::Resize && g_selTrack >= 0 && g_selTrack < nT && g_selClip >= 0 &&
                   g_selClip < (int)g_proj.tracks[(size_t)g_selTrack].clips.size()) {
            Clip& c = g_proj.tracks[(size_t)g_selTrack].clips[(size_t)g_selClip];
            c.dur = std::max(0.001, clampTimelineTime(xt(m.x) - c.start));
        }
    }

    if (arrangeKeys && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        // one Del removes everything that is selected, on every lane: MIDI regions AND Csound clips (the dots went above)
        if (!g_selMidiClips.empty()) deleteSelectedMidiClips();
        if (!g_selClips.empty()) deleteSelectedClips();
    }
    if (arrangeKeys && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && !g_selMidiClips.empty()) {
        // Enter: the selected MIDI clips open together in ONE piano roll (a single one opens its own piano roll)
        if (g_selMidiClips.size() == 1) { if (MidiClip* one = findMidiClip(*g_selMidiClips.begin())) one->editorOpen = true; }
        else { g_multiMidiIds.assign(g_selMidiClips.begin(), g_selMidiClips.end()); g_multiMidiOpen = true; }
    }
    if (arrangeKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !g_selClips.empty())
        duplicateSelectedClips();

    layout();  // heights may have changed above

    // ---- background
    dl->AddRectFilled(org, bodyMax, IM_COL32(22, 22, 25, 255));

    // ---- ruler
    dl->AddRectFilled(ImVec2(org.x, org.y), ImVec2(bodyMin.x, bodyMin.y), IM_COL32(33, 33, 37, 255));
    dl->AddRectFilled(rulerMin, rulerMax, IM_COL32(30, 30, 34, 255));

    static const double steps[] = {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800};
    double major = 3600;
    for (double s : steps) if (s * g_pps >= 70.0) { major = s; break; }
    const double minor = major / 4.0;
    const double t0 = g_scrollX, t1 = xt(bodyMax.x);

    ImGui::PushClipRect(ImVec2(rulerMin.x, org.y), bodyMax, true);
    if (g_loopSelecting) {
        const float x = tx(g_loopAnchor);
        dl->AddLine(ImVec2(x, rulerMin.y), ImVec2(x, rulerMax.y), IM_COL32(250, 165, 50, 255), S(2));
    } else if (g_proj.loopEnd - g_proj.loopStart >= 0.01) {
        const float a = tx(g_proj.loopStart), b = tx(g_proj.loopEnd);
        const ImU32 tint = g_proj.loopEnabled ? IM_COL32(240, 146, 45, 115) : IM_COL32(100, 100, 105, 90);
        dl->AddRectFilled(ImVec2(a, rulerMin.y), ImVec2(b, rulerMax.y), tint);
        dl->AddLine(ImVec2(a, rulerMin.y), ImVec2(a, rulerMax.y), IM_COL32(250, 165, 50, 255), S(2));
        dl->AddLine(ImVec2(b, rulerMin.y), ImVec2(b, rulerMax.y), IM_COL32(250, 165, 50, 255), S(2));
    }
    for (long long k = (long long)std::floor(t0 / minor); k * minor <= t1; ++k) {
        double t = k * minor;
        if (t < 0.0) continue;
        float x = tx(t);
        bool isMajor = (k % 4 == 0);
        dl->AddLine(ImVec2(x, isMajor ? org.y + S(8) : org.y + S(17)), ImVec2(x, bodyMin.y),
                    IM_COL32(90, 90, 98, 255));
        dl->AddLine(ImVec2(x, bodyMin.y), ImVec2(x, bodyMax.y),
                    isMajor ? IM_COL32(52, 52, 58, 255) : IM_COL32(36, 36, 41, 255));
        if (isMajor) {
            char buf[24];
            if (major >= 1.0) std::snprintf(buf, sizeof buf, "%d:%02d", (int)t / 60, (int)t % 60);
            else std::snprintf(buf, sizeof buf, major < 0.1 ? "%.3fs" : "%.2fs", t);
            dl->AddText(ImVec2(x + S(4), org.y + S(4)), IM_COL32(170, 170, 178, 255), buf);
        }
    }
    ImGui::PopClipRect();

    // Compact zoom lives next to the timeline ruler, not in the transport.
    dl->AddRectFilled(ImVec2(org.x + S(4), org.y + S(2)),
                      ImVec2(bodyMin.x - S(4), bodyMin.y - S(2)), IM_COL32(48, 52, 58, 255), S(4));
    ImGui::SetCursorScreenPos(ImVec2(org.x + S(8), org.y + S(5)));
    ImGui::TextDisabled("-");
    ImGui::SameLine(0, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.15f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.22f, 0.24f, 1.f));
    ImGui::SetNextItemWidth(std::max(S(45), headerW - S(48)));
    if (ImGui::SliderFloat("##timeline_zoom", &g_pps, kPpsMin, kPpsMax, "",
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput)) {
        g_scrollX = zoomAroundPlayhead(g_pps, g_play, std::max(1.f, bodyMax.x - bodyMin.x));
    }
    if (ImGui::IsItemHovered()) helpTip("Zoom around the playhead: left zooms out, right zooms in. Ctrl+mouse wheel also centres on the playhead.");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::SameLine(0, S(4));
    ImGui::TextDisabled("+");

    // ---- lanes + clips
    ImGui::PushClipRect(bodyMin, bodyMax, true);
    for (int i = 0; i < nT; ++i) {
        if (folderHdrH[(size_t)i] > 0.f) {   // folder header strip across the timeline
            const float hy1 = yTop[(size_t)i], hy0 = hy1 - folderHdrH[(size_t)i];
            if (hy1 >= bodyMin.y && hy0 <= bodyMax.y) {
                const TrackFolder* bf = findFolder(g_proj.tracks[(size_t)i].folderId);
                const uint32_t bc = bf && bf->color ? bf->color : 0x5A6EA0u;
                dl->AddRectFilled(ImVec2(bodyMin.x, hy0), ImVec2(bodyMax.x, hy1), IM_COL32(44, 46, 56, 255));
                dl->AddRectFilled(ImVec2(bodyMin.x, hy1 - S(3)), ImVec2(bodyMax.x, hy1), IM_COL32((bc >> 16) & 0xFF, (bc >> 8) & 0xFF, bc & 0xFF, 255));
                dl->AddLine(ImVec2(bodyMin.x, hy1), ImVec2(bodyMax.x, hy1), IM_COL32(12, 12, 14, 255));
            }
        }
        if (hiddenTrack(i)) continue;
        float y0 = yTop[(size_t)i], y1 = baseBottom[(size_t)i];
        if (y1 < bodyMin.y || y0 > bodyMax.y) continue;
        Track& t = g_proj.tracks[(size_t)i];
        dl->AddRectFilled(ImVec2(bodyMin.x, y0), ImVec2(bodyMax.x, y1), trackColor(i, 24, t.color));
        dl->AddLine(ImVec2(bodyMin.x, y1), ImVec2(bodyMax.x, y1), IM_COL32(12, 12, 14, 255));
        const float pad = S(5);
        for (int k = 0; k < (int)t.clips.size(); ++k) {
            const Clip& c = t.clips[(size_t)k];
            float x0 = tx(c.start), x1 = tx(c.start + c.dur);
            if (x1 < bodyMin.x || x0 > bodyMax.x) continue;
            bool sel = g_selClips.count(c.id) > 0;
            ImVec2 a(x0, y0 + pad), b(x1, y1 - pad);
            const uint32_t clipRgb = clipColorValue(t, c);
            dl->AddRectFilled(a, b, trackColor(i, sel ? 255 : 205, clipRgb), S(4));
            dl->AddRectFilled(a, ImVec2(b.x, std::min(b.y, a.y + S(20))), IM_COL32(0, 0, 0, 70), S(4),
                              ImDrawFlags_RoundCornersTop);
            dl->AddRect(a, b, sel ? IM_COL32(255, 255, 255, 235) : IM_COL32(0, 0, 0, 120), S(4), 0,
                        S(sel ? 1.8f : 1.f));
            dl->PushClipRect(ImVec2(a.x + 2, a.y), ImVec2(b.x - 2, b.y), true);
            const std::string clipLabel = c.runs > 1 ? c.name + "  x" + std::to_string(c.runs) : c.name;
            dl->AddText(ImVec2(a.x + S(7), a.y + S(3)), IM_COL32(255, 255, 255, 240), clipLabel.c_str());
            {
                std::string desc = clipDescription(c);
                char info[96];
                if (desc.empty()) {
                    std::snprintf(info, sizeof info, "%.2f s - %.2f s", c.start, c.start + c.dur);
                    desc = info;
                }
                dl->AddText(ImVec2(a.x + S(7), a.y + S(23)), IM_COL32(255, 255, 255, 170), desc.c_str());
            }
            if (c.muted) dl->AddRectFilled(a, b, IM_COL32(10, 10, 12, 150), S(4));
            dl->PopClipRect();
            if (x1 - x0 > S(46)) {   // small M button: mute this clip
                const ImVec2 ma(b.x - S(21), a.y + S(3)), mb(b.x - S(5), a.y + S(19));
                dl->AddRectFilled(ma, mb, c.muted ? IM_COL32(190, 60, 52, 255) : IM_COL32(0, 0, 0, 110), S(3));
                dl->AddText(ImVec2(ma.x + S(3), ma.y + S(1)), IM_COL32(255, 255, 255, c.muted ? 255 : 170), "M");
            }
        }
    }
    for (int i = 0; i < nT; ++i) {
        if (hiddenTrack(i)) continue;
        Track& track = g_proj.tracks[(size_t)i];
        float laneY = baseBottom[(size_t)i];
        ParsedTrack parsed;
        bool parsedReady = false;
        if (track.kind == 1 || track.kind == 2) {   // marker track: comment boxes; graph track: the same items in its first row
            const float top = yTop[(size_t)i] + S(6), bot = baseBottom[(size_t)i] - S(6);
            if (baseBottom[(size_t)i] >= bodyMin.y && yTop[(size_t)i] <= bodyMax.y) {
                for (size_t k = 0; k < track.marks.size(); ++k) {
                    const TrackMark& mk = track.marks[k];
                    const float x0 = tx(mk.t), x1 = std::max(tx(mk.t + mk.dur), x0 + S(24));
                    if (x1 < bodyMin.x || x0 > bodyMax.x) continue;
                    const uint32_t rgb = mk.color ? mk.color : track.color;
                    drawMarkItem(dl, i, track, mk, x0, x1, top, bot, rgb);
                }
            }
            if (track.kind == 1) continue;
        }
        int automationLaneIndex = 0;
        for (const auto& [channel, points] : track.automation) {
            if (!autoShown(track, channel)) continue;
            const float laneY1 = laneY + S(automationHeight(track, channel));
            if (laneY1 < bodyMin.y || laneY > bodyMax.y) { laneY = laneY1; continue; }
            if (!parsedReady) { parsed = pdParsedForUi(track); parsedReady = true; }
            const ImVec2 a(bodyMin.x, laneY), b(bodyMax.x, laneY1);
            dl->AddRectFilled(a, b, IM_COL32(18, 19, 22, 255));
            dl->AddLine(ImVec2(a.x, laneY1), ImVec2(b.x, laneY1), IM_COL32(12, 12, 14, 255));
            for (int k = 1; k < 4; ++k) {
                const float y = laneY + (laneY1 - laneY) * k / 4.f;
                dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(34, 35, 40, 255));
            }
            const CawParam* param = nullptr;
            for (const CawParam& p : parsed.params) if (p.chn == channel) { param = &p; break; }
            const double lo = param ? param->min : 0.0;
            const double hi = param && param->max > lo ? param->max : lo + 1.0;
            const double initial = param ? param->value : (lo + hi) * 0.5;
            const float padY = S(7), usable = std::max(1.f, laneY1 - laneY - 2.f * padY);
            auto valueY = [&](double value) {
                const double norm = (std::min(hi, std::max(lo, value)) - lo) / (hi - lo);
                return laneY1 - padY - (float)norm * usable;
            };
            const uint32_t laneColor = automationColor(track, channel, automationLaneIndex++);
            const bool selectedLane = track.id == g_autoSelTrackId && channel == g_autoSelChannel;
            if (points.empty()) {
                const float y = valueY(initial);
                dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), trackColor(i, 200, laneColor), S(1.5f));
            } else {
                std::vector<ImVec2> poly;
                poly.reserve(points.size());
                for (const AutoPoint& point : points) poly.push_back(ImVec2(tx(point.t), valueY(point.v)));
                if (poly.size() == 1) {
                    dl->AddLine(ImVec2(a.x, poly[0].y), ImVec2(b.x, poly[0].y), trackColor(i, 255, laneColor), S(2));
                } else {
                    dl->AddPolyline(poly.data(), (int)poly.size(), trackColor(i, 255, laneColor), 0, S(2));
                    if (poly.front().x > a.x) dl->AddLine(ImVec2(a.x, poly.front().y), poly.front(), trackColor(i, 160, laneColor), S(1));
                    if (poly.back().x < b.x) dl->AddLine(poly.back(), ImVec2(b.x, poly.back().y), trackColor(i, 160, laneColor), S(1));
                }
                for (int pi = 0; pi < (int)poly.size(); ++pi) {
                    const ImVec2& point = poly[(size_t)pi];
                    bool picked = selectedLane && g_autoSel.count(pi) > 0;
                    if (!picked && !g_autoSelMulti.empty()) {
                        const auto multi = g_autoSelMulti.find({track.id, channel});
                        picked = multi != g_autoSelMulti.end() && multi->second.count(pi) > 0;
                    }
                    const bool denseHere = pi > 0 && pi + 1 < (int)poly.size() && std::fabs(point.x - poly[(size_t)pi - 1].x) < S(9) && std::fabs(poly[(size_t)pi + 1].x - point.x) < S(9);
                    if (picked && denseHere) {
                        dl->AddCircleFilled(point, S(2.f), IM_COL32(255, 255, 255, 255));   // many selected dots (a wave): small, so the line stays readable
                    } else if (picked) {
                        dl->AddCircleFilled(point, S(4), IM_COL32(255, 255, 255, 255));
                        dl->AddCircle(point, S(5.5f), trackColor(i, 255, laneColor), 0, S(1.5f));
                    } else {
                        // pen strokes read as a plain line: dots packed close together on both sides are not drawn
                        // (a lone dot, or one at the end of a stroke, is still shown and can be grabbed)
                        const float packed = S(9);
                        const bool denseL = pi > 0 && std::fabs(point.x - poly[(size_t)pi - 1].x) < packed;
                        const bool denseR = pi + 1 < (int)poly.size() && std::fabs(poly[(size_t)pi + 1].x - point.x) < packed;
                        if (denseL && denseR) continue;
                        if (points[(size_t)pi].line) continue;   // inside a Pen 2 stroke: only the start and the end of the stroke have dots
                        dl->AddCircleFilled(point, S(2.8f), trackColor(i, 255, laneColor));
                        dl->AddCircle(point, S(4), IM_COL32(255, 255, 255, 150), 0, S(1));
                    }
                }
            }
            if (g_wavePrev.frame >= ImGui::GetFrameCount() - 2 && g_wavePrev.trackId == track.id && g_wavePrev.channel == channel && g_wavePrev.pts.size() >= 2) {
                std::vector<ImVec2> pv;   // ghost of the wave that Wave... would write
                for (const AutoPoint& q : g_wavePrev.pts) pv.push_back(ImVec2(tx(q.t), valueY(q.v)));
                dl->AddPolyline(pv.data(), (int)pv.size(), IM_COL32(255, 255, 255, 190), 0, S(1.5f));
            }
            if (selectedLane && g_autoSel.size() >= 2 && !points.empty()) {   // drag handles of the selected dots
                double tMin, tMax, vMin, vMax;
                if (autoSelBounds(points, g_autoSel, tMin, tMax, vMin, vMax)) {
                    const float hx0 = tx(tMin), hx1 = tx(tMax);
                    const WaveHandles wh = waveHandlePos(hx0, hx1, valueY(vMax), valueY(vMin), valueY((vMin + vMax) * 0.5), laneY, laneY1, bodyMax.x);
                    dl->AddRect(ImVec2(hx0 - S(3), valueY(vMax) - S(3)), ImVec2(hx1 + S(3), valueY(vMin) + S(3)), IM_COL32(255, 255, 255, 60), S(2));
                    drawWaveHandles(dl, wh, g_wh.active ? g_wh.kind : 0);
                }
            }
            laneY = laneY1;
        }
        if (!midiShown(track)) continue;
        const float midiY = midiLaneTop(track, baseBottom[(size_t)i]);
        const float midiBottom = midiY + S(kMidiLaneH);
        const ImVec2 midiA(bodyMin.x, midiY), midiB(bodyMax.x, midiBottom);
        dl->AddRectFilled(midiA, midiB, IM_COL32(25, 26, 30, 255));
        dl->AddLine(ImVec2(midiA.x, midiBottom), ImVec2(midiB.x, midiBottom), IM_COL32(12, 12, 14, 255));
        for (int k = 0; k < 4; ++k) {
            const double t = std::floor(g_scrollX / 1.0) + k;
            const float x = tx(t);
            dl->AddLine(ImVec2(x, midiY), ImVec2(x, midiBottom), IM_COL32(40, 41, 46, 255));
        }
        // MIDI clips: free regions, colored like the Csound clip they play
        bool anyNotes = false;
        const float padY = S(4);
        const float usableH = std::max(1.f, midiBottom - midiY - 2.f * padY);
        const int miniPitchLow = g_midiPitchLow;
        const int miniPitchHigh = miniPitchLow + kMidiVisiblePitches - 1;
        const float noteStep = usableH / (float)kMidiVisiblePitches;
        for (const MidiClip& mc : track.midiClips) {
            const Clip* tgt = findClip(mc.clipId);
            const uint32_t clipRgb = tgt ? clipColorValue(track, *tgt) : 0u;
            const float cx0 = tx(mc.start), cx1 = tx(mc.start + mc.dur);
            if (cx1 < bodyMin.x || cx0 > bodyMax.x) continue;
            const bool selectedMidi = g_selMidiClips.count(mc.id) > 0;
            dl->AddRectFilled(ImVec2(cx0, midiY + S(2)), ImVec2(cx1, midiBottom - S(2)), trackColor(i, selectedMidi ? 70 : 34, clipRgb), S(3));
            dl->AddRect(ImVec2(cx0, midiY + S(2)), ImVec2(cx1, midiBottom - S(2)),
                        selectedMidi ? IM_COL32(237, 128, 38, 255) : trackColor(i, 90, clipRgb), S(3), 0, selectedMidi ? S(2) : S(1));
            if (cx1 - cx0 > S(70)) {
                const std::string label = mc.name.empty() ? (tgt ? tgt->name : "?") : mc.name;
                dl->PushClipRect(ImVec2(cx0 + S(2), midiY + S(2)), ImVec2(cx1 - S(23), midiBottom - S(2)), true);
                dl->AddText(ImVec2(cx0 + S(5), midiY + S(4)), IM_COL32(190, 192, 198, 255), label.c_str());
                dl->PopClipRect();
            }
            for (const MidiNote& note : mc.notes) {
                if (note.start >= mc.dur) continue;
                anyNotes = true;
                const float x0 = tx(mc.start + note.start);
                const float x1 = std::min(cx1, tx(mc.start + note.start + note.dur));
                if (x1 < bodyMin.x || x0 > bodyMax.x || note.pitch < miniPitchLow || note.pitch > miniPitchHigh) continue;
                const float y0 = midiBottom - padY - (note.pitch - miniPitchLow + 1) * noteStep;
                const ImVec2 a(x0, y0 + S(1)), b(std::max(x0 + S(3), x1), y0 + noteStep - S(1));
                dl->AddRectFilled(a, b, trackColor(i, 230, clipRgb), S(2));
            }
            if (mc.muted)
                dl->AddRectFilled(ImVec2(cx0, midiY + S(2)), ImVec2(cx1, midiBottom - S(2)), IM_COL32(10, 10, 12, 150), S(3));
            if (cx1 - cx0 > S(46)) {
                const ImVec2 ma(cx1 - S(21), midiY + S(5)), mb(cx1 - S(5), midiY + S(21));
                dl->AddRectFilled(ma, mb, mc.muted ? IM_COL32(190, 60, 52, 255) : IM_COL32(0, 0, 0, 110), S(3));
                dl->AddText(ImVec2(ma.x + S(3), ma.y + S(1)), IM_COL32(255, 255, 255, mc.muted ? 255 : 170), "M");
            }
        }
    }
    {   // selection boxes go on top of every lane (clips, automation, MIDI)
        auto box = [&](ImVec2 a, ImVec2 b) {
            const ImVec2 q0(std::min(a.x, b.x), std::min(a.y, b.y)), q1(std::max(a.x, b.x), std::max(a.y, b.y));
            dl->AddRectFilled(q0, q1, IM_COL32(237, 128, 38, 50));
            dl->AddRect(q0, q1, IM_COL32(237, 128, 38, 230), 0.f, 0, S(1.5f));
        };
        if (g_drag == Drag::Marquee && g_clipMarqueeMoved) box(g_clipMarqueeStart, m);
        if (g_autoMode == AutoMode::Marquee && g_autoDragMoved) box(g_autoDragStartMouse, m);
    }
    ImGui::PopClipRect();

    // ---- instrument headers and matching automation-lane labels
    ImGui::PushClipRect(hdrMin, hdrMax, true);
    dl->AddRectFilled(hdrMin, hdrMax, IM_COL32(33, 33, 37, 255));
    int del = -1;
    int moveFrom = -1, moveTo = -1;
    int folderToggle = 0;
    int ctxFolderOpen = 0;
    for (int i = 0; i < nT; ++i) {
        if (folderHdrH[(size_t)i] > 0.f) {   // folder group header: arrow + name (click = hide / show its tracks, right-click = menu)
            TrackFolder* fo = findFolder(g_proj.tracks[(size_t)i].folderId);
            const float hy1 = yTop[(size_t)i], hy0 = hy1 - folderHdrH[(size_t)i];
            if (fo && hy1 >= bodyMin.y && hy0 <= bodyMax.y) {
                const ImVec2 a(org.x, hy0), b(bodyMin.x, hy1);
                const uint32_t fc = fo->color ? fo->color : 0x5A6EA0u;
                const ImU32 fcol = IM_COL32((fc >> 16) & 0xFF, (fc >> 8) & 0xFF, fc & 0xFF, 255);
                dl->AddRectFilled(a, b, IM_COL32(44, 46, 56, 255));
                dl->AddRectFilled(a, ImVec2(a.x + S(5), b.y), fcol);
                dl->AddRectFilled(ImVec2(a.x + S(5), b.y - S(3)), b, fcol);
                dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), IM_COL32(12, 12, 14, 255));
                const float cx = a.x + S(14), cy = (a.y + b.y) * 0.5f, rr = S(4.5f);
                drawChevron(dl, cx, cy, rr, fo->collapsed, IM_COL32(200, 200, 210, 255));
                const std::string label = fo->name + "  (" + std::to_string(folderMemberCount(fo->id)) + ")";
                dl->AddText(ImVec2(a.x + S(28), cy - ImGui::GetFontSize() * 0.5f), IM_COL32(225, 225, 235, 255), label.c_str());
                ImGui::PushID(300000 + fo->id);
                ImGui::SetCursorScreenPos(a);
                ImGui::InvisibleButton("##folder_hdr", ImVec2(std::max(S(20), b.x - a.x), std::max(S(8), b.y - a.y)));
                if (ImGui::IsItemClicked(0)) folderToggle = fo->id;
                if (ImGui::IsItemHovered()) helpTip("Folder group: click to hide / show the tracks inside. Right-click for rename and more.");
                if (ImGui::IsItemClicked(1)) ctxFolderOpen = fo->id;
                ImGui::PopID();
            }
        }
        if (hiddenTrack(i)) continue;
        const float y0 = yTop[(size_t)i], y1 = baseBottom[(size_t)i];
        const float groupBottom = yEnd[(size_t)i];
        if (groupBottom < bodyMin.y || y0 > bodyMax.y) continue;
        ImVec2 r0(org.x, y0), r1(bodyMin.x, y1);
        Track& t = g_proj.tracks[(size_t)i];

        dl->AddRectFilled(r0, r1, i == g_selTrack ? IM_COL32(50, 50, 57, 255) : IM_COL32(36, 36, 40, 255));
        dl->AddRectFilled(r0, ImVec2(r0.x + S(5), r1.y), trackColor(i, 255, t.color));
        if (t.muted) dl->AddRectFilled(ImVec2(r0.x + S(6), r0.y), ImVec2(r1.x, r1.y), IM_COL32(10, 10, 10, 34));
        dl->AddLine(ImVec2(r0.x, r1.y), ImVec2(r1.x, r1.y), IM_COL32(12, 12, 14, 255));

        // drag handle on the left: drag a track onto another one to change the order
        ImGui::PushID(i + 100000);
        ImGui::SetCursorScreenPos(ImVec2(r0.x, y0));
        ImGui::InvisibleButton("##grip", ImVec2(S(12), std::max(S(8), y1 - y0)));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("CAW_TRACK", &i, sizeof(int));
            ImGui::Text("Move track %s", t.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CAW_TRACK")) { moveFrom = *(const int*)payload->Data; moveTo = i; }
            ImGui::EndDragDropTarget();
        }
        for (int k = 0; k < 3; ++k) dl->AddCircleFilled(ImVec2(r0.x + S(8), (y0 + y1) * 0.5f + (k - 1) * S(6)), S(1.5f), IM_COL32(120, 120, 130, 255));
        ImGui::PopID();

        // per-track meter: one bar per output channel (mono 1, stereo 2, quad 4 ...) and a red clip LED. Always shown: the other elements give way.
        float meterReserve = 0.f;
        for (const LaneMeterInfo& lm : (g_liveCsound ? g_engineMeters : g_idleMeters))
            if (lm.trackId == t.id) { meterReserve = lm.channels * S(8) + S(12); break; }
        if (t.kind == 5) meterReserve = 2 * S(8) + S(12);
        {
            const std::vector<LaneMeterInfo>& layout = g_liveCsound ? g_engineMeters : g_idleMeters;
            for (const LaneMeterInfo& lm : layout) {
                if (lm.trackId != t.id) continue;
                const float barW = S(6), barGap = S(2);
                const float totalW = lm.channels * barW + (lm.channels - 1) * barGap;
                const float mx1 = r1.x - S(8), mx0 = mx1 - totalW;
                const float top = y0 + S(5), bot = y1 - S(5);
                if (bot - top > S(4)) {
                    for (int c = 0; c < lm.channels; ++c) {
                        const float bx0 = mx0 + c * (barW + barGap), bx1 = bx0 + barW;
                        const size_t idx = (size_t)(lm.first + c);
                        float db = (g_liveCsound && idx < g_meterDb.size()) ? g_meterDb[idx] : -90.f;
                        bool clipped = idx < g_meterClipHold.size() && g_meterClipHold[idx] > 0.f;
                        if(t.kind==6 && c<2) {
                            auto it=g_pdMeterDb.find(t.id);
                            db=it==g_pdMeterDb.end()?-90.f:it->second[c];clipped=db>=0.f;
                        }
                        if(t.kind==3 && g_vstEngineOn && c<2) {   // the instrument plugins render in the VST engine, not in the Csound layout
                            auto it=g_vstMeterDb.find(t.id);
                            db=it==g_vstMeterDb.end()?-90.f:it->second[c];clipped=db>=0.f;
                        }
                        const bool grayMeter = lm.csoundOnly || trackConsumedByMasterVste(g_proj, t);   // a master-mode (replace) VSTe suppresses this source's own master output: its meter goes gray
                        drawLedMeter(dl, bx0, bx1, top, bot, db, clipped, grayMeter);
                    }
                }
                break;
            }
        }
        if (t.kind == 5) {
            const auto it = g_scMeterDb.find(t.id);
            const float barW = S(6), gap = S(2), totalW = 2 * barW + gap;
            const float mx1 = r1.x - S(8), mx0 = mx1 - totalW;
            const float top = y0 + S(5), bot = y1 - S(5);
            const std::array<float, 2> values = it == g_scMeterDb.end()
                ? std::array<float, 2>{-100.f, -100.f} : it->second;
            if (bot - top > S(4)) {
                for (int channel = 0; channel < 2; ++channel) {
                    const float x0 = mx0 + channel * (barW + gap);
                    const float x1 = x0 + barW;
                    drawLedMeter(dl, x0, x1, top, bot, values[static_cast<size_t>(channel)],
                                 values[static_cast<size_t>(channel)] >= 0.f, false);
                }
                ImGui::PushID(700000 + i);
                ImGui::SetCursorScreenPos(ImVec2(mx0, top));
                ImGui::InvisibleButton("##sc_engine_meter", ImVec2(totalW, bot - top));
                if (ImGui::IsItemHovered())
                    helpTip("Live left/right peaks for this SC track. CAW routes the common ~cawgroup / Out.ar(0, ...) patterns to this track's isolated bus.");
                ImGui::PopID();
            }
        }

        const float fh = ImGui::GetFrameHeight();
        // the panel hides elements as it gets narrower; the meter stays, the name gives way
        // row 2 buttons are placed left to right; each one shows only if it still ends before the meter, so they vanish one after another from the right
        const float rowRight = r1.x - meterReserve;   // the meter's left edge (+ a small gap)
        const float btnGap = S(8);
        const ImGuiStyle& gst = ImGui::GetStyle();
        auto smallBtnW = [&](const char* label, float padX) { return ImGui::CalcTextSize(label, nullptr, true).x + padX * 2.f; };
        float prevRight = r0.x;   // right edge of the last placed button (set right after each button, before any popup)
        bool rowFull = false;   // once one button does not fit, all the later ones are hidden too
        auto fitsRow = [&](float w) {   // after the previous item (SameLine) is there room for a button of width w before the meter?
            if (rowFull) return false;
            const float nx = prevRight + btnGap;
            if (nx + w > rowRight) { rowFull = true; return false; }
            return true;
        };
        const float nameRoom = headerW - S(22) - meterReserve;
        const float nameW = std::max(S(48), std::min(S(170), nameRoom));   // the title never disappears
        ImGui::PushID(i);
        {
            ImGui::SetCursorScreenPos(ImVec2(r0.x + S(16), y0 + S(6)));
            ImGui::SetNextItemWidth(nameW);
            if (inputStr("##name", t.name, 0, nameFilter)) g_selTrack = i;
            if (!ImGui::IsItemActive()) ensureUniqueInstrumentName(i);
            if (ImGui::IsItemHovered()) helpTip(t.kind == 2 ? "Name of the graph (its lane name in the score). Right-click the track for more." : t.kind == 1 ? "Name of the marker track. Right-click the track for more." : "Lane name. Every clip on the lane is its own instr (right-click a clip to rename it). Right-click the track for more.", t.name.c_str());
        }
        if (r0.x + S(18) + smallBtnW("+", gst.FramePadding.x) <= rowRight) {   // second row: + (lanes), M, S
            ImGui::SetCursorScreenPos(ImVec2(r0.x + S(18), y0 + S(6) + fh + S(4)));
            // one (+) icon only: it opens the list that adds the automation lanes and the MIDI track.
            // (the old fold arrow was removed - the - on a lane removes the lane, so the pair covers everything)
            // the + works for every track kind: SuperCollider reads lanes with ~cawget, Pure Data with [r $0-cawget-name]
            const bool plusClicked = ImGui::SmallButton("+");
            prevRight = ImGui::GetItemRectMax().x;
            if (plusClicked) ImGui::OpenPopup("add_automation_lane");
            if (ImGui::IsItemHovered()) helpTip("Add automation lanes and the MIDI lane (the - on a lane removes it)");
            if (ImGui::BeginPopup("add_automation_lane")) {
                const ParsedTrack parsed = pdParsedForUi(t);
                const bool laneHasMidi = trackHasMidi(t);
                // one plain list: the channels that can still get a lane. A channel that already
                // has a visible lane is not listed - it is on the track, and the - on that lane removes it.
                bool anyAdd = false;
                for (const CawParam& param : parsed.params) {
                    const auto lane = t.automation.find(param.chn);
                    if (lane != t.automation.end() && t.hiddenAutomation.count(param.chn) == 0) continue;
                    anyAdd = true;
                    ImGui::PushID(param.chn.c_str());
                    if (ImGui::MenuItem(param.chn.c_str())) {
                        if (lane == t.automation.end()) {
                            t.automation.emplace(param.chn, std::vector<AutoPoint>{});
                            t.automationHeights[param.chn] = kAutomationLaneH;
                        }
                        t.hiddenAutomation.erase(param.chn);   // a hidden lane is shown again
                        g_collapsedAuto.erase(t.id);           // unfold the track so the lane is visible
                    }
                    ImGui::PopID();
                }
                if (!anyAdd) {
                    const char* noChannels = t.kind == 5 ? "Add ~cawget.(\"name\") to the SuperCollider code first"
                                           : t.kind == 6 ? "Add [r $0-cawget-name] to the saved PD patch first"
                                           : "Add a cawget channel in the clip code first";
                    ImGui::TextDisabled(parsed.params.empty() ? noChannels : "Every cawget channel has a lane");
                }
                if (laneHasMidi) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("MIDI track", nullptr, !t.midiHidden)) t.midiHidden = !t.midiHidden;
                }
                ImGui::EndPopup();
            }
            if (fitsRow(smallBtnW("M", gst.FramePadding.x))) {
                ImGui::SameLine(0, S(8));
                // The pop must match the push with the PRE-click state: popping by the post-click flag
                // underflowed ImGui's style stack on every click and recolored ALL buttons of the project.
                const bool wasMuted = t.muted;
                if (wasMuted) pushBtnOn(ImVec4(0.48f, 0.20f, 0.18f, 1.f));
                const bool mClicked = ImGui::SmallButton("M");
                prevRight = ImGui::GetItemRectMax().x;
                if (mClicked) { t.muted = !wasMuted; if (t.muted) t.solo = false; pushVstActiveLive(); }   // mute and solo are exclusive; push live so VSTe tracks react without a restart
                if (wasMuted) ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(t.muted ? "Unmute track" : "Mute track (turns solo off)");
            }
            if (fitsRow(smallBtnW("S", gst.FramePadding.x))) {
                ImGui::SameLine(0, S(8));
                const bool wasSolo = t.solo;
                if (wasSolo) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                const bool sClicked = ImGui::SmallButton("S");
                prevRight = ImGui::GetItemRectMax().x;
                if (sClicked) { t.solo = !wasSolo; if (t.solo) t.muted = false; pushVstActiveLive(); }   // mute and solo are exclusive; push live so VSTe tracks react without a restart
                if (wasSolo) ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(t.solo ? "Unsolo track" : "Solo track (turns mute off)");
            }
            if ((t.kind == 3 || t.kind == 4) && fitsRow(smallBtnW("Show", S(7)))) {   // VSTi / VSTe: open the plugin window again
                ImGui::SameLine(0, S(8));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(7), 0.f));
                const bool showClicked = ImGui::SmallButton("Show##vst");
                prevRight = ImGui::GetItemRectMax().x;
                ImGui::PopStyleVar();
                if (showClicked) vstui::openWindow(t.id);
                if (ImGui::IsItemHovered()) helpTip("Open the plugin window: pick the 8 parameters, show the plugin interface");
            }
            if (fitsRow(smallBtnW("sco", S(7)))) {
                ImGui::SameLine(0, S(8));
                { ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(7), 0.f));
                  const bool scoClicked = ImGui::SmallButton("sco##trk");
                  prevRight = ImGui::GetItemRectMax().x;
                  if (scoClicked) ImGui::OpenPopup("track_score_popup");
                  ImGui::PopStyleVar(); }
                if (ImGui::IsItemHovered()) helpTip(t.kind == 2 ? "Score: the lane of the whole graph. Give it the same name as another track and the graph is drawn in that track's lane (overlap or separated)." : "Score: the name of this track lane (the same name = the same lane) and how it shows");
                if (ImGui::BeginPopup("track_score_popup")) { drawTrackScoreMenu(t); ImGui::EndPopup(); }
            }
        }
        ImGui::PopID();
        if (ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseClicked(0) && edgeTrack < 0) g_selTrack = i;
        if (ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseClicked(1)) { g_ctxTrack = i; g_selTrack = i; ImGui::OpenPopup("##track_ctx"); }

        float laneTop = y1;
        int automationLaneIndex = 0;
        for (const auto& [channel, points] : t.automation) {
            if (!autoShown(t, channel)) continue;
            const float laneBottom = laneTop + S(automationHeight(t, channel));
            const uint32_t laneColor = automationColor(t, channel, automationLaneIndex++);
            if (laneBottom >= bodyMin.y && laneTop <= bodyMax.y) {
                const ImVec2 la(org.x, laneTop), lb(bodyMin.x, laneBottom);
                dl->AddRectFilled(la, lb, IM_COL32(26, 27, 31, 255));
                dl->AddRectFilled(ImVec2(la.x + S(5), la.y), ImVec2(la.x + S(7), lb.y), trackColor(i, 230, laneColor));
                dl->AddLine(la, ImVec2(lb.x, laneTop), IM_COL32(12, 12, 14, 255));
                dl->AddText(ImVec2(la.x + S(16), laneTop + S(3)), IM_COL32(195, 195, 205, 255), channel.c_str());
                ImGui::PushID(i);
                ImGui::PushID(channel.c_str());
                const float defPadX = ImGui::GetStyle().FramePadding.x;   // the padding the + button uses (captured before the lane narrows it)
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(3), ImGui::GetStyle().FramePadding.y));
                {   // the - sits at the bottom-left of the lane, under the lane name, pinned to the bottom line: it hides this lane
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(defPadX, ImGui::GetStyle().FramePadding.y * 0.25f));
                    ImGui::PushStyleColor(ImGuiCol_Button, 0u);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 30));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 55));
                    const float bh = ImGui::GetFrameHeight();
                    const float by = std::max(laneTop + S(2), laneBottom - bh - S(1));   // pinned to the bottom line of the lane, so resizing moves it in y
                    ImGui::SetCursorScreenPos(ImVec2(org.x + S(16), by));
                    if (ImGui::Button("-", ImVec2(ImGui::CalcTextSize("+").x + defPadX * 2.f, 0.f))) { t.hiddenAutomation.insert(channel); clearAutoSelection(); }
                    ImGui::PopStyleColor(3);
                    ImGui::PopStyleVar();
                    if (ImGui::IsItemHovered()) helpTip("Hide this automation lane (the data is kept, the + menu shows it again)");
                }
                // buttons sit at fixed offsets from the right edge; the one furthest left hides first when it would touch the lane name
                const float nameEnd = org.x + S(16) + ImGui::CalcTextSize(channel.c_str()).x + S(8);
                auto roomAt = [&](float off) { return bodyMin.x - S(off) >= nameEnd; };
                // M = mute this lane (score hides it, the sound goes back to the default value), S = solo (only soloed lanes of the track play and show)
                const float gmOff = t.kind == 2 ? 100.f : 140.f, gsOff = t.kind == 2 ? 70.f : 112.f;
                const bool showGM = roomAt(gmOff);
                const bool showGS = roomAt(gsOff);
                const bool showAutoSco = t.kind != 2 && roomAt(82);
                const bool showCol = roomAt(40);
                const float leftBtnOff = showGM ? gmOff : showGS ? gsOff : showAutoSco ? 82.f : showCol ? 40.f : 0.f;
                // R = live Rec: while the timeline plays, the value the code sends to "rec" is written into this lane as dots
                const float rangeX = nameEnd;
                const float limitX = bodyMin.x - S(std::max(40.f, leftBtnOff)) - S(4);
                const bool showRange = hasClips(t) && rangeX + S(26) <= limitX;
                const bool showRec = hasClips(t) && rangeX + S(showRange ? 58 : 26) <= limitX;
                if (showRange) {
                    ImGui::SetCursorScreenPos(ImVec2(rangeX, laneTop + S(2)));
                    if (ImGui::Button("R##range", ImVec2(S(26), 0))) ImGui::OpenPopup("range_popup");
                    if (ImGui::IsItemHovered()) helpTip("Range, step and default value for this automation lane.");
                    if (ImGui::BeginPopup("range_popup")) {
                        static CawParam p;
                        if (ImGui::IsWindowAppearing()) {
                            p = {channel, 0.0, 1.0, 0.001, 0.5};
                            const auto saved = t.automationRanges.find(channel);
                            if (saved != t.automationRanges.end()) p = saved->second;
                        }
                        ImGui::TextDisabled("Range: %s", channel.c_str());
                        ImGui::SetNextItemWidth(S(140));
                        bool changed = ImGui::InputDouble("Minimum", &p.min, 0, 0, "%.4g");
                        ImGui::SetNextItemWidth(S(140));
                        changed |= ImGui::InputDouble("Maximum", &p.max, 0, 0, "%.4g");
                        ImGui::SetNextItemWidth(S(140));
                        changed |= ImGui::InputDouble("Step", &p.step, 0, 0, "%.4g");
                        ImGui::SetNextItemWidth(S(140));
                        changed |= ImGui::InputDouble("Default", &p.value, 0, 0, "%.4g");
                        if (changed && std::isfinite(p.min) && std::isfinite(p.max) && std::isfinite(p.step) &&
                            std::isfinite(p.value) && p.max > p.min && p.step > 0) {
                            p.value = quantizeAutomation(p.value, p);
                            t.automationRanges[channel] = p;
                        }
                        if (!(p.max > p.min) || !(p.step > 0))
                            ImGui::TextDisabled("Maximum must exceed minimum, and step must be positive.");
                        ImGui::TextDisabled("Restart Csound to apply range changes to playback.");
                        ImGui::EndPopup();
                    }
                }
                if (showRec) {
                    const std::pair<int, std::string> armKey(t.id, channel);
                    const bool armed = g_recArm.count(armKey) > 0;
                    ImGui::SetCursorScreenPos(ImVec2(nameEnd + (showRange ? S(32) : 0.f), laneTop + S(2)));
                    if (armed) pushBtnOn(ImVec4(0.78f, 0.16f, 0.14f, 1.f));
                    if (ImGui::Button("W##rec_arm", ImVec2(S(26), 0))) {
                        if (armed) g_recArm.erase(armKey);
                        else {
                            g_recArm.insert(armKey);
                            if (t.kind!=6 && !g_proj.cs.monitorFeed) logLine("[caw] Writing automation needs monitor data. Enable it in Settings and restart Csound.");
                        }
                    }
                    if (armed) ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        if(t.kind==6)helpTip("Write: send numbers to [s $0-cawset-%s]. Arm W and press Play.",channel.c_str());
                        else if(t.kind==5)helpTip("Write: SC clips read lanes with ~cawget.(\"%s\") but cannot send lane values back yet - draw the dots or use Automation shape.", channel.c_str());
                        else helpTip("Write: use cawset kValue, \"%s\" in the code. Run Csound, arm W and press Play.", channel.c_str());
                    }
                }
                float rgb[3] = {((laneColor >> 16) & 0xFF) / 255.f, ((laneColor >> 8) & 0xFF) / 255.f, (laneColor & 0xFF) / 255.f};
                ImGui::SetCursorScreenPos(ImVec2(bodyMin.x - S(40), laneTop + S(2)));
                if (showCol && ImGui::ColorEdit3("##automation_color", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoTooltip)) {
                    t.automationColors[channel] = 0x01000000u |
                        ((uint32_t)std::lround(rgb[0] * 255.f) << 16) |
                        ((uint32_t)std::lround(rgb[1] * 255.f) << 8) |
                        (uint32_t)std::lround(rgb[2] * 255.f);
                }
                if (showCol && ImGui::IsItemHovered()) helpTip("Automation lane color");
                const float graphComboW = 0.f;
                if (t.kind == 2) {   // graph line: row 2 = shape and how it joins the other lines; row 1 right = M S color -
                    const float rowY = laneTop + S(4) + ImGui::GetFrameHeight() + S(6);
                    const auto shIt = t.scoreAutoShape.find(channel);
                    int gshape = shIt == t.scoreAutoShape.end() ? 0 : shIt->second;
                    const float gcw = S(104);
                    const float wallR = bodyMin.x - S(8);   // right wall: W hides first, then the second combo, then the first
                    const bool showShapeCb = org.x + S(42) + gcw <= wallR;
                    const bool showMergeCb = showShapeCb && org.x + S(42 + 8) + gcw * 2.f <= wallR;
                    const bool showLookBtn = showMergeCb && org.x + S(42 + 8 + 8) + gcw * 2.f + S(28) <= wallR;
                    if (showShapeCb) {
                    ImGui::SetCursorScreenPos(ImVec2(org.x + S(42), rowY));
                    ImGui::SetNextItemWidth(gcw);
                    if (caw_ui::combo("##graph_shape", &gshape, "Curve\0Steps\0Straight\0Line, no dots\0Dashed\0Mirror smooth\0Mirror boxes\0Hairpin < >\0Bars\0")) t.scoreAutoShape[channel] = gshape;
                    if (ImGui::IsItemHovered()) helpTip("Shape of this line in the score");
                    }
                    const auto mgIt = t.scoreAutoMerge.find(channel);
                    int gmerge = mgIt == t.scoreAutoMerge.end() ? 0 : mgIt->second;
                    if (showMergeCb) {
                    ImGui::SetCursorScreenPos(ImVec2(org.x + S(42 + 8) + gcw, rowY));
                    ImGui::SetNextItemWidth(gcw);
                    if (caw_ui::combo("##graph_merge", &gmerge, "Combine\0Half combine\0Separated\0")) t.scoreAutoMerge[channel] = gmerge;
                    if (ImGui::IsItemHovered()) helpTip("How this line joins the other lines of the graph in the score:\nCombine = one shape together with the other combined lines.\nHalf combine = drawn over them at 50%.\nSeparated = a strip of its own.");
                    }
                    if (showLookBtn) {   // look: stroke width and shape ratio
                        ImGui::SetCursorScreenPos(ImVec2(org.x + S(42 + 8 + 8) + gcw * 2.f, rowY));
                        const bool customLook = t.scoreAutoStroke.count(channel) > 0 || t.scoreAutoCurve.count(channel) > 0;
                        if (customLook) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
                        if (ImGui::Button("W##graph_look", ImVec2(S(28), 0))) ImGui::OpenPopup("graph_look_popup");
                        if (customLook) ImGui::PopStyleColor(3);
                        if (ImGui::IsItemHovered()) helpTip("Stroke width of the line and shape ratio (orange = changed)");
                        if (ImGui::BeginPopup("graph_look_popup")) {
                            ImGui::TextDisabled("Line %s in the score", channel.c_str());
                            drawAutoStrokeControls(t, channel, gshape);
                            ImGui::EndPopup();
                        }
                    }
                    {   // Y = how tall the line is in the score (how far it goes up and down)
                        const float yx = org.x + S(16) + ImGui::CalcTextSize(channel.c_str()).x + S(10);
                        if (yx + S(70) < bodyMin.x - S(std::max(140.f, leftBtnOff + 4.f))) {
                            const auto ysIt = t.scoreAutoYSize.find(channel);
                            int ypct = ysIt == t.scoreAutoYSize.end() ? 100 : ysIt->second;
                            ImGui::SetCursorScreenPos(ImVec2(yx, laneTop + S(2)));
                            ImGui::SetNextItemWidth(S(70));
                            if (ImGui::DragInt("##graph_y", &ypct, 1.f, 50, 400, "Y %d%%")) t.scoreAutoYSize[channel] = std::min(400, std::max(50, ypct));
                            if (ImGui::IsItemHovered()) helpTip("Y size: how tall this line is in the score, so how far it goes up and down (100% = normal). Drag to change.");
                        }
                    }
                }
                {
                    // M = hide this line in the score, S = show only the soloed lines
                    const bool gm = t.scoreAutoMuted.count(channel) > 0, gs = t.scoreAutoSolo.count(channel) > 0;
                    if (showGM) {
                    ImGui::SetCursorScreenPos(ImVec2(bodyMin.x - S(gmOff), laneTop + S(2)));
                    if (gm) pushBtnOn(ImVec4(0.48f, 0.20f, 0.18f, 1.f));
                    if (ImGui::Button("M##gm", ImVec2(S(24), 0))) { if (gm) t.scoreAutoMuted.erase(channel); else t.scoreAutoMuted.insert(channel); }
                    if (gm) ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) helpTip(gm ? "Unmute this lane" : t.kind == 2 ? "Mute: hide this line in the score" : "Mute this automation: it is not in the score and the sound goes back to its default value");
                    }
                    if (showGS) {
                    ImGui::SetCursorScreenPos(ImVec2(bodyMin.x - S(gsOff), laneTop + S(2)));
                    if (gs) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                    if (ImGui::Button("S##gs", ImVec2(S(24), 0))) { if (gs) t.scoreAutoSolo.erase(channel); else t.scoreAutoSolo.insert(channel); }
                    if (gs) ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) helpTip(gs ? "Unsolo this lane" : t.kind == 2 ? "Solo: show only the soloed lines in the score" : "Solo: only the soloed automation lanes of this track play and show in the score, the others go back to their default value");
                    }
                }
                if (showAutoSco) {   // put this automation in the printed score
                    ImGui::SetCursorScreenPos(ImVec2(bodyMin.x - S(82), laneTop + S(2)));
                    const auto sm = t.scoreAuto.find(channel);
                    const int smode = sm == t.scoreAuto.end() ? 1 : sm->second;
                    if (smode > 0) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
                    if (ImGui::Button("sco##auto", ImVec2(S(34), 0))) ImGui::OpenPopup("auto_score_popup");
                    if (smode > 0) ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) helpTip("Score: this automation gets its own lane in the printed score (orange = on)");
                    if (ImGui::BeginPopup("auto_score_popup")) { drawAutoScoreMenu(t, channel); ImGui::EndPopup(); }
                }
                if (t.id == g_autoSelTrackId && channel == g_autoSelChannel && !g_autoSel.empty()) {
                    auto laneIt = t.automation.find(channel);
                    if (laneIt != t.automation.end() && *g_autoSel.rbegin() < (int)laneIt->second.size()) {
                        // the value box sits between the lane name and the buttons on the right (Sc, color), never under them
                        const std::string laneLabel = channel;
                        const float leftX = org.x + S(16) + ImGui::CalcTextSize(laneLabel.c_str()).x + S(10) + (t.kind == 2 ? S(78) : 0.f) + graphComboW + (showRange ? S(32) : 0.f) + (showRec ? S(32) : 0.f);
                        const float rightX = bodyMin.x - S(leftBtnOff > 0.f ? leftBtnOff : 40.f) - S(8);
                        if (rightX - leftX >= S(56)) {
                            double lo, hi, initial;
                            laneValueRange(t, channel, lo, hi, initial);
                            double value = laneIt->second[(size_t)*g_autoSel.begin()].v;
                            ImGui::SetCursorScreenPos(ImVec2(leftX, laneTop + S(2)));
                            ImGui::SetNextItemWidth(std::min(S(130), rightX - leftX));
                            if (ImGui::InputDouble("##selected_dot_value", &value, 0.0, 0.0, g_autoSel.size() > 1 ? "all: %.4f" : "%.4f")) {
                                value = std::min(hi, std::max(lo, value));
                                for (int idx : g_autoSel) laneIt->second[(size_t)idx].v = value;
                            }
                            if (ImGui::IsItemDeactivatedAfterEdit())
                                for (int idx : g_autoSel)
                                    laneIt->second[(size_t)idx].v = laneQuantizeValue(t, channel, laneIt->second[(size_t)idx].v);
                            if (ImGui::IsItemHovered())
                                helpTip("Value of the %d selected dot(s), range %.4g .. %.4g", (int)g_autoSel.size(), lo, hi);
                        }
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopID();
                ImGui::PopID();
                if (ImGui::IsMouseHoveringRect(la, lb) && ImGui::IsMouseClicked(0) && edgeTrack < 0 && edgeAutomationTrack < 0) g_selTrack = i;
                if (ImGui::IsMouseHoveringRect(la, lb) && ImGui::IsMouseClicked(1) && edgeTrack < 0 && edgeAutomationTrack < 0) {
                    g_selTrack = i;   // right-click selects the lane and opens its menu
                    g_ctxAutoTrack = i;
                    g_ctxAutoChannel = channel;
                    ImGui::OpenPopup("##auto_lane_ctx");
                }
            }
            laneTop = laneBottom;
        }
        if (midiShown(t)) {
            const float midiTop = midiLaneTop(t, y1);
            const float midiBottom = midiTop + S(kMidiLaneH);
            ImVec2 ml0(org.x, midiTop), ml1(bodyMin.x, midiBottom);
            dl->AddRectFilled(ml0, ml1, IM_COL32(26, 27, 31, 255));
            dl->AddRectFilled(ImVec2(ml0.x + S(5), ml0.y), ImVec2(ml0.x + S(7), ml1.y), trackColor(i, 170, t.color));
            dl->AddLine(ml0, ImVec2(ml1.x, midiTop), IM_COL32(12, 12, 14, 255));
            dl->AddText(ImVec2(ml0.x + S(16), midiTop + S(4)), IM_COL32(195, 195, 205, 255), "MIDI track");
            const float midiTitleEnd = ml0.x + S(16) + ImGui::CalcTextSize("MIDI track").x + S(8);
            if (bodyMin.x - S(80) >= midiTitleEnd) {
                ImGui::PushID(i);
                ImGui::SetCursorScreenPos(ImVec2(bodyMin.x - S(80), midiTop + S(2)));
                if (ImGui::Button("sco##midi_score", ImVec2(S(34), 0))) ImGui::OpenPopup("midi_score_popup");
                if (ImGui::IsItemHovered()) helpTip("Score: the name of the MIDI lane (the same name = the same lane) and how the notes show");
                if (ImGui::BeginPopup("midi_score_popup")) { drawMidiScoreMenu(t); ImGui::EndPopup(); }
                ImGui::PopID();
            }
            if (bodyMin.x - S(40) >= midiTitleEnd) {
                ImGui::PushID(i);
                ImGui::SetCursorScreenPos(ImVec2(bodyMin.x - S(40), midiTop + S(2)));
                if (ImGui::Button("-##hide_midi", ImVec2(S(24), 0))) t.midiHidden = true;
                if (ImGui::IsItemHovered()) helpTip("Hide the MIDI lane (notes are kept, use + to show it again)");
                ImGui::PopID();
            }
            if (ImGui::IsMouseHoveringRect(ml0, ml1) && ImGui::IsMouseClicked(0) && edgeTrack < 0 && edgeAutomationTrack < 0)
                g_selTrack = i;   // click = select only
        }
    }
    ImGui::PopClipRect();

    if (folderToggle) { if (TrackFolder* fo = findFolder(folderToggle)) fo->collapsed = !fo->collapsed; }
    if (ctxFolderOpen) { g_ctxFolder = ctxFolderOpen; ImGui::OpenPopup("##folder_ctx"); }
    if (ImGui::BeginPopup("##folder_ctx")) {
        TrackFolder* fo = findFolder(g_ctxFolder);
        if (!fo) ImGui::CloseCurrentPopup();
        else {
            ImGui::TextDisabled("Folder group");
            ImGui::SetNextItemWidth(S(180));
            inputStr("##folder_name", fo->name, 0);
            {
                const uint32_t fc = fo->color ? fo->color : 0x5A6EA0u;
                float rgb[3] = {((fc >> 16) & 0xFF) / 255.f, ((fc >> 8) & 0xFF) / 255.f, (fc & 0xFF) / 255.f};
                ImGui::TextUnformatted("Color");
                ImGui::SameLine();
                if (ImGui::ColorEdit3("##folder_color", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                    fo->color = ((uint32_t)std::lround(rgb[0] * 255.f) << 16) | ((uint32_t)std::lround(rgb[1] * 255.f) << 8) | (uint32_t)std::lround(rgb[2] * 255.f);
            }
            if (ImGui::MenuItem(fo->collapsed ? "Show the tracks" : "Hide the tracks")) { fo->collapsed = !fo->collapsed; ImGui::CloseCurrentPopup(); }
            if (ImGui::MenuItem("Delete folder and ALL its tracks...")) {   // same confirmation as deleting selected tracks
                for (Track& tr : g_proj.tracks) tr.deleteSelected = tr.folderId == fo->id;
                g_confirmDeleteSelected = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Ungroup (keep the tracks)")) {
                for (Track& tr : g_proj.tracks) if (tr.folderId == fo->id) tr.folderId = 0;
                pruneFolders();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveFrom < (int)g_proj.tracks.size() && moveTo < (int)g_proj.tracks.size()) {
        int selectedId = -1;
        if (g_selTrack >= 0 && g_selTrack < (int)g_proj.tracks.size()) selectedId = g_proj.tracks[(size_t)g_selTrack].id;
        const int destFolder = g_proj.tracks[(size_t)moveTo].folderId;   // dropped on a track of a folder: joins it; dropped on a loose track: leaves its folder
        Track moved = std::move(g_proj.tracks[(size_t)moveFrom]);
        g_proj.tracks.erase(g_proj.tracks.begin() + moveFrom);
        moved.folderId = destFolder;
        g_proj.tracks.insert(g_proj.tracks.begin() + moveTo, std::move(moved));
        for (int k = 0; k < (int)g_proj.tracks.size(); ++k) if (g_proj.tracks[(size_t)k].id == selectedId) g_selTrack = k;
        pruneFolders();
    }

    if (ImGui::IsMouseHoveringRect(hdrMin, hdrMax) && ImGui::IsMouseClicked(1) && m.y >= yTop[(size_t)nT] && !ImGui::IsPopupOpen("##track_ctx")) { g_ctxTrack = -1; ImGui::OpenPopup("##track_ctx"); }
    if (ImGui::BeginPopup("##track_ctx")) {
        // Every kind of track can be added whatever the session list says: an unchecked engine
        // is only not RUN (code it now, enable the engine in Settings > Languages to hear it).
        if (ImGui::MenuItem("Add Csound track")) { g_selTrack = addLangTrack(0); g_selClip = -1; }
        if (!g_langCsound && ImGui::IsItemHovered()) helpTip("Csound is not enabled in Settings > Languages: the track and its code work, Run will not start Csound.");
        if (ImGui::MenuItem("Add SuperCollider track")) { g_selTrack = addNonCsoundTrack(true); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("Double-click the lane to add a SuperCollider code clip. Start SC with the transport Run button, then press Play.");
        if (ImGui::MenuItem("Add Pure Data track")) { g_selTrack = addNonCsoundTrack(false); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("Double-click the track to create a PD clip, then open the clip and load a .pd patch.");
        ImGui::Separator();
        if (ImGui::MenuItem("Add VSTi")) { g_selTrack = addVstTrack(true); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("A VST3 instrument with MIDI input and parameter automation.");
        if (ImGui::MenuItem("Add VSTe")) { g_selTrack = addVstTrack(false); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("A VST3 effect with audio input and parameter automation.");
        ImGui::Separator();
        if (ImGui::MenuItem("Add graph track")) { g_selTrack = addFreeAutomationTrack(); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("An automation lane that is not linked to a clip. Its Csound channel is the track name: read it anywhere with  kVal chnget \"Name\".");
        if (ImGui::MenuItem("Add marker track")) { g_selTrack = addMarkerTrack(); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("Comments and guidance for the project: double-click the lane to write a note.");
        {   // folder groups
            ImGui::Separator();
            const int fi = g_ctxTrack >= 0 && g_ctxTrack < nT ? g_ctxTrack : (g_selTrack >= 0 && g_selTrack < nT ? g_selTrack : -1);
            if (ImGui::MenuItem(g_ctxTrack >= 0 ? "New folder group with this track" : "New folder group with the selected track", nullptr, false, fi >= 0)) {
                const int trackId = g_proj.tracks[(size_t)fi].id;
                if (g_proj.tracks[(size_t)fi].folderId != 0) setTrackFolder(fi, 0);   // leave the old folder first so its tracks stay together
                for (int k = 0; k < (int)g_proj.tracks.size(); ++k) if (g_proj.tracks[(size_t)k].id == trackId) { newFolderFor(k); break; }
            }
            if (ImGui::IsItemHovered()) helpTip("A folder group holds tracks you can hide and show together. Click its header in the left panel to fold it. Drag a track onto a track of the folder to add it.");
            if (fi >= 0) {
                const int curFolder = g_proj.tracks[(size_t)fi].folderId;
                if (!g_proj.folders.empty() && ImGui::BeginMenu("Move track into folder")) {
                    for (const TrackFolder& f : g_proj.folders) {
                        if (f.id == curFolder) continue;
                        ImGui::PushID(f.id);
                        if (ImGui::MenuItem(f.name.c_str())) setTrackFolder(fi, f.id);
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                if (curFolder != 0 && ImGui::MenuItem("Take track out of its folder")) setTrackFolder(fi, 0);
            }
        }
        if (g_ctxTrack >= 0 && g_ctxTrack < nT) {
            ImGui::Separator();
            ImGui::TextDisabled("%s", g_proj.tracks[(size_t)g_ctxTrack].name.c_str());
            if (g_proj.tracks[(size_t)g_ctxTrack].kind == 3) {
                if (ImGui::MenuItem("Add MIDI clip")) {
                    // One step to a working instrument: a code clip (silent - the notes come from the
                    // MIDI clip) plus a MIDI clip at the playhead. The notes go straight to the plugin,
                    // so this plays like a DAW: draw notes, press Play.
                    historyFlush();
                    Track& lane = g_proj.tracks[(size_t)g_ctxTrack];
                    addClip(g_ctxTrack, g_play, 4.0, "# The notes come from the MIDI clip. CAW code here drives P1..P8 and can add more notes.\n");
                    if (!lane.clips.empty()) {
                        Clip& c = lane.clips.back();
                        c.midiEnabled = true;
                        syncSharedCode(c);
                        MidiClip mc;
                        mc.id = newMidiId();
                        mc.clipId = c.id;
                        mc.name = c.name;
                        mc.start = c.start;
                        mc.dur = std::max(0.01, c.dur);
                        lane.midiClips.push_back(mc);
                        lane.midiHidden = false;
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) helpTip("Creates a 4-second MIDI clip at the playhead. Its notes go straight to the instrument - draw notes (MIDI editor or double-click the clip), press Play and it sounds.");
            }
            if (ImGui::MenuItem("Delete track")) del = g_ctxTrack;
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##auto_lane_ctx")) {   // right-click menu of an automation lane
        if (g_ctxAutoTrack >= 0 && g_ctxAutoTrack < (int)g_proj.tracks.size()) {
            Track& ct = g_proj.tracks[(size_t)g_ctxAutoTrack];
            auto cLane = ct.automation.find(g_ctxAutoChannel);
            const int nDots = cLane == ct.automation.end() ? 0 : (int)cLane->second.size();
            ImGui::TextDisabled("%s", g_ctxAutoChannel.c_str());
            if (ImGui::MenuItem("Select all dots", nullptr, false, nDots > 0)) {
                g_autoSelMulti.clear();
                g_autoSel.clear();
                g_autoSelTrackId = ct.id;
                g_autoSelChannel = g_ctxAutoChannel;
                for (int idx = 0; idx < nDots; ++idx) g_autoSel.insert(idx);
            }
            if (ImGui::MenuItem("Delete all dots", "Del", nullptr, nDots > 0)) {
                // select everything of this lane, then the normal deletion runs (same undo behavior as Del)
                g_autoSelMulti.clear();
                g_autoSel.clear();
                g_autoSelTrackId = ct.id;
                g_autoSelChannel = g_ctxAutoChannel;
                for (int idx = 0; idx < nDots; ++idx) g_autoSel.insert(idx);
                historyFlush();   // the deletion becomes its own undo step
                deleteSelectedAutoPoints();
            }
            if (ImGui::MenuItem("Hide automation", nullptr, false, cLane != ct.automation.end())) {
                ct.hiddenAutomation.insert(g_ctxAutoChannel);
                clearAutoSelection();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (del >= 0 && del < (int)g_proj.tracks.size()) {
        const int removedId = g_proj.tracks[(size_t)del].id;
        g_proj.tracks.erase(g_proj.tracks.begin() + del);
        pruneFolders();
        if (g_autoDragTrackId == removedId) { g_autoDragTrackId = -1; g_autoDragPoint = -1; g_autoDragChannel.clear(); }
        (void)removedId;
        g_midiSelectedTrackId = -1; g_midiSelectedNote = -1;
        g_midiDragTrackId = -1; g_midiDragNote = -1;
        validateAutoSelection();
        if (g_selTrack == del) g_selTrack = -1;
        else if (g_selTrack > del) --g_selTrack;
        g_selClip = -1;
    }

    // ---- right-click clip menu: every clip is its own instrument (name, color, code, MIDI)
    if (ImGui::BeginPopup("##clip_context")) {
        int laneIndex = -1;
        Clip* clip = findClip(g_ctxClipId, &laneIndex);
        if (!clip) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextDisabled(g_proj.tracks[(size_t)laneIndex].kind == 5 ? "SuperCollider clip" : "Clip instrument");
            ImGui::SetNextItemWidth(S(170));
            const std::string nameBefore = clip->name;
            if (inputStr("##clip_ctx_name", clip->name, 0, nameFilter)) renameClipGroup(*clip, nameBefore);
            if (!ImGui::IsItemActive()) ensureUniqueClipName(*clip);
            if (ImGui::IsItemHovered()) helpTip(g_proj.tracks[(size_t)laneIndex].kind == 5 ? "Name of this SuperCollider clip" : "This clip's own Csound instrument name");
            ImGui::SameLine();
            const uint32_t rgbValue = clipColorValue(g_proj.tracks[(size_t)laneIndex], *clip);
            float rgb[3] = {((rgbValue >> 16) & 0xFF) / 255.f, ((rgbValue >> 8) & 0xFF) / 255.f, (rgbValue & 0xFF) / 255.f};
            if (ImGui::ColorEdit3("##clip_ctx_color", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                clip->color = 0x01000000u | ((uint32_t)std::lround(rgb[0] * 255.f) << 16) |
                              ((uint32_t)std::lround(rgb[1] * 255.f) << 8) | (uint32_t)std::lround(rgb[2] * 255.f);
            ImGui::SetNextItemWidth(S(120));
            double clipStart = clip->start;
            if (ImGui::InputDouble("Start (s)##clip_timing", &clipStart, 0.0, 0.0, "%.3f")) clip_timing::moveCodeClip(g_proj, *clip, clipStart);
            ImGui::SetNextItemWidth(S(120));
            double clipDur = clip->dur;
            if (ImGui::InputDouble("Duration (s)##clip_timing", &clipDur, 0.0, 0.0, "%.3f")) clip->dur = clip_timing::duration(clipDur, clip->dur, 0.001);
            drawClipQMenu();
            ImGui::Separator();
            if (ImGui::MenuItem("Edit this clip's code")) { clip->editorOpen = true; ImGui::CloseCurrentPopup(); }
            if (ImGui::MenuItem("Add MIDI clip", nullptr, false, g_proj.tracks[(size_t)laneIndex].kind == 0 || g_proj.tracks[(size_t)laneIndex].kind == 3 || g_proj.tracks[(size_t)laneIndex].kind == 6)) {
                historyFlush();
                Track& lane = g_proj.tracks[(size_t)laneIndex];
                clip->midiEnabled = true;
                syncSharedCode(*clip);
                MidiClip mc;
                mc.id = newMidiId();
                mc.clipId = clip->id;
                mc.name = clip->name;
                mc.start = clip->start;
                mc.dur = std::max(0.01, clip->dur);
                lane.midiClips.push_back(mc);
                lane.midiHidden = false;
                g_collapsedAuto.erase(lane.id);
                historyFlush();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Duplicate (same instrument)", "Ctrl+D")) {
                if (!g_selClips.count(g_ctxClipId)) { g_selClips.clear(); g_selClips.insert(g_ctxClipId); }
                duplicateSelectedClips();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Duplicate as new instrument")) {
                Track& lane = g_proj.tracks[(size_t)laneIndex];
                for (int ci = 0; ci < (int)lane.clips.size(); ++ci)
                    if (lane.clips[(size_t)ci].id == g_ctxClipId) { duplicateClipAsNew(laneIndex, ci); break; }
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete clip", "Del")) {
                Track& lane = g_proj.tracks[(size_t)laneIndex];
                lane.clips.erase(std::remove_if(lane.clips.begin(), lane.clips.end(),
                    [](const Clip& c) { return c.id == g_ctxClipId; }), lane.clips.end());
                removeMidiClipsOf(g_ctxClipId);
                g_selClips.erase(g_ctxClipId);
                g_selClip = -1;
                setPrimaryFromSet();
                g_ctxClipId = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- right-click menu of a MIDI clip
    if (ImGui::BeginPopup("##midi_context")) {
        int midiLane = -1;
        MidiClip* mc = findMidiClip(g_ctxMidiId, &midiLane);
        if (!mc) {
            ImGui::CloseCurrentPopup();
        } else {
            Track& lane = g_proj.tracks[(size_t)midiLane];
            const Clip* current = findClip(mc->clipId);
            ImGui::TextDisabled("MIDI clip  ->  %s", current ? current->name.c_str() : "(no instrument)");
            ImGui::SetNextItemWidth(S(120));
            double start = mc->start;
            if (ImGui::InputDouble("Start (s)##midi_clip_timing", &start, 0.0, 0.0, "%.3f")) mc->start = clip_timing::nonnegative(start, mc->start);
            ImGui::SetNextItemWidth(S(120));
            double dur = mc->dur;
            if (ImGui::InputDouble("Duration (s)##midi_clip_timing", &dur, 0.0, 0.0, "%.3f")) mc->dur = clip_timing::duration(dur, mc->dur, 0.01);
            drawClipQMenu();
            ImGui::Separator();
            if (ImGui::MenuItem("Open")) { mc->editorOpen = true; ImGui::CloseCurrentPopup(); }
            if (g_selMidiClips.size() > 1 && ImGui::MenuItem("Open selected clips together", "Enter")) {
                g_multiMidiIds.assign(g_selMidiClips.begin(), g_selMidiClips.end());
                g_multiMidiOpen = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Split at playhead")) {
                for (int k = 0; k < (int)lane.midiClips.size(); ++k)
                    if (lane.midiClips[(size_t)k].id == g_ctxMidiId) { cutMidiClipAt(midiLane, k, g_play); break; }
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Join selected")) { joinSelectedClips(); ImGui::CloseCurrentPopup(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete MIDI clip", "Del")) {
                if (!g_selMidiClips.count(g_ctxMidiId)) { g_selMidiClips.clear(); g_selMidiClips.insert(g_ctxMidiId); }
                deleteSelectedMidiClips();
                g_ctxMidiId = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- type exact automation values (double-click or right-click a dot)
    if (g_autoOpenPointPopup) {
        ImGui::OpenPopup("##automation_point");
        g_autoOpenPointPopup = false;
    }
    if (ImGui::BeginPopup("##automation_point")) {
        validateAutoSelection();
        int laneTrackIndex = -1;
        Track* laneTrack = findTrackById(g_autoSelTrackId, &laneTrackIndex);
        std::vector<AutoPoint>* lane = selectedAutoLane();
        if (!laneTrack || !lane || g_autoSel.empty()) {
            ImGui::CloseCurrentPopup();
        } else {
            double lo, hi, initial;
            laneValueRange(*laneTrack, g_autoSelChannel, lo, hi, initial);
            ImGui::TextDisabled("%s  |  %d dot(s) selected  |  range %.4g .. %.4g", g_autoSelChannel.c_str(),
                                (int)g_autoSel.size(), lo, hi);
            if (g_autoSel.size() == 1) {
                AutoPoint& point = (*lane)[(size_t)*g_autoSel.begin()];
                ImGui::SetNextItemWidth(S(150));
                if (ImGui::InputDouble("Value##auto_value", &point.v, 0.0, 0.0, "%.4f")) point.v = std::min(hi, std::max(lo, point.v));
                if (ImGui::IsItemDeactivatedAfterEdit())
                    point.v = laneQuantizeValue(*laneTrack, g_autoSelChannel, point.v);
                ImGui::SetNextItemWidth(S(150));
                if (ImGui::InputDouble("Time (s)##auto_time", &point.t, 0.01, 0.1, "%.3f")) point.t = clampTimelineTime(point.t);
                if (!ImGui::IsAnyItemActive()) sortLaneKeepSelection(*lane, g_autoSel);
            } else {
                static double groupValue = 0.0;
                if (ImGui::IsWindowAppearing()) groupValue = (*lane)[(size_t)*g_autoSel.begin()].v;
                ImGui::SetNextItemWidth(S(150));
                ImGui::InputDouble("##auto_group_value", &groupValue, 0.0, 0.0, "%.4f");
                ImGui::SameLine();
                if (ImGui::Button("Set all")) {
                    groupValue = laneQuantizeValue(*laneTrack, g_autoSelChannel, std::min(hi, std::max(lo, groupValue)));
                    for (int idx : g_autoSel) (*lane)[(size_t)idx].v = groupValue;
                }
                // ---- reshape the selected dots (live, from the dots as they were when this box opened)
                {
                    static std::vector<int> rsIdx;
                    static std::vector<AutoPoint> rsOrig;
                    static float rsDepthStart, rsDepthEnd, rsStretch, rsAmount, rsShift;
                    static int rsWave;
                    const bool fresh = ImGui::IsWindowAppearing() || rsIdx.size() != g_autoSel.size() || !std::equal(rsIdx.begin(), rsIdx.end(), g_autoSel.begin());
                    if (fresh) {
                        rsIdx.assign(g_autoSel.begin(), g_autoSel.end());
                        rsOrig.clear();
                        for (int idx : rsIdx) rsOrig.push_back((*lane)[(size_t)idx]);
                        rsDepthStart = rsDepthEnd = 100.f; rsStretch = 100.f; rsAmount = 0.f; rsShift = 0.f; rsWave = 0;
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Reshape the %d selected dots", (int)rsIdx.size());
                    bool changed = false;
                    ImGui::SetNextItemWidth(S(190));
                    changed |= ImGui::SliderFloat("Depth start##rs_d0", &rsDepthStart, 0.f, 300.f, "%.0f %%");
                    if (ImGui::IsItemHovered()) helpTip("How big the wave is at the first selected dot (100 % = as drawn, 0 % = flat in the middle).");
                    ImGui::SetNextItemWidth(S(190));
                    changed |= ImGui::SliderFloat("Depth end##rs_d1", &rsDepthEnd, 0.f, 300.f, "%.0f %%");
                    if (ImGui::IsItemHovered()) helpTip("How big the wave is at the last selected dot. Start 100 % and end 300 % = a wave that slowly becomes wider.");
                    ImGui::SetNextItemWidth(S(190));
                    changed |= ImGui::SliderFloat("Time stretch##rs_st", &rsStretch, 25.f, 400.f, "%.0f %%");
                    if (ImGui::IsItemHovered()) helpTip("Stretches or squeezes the selected dots in time, from the first dot.");
                    ImGui::SetNextItemWidth(S(190));
                    changed |= ImGui::SliderFloat("Move up / down##rs_sh", &rsShift, -100.f, 100.f, "%.0f %% of range");
                    changed |= comboFit(S(190), "Wave##rs_wave", &rsWave, "Keep\0Square\0Triangle\0Sine\0Soft square\0");
                    if (ImGui::IsItemHovered()) helpTip("Re-shapes the dots into another wave: a sine becomes a square (Square, Soft square) or a triangle. Triangle also turns a triangle into a sine.");
                    if (rsWave != 0) {
                        ImGui::SetNextItemWidth(S(190));
                        changed |= ImGui::SliderFloat("Amount##rs_am", &rsAmount, 0.f, 100.f, "%.0f %%");
                        if (ImGui::IsItemHovered()) helpTip("0 % = the drawn wave, 100 % = fully the chosen wave. Move slowly from 0 to 100 to morph.");
                    }
                    if (ImGui::Button("Reset##rs_reset")) {
                        rsDepthStart = rsDepthEnd = 100.f; rsStretch = 100.f; rsAmount = 0.f; rsShift = 0.f; rsWave = 0;
                        changed = true;
                    }
                    if (changed && rsOrig.size() == rsIdx.size() && !rsOrig.empty()) {
                        const ParsedTrack parsed = pdParsedForUi(*laneTrack);
                        const CawParam* param = hasClips(*laneTrack) ? laneParameter(parsed, g_autoSelChannel) : nullptr;
                        double vMin = rsOrig[0].v, vMax = rsOrig[0].v;
                        for (const AutoPoint& q : rsOrig) { vMin = std::min(vMin, q.v); vMax = std::max(vMax, q.v); }
                        const double center = (vMin + vMax) * 0.5, half = (vMax - vMin) * 0.5;
                        const double t0 = rsOrig.front().t, tSpan = rsOrig.back().t - t0;
                        const double amt = rsAmount / 100.0;
                        const double kPi = 3.14159265358979323846;
                        for (size_t k = 0; k < rsIdx.size(); ++k) {
                            const double x = tSpan > 1e-9 ? (rsOrig[k].t - t0) / tSpan : 0.0;
                            const double depth = (rsDepthStart + (rsDepthEnd - rsDepthStart) * x) / 100.0;
                            double n = half > 1e-12 ? (rsOrig[k].v - center) / half : 0.0;   // -1 .. 1
                            double w = n;
                            if (rsWave == 1) w = n >= 0.0 ? 1.0 : -1.0;
                            else if (rsWave == 2) w = (2.0 / kPi) * std::asin(std::min(1.0, std::max(-1.0, n)));
                            else if (rsWave == 3) w = std::sin(kPi * 0.5 * std::min(1.0, std::max(-1.0, n)));
                            else if (rsWave == 4) w = std::tanh(6.0 * n) / std::tanh(6.0);
                            n = rsWave == 0 ? n : n + (w - n) * amt;
                            AutoPoint& dst = (*lane)[(size_t)rsIdx[k]];
                            dst.v = std::min(hi, std::max(lo, center + n * half * depth + rsShift / 100.0 * (hi - lo)));
                            if (param) dst.v = quantizeAutomation(dst.v, *param);
                            dst.t = clampTimelineTime(t0 + (rsOrig[k].t - t0) * rsStretch / 100.0);
                        }
                    }
                }
            }
            if (ImGui::Button("Delete selected dots")) {
                deleteSelectedAutoPoints();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- highlight the lane edge while hovering / resizing
    {
        float ye = edgeY;
        bool drawEdge = edgeTrack >= 0 || edgeAutomationTrack >= 0;
        if (g_drag == Drag::ResizeTrack && g_resizeTrack >= 0 && g_resizeTrack < nT) {
            ye = baseBottom[(size_t)g_resizeTrack]; drawEdge = true;
        } else if (g_drag == Drag::ResizeAutomation && g_resizeTrack >= 0 && g_resizeTrack < nT) {
            ye = automationLaneTop(g_proj.tracks[(size_t)g_resizeTrack], baseBottom[(size_t)g_resizeTrack], g_resizeAutomationChannel)
                 + S(automationHeight(g_proj.tracks[(size_t)g_resizeTrack], g_resizeAutomationChannel));
            drawEdge = true;
        }
        if (drawEdge) {
            ImGui::PushClipRect(hdrMin, bodyMax, true);
            dl->AddLine(ImVec2(org.x, ye), ImVec2(bodyMax.x, ye), IM_COL32(237, 128, 38, 255), S(2));
            ImGui::PopClipRect();
        }
    }

    // ---- scrollbars + filler corners
    {
        dl->AddRectFilled(ImVec2(bodyMax.x, org.y), ImVec2(bodyMax.x + sb, bodyMin.y), IM_COL32(30, 30, 34, 255));
        dl->AddRectFilled(ImVec2(org.x, bodyMax.y), ImVec2(bodyMin.x, bodyMax.y + sb), IM_COL32(33, 33, 37, 255));
        dl->AddRectFilled(vBarA, vBarB, IM_COL32(26, 26, 30, 255));
        dl->AddRectFilled(hBarA, hBarB, IM_COL32(26, 26, 30, 255));
        dl->AddRectFilled(ImVec2(bodyMax.x, bodyMax.y), ImVec2(bodyMax.x + sb, bodyMax.y + sb), IM_COL32(33, 33, 37, 255));
        const float vTop = bodyMin.y + (maxY > 0.f ? g_scrollY / maxY * vRange : 0.f);
        const float hLeft = bodyMin.x + (scrollSpan > 0.0 ? (float)std::min(1.0, std::max(0.0, (g_scrollX - minX) / scrollSpan)) * hRange : 0.f);
        const ImVec2 vt0(vBarA.x + S(2), vTop), vt1(vBarB.x - S(2), vTop + vThumbLen);
        const ImVec2 ht0(hLeft, hBarA.y + S(2)), ht1(hLeft + hThumbLen, hBarB.y - S(2));
        auto thumbCol = [&](const ImVec2& a, const ImVec2& b, bool active) {
            return active ? IM_COL32(237, 128, 38, 255)
                          : (ImGui::IsMouseHoveringRect(a, b) ? IM_COL32(120, 120, 132, 255) : IM_COL32(86, 86, 96, 255));
        };
        dl->AddRectFilled(vt0, vt1, thumbCol(vt0, vt1, g_drag == Drag::ScrollY), S(4));
        dl->AddRectFilled(ht0, ht1, thumbCol(ht0, ht1, g_drag == Drag::ScrollX), S(4));
    }

    // ---- playhead
    float px = tx(g_play);
    if (px >= bodyMin.x - 1 && px <= bodyMax.x) {
        ImGui::PushClipRect(ImVec2(rulerMin.x, org.y), bodyMax, true);
        const ImU32 col = IM_COL32(255, 85, 70, 255);
        dl->AddLine(ImVec2(px, org.y), ImVec2(px, bodyMax.y), col, S(1.5f));
        dl->AddTriangleFilled(ImVec2(px - S(6), org.y + S(10)), ImVec2(px + S(6), org.y + S(10)),
                              ImVec2(px, org.y + S(22)), col);
        ImGui::PopClipRect();
    }

    dl->AddLine(ImVec2(bodyMin.x, org.y), ImVec2(bodyMin.x, bodyMax.y), IM_COL32(12, 12, 14, 255), S(2));
    dl->AddLine(ImVec2(org.x, bodyMin.y), ImVec2(bodyMax.x, bodyMin.y), IM_COL32(12, 12, 14, 255), S(2));

    if (inBody && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (g_tool == Tool::Cut || g_tool == Tool::Pen || g_tool == Tool::Pen2)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImU32 ink = IM_COL32(240, 240, 244, 255), shadow = IM_COL32(8, 8, 10, 230);
        if (g_tool == Tool::Cut) {
            const ImVec2 a(m.x + S(5), m.y + S(14)), b(m.x + S(13), m.y + S(14));
            fg->AddLine(ImVec2(m.x + S(6), m.y + S(12)), ImVec2(m.x + S(17), m.y + S(1)), shadow, S(4));
            fg->AddLine(ImVec2(m.x + S(12), m.y + S(12)), ImVec2(m.x + S(1), m.y + S(1)), shadow, S(4));
            fg->AddLine(ImVec2(m.x + S(6), m.y + S(12)), ImVec2(m.x + S(17), m.y + S(1)), ink, S(1.7f));
            fg->AddLine(ImVec2(m.x + S(12), m.y + S(12)), ImVec2(m.x + S(1), m.y + S(1)), ink, S(1.7f));
            fg->AddCircle(a, S(3), ink, 12, S(1.7f)); fg->AddCircle(b, S(3), ink, 12, S(1.7f));
        } else {
            const ImVec2 tip(m.x, m.y), back(m.x + S(13), m.y - S(13));
            fg->AddLine(tip, back, shadow, S(6));
            fg->AddLine(tip, back, ink, S(3));
            fg->AddTriangleFilled(tip, ImVec2(m.x + S(3), m.y - S(6)), ImVec2(m.x + S(6), m.y - S(3)), IM_COL32(237, 128, 38, 255));
        }
    }
    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(avail);
    endChildTight();
}

// ---------------------------------------------------------------- master monitor
// Cubic interpolation between p1 and p2 (t = 0..1), p0 and p3 are the neighbours.
float catmullRom(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t * t + (-p0 + 3.f * p1 - 3.f * p2 + p3) * t * t * t);
}

struct PRect { ImVec2 a, b; };

// Console error -> the clip: opens its editor, puts the cursor on the line, scrolls the timeline to where the clip starts.
void gotoClipLine(int clipId, int line) {
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti) {
        Track& t = g_proj.tracks[ti];
        for (size_t ci = 0; ci < t.clips.size(); ++ci) {
            Clip& c = t.clips[ci];
            if (c.id != clipId) continue;
            c.editorOpen = true;
            g_selTrack = (int)ti; g_selClip = (int)ci;
            g_focusClipWin = clipId;
            if (line > 0) g_gotoLine[clipId] = line;
            g_scrollX = (float)std::max(0.0, c.start - 1.0);
            return;
        }
    }
}

void drawConsolePanel(ImDrawList* dl, const PRect& r, ImU32 panel, ImU32 frame, ImU32 label) {
    int errCount = 0;
    for (const LogLine& l : g_log) if (l.level == kLogError) ++errCount;
    dl->AddRectFilled(r.a, r.b, panel, S(4));
    dl->AddRect(r.a, r.b, frame, S(4));
    dl->AddText(ImVec2(r.a.x + S(6), r.a.y + S(3)), label, "CONSOLE");

    if (r.b.x - r.a.x > S(430)) {   // Problems: only the errors, each with its clip, line and timeline time (click the red tag to jump there)
        ImGui::SetCursorScreenPos(ImVec2(r.b.x - S(330), r.a.y + S(1)));
        if (errCount > 0) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.38f, 1.f));
        // Pre-click snapshot: the pop must match the push on the click frame (post-click pops corrupted the style).
        const bool problemsOn = g_logProblems;
        if (problemsOn) pushBtnOn(ImVec4(0.55f, 0.22f, 0.18f, 1.f));
        if (ImGui::SmallButton((std::string("Problems ") + std::to_string(errCount) + "##logproblems").c_str())) g_logProblems = !g_logProblems;
        if (problemsOn) ImGui::PopStyleColor(3);
        if (errCount > 0) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) helpTip("Show only the errors. Every error tells the clip, the line of its code and where the clip starts on the timeline. Click the tag to open the clip at that line.");
    }
    ImGui::SetCursorScreenPos(ImVec2(r.b.x - S(196), r.a.y + S(1)));
    if (ImGui::SmallButton("Filter")) ImGui::OpenPopup("##consolefilter");
    if (ImGui::IsItemHovered()) helpTip("How much the console tells, and from which language or clip");
    ImGui::SameLine();
    // Pre-click snapshot: the pop must match the push on the click frame (post-click pops corrupted the style).
    const bool logAutoOn = g_logAuto;
    if (logAutoOn) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
    if (ImGui::SmallButton("Auto")) g_logAuto = !g_logAuto;
    if (logAutoOn) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) helpTip("Auto-scroll: the console follows the newest line");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        g_log.clear();
        g_scErrorLogIndex = -1;
    }
    if (ImGui::BeginPopup("##consolefilter")) {
        ImGui::TextDisabled("Detail");
        ImGui::RadioButton("Important: errors, warnings, what runs", &g_logDetail, kLogImportant);
        ImGui::RadioButton("Messages: + everything the app says", &g_logDetail, kLogMessage);
        ImGui::RadioButton("Everything: + raw output of Csound", &g_logDetail, kLogDetail);
        ImGui::MenuItem("Problems only (errors with clip, line and time)", nullptr, &g_logProblems);
        ImGui::Separator();
        ImGui::TextDisabled("Clip");
        ImGui::SetNextItemWidth(S(200));
        if (caw_ui::beginCombo("##logclip", g_logClip.empty() ? "All clips" : g_logClip.c_str())) {
            if (ImGui::Selectable("All clips", g_logClip.empty())) g_logClip.clear();
            for (const Track& t : g_proj.tracks)
                for (const Clip& c : t.clips)
                    if (ImGui::Selectable(c.name.c_str(), g_logClip == c.name)) g_logClip = c.name;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(S(200));
        ImGui::InputTextWithHint("##logsearch", "search text", g_logSearch, sizeof g_logSearch);
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(r.a.x + S(6), r.a.y + S(24)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1.f));
    ImGui::BeginChild("##consolelog", ImVec2(r.b.x - r.a.x - S(12), r.b.y - r.a.y - S(30)), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    std::string needle = g_logSearch;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (size_t li = 0; li < g_log.size(); ++li) {
        const LogLine& l = g_log[li];
        if (l.level < g_logDetail || (!g_logClip.empty() && l.clip != g_logClip)) continue;
        if (g_logProblems && l.level != kLogError) continue;
        if (!needle.empty()) {
            std::string low = l.text;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find(needle) == std::string::npos) continue;
        }
        ImVec4 col(0.80f, 0.80f, 0.83f, 1.f);
        if (l.level == kLogError) col = ImVec4(0.95f, 0.40f, 0.35f, 1.f);
        else if (l.text.find("arning") != std::string::npos || l.text.find("WARNING") != std::string::npos) col = ImVec4(0.95f, 0.80f, 0.35f, 1.f);
        else if (l.level >= kLogMessage) col = ImVec4(0.55f, 0.78f, 0.95f, 1.f);
        if (!l.clip.empty()) { ImGui::TextDisabled("%s", l.clip.c_str()); ImGui::SameLine(0, S(6)); }
        if (l.clipId >= 0 && l.level >= kLogImportant) {   // clip-linked error/warning, line when known, and timeline time
            ImGui::PushID((int)li);
            const bool isError = l.level == kLogError;
            ImGui::PushStyleColor(ImGuiCol_Button, isError ? ImVec4(0.55f, 0.22f, 0.12f, 0.55f) : ImVec4(0.50f, 0.38f, 0.10f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isError ? ImVec4(0.75f, 0.30f, 0.15f, 0.85f) : ImVec4(0.72f, 0.52f, 0.12f, 0.82f));
            if (l.line > 0) {
                const std::string chip = "line " + std::string(l.lineApprox ? "~" : "") + std::to_string(l.line);
                if (ImGui::SmallButton(chip.c_str())) gotoClipLine(l.clipId, l.line);
                if (ImGui::IsItemHovered()) {
                    std::string tip = "Open clip " + l.clip;
                    tip += l.lineApprox
                        ? " at likely line " + std::to_string(l.line) + " (inferred from the engine error)"
                        : " at line " + std::to_string(l.line);
                    tip += " and show it on the timeline.";
                    if (!l.times.empty()) { tip += "\nThe clip starts at:"; for (size_t k = 0; k < l.times.size() && k < 8; ++k) tip += "\n  " + timeText(l.times[k]); }
                    if (l.playhead >= 0.0) tip += "\nPlayhead when this came: " + timeText(l.playhead);
                    helpTip("%s", tip.c_str());
                }
            } else if (!l.lineCandidates.empty()) {
                for (size_t candidateIndex = 0; candidateIndex < l.lineCandidates.size(); ++candidateIndex) {
                    const int candidate = l.lineCandidates[candidateIndex];
                    if (candidateIndex) ImGui::SameLine(0, S(3));
                    ImGui::PushID(candidate);
                    const std::string chip = "line ~" + std::to_string(candidate) + "?";
                    if (ImGui::SmallButton(chip.c_str())) gotoClipLine(l.clipId, candidate);
                    if (ImGui::IsItemHovered())
                        helpTip("Possible location only: Pure Data did not name the failing object. Open %s at line %d to inspect it.",
                                l.clip.c_str(), candidate);
                    ImGui::PopID();
                }
                ImGui::SameLine(0, S(6));
                ImGui::TextDisabled("possible");
            } else {
                if (ImGui::SmallButton("clip")) gotoClipLine(l.clipId, 0);
            }
            if (!l.times.empty()) {
                ImGui::SameLine(0, S(6));
                ImGui::TextDisabled("@ %s%s", timeText(l.times.front()).c_str(),
                    l.times.size() > 1 ? (" +" + std::to_string(l.times.size() - 1)).c_str() : "");
            }
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            ImGui::SameLine(0, S(6));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }
    if (g_logAuto && g_logDirty) ImGui::SetScrollHereY(1.0f);
    g_logDirty = false;
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Master level meter in the right panel: one bar per output channel (nchnls). Click it to reset the clip lights.
void drawMeterPanel(float dt, float width, float belowH) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.f));
    ImGui::BeginChild("meterpanel", ImVec2(width, -belowH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const int masterN = g_liveCsound ? g_engineMaster : masterChannels(g_proj.cs);
    const ImU32 panel = IM_COL32(14, 14, 16, 255), frame = IM_COL32(40, 40, 46, 255), label = IM_COL32(130, 130, 140, 255);

    if ((int)g_dispDb.size() != masterN) {
        g_dispDb.assign((size_t)masterN, -90.f);
        g_holdDb.assign((size_t)masterN, -90.f);
        g_holdT.assign((size_t)masterN, 0.f);
    }
    for (int ch = 0; ch < masterN; ++ch) {
        const float pk = (g_monAge < 0.5f && (size_t)ch < g_peakIn.size()) ? g_peakIn[(size_t)ch] : 0.f;
        const float d = 20.f * std::log10(std::max(pk, 1e-5f));
        g_dispDb[(size_t)ch] = std::max(d, g_dispDb[(size_t)ch] - 36.f * dt);
        if (d >= g_holdDb[(size_t)ch]) { g_holdDb[(size_t)ch] = d; g_holdT[(size_t)ch] = 1.5f; }
        else {
            g_holdT[(size_t)ch] -= dt;
            if (g_holdT[(size_t)ch] <= 0.f) g_holdDb[(size_t)ch] = std::max(d, g_holdDb[(size_t)ch] - 20.f * dt);
        }
    }

    {   // drag the left edge to resize the panel, double-click = automatic width again
        ImGui::SetCursorScreenPos(org);
        ImGui::InvisibleButton("##meter_resize", ImVec2(S(6), av.y));
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) g_meterW = 0.f;
        if (ImGui::IsItemActivated() && g_meterW <= 0.f) g_meterW = width / g_ui;
        if (active) g_meterW = std::min(700.f, std::max(120.f, g_meterW - ImGui::GetIO().MouseDelta.x / g_ui));
        if (active || ImGui::IsItemHovered()) dl->AddLine(ImVec2(org.x + S(2), org.y + S(6)), ImVec2(org.x + S(2), org.y + av.y - S(6)), active ? IM_COL32(237, 128, 38, 255) : IM_COL32(150, 150, 160, 255), S(2));
    }
    const ImVec2 mA(org.x + S(8), org.y + S(8)), mB(org.x + av.x - S(4), org.y + av.y - S(8));
    dl->AddRectFilled(mA, mB, panel, S(4));
    dl->AddRect(mA, mB, frame, S(4));
    const float fh = ImGui::GetTextLineHeight();
    const float gap = S(masterN > 8 ? 2.f : 8.f);
    const float bx0 = mA.x + S(34);
    const float availBars = std::max(S(8), (mB.x - mA.x) - S(34) - S(8));
    const float barW = std::max(S(2), std::min(S(20), (availBars - gap * (float)(masterN - 1)) / (float)masterN));
    const float barGroupW = barW * masterN + gap * std::max(0, masterN - 1);
    const float gainBoxW = std::min(std::max(S(42), barW * std::min(masterN, 2) + (masterN > 1 ? gap : 0.f)),
                                    std::max(S(42), mB.x - bx0));
    const bool legacyOutput = g_playing && !g_useCsoundApi;
    if (legacyOutput) ImGui::BeginDisabled();
    const float dbValue = g_proj.cs.masterGain <= 0.f ? -80.f : 20.f * std::log10(g_proj.cs.masterGain);
    int db = std::min(6, std::max(-80, (int)std::lround(dbValue)));
    ImGui::SetCursorScreenPos(ImVec2(bx0 + (barGroupW - gainBoxW) * .5f, mA.y + S(6)));
    ImGui::SetNextItemWidth(gainBoxW);
    if (ImGui::DragInt("##master_gain_db", &db, 1.f, -80, 6, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        g_proj.cs.masterGain = std::pow(10.f, db / 20.f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        helpTip(legacyOutput ? "Master gain requires CAW host audio. Restart with the Csound API." : "Master gain in whole dB: drag, or Ctrl-click to type. 0 is unity.");
    if (legacyOutput) ImGui::EndDisabled();
    ImGui::SameLine(0, S(3));
    ImGui::TextDisabled("dB");
    const float barTop = mA.y + S(12) + ImGui::GetFrameHeight(), barBot = mB.y - (fh * 2.f + S(10));
    auto dbY = [&](float d) {
        const float f = std::min(1.f, std::max(0.f, (d + 60.f) / 60.f));
        return barBot - std::pow(f, 1.8f) * std::max(0.f, barBot - barTop - S(12.f));
    };
    const bool roomForNumbers = barW + gap >= S(26), roomForLabels = barW + gap >= S(11);
    for (int ch = 0; ch < masterN; ++ch) {
        const float x0 = bx0 + ch * (barW + gap), x1 = x0 + barW;
        const bool clipped = (size_t)ch < g_meterClipHold.size() && g_meterClipHold[(size_t)ch] > 0.f;
        drawLedMeter(dl, x0, x1, barTop, barBot, g_dispDb[(size_t)ch], clipped, false, true);
        if (roomForNumbers) {
            char buf[16];
            if (g_holdDb[(size_t)ch] < -90.f) std::snprintf(buf, sizeof buf, "-inf");
            else std::snprintf(buf, sizeof buf, "%.1f", g_holdDb[(size_t)ch]);
            dl->AddText(ImVec2(x0, barBot + S(4)), IM_COL32(200, 200, 205, 255), buf);
        }
        if (roomForLabels) {
            char lab[8];
            if (masterN == 2) std::snprintf(lab, sizeof lab, "%s", ch ? "R" : "L");
            else if (masterN == 1) std::snprintf(lab, sizeof lab, "M");
            else std::snprintf(lab, sizeof lab, "%d", ch + 1);
            dl->AddText(ImVec2(x0 + S(5), barBot + S(6) + fh), label, lab);
        }
    }
    for (float tk : {0.f, -6.f, -12.f, -24.f, -48.f}) {
        const float y = dbY(tk);
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", (int)tk);
        if (y - fh * 0.5f > barTop) dl->AddText(ImVec2(mA.x + S(6), y - fh * 0.5f), label, buf);
        dl->AddLine(ImVec2(bx0 - S(4), y), ImVec2(bx0, y), label);
    }
    ImGui::SetCursorScreenPos(ImVec2(mA.x, barTop));
    if (ImGui::InvisibleButton("##meterreset", ImVec2(mB.x - mA.x, std::max(S(1), mB.y - barTop))))
        for (int ch = 0; ch < masterN && (size_t)ch < g_meterClipHold.size(); ++ch) g_meterClipHold[(size_t)ch] = 0.f;
    if (ImGui::IsItemHovered()) helpTip("%d output channel(s). Red clip LEDs hold for 1.5 seconds.", masterN);
    endChildTight();
    ImGui::PopStyleColor();
}

void drawMonitorSplitter() {
    ImGui::InvisibleButton("##monitor_splitter", ImVec2(-1, S(kMonitorSplitterH)));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImU32 color = ImGui::IsItemActive() ? IM_COL32(237, 128, 38, 255) :
                        (hovered ? IM_COL32(150, 150, 160, 255) : IM_COL32(75, 75, 84, 255));
    const float midY = (a.y + b.y) * 0.5f, midX = (a.x + b.x) * 0.5f;
    dl->AddLine(ImVec2(a.x, midY), ImVec2(b.x, midY), color, S(2));
    for (int k = -2; k <= 2; ++k) dl->AddCircleFilled(ImVec2(midX + k * S(8), midY), S(2), color);   // grip dots
    if (ImGui::IsItemActive()) {
        // the monitor may grow up to most of the window
        const float maxH = std::max(kMonitorMaxH, ImGui::GetIO().DisplaySize.y / g_ui - 230.f);
        g_monitorH = std::min(maxH, std::max(kMonitorMinH, g_monitorH - ImGui::GetIO().MouseDelta.y / g_ui));
    }
}

void drawMonitor(float dt) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.f));
    ImGui::BeginChild("monitor", ImVec2(0, S(g_monitorH)), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const int sr = g_proj.cs.sr;

    const ImU32 panel = IM_COL32(14, 14, 16, 255), frame = IM_COL32(40, 40, 46, 255);
    const ImU32 label = IM_COL32(130, 130, 140, 255);

    // ---------------- panel layout (only the visible ones, in the order the user arranged them)
    const float pad = S(8);
    const bool visible[kPanelCount] = {g_viewSpectrum, g_viewScope, g_viewConsole};
    std::vector<int> order;
    for (int k = 0; k < kPanelCount; ++k)
        if (visible[g_panelOrder[k]]) order.push_back(g_panelOrder[k]);
    const int n = (int)order.size();
    const float minW[kPanelCount] = {S(120), S(100), S(160)};
    const float flexWeight[kPanelCount] = {3.f, 2.f, 0.f};
    const float total = av.x - 2 * pad - pad * (float)std::max(0, n - 1);
    float pw[kPanelCount] = {0.f, 0.f, 0.f};
    {
        float fixedSum = 0.f, weightSum = 0.f;
        for (int id : order) {
            float fw = g_panelW[id] > 0.f ? S(g_panelW[id]) : 0.f;
            if (fw <= 0.f) {
                if (id == kPanelConsole) fw = std::max(S(260), av.x * 0.28f);
            }
            if (fw > 0.f) { pw[id] = std::max(minW[id], fw); fixedSum += pw[id]; }
            else weightSum += flexWeight[id];
        }
        const float flex = total - fixedSum;
        if (weightSum > 0.f) {
            for (int id : order) if (pw[id] <= 0.f) pw[id] = std::max(minW[id], flex * flexWeight[id] / weightSum);
        } else if (n > 0) {
            const int last = order.back();   // everything has a fixed width: the last panel takes what is left
            pw[last] = std::max(minW[last], pw[last] + flex);
        }
        float sum = 0.f;
        for (int id : order) sum += pw[id];
        if (sum > total && sum > 0.f) for (int id : order) pw[id] *= total / sum;   // window too small: shrink everything evenly
    }
    const float y0 = org.y + pad, y1 = org.y + av.y - pad;
    PRect pr[kPanelCount] = {};
    {
        float cx = org.x + pad;
        for (int id : order) {
            pr[id] = PRect{ImVec2(cx, y0), ImVec2(cx + pw[id], y1)};
            cx += pw[id] + pad;
        }
    }
    const PRect rs = pr[kPanelSpectrum], rc = pr[kPanelScope], rk = pr[kPanelConsole];

    // drag the gap between two neighbouring panels to resize them (double-click = automatic widths again)
    for (int k = 0; k + 1 < n; ++k) {
        const int ia = order[(size_t)k], ib = order[(size_t)k + 1];
        ImGui::SetCursorScreenPos(ImVec2(pr[ia].b.x, y0));
        ImGui::PushID(ia * 16 + ib + 7000);
        ImGui::InvisibleButton("##panel_split", ImVec2(pad, std::max(S(4), y1 - y0)));
        ImGui::PopID();
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            for (int id = 0; id < kPanelCount; ++id) g_panelW[id] = 0.f;
        if (ImGui::IsItemActivated())
            for (int id : order) g_panelW[id] = pw[id] / g_ui;   // freeze the current widths, then move only the two neighbours
        if (active) {
            float dx = ImGui::GetIO().MouseDelta.x / g_ui;
            dx = std::max(dx, minW[ia] / g_ui - g_panelW[ia]);
            dx = std::min(dx, g_panelW[ib] - minW[ib] / g_ui);
            g_panelW[ia] += dx;
            g_panelW[ib] -= dx;
        }
        const float lx = pr[ia].b.x + pad * 0.5f;
        dl->AddLine(ImVec2(lx, y0 + S(6)), ImVec2(lx, y1 - S(6)),
                    active ? IM_COL32(237, 128, 38, 255) : (ImGui::IsItemHovered() ? IM_COL32(150, 150, 160, 255) : IM_COL32(60, 60, 68, 255)), S(2));
    }

    // ---------------- audio-rate analyzer: FFT, waveform, or spectrogram
    if (g_viewSpectrum) {
        const ImVec2 sA = rs.a, sB = rs.b;
        dl->AddRectFilled(sA, sB, IM_COL32(18, 21, 24, 255), S(5));
        dl->AddRect(sA, sB, IM_COL32(43, 49, 53, 255), S(5));
        dl->AddText(ImVec2(sA.x + S(10), sA.y + S(5)), IM_COL32(192, 200, 202, 255),
                    g_analyzerMode == 0 ? "SPECTRUM" : g_analyzerMode == 1 ? "WAVEFORM" : "SPECTROGRAM");
        const bool spectrumAvailable = (int)g_fftDb.size() == monitor_fft::bins && (g_rawAge < 0.6f || g_specFreeze);
        if (sB.x - sA.x > S(180)) {
            const char* state = g_specFreeze ? "HOLD" : g_rawAge < 0.6f ? "LIVE" : "WAIT";
            dl->AddText(ImVec2(sB.x - S(58), sA.y + S(5)), IM_COL32(111, 181, 171, 255), state);
        }
        const float x0 = sA.x + S(36), x1 = sB.x - S(18), py0 = sA.y + S(30), py1 = sB.y - S(24);
        const float w = x1 - x0, h = py1 - py0;
        const float analysisMax = std::max(100.f, std::min(18000.f, g_rawSampleRate * 0.5f));
        const float fHi = std::max(50.f, std::min(g_specMaxHz, analysisMax));
        const float fLo = std::max(25.f, std::min(g_specMinHz, fHi - 1.f));
        const bool logarithmic = g_specScale == 0;
        auto fx = [&](float f) { return x0 + monitor_math::frequencyPosition(f, fLo, fHi, logarithmic) * w; };
        auto dy = [&](float d) { return py1 - monitor_math::clamp01((d - g_specDbFloor) / -g_specDbFloor) * h; };
        if (w > S(40) && h > S(35)) {
            ImGui::PushClipRect(sA, sB, true);
            if (g_analyzerMode == 0) {
            if (g_specGrid) {
                const int gridStep = h >= S(230) ? 12 : 24;
                for (int d = 0; d >= (int)g_specDbFloor; d -= gridStep) {
                    const float yy = dy((float)d);
                    dl->AddLine(ImVec2(x0, yy), ImVec2(x1, yy), IM_COL32(48, 55, 58, 140));
                    char txt[12]; std::snprintf(txt, sizeof txt, "%d", d);
                    dl->AddText(ImVec2(sA.x + S(6), yy - S(6)), IM_COL32(103, 115, 118, 255), txt);
                }
                static const float ticks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 18000};
                float prevX = -10000.f;
                for (float f : ticks) {
                    if (f < fLo || f > fHi) continue;
                    const float px = fx(f);
                    if (px - prevX < S(34)) continue;
                    dl->AddLine(ImVec2(px, py0), ImVec2(px, py1), IM_COL32(44, 51, 54, 120));
                    char txt[12];
                    if (f >= 1000.f) std::snprintf(txt, sizeof txt, "%gk", f / 1000.f);
                    else std::snprintf(txt, sizeof txt, "%.0f", f);
                    const float textW = ImGui::CalcTextSize(txt).x;
                    const float textX = std::max(sA.x + S(3), std::min(px - textW * 0.5f, sB.x - textW - S(3)));
                    dl->AddText(ImVec2(textX, py1 + S(4)), IM_COL32(125, 137, 139, 255), txt);
                    prevX = px;
                }
            }
            // Optional detailed FFT line. The default is narrower 1/6-octave columns.
            const int samples = std::max(16, std::min(1600, (int)w));
            std::vector<ImVec2> curve;
            if (g_specStyle == 2 && spectrumAvailable) {
              curve.reserve((size_t)samples + 1);
              for (int j = 0; j <= samples; ++j) {
                const float u = (float)j / samples;
                const float freq = monitor_math::frequencyAt(u, fLo, fHi, logarithmic);
                const float nextFreq = monitor_math::frequencyAt(std::min(1.f, (float)(j + 1) / samples), fLo, fHi, logarithmic);
                const float bin = freq * monitor_fft::fftSize / g_rawSampleRate;
                const float nextBin = nextFreq * monitor_fft::fftSize / g_rawSampleRate;
                float v = g_specDbFloor;
                if (spectrumAvailable) {
                    v = std::max(monitor_math::sample(g_fftDb.data(), monitor_fft::bins, bin, false),
                                 monitor_math::sample(g_fftDb.data(), monitor_fft::bins, nextBin, false));
                    int a = std::max(0, (int)std::ceil(bin)), b = std::min(monitor_fft::bins - 1, (int)std::floor(nextBin));
                    for (int k = a; k <= b; ++k) v = std::max(v, g_fftDb[(size_t)k]);
                }
                const float xx = x0 + u * w, yy = dy(v);
                curve.push_back(ImVec2(xx, yy));
              }
            }
            if (spectrumAvailable) {
                if (g_specStyle == 0) {
                    for (const auto& peak : g_tonePeaks) {
                        const auto bounds = monitor_peaks::visualBounds(peak);
                        if (bounds.second <= fLo || bounds.first >= fHi || peak.db <= g_specDbFloor) continue;
                        const float a = fx((float)std::max<double>(fLo, bounds.first));
                        const float b = fx((float)std::min<double>(fHi, bounds.second));
                        if (b > a) dl->AddRectFilled(ImVec2(a, dy((float)peak.db)),
                                                     ImVec2(b, py1), IM_COL32(121, 215, 192, 240));
                    }
                } else if (g_specStyle == 3) {
                    for (const auto& band : g_columnBands) {
                        if (band.hi <= fLo || band.lo >= fHi || band.db <= g_specDbFloor) continue;
                        const float a = fx((float)std::max<double>(fLo, band.lo));
                        const float b = fx((float)std::min<double>(fHi, band.hi));
                        if (b <= a) continue;
                        // Each bar fills its own 1/6-octave interval. Shared
                        // edges meet, without altering the logarithmic axis.
                        dl->AddRectFilled(ImVec2(a, dy((float)band.db)),
                                          ImVec2(b, py1), band.resolved ? IM_COL32(121, 215, 192, 240) : IM_COL32(88, 128, 126, 200));
                    }
                } else if (g_specStyle == 1 && !g_spectrumBands.empty()) {
                    // Smoothly connect measured 1/3-octave band powers. The
                    // cubic interpolation is bounded to avoid invented peaks.
                    std::vector<float> levels;
                    for (const auto& band : g_spectrumBands) levels.push_back((float)band.db);
                    std::vector<ImVec2> outline;
                    outline.reserve((size_t)samples + 1);
                    for (int j = 0; j <= samples; ++j) {
                        const float u = (float)j / samples;
                        const float freq = monitor_math::frequencyAt(u, fLo, fHi, logarithmic);
                        const float idx = (float)(3.0 * std::log(freq / g_spectrumBands.front().center) / std::log(2.0));
                        const float db = monitor_math::sample(levels.data(), (int)levels.size(), idx, true);
                        const float xx = x0 + u * w, yy = dy(db);
                        outline.push_back(ImVec2(xx, yy));
                        if (j < samples && yy < py1)
                            dl->AddRectFilled(ImVec2(xx, yy), ImVec2(x0 + (float)(j + 1) * w / samples + 0.2f, py1), IM_COL32(121, 215, 192, 170));
                    }
                    if (outline.size() > 1) dl->AddPolyline(outline.data(), (int)outline.size(), IM_COL32(160, 235, 213, 245), 0, S(1.f));
                } else if (curve.size() > 1) {
                    dl->AddPolyline(curve.data(), (int)curve.size(), IM_COL32(150, 229, 207, 245), 0, S(1.5f));
                }
                const int loBin = std::max(1, (int)std::ceil(fLo * monitor_fft::fftSize / g_rawSampleRate));
                const int hiBin = std::min(monitor_fft::bins - 2, (int)std::floor(fHi * monitor_fft::fftSize / g_rawSampleRate));
                if (hiBin >= loBin) {
                    const auto it = std::max_element(g_fftDb.begin() + loBin, g_fftDb.begin() + hiBin + 1);
                    const int peak = (int)(it - g_fftDb.begin());
                    if (*it > g_specDbFloor + 3.f) {
                        const float a = g_fftDb[(size_t)peak - 1], b = *it, c = g_fftDb[(size_t)peak + 1];
                        const float denom = a - 2 * b + c;
                        const float delta = std::abs(denom) > 1e-9f ? std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / denom)) : 0.f;
                        const float peakHz = (peak + delta) * g_rawSampleRate / monitor_fft::fftSize;
                        const float peakDb = b - 0.25f * (a - c) * delta;
                        if (g_specStyle == 2)
                            dl->AddCircleFilled(ImVec2(fx(peakHz), dy(peakDb)), S(2.5f), IM_COL32(192, 243, 227, 255));
                        char readout[80];
                        std::snprintf(readout, sizeof readout, "%s  %.1f Hz   %.1f dBFS", g_fftChannel == 0 ? "L" : g_fftChannel == 1 ? "R" : "MID", peakHz, peakDb);
                        const float tw = ImGui::CalcTextSize(readout).x;
                        if (tw + S(12) < w) dl->AddText(ImVec2(x1 - tw, py0 + S(5)), IM_COL32(166, 191, 184, 255), readout);
                    }
                }
            } else {
                dl->AddText(ImVec2(x0 + S(8), py0 + S(8)), IM_COL32(115, 129, 128, 255), "Waiting for complete audio frame");
            }
            } else if (g_analyzerMode == 1 && g_haveRaw && (g_rawAge < 0.6f || g_specFreeze)) {
                std::vector<ImVec2> points;
                points.reserve(monitor_fft::size);
                const float* waveformLeft = g_specFreeze ? g_analyzerFrozenLeft : g_rawLeft;
                const float* waveformRight = g_specFreeze ? g_analyzerFrozenRight : g_rawRight;
                for (int i = 0; i < monitor_fft::size; ++i) {
                    const float v = g_fftChannel == 1 ? waveformRight[i] : g_fftChannel == 2 ? (waveformLeft[i] + waveformRight[i]) * 0.5f : waveformLeft[i];
                    points.emplace_back(x0 + w * i / (monitor_fft::size - 1.f), (py0 + py1) * 0.5f - std::max(-1.f, std::min(1.f, v)) * h * 0.48f);
                }
                dl->AddPolyline(points.data(), (int)points.size(), IM_COL32(135, 223, 204, 240), 0, S(1));
                char duration[64]; std::snprintf(duration, sizeof duration, "%.1f ms, sample-level", 1000.0 * monitor_fft::size / g_rawSampleRate);
                dl->AddText(ImVec2(x0, py1 + S(3)), IM_COL32(103, 115, 118, 255), duration);
            } else if (g_analyzerMode == 2 && !g_fftHistory.empty() && (g_playing || g_specFreeze)) {
                const int rows = std::min(100, std::max(16, (int)h));
                for (size_t col = 0; col < g_fftHistory.size(); ++col) {
                    const float xa = x0 + w * (float)col / g_fftHistory.size();
                    const float xb = x0 + w * (float)(col + 1) / g_fftHistory.size() + 1;
                    for (int row = 0; row < rows; ++row) {
                        const float u = (float)row / rows;
                        const float hz = monitor_math::frequencyAt(u, fLo, fHi, logarithmic);
                        const float hz2 = monitor_math::frequencyAt((float)(row + 1) / rows, fLo, fHi, logarithmic);
                        const int a = std::max(0, (int)std::floor(hz * monitor_fft::fftSize / g_rawSampleRate));
                        const int b = std::min(monitor_fft::bins - 1, (int)std::ceil(hz2 * monitor_fft::fftSize / g_rawSampleRate));
                        float db = g_specDbFloor;
                        for (int k = a; k <= b; ++k) db = std::max(db, g_fftHistory[col][(size_t)k]);
                        const float q = monitor_math::clamp01((db - g_specDbFloor) / -g_specDbFloor);
                        const ImU32 color = IM_COL32((int)(q * 240), (int)(q * q * 180), (int)(q * 110), 255);
                        dl->AddRectFilled(ImVec2(xa, py1 - h * (row + 1) / rows), ImVec2(xb, py1 - h * row / rows + 1), color);
                    }
                }
                dl->AddText(ImVec2(x0, py1 + S(3)), IM_COL32(103, 115, 118, 255), "older                         now");
                char high[16], low[16];
                std::snprintf(high, sizeof high, "%.0fk", fHi / 1000.f);
                std::snprintf(low, sizeof low, "%.0f", fLo);
                dl->AddText(ImVec2(sA.x + S(4), py0), IM_COL32(145, 155, 158, 255), high);
                dl->AddText(ImVec2(sA.x + S(4), py1 - S(12)), IM_COL32(145, 155, 158, 255), low);
            }
            ImGui::PopClipRect();
        }
        ImGui::SetCursorScreenPos(ImVec2(sA.x, sA.y + S(22)));
        ImGui::InvisibleButton("##specmenu", ImVec2(std::max(1.f, sB.x - sA.x), std::max(1.f, sB.y - sA.y - S(22))));
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (g_analyzerMode == 0 && spectrumAvailable && ImGui::IsItemHovered() &&
            mouse.x >= x0 && mouse.x <= x1 && mouse.y >= py0 && mouse.y <= py1) {
            const float freq = monitor_math::frequencyAt((mouse.x - x0) / w, fLo, fHi, logarithmic);
            dl->AddLine(ImVec2(mouse.x, py0), ImVec2(mouse.x, py1), IM_COL32(165, 190, 182, 105));
            if (g_specStyle == 0) {
                const monitor_peaks::Peak* closest = nullptr;
                double distance = 1e30;
                for (const auto& peak : g_tonePeaks) {
                    const auto bounds = monitor_peaks::visualBounds(peak);
                    if (freq < bounds.first || freq > bounds.second) continue;
                    const double d = std::abs(std::log(freq / peak.frequency));
                    if (d < distance) { distance = d; closest = &peak; }
                }
                if (closest) ImGui::SetTooltip("%.1f Hz  |  %.1f dBFS\nEstimated tonal peak; visual width is not measured bandwidth.",
                                               closest->frequency, closest->db);
            } else if (g_specStyle == 3) {
                const auto found = std::find_if(g_columnBands.begin(), g_columnBands.end(),
                    [freq](const monitor_bands::Band& band) { return freq >= band.lo && freq < band.hi; });
                if (found != g_columnBands.end())
                    ImGui::SetTooltip("%.1f Hz band  |  %.1f dBFS\n%.1f to %.1f Hz; peak-sine reference%s", found->center, found->db, found->lo, found->hi,
                                  found->resolved ? "" : "\nNarrower than the analyzer resolves: level is for the whole window lobe.");
            } else if (g_specStyle == 1 && !g_spectrumBands.empty()) {
                std::vector<float> levels;
                for (const auto& band : g_spectrumBands) levels.push_back((float)band.db);
                const float idx = (float)(3.0 * std::log(freq / g_spectrumBands.front().center) / std::log(2.0));
                const float db = monitor_math::sample(levels.data(), (int)levels.size(), idx, true);
                ImGui::SetTooltip("%.1f Hz   %.1f dBFS\nInterpolated 1/3-octave band-energy shape", freq, db);
            } else {
                const float bin = freq * monitor_fft::fftSize / g_rawSampleRate;
                const float db = monitor_math::sample(g_fftDb.data(), monitor_fft::bins, bin, false);
                ImGui::SetTooltip("%.1f Hz   %.1f dBFS\nHann-window FFT sample", freq, db);
            }
        }
        if (ImGui::BeginPopupContextItem("##specmenu_popup")) {
            ImGui::TextDisabled("AUDIO ANALYZER");
            ImGui::Separator();
            if (g_analyzerMode == 0) {
                if (ImGui::MenuItem("Tonal peaks (smooth position, estimated)", nullptr, g_specStyle == 0)) { g_specStyle = 0; updateSpectrumDerived(); }
                if (ImGui::MenuItem("Measured band-energy columns", nullptr, g_specStyle == 3)) { g_specStyle = 3; updateSpectrumDerived(); }
                if (ImGui::MenuItem("Combined shape", nullptr, g_specStyle == 1)) { g_specStyle = 1; updateSpectrumDerived(); }
                if (ImGui::MenuItem("Detailed FFT line", nullptr, g_specStyle == 2)) g_specStyle = 2;
                ImGui::Separator();
            }
            bool hold = g_specFreeze;
            if (ImGui::MenuItem("Hold display", nullptr, &hold)) {
                if (hold) {
                    std::copy(g_rawLeft, g_rawLeft + monitor_fft::size, g_analyzerFrozenLeft);
                    std::copy(g_rawRight, g_rawRight + monitor_fft::size, g_analyzerFrozenRight);
                    if (g_haveRaw) { analyzeCurrentRaw(); updateSpectrumDerived(); }
                }
                g_specFreeze = hold;
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Spectrum", nullptr, g_analyzerMode == 0)) { g_analyzerMode = 0; updateSpectrumDerived(); }
                if (ImGui::MenuItem("Waveform", nullptr, g_analyzerMode == 1)) g_analyzerMode = 1;
                if (ImGui::MenuItem("Spectrogram", nullptr, g_analyzerMode == 2)) { if (g_analyzerMode != 2) g_fftHistory.clear(); g_analyzerMode = 2; }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Fast response (512 samples, coarse)", nullptr, g_specFast)) {
                g_specFast = !g_specFast;
                if (g_haveRaw && !g_specFreeze) { analyzeCurrentRaw(); updateSpectrumDerived(); }
                g_fftHistory.clear();
            }
            ImGui::BeginDisabled(g_specFreeze);
            if (ImGui::BeginMenu("Input")) {
                if (ImGui::MenuItem("Left", nullptr, g_fftChannel == 0)) { g_fftChannel = 0; g_fftHistory.clear(); g_fftDb.clear(); g_spectrumBands.clear(); g_columnBands.clear(); g_tonePeaks.clear(); }
                if (ImGui::MenuItem("Right", nullptr, g_fftChannel == 1)) { g_fftChannel = 1; g_fftHistory.clear(); g_fftDb.clear(); g_spectrumBands.clear(); g_columnBands.clear(); g_tonePeaks.clear(); }
                if (ImGui::MenuItem("Mid (L + R) / 2", nullptr, g_fftChannel == 2)) { g_fftChannel = 2; g_fftHistory.clear(); g_fftDb.clear(); g_spectrumBands.clear(); g_columnBands.clear(); g_tonePeaks.clear(); }
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            if (ImGui::BeginMenu("Frequency scale")) {
                if (ImGui::MenuItem("Logarithmic", nullptr, g_specScale == 0)) g_specScale = 0;
                if (ImGui::MenuItem("Linear", nullptr, g_specScale == 1)) g_specScale = 1;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Frequency range")) {
                ImGui::SetNextItemWidth(S(170));
                ImGui::SliderFloat("Min Hz", &g_specMinHz, 25.f, 2000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
                ImGui::SetNextItemWidth(S(170));
                ImGui::SliderFloat("Max Hz", &g_specMaxHz, 1000.f, 18000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
                g_specMinHz = std::min(g_specMinHz, g_specMaxHz - 1.f);
                ImGui::TextDisabled("Available up to %.0f Hz", analysisMax);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Level range")) {
                for (int floor : {-60, -90, -120}) {
                    char text[24]; std::snprintf(text, sizeof text, "%d to 0 dB", floor);
                    if (ImGui::MenuItem(text, nullptr, g_specDbFloor == (float)floor)) g_specDbFloor = (float)floor;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Appearance")) {
                ImGui::MenuItem("Grid", nullptr, &g_specGrid);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            const int lenShown = g_specFast ? monitor_fft::fastSize : monitor_fft::size;
            ImGui::TextDisabled("%d-sample Hann, zero padded to %d.", lenShown, monitor_fft::fftSize);
            ImGui::TextDisabled("Observation spacing %.2f Hz; zero padding does not improve resolution.", (float)g_rawSampleRate / lenShown);
            ImGui::TextDisabled("A new sound fades in over about %.0f ms; quiet high partials appear later.", 1000.0 * lenShown / g_rawSampleRate * 0.6);
            ImGui::TextDisabled("Tonal peaks are estimates; use band energy for unsuppressed data.");
            ImGui::EndPopup();
        }
    }

    // ---------------- signal: minimal multi-channel monitor
    if (g_viewScope) {
        const ImVec2 cA = rc.a, cB = rc.b;
        dl->AddRectFilled(cA, cB, IM_COL32(18, 21, 24, 255), S(5));
        dl->AddRect(cA, cB, IM_COL32(43, 49, 53, 255), S(5));
        dl->AddText(ImVec2(cA.x + S(10), cA.y + S(5)), IM_COL32(192, 200, 202, 255), "SIGNAL");
        if (cB.x - cA.x > S(150)) {
            const char* state = g_scopeFreeze ? "HOLD" : g_rawAge >= 0.6f ? "WAIT" : g_scopeDisplay ? "TRIG" : "LIVE";
            dl->AddText(ImVec2(cB.x - S(58), cA.y + S(5)), IM_COL32(111, 181, 171, 255), state);
        }
        const float x0 = cA.x + S(10), x1 = cB.x - S(10), py0 = cA.y + S(28), py1 = cB.y - S(20);
        const float w = x1 - x0, mid = (py0 + py1) * 0.5f, hh = (py1 - py0) * 0.47f;
        if (w > S(35) && hh > S(10)) {
            if (g_scopeGrid) {
                for (int i = 0; i <= 4; ++i) {
                    const float xx = x0 + w * (float)i / 4.f;
                    dl->AddLine(ImVec2(xx, py0), ImVec2(xx, py1), IM_COL32(44, 51, 54, 125));
                }
                for (int i = -1; i <= 1; ++i) {
                    const float yy = mid + hh * 0.5f * (float)i;
                    dl->AddLine(ImVec2(x0, yy), ImVec2(x1, yy),
                                i == 0 ? IM_COL32(68, 79, 81, 180) : IM_COL32(44, 51, 54, 125));
                }
            }
            const bool available = g_scopeFreeze ? g_scopeFrozenHasFrame : (g_haveRaw && g_rawAge < 0.6f);
            const float* left = g_scopeFreeze ? g_rawFrozenLeft : g_rawLeft;
            const float* right = g_scopeFreeze ? g_rawFrozenRight : g_rawRight;
            if (available) {
                const auto window = monitor_math::scopeWindow(left, right, monitor_fft::size, g_rawSampleRate,
                    (float)g_scopeMs, g_scopeDisplay == 1, g_scopeChannels);
                const int count = window.samples - 1;
                ImGui::PushClipRect(ImVec2(x0, py0), ImVec2(x1, py1), true);
                const int traces = g_scopeChannels == 0 ? 2 : 1;
                for (int trace = 0; trace < traces; ++trace) {
                    const int ch = g_scopeChannels == 0 ? 1 - trace : g_scopeChannels == 2 ? 1 : 0;
                    std::vector<ImVec2> points;
                    points.reserve((size_t)count + 1);
                    for (int i = 0; i <= count; ++i) {
                        const float u = (float)i / count;
                        const float pos = window.start + u * count;
                        const float l = monitor_math::sample(left, monitor_fft::size, pos, false);
                        const float r = monitor_math::sample(right, monitor_fft::size, pos, false);
                        const float v = g_scopeChannels == 3 ? (l + r) * 0.5f : ch == 0 ? l : r;
                        points.emplace_back(x0 + u * w, mid - std::max(-1.f, std::min(1.f, v)) * hh);
                    }
                    const ImU32 color = ch == 0 ? IM_COL32(135, 223, 204, 235) : IM_COL32(241, 167, 112, 195);
                    const ImU32 glow = ch == 0 ? IM_COL32(135, 223, 204, 30) : IM_COL32(241, 167, 112, 25);
                    dl->AddPolyline(points.data(), (int)points.size(), glow, 0, S(4.f));
                    dl->AddPolyline(points.data(), (int)points.size(), color, 0, S(1.5f));
                }
                ImGui::PopClipRect();
                char timeText[32];
                const float actual = 1000.f * (window.samples - 1.f) / g_rawSampleRate;
                if (actual >= 1000.f) std::snprintf(timeText, sizeof timeText, "%.2f s", actual / 1000.f);
                else std::snprintf(timeText, sizeof timeText, "%.1f ms", actual);
                dl->AddText(ImVec2(x0, py1 + S(3)), IM_COL32(103, 115, 118, 255), timeText);
                if (g_scopeDisplay == 1 && !window.triggered && w > S(125)) {
                    const char* text = "no trigger";
                    dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(text).x, py1 + S(3)), IM_COL32(103, 115, 118, 255), text);
                }
            } else {
                dl->AddText(ImVec2(x0 + S(6), mid - S(7)), IM_COL32(101, 113, 116, 255), "Waiting for signal");
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(cA.x, cA.y + S(22)));
        ImGui::InvisibleButton("##scopemenu", ImVec2(std::max(1.f, cB.x - cA.x), std::max(1.f, cB.y - cA.y - S(22))));
        if (ImGui::BeginPopupContextItem("##scopemenu_popup")) {
            ImGui::TextDisabled("SIGNAL");
            ImGui::Separator();
            bool freeze = g_scopeFreeze;
            if (ImGui::MenuItem("Hold display", nullptr, &freeze)) {
                if (freeze) {
                    std::copy(g_rawLeft, g_rawLeft + monitor_fft::size, g_rawFrozenLeft);
                    std::copy(g_rawRight, g_rawRight + monitor_fft::size, g_rawFrozenRight);
                    g_scopeFrozenHasFrame = g_haveRaw && g_rawAge < 0.6f;
                }
                g_scopeFreeze = freeze;
            }
            if (ImGui::BeginMenu("Display")) {
                if (ImGui::MenuItem("Free-running sweep", nullptr, g_scopeDisplay == 0)) g_scopeDisplay = 0;
                if (ImGui::MenuItem("Triggered frames", nullptr, g_scopeDisplay == 1)) g_scopeDisplay = 1;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Channels")) {
                if (ImGui::MenuItem("Stereo overlay", nullptr, g_scopeChannels == 0)) g_scopeChannels = 0;
                if (ImGui::MenuItem("Left", nullptr, g_scopeChannels == 1)) g_scopeChannels = 1;
                if (ImGui::MenuItem("Right", nullptr, g_scopeChannels == 2)) g_scopeChannels = 2;
                if (ImGui::MenuItem("Mid (L + R)", nullptr, g_scopeChannels == 3)) g_scopeChannels = 3;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Time window")) {
                for (int ms : {2, 4, 6, 10, 20, 40}) {
                    char text[32]; std::snprintf(text, sizeof text, "%d ms", ms);
                    if (ImGui::MenuItem(text, nullptr, g_scopeMs == ms)) g_scopeMs = ms;
                }
                ImGui::TextDisabled("Max %.1f ms at this sample rate.", 1000.f * monitor_fft::size / g_rawSampleRate);
                ImGui::EndMenu();
            }
            ImGui::MenuItem("Grid", nullptr, &g_scopeGrid);
            ImGui::Separator();
            ImGui::TextDisabled("2048 consecutive audio-rate samples; up to 30 frames/s.");
            ImGui::EndPopup();
        }
    }

    // ---------------- console
    if (g_viewConsole) drawConsolePanel(dl, rk, panel, frame, label);

    if (n == 0) {
        dl->AddText(ImVec2(org.x + pad, org.y + pad), label, "All panels are hidden - use View to show them.");
    }

    // drag a panel title onto another panel to change the order
    {
        int dragFrom = -1, dragTo = -1;
        for (int id : order) {
            ImGui::SetCursorScreenPos(ImVec2(pr[id].a.x, pr[id].a.y));
            ImGui::PushID(id + 9000);
            const float panelWidth = pr[id].b.x - pr[id].a.x;
            const float gripW = id == kPanelConsole ? std::max(S(10), std::min(S(86), panelWidth - S(156))) : std::min(S(86), panelWidth);   // keep the console's Auto / Clear reachable
            ImGui::InvisibleButton("##panel_title", ImVec2(gripW, S(20)));
            ImGui::PopID();
            if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); helpTip("Drag the title onto another panel to change the order"); }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload("CAW_PANEL", &id, sizeof(int));
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CAW_PANEL")) { dragFrom = *(const int*)payload->Data; dragTo = id; }
                ImGui::EndDragDropTarget();
            }
        }
        if (dragFrom >= 0 && dragTo >= 0 && dragFrom != dragTo) {
            int posFrom = 0, posTo = 0;
            for (int k = 0; k < kPanelCount; ++k) { if (g_panelOrder[k] == dragFrom) posFrom = k; if (g_panelOrder[k] == dragTo) posTo = k; }
            const int moved = g_panelOrder[posFrom];
            if (posFrom < posTo) for (int k = posFrom; k < posTo; ++k) g_panelOrder[k] = g_panelOrder[k + 1];
            else for (int k = posFrom; k > posTo; --k) g_panelOrder[k] = g_panelOrder[k - 1];
            g_panelOrder[posTo] = moved;
        }
    }

    endChildTight();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- settings window
bool intCombo(const char* label, int& v, std::initializer_list<int> opts) {
    std::string cur = std::to_string(v);
    bool changed = false;
    if (caw_ui::beginCombo(label, cur.c_str())) {
        for (int o : opts) {
            std::string s = std::to_string(o);
            if (ImGui::Selectable(s.c_str(), o == v)) { v = o; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool nameHasTag(const std::string& name, const std::string& tag) {   // case-insensitive substring; empty tag matches everything
    if (tag.empty()) return true;
    auto low = [](std::string t) { for (char& c : t) c = (char)std::tolower((unsigned char)c); return t; };
    return low(name).find(low(tag)) != std::string::npos;
}

bool devCombo(const char* label, int& idx, const std::vector<AudioDevice>& list, const std::string& tag = std::string()) {
    std::string current = idx == -2 ? "None" : idx < 0 ? "Default" : "Missing device " + std::to_string(idx);
    for (const AudioDevice& device : list) if (device.id == idx) current = std::to_string(device.id) + ": " + device.name;
    bool changed = false;
    ImGui::SetNextItemWidth(std::max(S(260), ImGui::GetContentRegionAvail().x - S(8)));   // device names are long: use the whole row
    if (caw_ui::beginCombo(label, current.c_str())) {
        if (ImGui::Selectable("None", idx == -2)) { idx = -2; changed = true; }
        if (ImGui::Selectable("Default", idx == -1)) { idx = -1; changed = true; }
        for (const AudioDevice& device : list) {
            if (!cawDeviceHasHostApi(device.name, tag) && device.id != idx) continue;
            ImGui::PushID(device.id);
            const std::string labelText = std::to_string(device.id) + ": " + device.name;
            if (ImGui::Selectable(labelText.c_str(), idx == device.id)) { idx = device.id; changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// ---- engine settings as typed text: ONE box holds the Csound options line and the header values
//      -odac -+rtaudio=portaudio -b1024 -B4096 -d
//      sr = 48000
//      ksmps = 32 ...
std::string engineSettingsToText(const CsoundSettings& s) {
    std::ostringstream o;
    o << (s.options.empty() ? defaultOptions(s) : s.options) << "\n";
    o << "sr = " << s.sr << "\nksmps = " << s.ksmps << "\nnchnls = " << s.nchnls << "\n";
    if (s.nchnlsIn > 0) o << "nchnls_i = " << s.nchnlsIn << "\n";
    o << "0dbfs = " << s.zdbfs << "\nA4 = " << s.a4 << "\n";
    return o.str();
}

// Reads one "name = number" header line. Option lines (they start with '-') are handled separately.
bool parseEngineLine(const std::string& raw, std::string& key, double& value) {
    std::string line = raw.substr(0, raw.find(';'));
    auto trim = [](std::string v) {
        const size_t a = v.find_first_not_of(" \t\r");
        if (a == std::string::npos) return std::string();
        return v.substr(a, v.find_last_not_of(" \t\r") - a + 1);
    };
    line = trim(line);
    if (line.empty()) return false;
    const size_t split = line.find('=');
    if (split == std::string::npos) return false;
    key = trim(line.substr(0, split));
    std::string rest = trim(line.substr(split + 1));
    if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
    char* end = nullptr;
    value = std::strtod(rest.c_str(), &end);
    return end && end != rest.c_str() && *end == '\0';
}

bool isOptionLine(const std::string& raw) {
    const std::string code = raw.substr(0, raw.find(';'));
    const size_t a = code.find_first_not_of(" \t\r");
    return a != std::string::npos && code[a] == '-';
}

void applyEngineText(CsoundSettings& s, const std::string& text) {
    std::istringstream in(text);
    std::string raw, key, options;
    double v = 0.0, kr = 0.0;
    bool sawKsmps = false;
    while (std::getline(in, raw)) {
        if (isOptionLine(raw)) {   // every line starting with '-' belongs to the Csound options
            std::string code = raw.substr(0, raw.find(';'));
            const size_t a = code.find_first_not_of(" \t\r"), b = code.find_last_not_of(" \t\r");
            options += (options.empty() ? "" : " ") + code.substr(a, b - a + 1);
            continue;
        }
        if (!parseEngineLine(raw, key, v)) continue;
        if (key == "sr") s.sr = std::min(384000, std::max(1000, (int)v));
        else if (key == "ksmps") { s.ksmps = std::min(4096, std::max(1, (int)v)); sawKsmps = true; }
        else if (key == "kr") kr = v;
        else if (key == "nchnls") s.nchnls = std::min(64, std::max(1, (int)v));
        else if (key == "nchnls_i") s.nchnlsIn = std::min(64, std::max(0, (int)v));
        else if (key == "0dbfs") s.zdbfs = std::max(0.0001, v);
        else if (key == "A4") s.a4 = std::min(2000.0, std::max(20.0, v));
    }
    s.options = options;                 // empty = the generated default
    if (!options.empty()) syncSettingsFromOptions(s);   // keep the device / backend / buffer fields in step with what was typed
    if (kr > 0.0 && !sawKsmps) s.ksmps = std::min(4096, std::max(1, (int)std::lround((double)s.sr / kr)));
}

std::string engineTextProblems(const std::string& text) {
    static const std::set<std::string> known = {"sr", "ksmps", "kr", "nchnls", "nchnls_i", "0dbfs", "A4"};
    std::istringstream in(text);
    std::string raw, key, out;
    double v = 0.0;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string code = raw.substr(0, raw.find(';'));
        if (code.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (isOptionLine(raw)) continue;
        if (!parseEngineLine(raw, key, v)) out += "line " + std::to_string(lineNo) + ": expected  name = number  or a Csound option starting with -\n";
        else if (!known.count(key)) out += "line " + std::to_string(lineNo) + ": unknown setting '" + key + "'\n";
    }
    return out;
}

void drawSettings() {
    if (!g_settingsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(600), S(560)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(260), S(90)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Settings", &g_settingsOpen)) {
        CsoundSettings& s = g_proj.cs;
        const float labelW = S(170);
        auto row = [&](const char* name) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name);
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(S(260));
        };
        const ImVec4 head(0.95f, 0.60f, 0.20f, 1.f);
        if (ImGui::BeginTabBar("##settings_tabs")) {

            if (ImGui::BeginTabItem("General", nullptr, g_settingsGoto == 6 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "AUTOSAVE");
                ImGui::Checkbox("Autosave recovery copy", &s.autosaveEnabled);
                row("Every (minutes)");
                ImGui::SetNextItemWidth(S(100));
                if (ImGui::InputInt("##autosave_minutes", &s.autosaveMinutes, 1, 5))
                    s.autosaveMinutes = std::min(120, std::max(1, s.autosaveMinutes));
                ImGui::TextWrapped("A separate recovery file is saved next to the project. Unsaved projects use the CAW Autosaves folder in your home directory. The original project is never overwritten.");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Audio: sound card, sample rate, buffers, latency
            if (ImGui::BeginTabItem("Audio", nullptr, g_settingsGoto == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!g_audioDevicesReady && !g_pdEngineOn && !g_playing) refreshAudioDevices(false);
                ImGui::TextColored(head, "SOUND CARD");
                if (s.rtaudio != "portaudio") {   // one audio system only: PortAudio (ASIO / WASAPI / DirectSound ...)
                    s.rtaudio = "portaudio";
                    s.outDev = s.inDev = -1;
                    syncOptionsFromSettings(s);
                    refreshAudioDevices(g_languageChoiceMade && g_langCsound);
                }
                // Like Cabbage: first the device TYPE (ASIO / WASAPI / Core Audio / ALSA ...), then only the devices of that type.
                // EVERY driver type of this operating system is always listed (even when Csound's PortAudio found no device for it), so none is missing.
                struct DriverType { const char* name; const char* tag; bool win, mac, lin; };
                static const DriverType kDrivers[] = {
                    {"All",         "",            true,  true,  true },
                    {"ASIO",        "ASIO",        true,  false, false},
                    {"WASAPI",      "WASAPI",      true,  false, false},
                    {"DirectSound", "DirectSound", true,  false, false},
                    {"MME",         "MME",         true,  false, false},
                    {"WDM-KS",      "WDM-KS",      true,  false, false},
                    {"Core Audio",  "Core Audio",  false, true,  false},
                    {"ALSA",        "ALSA",        false, false, true },
                    {"JACK",        "JACK",        false, false, true },
                    {"OSS",         "OSS",         false, false, true },
                };
                static std::vector<int> osDrivers;   // indices into kDrivers that exist on this OS
                if (osDrivers.empty())
                    for (int i = 0; i < (int)(sizeof(kDrivers) / sizeof(kDrivers[0])); ++i)
                        if (plat::isWindows ? kDrivers[i].win : plat::isMac ? kDrivers[i].mac : kDrivers[i].lin) osDrivers.push_back(i);
                const int driverCount = (int)(sizeof(kDrivers) / sizeof(kDrivers[0]));
                static int devType = -1;   // index into kDrivers; 0 = All
                bool driverHasDevices[16] = {true};
                for (const AudioDevice& d : g_outDevs)
                    for (int t = 1; t < driverCount; ++t)
                        if (cawDeviceHasHostApi(d.name, kDrivers[t].tag)) driverHasDevices[t] = true;
                // Windows: the Windows default card lives in WASAPI. Other systems start with All (PortAudio's default device).
                const int defaultType = plat::isWindows ? 2 : 0;
                if (devType < 0) {
                    devType = (s.outDev < 0 && driverHasDevices[defaultType]) ? defaultType : 0;
                    for (const AudioDevice& d : g_outDevs)
                        if (d.id == s.outDev) for (int t = 1; t < driverCount; ++t)
                            if (cawDeviceHasHostApi(d.name, kDrivers[t].tag)) devType = t;
                }
                if (s.rtaudio == "portaudio") {
                    row("Audio device type");
                    auto typeLabel = [&](int t) { return std::string(kDrivers[t].name) + (t > 0 && !driverHasDevices[t] ? "  (no devices found)" : ""); };
                    if (caw_ui::beginCombo("##devtype", kDrivers[devType].name)) {
                        for (int t : osDrivers) {
                            if (!ImGui::Selectable(typeLabel(t).c_str(), devType == t)) continue;
                            devType = t;
                            const std::string tag = kDrivers[t].tag;
                            const bool isSystemDefaultType = plat::isWindows && tag == "WASAPI";   // -1 follows the Windows default card
                            bool okOut = s.outDev < 0 && (isSystemDefaultType || t == 0), okIn = s.inDev < 0;
                            for (const AudioDevice& d : g_outDevs) if (d.id == s.outDev && cawDeviceHasHostApi(d.name, tag)) okOut = true;
                            for (const AudioDevice& d : g_inDevs) if (d.id == s.inDev && cawDeviceHasHostApi(d.name, tag)) okIn = true;
                            if (t > 0 && !okOut) s.outDev = isSystemDefaultType ? -1 : pickDefaultDevice(g_outDevs, tag, false);
                            if (t > 0 && !okIn) s.inDev = isSystemDefaultType ? -1 : pickDefaultDevice(g_inDevs, tag, true);
                            if (tag == "ASIO") { s.bufHW = std::max(1024, s.bufSW * 4); }
                            syncOptionsFromSettings(s);
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) helpTip("Choose an available audio driver. Refresh the device list after connecting a soundcard.");
                    if (devType > 0 && !driverHasDevices[devType])
                        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "CAW found no %s output devices. Check the driver or choose All.", kDrivers[devType].name);
                }
                const std::string devTag = s.rtaudio == "portaudio" ? std::string(kDrivers[devType].tag) : std::string();
                const bool asioMode = s.rtaudio == "portaudio" && std::string(kDrivers[devType].tag) == "ASIO";
                row("Output device");
                if (devCombo("##out", s.outDev, g_outDevs, devTag)) syncOptionsFromSettings(s);
                if (ImGui::IsItemHovered()) helpTip("The device that plays the sound (-odac). System default or None are possible.");
                if (g_langCsound) {
                    row("Input device");
                    if (devCombo("##in", s.inDev, g_inDevs, devTag)) syncOptionsFromSettings(s);
                    if (ImGui::IsItemHovered()) helpTip("Csound recording input, used by instruments such as inch / in.");
                } else if (g_langPd) ImGui::TextDisabled("PD currently supports output only.");
                row("");
                if (ImGui::Button("Refresh device list")) refreshAudioDevices(g_languageChoiceMade && g_langCsound);
                row("Test sound");
                const bool soundTestBusy = g_testSound.running() || g_playing || g_pdEngineOn || g_pdPreview.active() || g_scEngineOn;
                if (soundTestBusy) ImGui::BeginDisabled();
                if (ImGui::Button("Play test sound")) {
                    int selectedId = s.outDev;
                    if (selectedId == -1 && plat::isWindows) selectedId = pickDefaultDevice(g_outDevs, "WASAPI", false);   // elsewhere: PortAudio default (empty label)
                    std::string selected;
                    if (selectedId >= 0) {
                        selected = "<selected device missing>";
                        for (const auto& device : g_outDevs)
                            if (device.id == selectedId) { selected = device.name; break; }
                    }
                    g_testSound.start(selected, s.outDev == -2, s.sr, s.nchnls, s.bufSW, s.bufHW);
                }
                if (soundTestBusy) ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) helpTip("Plays a quiet tone directly on the selected output. Stop other playback first if the device is busy.");
                ImGui::TextWrapped("Tests the selected output directly. Stop other playback first if the device is busy.");
                const std::string testMessage = g_testSound.message();
                if (!testMessage.empty()) ImGui::TextWrapped("%s", testMessage.c_str());

                ImGui::Spacing();
                ImGui::TextColored(head, "ENGINE");
                row("Sample rate");
                {
                    static const int rates[] = {44100, 48000, 88200, 96000, 192000};
                    if (caw_ui::beginCombo("##sr", (std::to_string(s.sr) + " Hz").c_str())) {
                        for (int r : rates) if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), s.sr == r)) s.sr = r;
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) helpTip("Use the sample rate of your sound card / ASIO driver (see its control panel). A different rate makes the system or the driver convert, which can crackle.");
                }
                if (g_langCsound) {
                    row("Control period (ksmps)");
                    ImGui::InputInt("##ksmps", &s.ksmps, 0, 0);
                    s.ksmps = std::min(4096, std::max(1, s.ksmps));
                    row("Output channels (nchnls)");
                    ImGui::InputInt("##nchnls", &s.nchnls, 1, 1);
                    s.nchnls = std::min(64, std::max(1, s.nchnls));
                    ImGui::TextDisabled("kr = sr / ksmps = %.1f Hz", (double)s.sr / std::max(1, s.ksmps));
                } else if (g_langPd) ImGui::TextDisabled("PD: stereo output, 64-sample DSP ticks.");

                ImGui::Spacing();
                ImGui::TextColored(head, "BUFFER AND LATENCY");
                {
                    row("Audio buffer size");
                    {
                        static const int sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
                        const double srv = (double)std::max(1000, s.sr);
                        char cur[64];
                        std::snprintf(cur, sizeof cur, "%d samples (%.1f ms)", s.bufSW, 1000.0 * s.bufSW / srv);
                        if (caw_ui::beginCombo("##bufsize", cur)) {
                            for (int n : sizes) {
                                char item[64];
                                std::snprintf(item, sizeof item, "%d samples (%.1f ms)", n, 1000.0 * n / srv);
                                if (ImGui::Selectable(item, s.bufSW == n)) { s.bufSW = n; s.bufHW = std::max(1024, n * 4);   /* Csound hardware buffer: at least 4 periods, 2 periods crackles */ syncOptionsFromSettings(s); }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered()) helpTip("Like the buffer size in Cabbage: ONE number. With ASIO choose the buffer size of your ASIO driver (its control panel); the hardware buffer is set to twice that.\nSmaller = less latency but more risk of crackling.");
                    }
                    row("Sound sync (ms)");
                    ImGui::SliderInt("##syncms", &s.syncMs, -100, 400, "%d ms");
                    if (ImGui::IsItemHovered()) helpTip("The timeline and the meters are moved by the output latency so that you hear a note when the playhead reaches it.\nASIO drivers often have more latency than the buffer size says: raise this until the sound meets the playhead. Also moves the meters.");
                    const double srv = (double)std::max(1000, s.sr);
                    const double outMs = 1000.0 * (double)s.bufHW / srv;
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.f), "Output latency about %.1f ms (+ %d ms sync)", outMs, s.syncMs);
                    {
                        char btn[96];
                        std::snprintf(btn, sizeof btn, "Apply %.0f ms latency to the sync slider", outMs);
                        if (ImGui::Button(btn)) s.syncMs = std::min(400, std::max(-100, (int)std::lround(outMs)));
                        if (ImGui::IsItemHovered()) helpTip("Copies the output latency shown above into the Sound sync slider.");
                    }
                    if (asioMode) ImGui::TextDisabled("ASIO: ONE buffer size. It must be the buffer of the ASIO driver itself (its control panel, or Voicemeeter's ASIO buffer setting). -B (hardware ring) is set to 4x that automatically.");
                    if (!asioMode && caw_ui::treeNode("Advanced buffers (-b / -B)")) {
                        row("Software buffer (-b)");
                        if (ImGui::InputInt("##bufsw", &s.bufSW, 64, 256)) { s.bufSW = std::min(16384, std::max(16, s.bufSW)); syncOptionsFromSettings(s); }
                        row("Hardware buffer (-B)");
                        if (ImGui::InputInt("##bufhw", &s.bufHW, 256, 1024)) { s.bufHW = std::min(65536, std::max(16, s.bufHW)); syncOptionsFromSettings(s); }
                        ImGui::TextDisabled("Input latency about %.1f ms | round trip %.1f ms", outMs, outMs * 2.0);
                        ImGui::TreePop();
                    }
                    if (s.bufHW < s.bufSW) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.f), "-B should be at least as large as -b");
                    else if (!asioMode && s.bufHW % std::max(1, s.bufSW) != 0) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "-B should be a multiple of -b (e.g. 2 times), otherwise the sound can glitch");
                    if (g_langCsound) {
                        if (s.bufSW < s.ksmps) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.f), "-b should be at least ksmps");
                        else if (s.bufSW % std::max(1, s.ksmps) != 0) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "-b should be a multiple of ksmps");
                    }
                }
                if (g_langCsound) {
                    ImGui::Spacing();
                    ImGui::Checkbox("Send meter data from Csound (master, scope, spectrum, track meters)", &s.monitorFeed);
                    if (ImGui::IsItemHovered()) helpTip("Adds a small monitor instrument to the engine. Untick it if Csound reports an error about it, or to give a slow computer more room (less crackling). Applies on the next Run.");
                }
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Programs
            if (ImGui::BeginTabItem("Programs", nullptr, g_settingsGoto == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "PROGRAMS");
                if (g_langCsound) {
                    static std::string csTestResult, csShownTyped, csShownExe;
                    ImGui::Spacing();
                    row("Csound");
                    if (ImGui::Button("Browse##csoundexe")) {
                        // The dialog opens in CAW's own folder: picking the csound.exe there
                        // (e.g. <CAW folder>\Csound\bin\csound.exe) is one click.
                        const auto path = plat::chooseFile(false, "Choose csound.exe", "Csound program", {"exe"}, plat::cawOwnFolder());
                        if (!path.empty()) { s.csoundExe = path; csTestResult.clear(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Test##csoundexe")) {
                        // The same check the engine runs before Play: load the Csound library beside the
                        // chosen exe and compile tiny abs / sqrt / pow probe orchestras.
                        const bool ok = ensureCsoundMathProbe(true);
                        csTestResult = ok ? std::string("Test passed: the meters use ") + (g_mathHasAbs ? "abs" : "sqrt(x*x)") + "."
                                          : "Test failed: this Csound cannot compile abs or sqrt" + (g_mathProbeDetail.empty() ? std::string(".") : ": " + g_mathProbeDetail);
                        logLine("[caw] Csound test: " + csTestResult);
                    }
                    if (csShownTyped != s.csoundExe) { csShownTyped = s.csoundExe; csShownExe = csoundExePath(); }
                    ImGui::TextDisabled("%s. ", plat::csoundProgramName());
                    ImGui::TextWrapped("CAW will use: %s", csShownExe.empty() ? "(no Csound found - Browse for a csound.exe)" : csShownExe.c_str());
                    if (!csTestResult.empty()) ImGui::TextWrapped("%s", csTestResult.c_str());
                }
                if (g_langSc) {
                    ImGui::Spacing();
                    row("SuperCollider language (sclang)");
                    if (g_scEngine.embedded()) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(S(330));
                    if (inputStr("##sc_sclang", g_scLangPath, 0)) saveScLangPath();
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##scsclang")) {
                        const auto path = plat::chooseFile(false, "Choose SuperCollider language", "SuperCollider language", {"exe"}, "");
                        if (!path.empty()) { g_scLangPath = path; saveScLangPath(); }
                    }
                    if (g_scEngine.embedded()) ImGui::EndDisabled();
                    ImGui::TextDisabled("Choose sclang.exe from the CAW SuperCollider build. CAW embeds libscsynth and shares its PortAudio output with Csound and PD.");
                    row("SuperCollider synthesis library");
                    if (g_scEngine.embedded()) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(S(330));
                    if (inputStr("##sc_synthlib", g_scSynthPath, 0)) saveScSynthPath();
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##scsynthlib")) {
                        const char* extension = plat::isWindows ? "dll" : plat::isMac ? "dylib" : "so";
                        const auto path = plat::chooseFile(false, "Choose libscsynth", "SuperCollider synthesis library", {extension}, "");
                        if (!path.empty()) { g_scSynthPath = path; saveScSynthPath(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Auto##scsynthlib")) { g_scSynthPath.clear(); saveScSynthPath(); }
                    if (g_scEngine.embedded()) ImGui::EndDisabled();
                    ImGui::TextDisabled("Usually libscsynth.dll beside sclang.exe. Auto finds it in CAW's cached build/runtime folders.");
                }
                if (g_langPd) {
                    static std::string pdTestResult;
                    ImGui::Spacing();
                    row("Pure Data library");
                    if (g_pdEngineOn || g_pdPreview.active()) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(S(245));
                    if (inputStr("##pd_library", g_pdLibraryPath, 0)) { savePdLibraryPath(); pdTestResult.clear(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##pdlib")) {
                        const auto path = plat::chooseFile(false, "Choose libpd library", "Library", {"dll", "so", "dylib"}, "");
                        if (!path.empty()) { g_pdLibraryPath = path; savePdLibraryPath(); pdTestResult.clear(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Test##pdlib")) {
                        std::string error;
                        pdTestResult = testPdLibrary(error) ? "Library test passed." : "Library test failed: " + error;
                        logLine("[caw] " + pdTestResult);
                    }
                    if (g_pdEngineOn || g_pdPreview.active()) ImGui::EndDisabled();
                    ImGui::TextDisabled("Select the libpd library. The test checks loading, symbols and a test patch.");
                    if (!pdTestResult.empty()) ImGui::TextWrapped("%s", pdTestResult.c_str());
                }
                row("MP3 encoder");
                ImGui::TextDisabled("Automatic: native encoder, bundled FFmpeg, or FFmpeg on PATH.");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Editor
            if (ImGui::BeginTabItem("Editor", nullptr, g_settingsGoto == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "PROGRAM INTERFACE");
                row("Program font size");
                ImGui::SliderFloat("##program_font_size", &g_proj.cs.programFontSize, 12.f, 26.f, "%.0f px");
                ImGui::TextColored(head, "CODE EDITOR");
                row("Code font size");
                ImGui::SliderFloat("##editor_font_size", &g_proj.cs.editorFontSize, 10.f, 30.f, "%.0f px");
                row("Font style");
                const char* editorStyles[] = {"Regular", "Bold", "Italic"};
                g_proj.cs.editorFontStyle = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
                if (caw_ui::beginCombo("##editor_font_style", editorStyles[g_proj.cs.editorFontStyle])) {
                    for (int i = 0; i < 3; ++i)
                        if (ImGui::Selectable(editorStyles[i], g_proj.cs.editorFontStyle == i)) g_proj.cs.editorFontStyle = i;
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("Source is syntax-highlighted live while you edit.");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Advanced: the raw Csound options + header
            if (g_langCsound && ImGui::BeginTabItem("Advanced", nullptr, g_settingsGoto == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "CSOUND OPTIONS + HEADER (type here)");
                ImGui::TextDisabled("Everything on the Audio tab is also written here. Type any Csound flag (a line starting with -) or a header value.");
                static std::string text;
                static bool editing = false;
                if (!editing) text = engineSettingsToText(s);
                const ImVec2 boxSize(-FLT_MIN, S(230));
                if (inputMulti("##engine_text", text, boxSize, 0)) applyEngineText(s, text);
                editing = ImGui::IsItemActive();
                const std::string problems = engineTextProblems(text);
                if (!problems.empty()) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.f), "%s", problems.c_str());
                const std::string effective = s.options.empty() ? defaultOptions(s) : s.options;
                if (effective.find("-odac") == std::string::npos && effective.find("-o") == std::string::npos)
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "No -odac in the options: Csound will not play to the sound card.");
                ImGui::TextDisabled(
                    "Options: any Csound flag, e.g.  -odac -+rtaudio=portaudio -b1024 -B4096 -d\n"
                    "sr / ksmps / kr   sample rate, samples per control period (kr = sr / ksmps)\n"
                    "nchnls / nchnls_i number of output / input channels\n"
                    "0dbfs             value of digital full scale (1 = floating point -1..1)\n"
                    "A4                tuning of A4 in Hz (440 default)\n"
                    "-b N / -B N       software / hardware buffer in sample frames; -odacN picks the output device");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Files: project media references
            if (ImGui::BeginTabItem("Files", nullptr, g_settingsGoto == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "PROJECT FILES");
                ImGui::TextWrapped("File stores named references. CAW saves each path but does not copy the file.");
                ImGui::Spacing();
                if (ImGui::Button("Open File pool")) g_audioPoolOpen = true;
                ImGui::SameLine();
                if (ImGui::Button("Open Includes")) g_includePoolOpen = true;
                ImGui::SameLine();
                ImGui::TextDisabled("%zu file(s), %zu include(s)", g_proj.audioPool.size(), g_proj.includes.size());
                ImGui::Separator();
                if (g_proj.audioPool.empty() && g_proj.includes.empty()) {
                    ImGui::TextDisabled("No files or includes in this project yet.");
                } else {
                    ImGui::BeginChild("##settings_files_list", ImVec2(0, 0), true);
                    for (const AudioAsset& asset : g_proj.audioPool) {
                        std::error_code ec;
                        const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                        ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.60f, 1.f) : ImVec4(0.95f, 0.55f, 0.35f, 1.f),
                                           "%s", exists ? "Available" : "Missing");
                        ImGui::SameLine();
                        ImGui::Text("%s", asset.name.c_str());
                        ImGui::Indent(S(18));
                        ImGui::TextWrapped("%s", asset.path.c_str());
                        if (!exists) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.f), "File not found on this computer");
                        ImGui::Unindent(S(18));
                    }
                    for (const IncludeAsset& asset : g_proj.includes) {
                        std::error_code ec;
                        const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                        ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.60f, 1.f) : ImVec4(0.95f, 0.55f, 0.35f, 1.f),
                                           "%s", exists ? "Available" : "Missing");
                        ImGui::SameLine();
                        ImGui::Text("%s  (%s)", asset.name.c_str(), asset.score ? "Score include" : "Orchestra include");
                        ImGui::Indent(S(18));
                        ImGui::TextWrapped("%s", asset.path.c_str());
                        if (!exists) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.f), "File not found on this computer");
                        ImGui::Unindent(S(18));
                    }
                    ImGui::EndChild();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            g_settingsGoto = -1;
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- instrument editor
const TextEditor::LanguageDefinition& csoundLanguage() {
    static TextEditor::LanguageDefinition lang;
    static int built = 0;
    if (built == g_langVersion) return lang;
    built = g_langVersion;
    lang = TextEditor::LanguageDefinition::CPlusPlus();
    lang.mName = "Csound";
    lang.mSingleLineComment = ";";
    lang.mKeywords.clear();      // the C++ keywords / functions do not belong here
    lang.mIdentifiers.clear();
    for (const char* keyword : {"instr", "endin", "opcode", "endop", "if", "then", "elseif", "else", "endif", "ithen", "kthen",
                                "while", "until", "do", "od", "goto", "igoto", "kgoto", "tigoto", "rigoto", "cggoto", "cigoto", "ckgoto",
                                "cngoto", "cnkgoto", "timout", "reinit", "rireturn", "return", "this", "caw", "cawget"})
        lang.mKeywords.insert(keyword);
    TextEditor::Identifier known;
    known.mDeclaration = "Csound opcode";
    for (const char* op : kCsoundOpcodes) lang.mIdentifiers.insert(std::make_pair(std::string(op), known));
    for (const char* op : {"cawMath", "cawRun", "dawrun", "cawset", "cawfile"}) lang.mIdentifiers.insert(std::make_pair(std::string(op), known));
    for (const std::string& op : g_extraOpcodes) lang.mIdentifiers.insert(std::make_pair(op, known));
    // first in the list = wins over the generic identifier rule
    lang.mTokenRegexStrings.insert(lang.mTokenRegexStrings.begin(), {"p[0-9]+\\b", TextEditor::PaletteIndex::PreprocIdentifier});
    lang.mTokenRegexStrings.insert(lang.mTokenRegexStrings.begin(), {"<\\/?caw>", TextEditor::PaletteIndex::Preprocessor});
    return lang;
}

// ---- maximize / restore for the editor windows
struct WinMax { bool maxed = false; bool restore = false; ImVec2 pos, size; };
std::unordered_map<int, WinMax> g_winMax;

void winMaxPrepare(int key) {   // call right before ImGui::Begin
    WinMax& w = g_winMax[key];
    if (w.maxed) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    } else if (w.restore) {
        ImGui::SetNextWindowPos(w.pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(w.size, ImGuiCond_Always);
        w.restore = false;
    }
}

// A maximize / restore box in the title bar, right next to the close button (like a normal window).
// Call it first inside the window (before the font scale is changed).
void winMaxTitleButton(int key) {
    WinMax& w = g_winMax[key];
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fs = ImGui::GetFontSize();
    const float closeLeft = pos.x + size.x - st.FramePadding.x - fs;
    const float x0 = closeLeft - fs - st.FramePadding.x, y0 = pos.y + st.FramePadding.y;
    const ImVec2 a(x0, y0), b(x0 + fs, y0 + fs);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushClipRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + size.x, pos.y + ImGui::GetFrameHeight()), false);
    if (hovered) dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), fs * 0.5f, IM_COL32(110, 110, 120, 140));
    const ImU32 ink = IM_COL32(225, 225, 230, 255);
    const float m = fs * 0.28f;
    if (w.maxed) {   // restore icon: two overlapping squares
        dl->AddRect(ImVec2(a.x + m + fs * 0.12f, a.y + m - fs * 0.08f), ImVec2(b.x - m + fs * 0.08f, b.y - m - fs * 0.12f), ink, 0.f, 0, 1.2f);
        dl->AddRect(ImVec2(a.x + m - fs * 0.08f, a.y + m + fs * 0.12f), ImVec2(b.x - m - fs * 0.12f, b.y - m + fs * 0.08f), ink, 0.f, 0, 1.2f);
    } else {         // maximize icon: one square
        dl->AddRect(ImVec2(a.x + m, a.y + m), ImVec2(b.x - m, b.y - m), ink, 0.f, 0, 1.2f);
    }
    ImGui::PopClipRect();
    if (hovered) helpTip(w.maxed ? "Restore" : "Maximize");
    if (hovered && ImGui::IsMouseClicked(0)) {
        if (w.maxed) { w.maxed = false; w.restore = true; }
        else { w.pos = pos; w.size = size; w.maxed = true; }
    }
}

// Source editor bound to one std::string; each clip (and each lane template) has its own instance.
// Returns true when the user edited the text this frame.
bool codeEditorBody(std::unordered_map<int, std::unique_ptr<TextEditor>>& editors, int key, std::string& code, float reserveBottom = 0.f, int lang = 0, bool diag = false) {
    static std::unordered_map<const void*, std::string> lastText;  // what each editor showed last frame
    static std::unordered_map<const void*, std::string> lastDisplayed;
    auto& editor = editors[key];
    if (!editor) {
        editor = std::make_unique<TextEditor>();
        editor->SetPalette(TextEditor::GetDarkPalette());
        editor->SetLanguageDefinition(csoundLanguage());
        editor->SetText(code);
        lastText[editor.get()] = code;
        lastDisplayed[editor.get()] = editor->GetText();
    }
    static std::unordered_map<const void*, int> langApplied;
    int& applied = langApplied[editor.get()];
    const int wanted = g_langVersion * 4 + lang;
    if (applied != wanted) {
        if (lang == 0) { scanUserOpcodes(g_proj.udoCode); scanUserOpcodes(code); }
        if (lang == 0) editor->SetLanguageDefinition(csoundLanguage());
        else if (lang == 2) {   // SuperCollider: compiler-style colors (sclang keywords, common classes, the caw names)
            auto plain = TextEditor::LanguageDefinition::CPlusPlus();
            plain.mName = "SuperCollider";
            plain.mKeywords.clear(); plain.mIdentifiers.clear();
            for (const char* keyword : {"var", "arg", "classvar", "const", "if", "else", "while", "for", "forBy", "do",
                                        "case", "switch", "true", "false", "nil", "inf", "this", "super"})
                plain.mKeywords.insert(keyword);
            TextEditor::Identifier known;
            known.mDeclaration = "SuperCollider class / method";
            for (const char* name : {"SinOsc", "SinOscFB", "Saw", "Pulse", "LFTri", "LFSaw", "LFNoise0", "LFNoise1", "LFNoise2",
                                     "Blip", "PlayBuf", "BufRd", "BufWr", "RecordBuf", "Env", "EnvGen", "Line", "XLine",
                                     "Out", "OffsetOut", "ReplaceOut", "In", "InFeedback", "Free", "FreeSelf", "Done",
                                     "Pan2", "Pan4", "Balance2", "Mix", "LPF", "HPF", "BPF", "Resonz", "RHPF",
                                     "DelayN", "DelayL", "DelayC", "CombN", "CombL", "AllpassN", "AllpassL", "FreeVerb",
                                     "FreeVerb2", "GVerb", "PitchShift", "Limiter", "Compander", "Normalizer",
                                     "Synth", "Group", "Bus", "Buffer", "Routine", "Task", "Ndef", "Pbind", "Pdef",
                                     "Server", "OSCdef", "OSCFunc", "IdentityDictionary",
                                     "play", "stop", "free", "set", "ar", "kr", "ir", "new", "value", "isPlaying", "wait",
                                     "cawget", "cawGet", "cawgroup", "cawGroup", "cawr", "caws"})
                plain.mIdentifiers.insert(std::make_pair(std::string(name), known));
            plain.mSingleLineComment = "//";
            editor->SetLanguageDefinition(plain);
        }
        else {   // Pure Data readable patch: comment rows start with "; " and color like Csound comments
            auto plain = TextEditor::LanguageDefinition::CPlusPlus();
            plain.mName = "Pure Data source";
            plain.mKeywords.clear(); plain.mIdentifiers.clear();
            plain.mTokenRegexStrings.clear();
            plain.mSingleLineComment = ";";
            editor->SetLanguageDefinition(plain);
        }
        applied = g_langVersion * 4 + lang;
    }
    std::string& last = lastText[editor.get()];
    if (code != last) { editor->SetText(code); last = code; lastDisplayed[editor.get()] = editor->GetText(); }
    if (diag) {   // clip editors: red marks on the lines the console reported, and "go to line" from the console
        const auto mk = g_diagMarks.find(key);
        editor->SetErrorMarkers(mk == g_diagMarks.end() ? TextEditor::ErrorMarkers() : mk->second);
        const auto gl = g_gotoLine.find(key);
        if (gl != g_gotoLine.end()) { editor->SetCursorPosition(TextEditor::Coordinates(std::max(0, gl->second - 1), 0)); g_gotoLine.erase(gl); }
    }
    const float editorHeight = std::max(S(120), ImGui::GetContentRegionAvail().y - S(4) - reserveBottom);
    editor->Render(("Code##" + std::to_string(key)).c_str(), ImVec2(-FLT_MIN, editorHeight), true);
    if (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered()) g_codeClickHit = true;
    std::string edited = editor->GetText();
    if ((lang == 0 && edited != last) || (lang != 0 && edited != lastDisplayed[editor.get()])) {
        if (diag) g_diagMarks.erase(key);   // the lines moved: the old marks would sit on the wrong lines
        code = edited;
        last = std::move(edited);
        lastDisplayed[editor.get()] = last;
        if (lang == 0) { scanUserOpcodes(g_proj.udoCode); scanUserOpcodes(code); }
        return true;
    }
    return false;
}

bool colorPicker(const char* id, uint32_t& color, uint32_t fallback) {
    const uint32_t v = color ? color : fallback;
    float rgb[3] = {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f};
    if (!ImGui::ColorEdit3(id, rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) return false;
    color = 0x01000000u | ((uint32_t)std::lround(rgb[0] * 255.f) << 16) |
            ((uint32_t)std::lround(rgb[1] * 255.f) << 8) | (uint32_t)std::lround(rgb[2] * 255.f);
    return true;
}

struct UdoPoolEntry {
    std::string name, outputs, inputs, source;
    size_t begin = 0, end = 0;
};

std::vector<UdoPoolEntry> projectUdoEntries() {
    static const std::regex headerRe(R"re(^[ \t]*opcode[ \t]+([A-Za-z_]\w*)[ \t]*,[ \t]*([^,\r\n]+),[ \t]*([^\r\n]+))re");
    static const std::regex endRe(R"re(^[ \t]*endop\b.*$)re");
    std::vector<UdoPoolEntry> entries;
    UdoPoolEntry current;
    bool inUdo = false;
    size_t lineStart = 0;
    while (lineStart < g_proj.udoCode.size()) {
        const size_t lineEnd = g_proj.udoCode.find('\n', lineStart);
        const size_t afterLine = lineEnd == std::string::npos ? g_proj.udoCode.size() : lineEnd + 1;
        std::string line = g_proj.udoCode.substr(lineStart,
            (lineEnd == std::string::npos ? g_proj.udoCode.size() : lineEnd) - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (!inUdo && std::regex_match(line, match, headerRe)) {
            current = UdoPoolEntry();
            current.name = match[1].str();
            current.outputs = match[2].str();
            current.inputs = match[3].str();
            current.begin = lineStart;
            inUdo = true;
        } else if (inUdo && std::regex_match(line, endRe)) {
            current.end = afterLine;
            current.source = g_proj.udoCode.substr(current.begin, current.end - current.begin);
            entries.push_back(current);
            inUdo = false;
        }
        lineStart = afterLine;
    }
    if (inUdo) {
        current.end = g_proj.udoCode.size();
        current.source = g_proj.udoCode.substr(current.begin, current.end - current.begin);
        entries.push_back(std::move(current));
    }
    return entries;
}

void poolSplitter(const char* id, float& width) {
    ImGui::SameLine(0, 0);
    const float height = ImGui::GetContentRegionAvail().y;
    ImGui::InvisibleButton(id, ImVec2(S(7), height));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive())
        width = std::min(500.f, std::max(150.f, width + ImGui::GetIO().MouseDelta.x / std::max(0.25f, g_ui)));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImU32 color = (hovered || ImGui::IsItemActive()) ? IM_COL32(230, 120, 40, 255) : IM_COL32(75, 78, 84, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, color);
    ImGui::SameLine(0, 0);
}

std::string audioVariableName(const Clip& clip, const AudioAsset& asset) {
    std::string base = "aPool_";
    for (unsigned char c : asset.name)
        base.push_back((std::isalnum(c) || c == '_') ? (char)c : '_');
    if (base == "aPool_") base += "Sample";
    std::string name = base;
    int suffix = 2;
    while (std::regex_search(clip.code, std::regex("\\b" + name + "\\b")))
        name = base + std::to_string(suffix++);
    return name;
}

void insertAudioIntoClip(Clip& clip, const AudioAsset& asset) {
    if (asset.path.empty()) return;
    std::string path = std::filesystem::path(asset.path).generic_string();
    std::string escapedPath;
    escapedPath.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '"') escapedPath += "\\\"";
        else escapedPath.push_back(c);
    }
    std::string label = asset.name;
    for (char& c : label) if (c == '\n' || c == '\r') c = ' ';
    const std::string variable = audioVariableName(clip, asset);
    if (!clip.code.empty() && clip.code.back() != '\n') clip.code.push_back('\n');
    clip.code += "\n; Audio: " + label + "\n" + variable + " diskin2 \"" + escapedPath + "\", 1, 0, 1\nout " + variable + "\n";
    syncSharedCode(clip);
    clip.editorOpen = true;
}

void drawUdoPool() {
    if (!g_udoPoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(920), S(600)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(220), S(100)), ImGuiCond_FirstUseEver);
    bool open = g_udoPoolOpen;
    if (ImGui::Begin("UDO", &open)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
        ImGui::TextDisabled("Project-wide Csound opcodes, available to every instrument.");
        if (ImGui::Button("Add UDO template")) {
            const auto entries = projectUdoEntries();
            std::set<std::string> used;
            for (const UdoPoolEntry& entry : entries) used.insert(entry.name);
            std::string name = "MyUDO";
            for (int n = 2; used.count(name); ++n) name = "MyUDO" + std::to_string(n);
            if (!g_proj.udoCode.empty() && g_proj.udoCode.back() != '\n') g_proj.udoCode.push_back('\n');
            g_proj.udoCode += "\nopcode " + name + ", a, a\naIn xin\nxout aIn\nendop\n";
            scanUserOpcodes(g_proj.udoCode);
            g_udoPoolSelected = (int)projectUdoEntries().size() - 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Select a name to edit only that opcode.");
        ImGui::Separator();

        const std::vector<UdoPoolEntry> entries = projectUdoEntries();
        if (entries.empty()) g_udoPoolSelected = -1;
        else if (g_udoPoolSelected < 0 || g_udoPoolSelected >= (int)entries.size()) g_udoPoolSelected = 0;
        ImGui::BeginChild("##udo_name_list", ImVec2(S(g_udoSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "UDO NAMES");
        ImGui::Separator();
        if (entries.empty()) {
            ImGui::TextWrapped("No UDOs found yet. Add a template to get started.");
        } else {
            for (size_t i = 0; i < entries.size(); ++i) {
                ImGui::PushID((int)i);
                if (ImGui::Selectable(entries[i].name.c_str(), g_udoPoolSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                    g_udoPoolSelected = (int)i;
                ImGui::TextDisabled("%s <- %s", entries[i].outputs.c_str(), entries[i].inputs.c_str());
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        poolSplitter("##udo_splitter", g_udoSidebarWidth);
        ImGui::BeginChild("##udo_source", ImVec2(0, 0), false);
        if (g_udoPoolSelected >= 0 && g_udoPoolSelected < (int)entries.size()) {
            const UdoPoolEntry& selected = entries[(size_t)g_udoPoolSelected];
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%s", selected.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s <- %s", selected.outputs.c_str(), selected.inputs.c_str());
            ImGui::Separator();
            std::string source = selected.source;
            const bool hasTrailingNewline = !source.empty() && source.back() == '\n';
            const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
            if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
            ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
            const int editorKey = -200001 - g_udoPoolSelected;
            if (codeEditorBody(g_codeEditors, editorKey, source, 0.f, 0, false)) {
                if (hasTrailingNewline && (source.empty() || source.back() != '\n')) source.push_back('\n');
                g_proj.udoCode.replace(selected.begin, selected.end - selected.begin, source);
                scanUserOpcodes(g_proj.udoCode);
            }
            ImGui::SetWindowFontScale(1.f);
            if (g_codeFonts[style]) ImGui::PopFont();
        } else {
            ImGui::TextDisabled("Add a UDO template to get started.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
    g_udoPoolOpen = open;
}

std::string poolInstrName(const std::string& code) {
    static const std::regex re(R"re((?:^|\n)[ \t]*instr[ \t]+([A-Za-z_][A-Za-z_0-9]*|[0-9]+))re");
    std::smatch match;
    return std::regex_search(code, match, re) ? match[1].str() : "(unnamed)";
}

void drawInstrPool() {
    if (!g_instrPoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(920), S(600)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(220), S(100)), ImGuiCond_FirstUseEver);
    bool open = g_instrPoolOpen;
    if (ImGui::Begin("Instrument pool", &open)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
        ImGui::TextDisabled("Project-wide instr definitions. Schedule them from clip code; use names distinct from clips.");
        if (ImGui::Button("Add instrument template")) {
            std::set<std::string> used;
            for (const std::string& source : g_proj.instrPool) used.insert(poolInstrName(source));
            for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) used.insert(c.name);
            std::string name = "PoolInstr";
            for (int n = 2; used.count(name); ++n) name = "PoolInstr" + std::to_string(n);
            g_proj.instrPool.push_back("instr " + name + "\n; Add Csound code here\nendin\n");
            g_instrPoolSelected = (int)g_proj.instrPool.size() - 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Select an instrument to edit it.");
        ImGui::Separator();
        if (g_proj.instrPool.empty()) g_instrPoolSelected = -1;
        else if (g_instrPoolSelected < 0 || g_instrPoolSelected >= (int)g_proj.instrPool.size()) g_instrPoolSelected = 0;
        ImGui::BeginChild("##instr_name_list", ImVec2(S(g_instrSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "INSTRUMENTS");
        ImGui::Separator();
        for (size_t i = 0; i < g_proj.instrPool.size(); ++i) {
            ImGui::PushID((int)i);
            const std::string name = poolInstrName(g_proj.instrPool[i]);
            if (ImGui::Selectable(name.c_str(), g_instrPoolSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                g_instrPoolSelected = (int)i;
            ImGui::PopID();
        }
        ImGui::EndChild();
        poolSplitter("##instr_splitter", g_instrSidebarWidth);
        ImGui::BeginChild("##instr_source", ImVec2(0, 0), false);
        if (g_instrPoolSelected >= 0 && g_instrPoolSelected < (int)g_proj.instrPool.size()) {
            const int idx = g_instrPoolSelected;
            const std::string name = poolInstrName(g_proj.instrPool[(size_t)idx]);
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%s", name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove instrument")) {
                g_proj.instrPool.erase(g_proj.instrPool.begin() + idx);
                g_instrPoolSelected = -1;
                g_codeEditors.clear();
            } else {
                ImGui::Separator();
                const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
                if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
                ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
                (void)codeEditorBody(g_codeEditors, -300001 - idx, g_proj.instrPool[(size_t)idx], 0.f, 0, false);
                ImGui::SetWindowFontScale(1.f);
                if (g_codeFonts[style]) ImGui::PopFont();
            }
        } else ImGui::TextDisabled("Add an instrument template to get started.");
        ImGui::EndChild();
    }
    ImGui::End();
    g_instrPoolOpen = open;
}

void drawAudioPool() {
    if (!g_audioPoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(820), S(540)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(260), S(120)), ImGuiCond_FirstUseEver);
    bool open = g_audioPoolOpen;
    if (ImGui::Begin("File pool", &open)) {
        if (ImGui::Button("Add file")) {
            const std::string picked = chooseAudioPath();
            if (!picked.empty()) {
                std::error_code ec;
                std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(picked), ec);
                if (ec) p = std::filesystem::path(picked);
                std::string name = p.filename().string();
                if (name.empty()) name = "File";
                const auto samePath = std::find_if(g_proj.audioPool.begin(), g_proj.audioPool.end(),
                    [&](const AudioAsset& a) { return a.path == p.string(); });
                if (samePath != g_proj.audioPool.end()) {
                    g_audioPoolSelected = (int)std::distance(g_proj.audioPool.begin(), samePath);
                } else {
                    g_proj.audioPool.push_back({name, p.string()});
                    g_audioPoolSelected = (int)g_proj.audioPool.size() - 1;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Files stay on disk. Refer to their names with cawfile in code.");
        ImGui::Separator();

        if (g_proj.audioPool.empty()) g_audioPoolSelected = -1;
        else if (g_audioPoolSelected < 0 || g_audioPoolSelected >= (int)g_proj.audioPool.size()) g_audioPoolSelected = 0;
        ImGui::BeginChild("##audio_list", ImVec2(S(g_audioSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "FILES");
        ImGui::Separator();
        if (g_proj.audioPool.empty()) {
            ImGui::TextWrapped("Add a file to see it here.");
        } else {
            for (size_t i = 0; i < g_proj.audioPool.size(); ++i) {
                const AudioAsset& asset = g_proj.audioPool[i];
                std::error_code ec;
                const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                ImGui::PushID((int)i);
                if (ImGui::Selectable(asset.name.c_str(), g_audioPoolSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                    g_audioPoolSelected = (int)i;
                ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.60f, 1.f) : ImVec4(0.95f, 0.52f, 0.35f, 1.f),
                                   "%s", exists ? "Available" : "Missing");
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        poolSplitter("##audio_splitter", g_audioSidebarWidth);
        ImGui::BeginChild("##audio_details", ImVec2(0, 0), false);

        if (g_audioPoolSelected >= 0 && g_audioPoolSelected < (int)g_proj.audioPool.size()) {
            AudioAsset& asset = g_proj.audioPool[(size_t)g_audioPoolSelected];
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "SELECTED FILE");
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Name");
            ImGui::SameLine(S(80));
            ImGui::SetNextItemWidth(std::max(S(80), std::min(S(360), ImGui::GetContentRegionAvail().x - S(120))));
            inputStr("##audio_pool_name", asset.name, 0);
            if (ImGui::Button("Change file...")) {
                const std::string picked = chooseAudioPath();
                if (!picked.empty()) {
                    std::error_code ec;
                    const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(picked), ec);
                    asset.path = (ec ? std::filesystem::path(picked) : absolute).string();
                }
            }
            ImGui::TextDisabled("File path");
            ImGui::TextWrapped("%s", asset.path.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("Sfile cawfile \"%s\"", asset.name.c_str());
            if (ImGui::Button("Remove from list")) {
                g_proj.audioPool.erase(g_proj.audioPool.begin() + g_audioPoolSelected);
                if (g_proj.audioPool.empty()) g_audioPoolSelected = -1;
                else g_audioPoolSelected = std::min(g_audioPoolSelected, (int)g_proj.audioPool.size() - 1);
            }
        } else {
            ImGui::TextDisabled("Select a file on the left, or add one.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
    g_audioPoolOpen = open;
}

void drawOrchestraPool() {
    if (!g_orchestraOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(850), S(560)), ImGuiCond_FirstUseEver);
    bool open = g_orchestraOpen;
    if (ImGui::Begin("Orchestra", &open)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
        ImGui::TextDisabled("PROJECT ORCHESTRA");
        ImGui::TextDisabled("Global variables, tables and orchestra code shared by every clip.");
        ImGui::Separator();
        const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
        if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
        ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
        (void)codeEditorBody(g_codeEditors, -400001, g_proj.orchestraCode, 0.f, 0, false);
        ImGui::SetWindowFontScale(1.f);
        if (g_codeFonts[style]) ImGui::PopFont();
    }
    ImGui::End();
    g_orchestraOpen = open;
}

void drawIncludePool() {
    if (!g_includePoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(860), S(560)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(280), S(130)), ImGuiCond_FirstUseEver);
    bool open = g_includePoolOpen;
    if (ImGui::Begin("Includes", &open)) {
        if (ImGui::Button("Add include...")) {
            const std::string picked = chooseIncludePath();
            if (!picked.empty()) {
                std::error_code ec;
                std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(picked), ec);
                if (ec) p = std::filesystem::path(picked);
                const auto samePath = std::find_if(g_proj.includes.begin(), g_proj.includes.end(),
                    [&](const IncludeAsset& a) { return a.path == p.string(); });
                if (samePath != g_proj.includes.end()) {
                    g_includeSelected = (int)std::distance(g_proj.includes.begin(), samePath);
                } else {
                    std::string ext = p.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    g_proj.includes.push_back({p.filename().string(), p.string(), ext == ".sco"});
                    g_includeSelected = (int)g_proj.includes.size() - 1;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(".udo / .txt files default to Orchestra; .sco files default to Score.");
        ImGui::Separator();

        if (g_proj.includes.empty()) g_includeSelected = -1;
        else if (g_includeSelected < 0 || g_includeSelected >= (int)g_proj.includes.size()) g_includeSelected = 0;
        ImGui::BeginChild("##include_list", ImVec2(S(g_includeSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "INCLUDED FILES");
        ImGui::Separator();
        if (g_proj.includes.empty()) {
            ImGui::TextWrapped("Add a .udo, .sco, or .txt file.");
        } else {
            for (size_t i = 0; i < g_proj.includes.size(); ++i) {
                const IncludeAsset& asset = g_proj.includes[i];
                std::error_code ec;
                const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                ImGui::PushID((int)i);
                if (ImGui::Selectable(asset.name.c_str(), g_includeSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                    g_includeSelected = (int)i;
                ImGui::TextDisabled("%s · %s", asset.score ? "Score" : "Orchestra", exists ? "Available" : "Missing");
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        poolSplitter("##include_splitter", g_includeSidebarWidth);
        ImGui::BeginChild("##include_details", ImVec2(0, 0), false);
        if (g_includeSelected >= 0 && g_includeSelected < (int)g_proj.includes.size()) {
            IncludeAsset& asset = g_proj.includes[(size_t)g_includeSelected];
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%s", asset.name.c_str());
            ImGui::Separator();
            ImGui::TextDisabled("Full path");
            ImGui::TextWrapped("%s", asset.path.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("Csound section");
            ImGui::SameLine();
            int section = asset.score ? 1 : 0;
            if (comboFit(S(160), "##include_section", &section, "Orchestra\0Score\0")) asset.score = section == 1;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Orchestra includes are emitted inside <CsInstruments>; score includes are emitted inside <CsScore>.");
            if (asset.score) ImGui::TextDisabled("Score includes should omit an `e` terminator; CAW closes the generated score.");
            if (ImGui::Button("Change file...")) {
                const std::string picked = chooseIncludePath();
                if (!picked.empty()) {
                    std::error_code ec;
                    const std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(picked), ec);
                    asset.path = (ec ? std::filesystem::path(picked) : p).string();
                    asset.name = std::filesystem::path(asset.path).filename().string();
                    std::string ext = std::filesystem::path(asset.path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    asset.score = ext == ".sco";
                }
            }
            ImGui::SameLine();
            const bool remove = ImGui::Button("Remove from list");
            Clip* clip = selectedClip();
            ImGui::Separator();
            ImGui::TextWrapped("Generated as: #include \"%s\"", std::filesystem::path(asset.path).generic_string().c_str());
            if (clip) ImGui::TextDisabled("Applies to the project, including clip %s.", clip->name.c_str());
            if (remove) {
                g_proj.includes.erase(g_proj.includes.begin() + g_includeSelected);
                if (g_proj.includes.empty()) g_includeSelected = -1;
                else g_includeSelected = std::min(g_includeSelected, (int)g_proj.includes.size() - 1);
            }
        } else {
            ImGui::TextDisabled("Select an include on the left, or add a file.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
    g_includePoolOpen = open;
}

// MIDI on/off in a clip editor. ON creates the clip's MIDI track (lane + region).
// OFF removes it, and asks first because the notes of the region are deleted.
void midiToggleButton(int laneIndex, Clip& clip, const char* idSuffix) {
    const std::string label = std::string(clip.midiEnabled ? "MIDI on" : "MIDI off") + "##" + idSuffix;
    const std::string popup = "Remove MIDI track?###midi_rm_" + std::string(idSuffix) + std::to_string(clip.id);
    if (ImGui::Button(label.c_str())) {
        if (!clip.midiEnabled) {
            clip.midiEnabled = true;
            syncSharedCode(clip);
            ensureMidiClipFor(laneIndex, clip);
        } else {
            bool hasRegion = false;
            if (laneIndex >= 0 && laneIndex < (int)g_proj.tracks.size())
                for (const MidiClip& mc : g_proj.tracks[(size_t)laneIndex].midiClips) if (mc.clipId == clip.id) { hasRegion = true; break; }
            if (hasRegion) ImGui::OpenPopup(popup.c_str());
            else { clip.midiEnabled = false; syncSharedCode(clip); }
        }
    }
    if (ImGui::IsItemHovered()) helpTip(clip.midiEnabled ? "MIDI is on for this clip. Click to remove its MIDI track (asks first)."
                                                         : "Turn MIDI on and create the MIDI track for this clip. Notes send pitch and velocity in p4 and p5.");
    if (ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        size_t notes = 0;
        if (laneIndex >= 0 && laneIndex < (int)g_proj.tracks.size())
            for (const MidiClip& mc : g_proj.tracks[(size_t)laneIndex].midiClips) if (mc.clipId == clip.id) notes += mc.notes.size();
        ImGui::TextWrapped("Remove the MIDI track of clip \"%s\"? Its MIDI region and %zu note%s will be deleted.",
                           clip.name.c_str(), notes, notes == 1 ? "" : "s");
        ImGui::TextDisabled("You can undo this with Ctrl+Z.");
        if (ImGui::Button("Remove MIDI track")) {
            if (laneIndex >= 0 && laneIndex < (int)g_proj.tracks.size()) {
                for (const MidiClip& mc : g_proj.tracks[(size_t)laneIndex].midiClips)
                    if (mc.clipId == clip.id) g_selMidiClips.erase(mc.id);
            }
            removeMidiClipsOf(clip.id);
            clip.midiEnabled = false;
            syncSharedCode(clip);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawX3FieldsPopup(Clip& clip, bool pd) {
    (void)pd;
    int runs=clip.runs;
    ImGui::SetNextItemWidth(S(80));
    if(ImGui::InputInt("Runs", &runs, 0, 0))clip.runs=std::min(256,std::max(1,runs));
    if(inputMulti("##x_field_rules",clip.pFieldRules,ImVec2(S(320),S(110))))syncSharedCode(clip);
    if(clip.pFieldRules.empty()&&!ImGui::IsItemActive()) {
        const ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        const ImVec2 pad=ImGui::GetStyle().FramePadding;
        ImDrawList* dl=ImGui::GetWindowDrawList();
        dl->PushClipRect(a,b,true);
        dl->AddText(ImVec2(a.x+pad.x,a.y+pad.y),ImGui::GetColorU32(ImGuiCol_TextDisabled),
            "{p: 6, from: 60, to: 90}\n{p: 6, from: 60, +5}\n{p: 6, from: 60, *2}");
        dl->PopClipRect();
    }
}

void drawEditor() {
    g_textEditorFocused = false;
    g_codeClickHit = false;
    struct TypingUpdate { ~TypingUpdate() {   // a click in a code text area = typing; a click anywhere else (title, fields, timeline) = not typing
        if (ImGui::IsMouseClicked(0)) g_codeTyping = g_codeClickHit;
        if (!g_textEditorFocused) g_codeTyping = false;
    } } typingUpdate;
    const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti) {
        Track& lane = g_proj.tracks[ti];

        // ---- one window per clip: its own instr name, color, timing and code
        for (Clip& clip : lane.clips) {
            if (!clip.editorOpen) { g_clipWinSeen.erase(clip.id); continue; }
            if (lane.kind == 5) {
                const std::string title = "Code editor - " + clip.name + " (SuperCollider)###sc_clip_" + std::to_string(clip.id);
                bool open = clip.editorOpen;
                ImGui::SetNextWindowSize(ImVec2(S(640), S(440)), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowPos(ImVec2(S(300), S(150)), ImGuiCond_FirstUseEver);
                winMaxPrepare(clip.id);
                if (ImGui::Begin(title.c_str(), &open)) {
                    winMaxTitleButton(clip.id);
                    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
                    ImGui::SetNextItemWidth(S(150));
                    const std::string nameBefore = clip.name;
                    if (inputStr("##sc_clip_name", clip.name, 0, nameFilter)) renameClipGroup(clip, nameBefore);
                    if (!ImGui::IsItemActive()) ensureUniqueClipName(clip);
                    ImGui::SameLine(); colorPicker("##sc_clip_color", clip.color, lane.color);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
                    inputStrHint("##sc_clip_note", "Describe this clip...", clip.note);

                    const bool running = scClipRunning(clip.id);
                    if (running) pushBtnOn(ImVec4(0.85f, 0.35f, 0.10f, 1.f));
                    else pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                    if (ImGui::Button(running ? "Stop clip" : "Run clip")) {
                        if (g_scPendingClipId == clip.id && !g_scReady) g_scPendingClipId = 0;
                        else if (running) stopScClip(clip.id);
                        else launchScClip(clip);
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) helpTip("Evaluate this SuperCollider clip in its own group. CAW frees the group when the clip duration ends.");
                    ImGui::SameLine();
                    midiToggleButton((int)ti, clip, "sc");
                    ImGui::SameLine();
                    if (!clip.midiEnabled) ImGui::BeginDisabled();
                    if (ImGui::Button("Edit MIDI##sc")) openMidiEditorFor((int)ti, clip);
                    if (!clip.midiEnabled) ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("x-fields##sc")) ImGui::OpenPopup("sc_xfields_popup");
                    if (ImGui::BeginPopup("sc_xfields_popup")) {
                        drawX3FieldsPopup(clip, false);
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Score##sc")) ImGui::OpenPopup("sc_score_popup");
                    if (ImGui::BeginPopup("sc_score_popup")) {
                        drawClipScoreMenu(clip, &lane);
                        ImGui::EndPopup();
                    }
                    const ImGuiStyle& st = ImGui::GetStyle();
                    const float bottomRow = ImGui::GetFrameHeight() + st.ItemSpacing.y * 3.f + S(8);
                    if (g_codeFonts[0]) ImGui::PushFont(g_codeFonts[0]);
                    ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
                    const bool codeEdited = codeEditorBody(g_clipCodeEditors, clip.id, clip.code, bottomRow, 2, true);   // lang 2 = SuperCollider: // comments, uniform strings
                    ImGui::SetWindowFontScale(1.f);
                    if (g_codeFonts[0]) ImGui::PopFont();
                    if (codeEdited) syncSharedCode(clip);
                    ImGui::Separator();
                    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Start"); ImGui::SameLine(0, S(6));
                    ImGui::SetNextItemWidth(S(100));
                    if (ImGui::InputDouble("##sc_clip_start", &clip.start, 0.0, 0.0, "%.3f s")) clip.start = clampTimelineTime(clip.start);
                    ImGui::SameLine(0, S(12)); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Duration"); ImGui::SameLine(0, S(6));
                    ImGui::SetNextItemWidth(S(100));
                    if (ImGui::InputDouble("##sc_clip_duration", &clip.dur, 0.0, 0.0, "%.3f s")) clip.dur = std::max(0.001, clip.dur);
                    ImGui::SameLine(0, S(12)); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("End %.3f s", clip.start + clip.dur);
                    ImGui::TextDisabled("SC clips run in real time. Audio export is not implemented for SuperCollider yet.");
                }
                ImGui::End();
                clip.editorOpen = open;
                continue;
            }
            if (lane.kind == 6) {
                const std::string title = "Code editor - " + clip.name + " (Pure Data)###pd_clip_" + std::to_string(clip.id);
                bool open = clip.editorOpen;
                ImGui::SetNextWindowSize(ImVec2(S(640), S(440)), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowPos(ImVec2(S(300), S(150)), ImGuiCond_FirstUseEver);
                winMaxPrepare(clip.id);
                if (ImGui::Begin(title.c_str(), &open)) {
                    winMaxTitleButton(clip.id);
                    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused=true;
                    const ImGuiStyle& st=ImGui::GetStyle();
                    // ---- row 1: name | color | description
                    ImGui::SetNextItemWidth(S(150));
                    const std::string nameBefore = clip.name;
                    if (inputStr("##pd_clip_name", clip.name, 0, nameFilter)) renameClipGroup(clip, nameBefore);
                    if (!ImGui::IsItemActive()) ensureUniqueClipName(clip);
                    ImGui::SameLine(); colorPicker("##pd_clip_color", clip.color, lane.color);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
                    inputStrHint("##pd_clip_note", "Describe this clip...", clip.note);
                    // ---- row 2: Run | MIDI | x-fields | Score | Automation shape
                    const bool mixedRunning=g_mixedPd && g_apiProc.auditioningPd(clip.id);
                    const bool running = mixedRunning || (g_pdPlayingClip == clip.id && g_pdPreview.active() && !g_pdTimelineMode);
                    if (running) pushBtnOn(ImVec4(0.85f,0.35f,0.10f,1.f));
                    else pushBtnOn(ImVec4(0.20f,0.42f,0.24f,1.f));
                    if (g_timelineRun && !g_mixedPd && !running) ImGui::BeginDisabled();
                    if (ImGui::Button(running ? "  Stop  ##pd" : "  Run  ##pd")) {
                        if(g_mixedPd) {
                            const auto* event=pd_timeline::find(g_pdTimelineEvents,clip.id);
                            if(!event)logLine("[caw] This PD clip is not in the current engine plan. Restart the engine after changing its patch, mute or solo state.");
                            else if(!g_apiProc.auditionPd(*event,!mixedRunning))
                                logLine("[caw] Wait until the shared PD engine is ready.");
                        } else if (running) stopPdPatch();
                        else {
                            std::filesystem::path patch=std::filesystem::u8path(clip.code);
                            if(patch.is_relative()&&!g_projectPath.empty())patch=std::filesystem::u8path(g_projectPath).parent_path()/patch;
                            std::string error;
                            if(clip.code.empty()) logLine("[caw] Select a .pd patch first.");
                            else if(!startPdPatch(patch.u8string(),clip.id,error))
                                logClipError("PureData", clip, error.empty()?"Could not start this patch.":error);
                        }
                    }
                    if (g_timelineRun && !g_mixedPd && !running) ImGui::EndDisabled();
                    ImGui::PopStyleColor(3);
                    if(ImGui::IsItemHovered())helpTip("Run this patch. While it runs, everything you type is applied to the sound.");
                    ImGui::SameLine();
                    midiToggleButton((int)ti, clip, "pd");
                    ImGui::SameLine();
                    if (!clip.midiEnabled) ImGui::BeginDisabled();
                    if (ImGui::Button("Edit MIDI##pd")) openMidiEditorFor((int)ti, clip);
                    if (!clip.midiEnabled) ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::Button("x-fields##pd")) ImGui::OpenPopup("PD x-fields");
                    if (ImGui::IsItemHovered()) helpTip("Runs and p-field rules sent to [r $0-caw-pN].");
                    if (ImGui::BeginPopup("PD x-fields")) {drawX3FieldsPopup(clip,true);ImGui::EndPopup();}
                    ImGui::SameLine();
                    if (ImGui::Button("Score##pd")) ImGui::OpenPopup("PD score");
                    if (ImGui::BeginPopup("PD score")) { drawClipScoreMenu(clip, &lane); ImGui::EndPopup(); }
                    ImGui::SameLine();
                    if(ImGui::Button("Automation shape##pd"))ImGui::OpenPopup("pd_clip_wave_popup");
                    ImGui::SetNextWindowSize(ImVec2(S(390),0),ImGuiCond_Appearing);
                    if(ImGui::BeginPopup("pd_clip_wave_popup")){drawWavePopup(lane,clip);ImGui::EndPopup();}
                    // ---- row 3: short path | Browse | more
                    ImGui::SetNextItemWidth(S(230));
                    if (inputStrHint("##pd_clip_path", "patch.pd", clip.code)) syncSharedCode(clip);
                    if (ImGui::IsItemHovered()) helpTip("%s", clip.code.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##pd")) {
                        const auto path = plat::chooseFile(false, "Choose Pure Data patch", "Pure Data", {"pd"}, "");
                        if (!path.empty()) { clip.code = path; syncSharedCode(clip); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("...##pd_more")) ImGui::OpenPopup("pd_more_popup");
                    std::filesystem::path file = std::filesystem::u8path(clip.code);
                    bool usablePath = !clip.code.empty();
                    if (usablePath && file.is_relative()) {
                        if (g_projectPath.empty()) usablePath = false;
                        else file = std::filesystem::u8path(g_projectPath).parent_path() / file;
                    }
                    std::error_code pathError;
                    if (usablePath) file = std::filesystem::absolute(file, pathError).lexically_normal();
                    const bool haveFile = usablePath && !pathError;
                    if (ImGui::BeginPopup("pd_more_popup")) {
                        if (ImGui::MenuItem("Use test tone")) {
                            std::string path, error;
                            if (pd_test_patch::create(workDir(), path, error)) { clip.code = path; syncSharedCode(clip); }
                            else logLine("[caw] " + error);
                        }
                        if (ImGui::MenuItem("Use automation example")) {
                            std::string path, error;
                            if (pd_automation_patch::create(workDir(), path, error)) { clip.code = path; syncSharedCode(clip); }
                            else logLine("[caw] " + error);
                        }
                        if (ImGui::MenuItem("Open in Pure Data", nullptr, false, haveFile)) {
                            std::error_code ec;
                            if (std::filesystem::is_regular_file(file, ec) && !ec) plat::openWithDefaultProgram(file.u8string());
                            else logLine("[caw] Choose an existing PD patch first.");
                        }
                        if (ImGui::MenuItem("Reload from disk", nullptr, false, haveFile)) {
                            auto& b = g_pdPatchBuffers[file.u8string()];
                            pd_patch_editor::load(file, b);
                        }
                        ImGui::EndPopup();
                    }
                    const float footerH = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2.f + S(6);
                    if (!haveFile) {
                        ImGui::TextDisabled("Choose a .pd file (relative paths need a saved project). The ... menu creates an automation example patch with cawget.");
                    } else {
                        auto& buffer = g_pdPatchBuffers[file.u8string()];
                        if (!buffer.loaded && buffer.message.empty()) pd_patch_editor::load(file, buffer);
                        if (!buffer.loaded) {
                            ImGui::TextWrapped("%s", buffer.message.c_str());
                        } else {
                            // Show only object/message/comment contents. The patch still retains
                            // coordinates, record types, canvases and connections on disk.
                            auto saveAndApply = [&]() {
                                if (!pd_patch_editor::save(file, buffer)) { logLine("[caw] PD patch: " + buffer.message); return false; }
                                if (g_mixedPd) {
                                    for (const auto& event : g_pdTimelineEvents) {
                                        std::error_code ec;
                                        auto eventPath = std::filesystem::absolute(std::filesystem::u8path(event.patch), ec).lexically_normal();
                                        if (!ec && eventPath == file && g_apiProc.auditioningPd(event.clipId))
                                            g_apiProc.auditionPd(event, true);
                                    }
                                } else if (g_pdPreview.active() && g_pdPlayingClip == clip.id && !g_pdTimelineMode) {
                                    g_pdPreview.reload(file.u8string());
                                }
                                return true;
                            };
                            struct LiveEdit { std::size_t hash = 0; double changedAt = 0.0, checkedAt = 0.0; bool tried = false; };
                            static std::map<int, LiveEdit> liveEdits;
                            LiveEdit& live = liveEdits[clip.id];
                            const double now = ImGui::GetTime();
                            const std::size_t h = std::hash<std::string>{}(buffer.readable);
                            if (h != live.hash) { live.hash = h; live.changedAt = now; live.tried = false; }
                            if (buffer.dirty()) {
                                if (!live.tried && (now - live.changedAt > 0.5 || !open)) { live.tried = true; saveAndApply(); }
                            } else if (now - live.checkedAt > 1.0) {   // the file was changed elsewhere (Pure Data): show it
                                live.checkedAt = now;
                                std::string disk, why;
                                if (pd_patch_editor::read(file, disk, why) && disk != buffer.original) pd_patch_editor::load(file, buffer);
                            }
                            if (running && !mixedRunning) {   // tell the log when the running patch could not take an edit
                                static std::string lastPreviewStatus;
                                const std::string status = g_pdPreview.status();
                                if (status != lastPreviewStatus) {
                                    lastPreviewStatus = status;
                                    if (status.find("reload") != std::string::npos) logLine("[caw] " + status);
                                }
                            }
                            const bool showMessage = !buffer.message.empty() && buffer.message != "Patch saved.";
                            const float messageH = showMessage ? ImGui::GetTextLineHeightWithSpacing() * 2.f : 0.f;
                            ImGui::TextDisabled("Edit lines in place. For adding, removing or moving objects, use Pure Data.");
                            if (g_codeFonts[style])ImGui::PushFont(g_codeFonts[style]);
                            ImGui::SetWindowFontScale(g_proj.cs.editorFontSize/g_proj.cs.programFontSize);
                            (void)codeEditorBody(g_clipCodeEditors,clip.id,buffer.readable,footerH+messageH,1,true);
                            ImGui::SetWindowFontScale(1.f);
                            if(g_codeFonts[style])ImGui::PopFont();
                            if (showMessage) ImGui::TextColored(ImVec4(1.f, .7f, .3f, 1.f), "%s", buffer.message.c_str());
                        }
                    }
                    ImGui::AlignTextToFramePadding();ImGui::TextDisabled("Start");ImGui::SameLine(0,S(6));
                    ImGui::SetNextItemWidth(S(96));
                    double newStart=clip.start;
                    if(ImGui::InputDouble("##pd_clip_start",&newStart,0.0,0.0,"%.3f s")) {
                        newStart=clampTimelineTime(newStart);ClipAutomationMove move;
                        move.capture(g_proj,std::set<int>{clip.id});
                        const double delta=newStart-clip.start;clip.start=newStart;move.apply(g_proj,delta);
                    }
                    ImGui::SameLine(0,S(14));ImGui::AlignTextToFramePadding();ImGui::TextDisabled("Duration");
                    ImGui::SameLine(0,S(6));ImGui::SetNextItemWidth(S(96));
                    if(ImGui::InputDouble("##pd_clip_dur",&clip.dur,0.0,0.0,"%.3f s"))clip.dur=std::max(.001,clip.dur);
                    ImGui::SameLine(0,S(14));ImGui::AlignTextToFramePadding();ImGui::TextDisabled("End");
                    ImGui::SameLine(0,S(6));ImGui::Text("%.3f s",clip.start+clip.dur);
                }
                ImGui::End();
                if (!open && !g_pdTimelineMode && g_pdPlayingClip == clip.id) stopPdPatch();
                clip.editorOpen = open;
                continue;
            }
            // Clips that share an instrument (duplicates) have the same name. Each still gets its own window: a newly opened
            // one is cascaded (never exactly on top of another editor), comes to the front, and its title tells the start time.
            // A clip whose instrument (same name) is already open in another editor window does not get a second window:
            // the window that is already open comes to the front instead.
            if (!g_clipWinSeen.count(clip.id)) {
                int openId = -1;
                for (const Track& ot : g_proj.tracks) for (const Clip& oc : ot.clips)
                    if (oc.id != clip.id && oc.name == clip.name && oc.editorOpen && g_clipWinSeen.count(oc.id)) openId = oc.id;
                if (openId >= 0) { clip.editorOpen = false; g_focusClipWin = openId; continue; }
            }
            const bool justOpened = g_clipWinSeen.insert(clip.id).second;
            if (justOpened) {
                int others = 0;
                for (const Track& ot : g_proj.tracks) for (const Clip& oc : ot.clips) if (oc.id != clip.id && oc.editorOpen && g_clipWinSeen.count(oc.id)) ++others;
                ImGui::SetNextWindowPos(ImVec2(S(300) + S(28) * (float)(others % 8), S(150) + S(28) * (float)(others % 8)), ImGuiCond_Always);
                g_focusClipWin = clip.id;
            }
            ImGui::SetNextWindowSize(ImVec2(S(760), S(520)), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(S(300), S(150)), ImGuiCond_FirstUseEver);
            winMaxPrepare(clip.id);
            if (g_focusClipWin == clip.id) { ImGui::SetNextWindowCollapsed(false); ImGui::SetNextWindowFocus(); g_focusClipWin = -1; }
            int sharedCount = 0;
            for (const Track& ot : g_proj.tracks) for (const Clip& oc : ot.clips) if (oc.name == clip.name) ++sharedCount;
            char startText[32];
            std::snprintf(startText, sizeof startText, "%.2f", clip.start);
            const std::string title = "Code editor - " + clip.name + (sharedCount > 1 ? "  (clip at " + std::string(startText) + " s)" : std::string()) +
                                      "###clip_editor_" + std::to_string(clip.id);
            bool open = clip.editorOpen;
            if (ImGui::Begin(title.c_str(), &open)) {
                winMaxTitleButton(clip.id);
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
                const ImGuiStyle& st = ImGui::GetStyle();
                auto btnW = [&](const char* t) { return ImGui::CalcTextSize(t).x + st.FramePadding.x * 2.f; };
                // right-align a group of buttons: put the cursor so the group ends at the right edge (never left of the current x)
                auto alignRight = [&](float groupW) {
                    const float x = ImGui::GetContentRegionMax().x - groupW;
                    if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
                };

                // ---- row 1: WHO  =  name | color | note
                ImGui::SetNextItemWidth(S(150));
                const std::string nameBefore = clip.name;
                if (inputStr("##clip_instrument_name", clip.name, 0, nameFilter)) renameClipGroup(clip, nameBefore);
                if (!ImGui::IsItemActive()) ensureUniqueClipName(clip);
                if (ImGui::IsItemHovered()) helpTip("Instrument name (unique in the project)");
                ImGui::SameLine();
                colorPicker("##clip_color", clip.color, lane.color);
                if (ImGui::IsItemHovered()) helpTip("Clip color");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-FLT_MIN);
                inputStrHint("##clip_note", "Describe this clip...", clip.note);
                if (ImGui::IsItemHovered()) helpTip("Info shown on the clip (empty = first comment line of the code)");

                // ---- row 2: ACTIONS  =  Run on the left; MIDI | runs | Score | Automation shape on the right
                const bool clipIsRunning = clipRunning(clip);
                if (clipIsRunning) pushBtnOn(ImVec4(0.85f, 0.35f, 0.10f, 1.f));
                else pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                if (ImGui::Button(clipIsRunning ? "  Stop  " : "  Run  ")) {
                    if (clipIsRunning) stopClipRun(clip); else runClip(clip);
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(clipIsRunning ? "Stop this clip" : "Play this clip in the running Csound (Run), or run it alone without sound and print the errors in the console, with the instrument name and the line of this code");
                const std::string runLabel = "x-fields";
                const char* scoreLabel = "Score";
                const char* shapeLabel = "Automation shape";
                const float groupW = btnW("MIDI off") + btnW("Edit MIDI") + btnW(runLabel.c_str()) + btnW(scoreLabel) + btnW(shapeLabel) + st.ItemSpacing.x * 4.f;
                ImGui::SameLine();
                alignRight(groupW);
                {
                    midiToggleButton((int)ti, clip, "clip_midi");
                }
                ImGui::SameLine();
                if(!clip.midiEnabled)ImGui::BeginDisabled();
                if(ImGui::Button("Edit MIDI##clip"))openMidiEditorFor((int)ti,clip);
                if(!clip.midiEnabled)ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(runLabel.c_str())) ImGui::OpenPopup("clip_runs_popup");
                if (ImGui::IsItemHovered()) helpTip("Set the number of runs and optional x-field rules.");
                if (ImGui::BeginPopup("clip_runs_popup")) {
                    drawX3FieldsPopup(clip,false);
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(scoreLabel)) ImGui::OpenPopup("clip_score_popup");
                if (ImGui::IsItemHovered()) helpTip("How this clip appears in the printed score: shape, overall or detail, role, language");
                if (ImGui::BeginPopup("clip_score_popup")) { drawClipScoreMenu(clip, &lane); ImGui::EndPopup(); }
                ImGui::SameLine();
                if (ImGui::Button(shapeLabel)) ImGui::OpenPopup("clip_wave_popup");
                if (ImGui::IsItemHovered()) helpTip("Create an automation shape.");
                ImGui::SetNextWindowSize(ImVec2(S(390), 0), ImGuiCond_Appearing);
                if (ImGui::BeginPopup("clip_wave_popup")) { drawWavePopup(lane, clip); ImGui::EndPopup(); }

                // ---- code (takes all the room that is left)
                const float bottomRow = ImGui::GetFrameHeight() + st.ItemSpacing.y * 3.f + S(8);
                bool codeEdited = false;
                if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
                ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
                codeEdited = codeEditorBody(g_clipCodeEditors, clip.id, clip.code, bottomRow, 0, true) || codeEdited;
                ImGui::SetWindowFontScale(1.f);
                if (g_codeFonts[style]) ImGui::PopFont();
                if (codeEdited) {
                    syncSharedCode(clip);
                    markLiveCodeEdit(clip);   // VSTi / VSTe / Csound: recompile into the running engine after the pause
                }

                // ---- ONE slim bottom row: Start | Duration | End ........ shared-instrument note
                ImGui::Separator();
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Start");
                    ImGui::SameLine(0, S(6));
                    ImGui::SetNextItemWidth(S(96));
                    double newStart = clip.start;
                    if (ImGui::InputDouble("##clip_start", &newStart, 0.0, 0.0, "%.3f s")) {
                        newStart = clampTimelineTime(newStart);
                        ClipAutomationMove move;
                        move.capture(g_proj, std::set<int>{clip.id});
                        const double delta = newStart - clip.start;
                        clip.start = newStart;
                        move.apply(g_proj, delta);
                    }
                    ImGui::SameLine(0, S(14));
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Duration");
                    ImGui::SameLine(0, S(6));
                    ImGui::SetNextItemWidth(S(96));
                    if (ImGui::InputDouble("##clip_dur", &clip.dur, 0.0, 0.0, "%.3f s")) clip.dur = std::max(0.001, clip.dur);
                    ImGui::SameLine(0, S(14));
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("End");
                    ImGui::SameLine(0, S(6));
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%.3f s", clip.start + clip.dur);
                    if (sharedCount > 1) {
                        char shareText[64];
                        std::snprintf(shareText, sizeof shareText, "shared by %d clips", sharedCount);
                        ImGui::SameLine();
                        alignRight(ImGui::CalcTextSize(shareText).x);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", shareText);
                        if (ImGui::IsItemHovered()) helpTip("These clips are one instrument (same name): editing the code here changes all of them.");
                    }
                }
            }
            ImGui::End();
            clip.editorOpen = open;
        }

        // ---- lane template: the code a NEW clip on this lane starts from
        if (lane.editorOpen) {
            ImGui::SetNextWindowSize(ImVec2(S(700), S(500)), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(S(320), S(170)), ImGuiCond_FirstUseEver);
            winMaxPrepare(100000 + lane.id);
            const std::string title = "Lane template - " + lane.name + "###lane_template_" + std::to_string(lane.id);
            bool open = lane.editorOpen;
            if (ImGui::Begin(title.c_str(), &open)) {
                winMaxTitleButton(100000 + lane.id);
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
                ImGui::TextDisabled("New clips on lane %s copy this code.", lane.name.c_str());
                if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
                ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
                (void)codeEditorBody(g_codeEditors, lane.id, lane.code);
                ImGui::SetWindowFontScale(1.f);
                if (g_codeFonts[style]) ImGui::PopFont();
            }
            ImGui::End();
            lane.editorOpen = open;
        }
    }
    tickLiveCodeEdits();   // apply paused code edits to the running engine (one recompile per clip)
    drawUdoPool();
    drawInstrPool();
    drawOrchestraPool();
}

const char* midiPitchName(int pitch) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static char text[16];
    std::snprintf(text, sizeof(text), "%s%d", names[pitch % 12], pitch / 12 - 1);
    return text;
}

// Match the project timeline's logarithmic zoom and playhead-centered focus.
bool midiZoomSlider(const char* id, float& zoom) {
    ImGui::TextDisabled("-");
    ImGui::SameLine(0, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.15f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.22f, 0.24f, 1.f));
    ImGui::SetNextItemWidth(S(110));
    const bool changed = ImGui::SliderFloat(id, &zoom, kPpsMin, kPpsMax, "",
        ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput);
    if (ImGui::IsItemHovered()) helpTip("Zoom around the playhead. Ctrl+mouse wheel also zooms.");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::SameLine(0, S(4));
    ImGui::TextDisabled("+");
    return changed;
}

// Same 110-pixel travel as the zoom slider, rotated to scroll pitch vertically.
void midiPitchSlider(const char* id, int& pitchLow) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.15f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.22f, 0.24f, 1.f));
    ImGui::VSliderInt(id, ImVec2(S(18), S(110)), &pitchLow, 0, 127 - kMidiVisiblePitches + 1, "");
    if (ImGui::IsItemHovered()) helpTip("Scroll the visible pitch range.");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ONE piano roll for several MIDI clips (select the MIDI clips in the timeline, press Enter). Every clip keeps its own notes and colour;
// new notes go to the clip chosen in "Add notes to". Note times are stored relative to their MIDI clip, like in the single editor.
void drawMultiMidiEditor() {
    if (!g_multiMidiOpen) return;
    g_multiMidiIds.erase(std::remove_if(g_multiMidiIds.begin(), g_multiMidiIds.end(), [](int id) { return findMidiClip(id) == nullptr; }), g_multiMidiIds.end());
    if (g_multiMidiIds.empty()) { g_multiMidiOpen = false; return; }
    static float scrollX = 0.f, pps = 120.f;
    static int pitchLow = 48, activeId = -1;
    static std::set<std::pair<int, int>> sel;                          // (MIDI clip id, note index)
    struct Orig { int id, idx; double start; int pitch; };
    static std::vector<Orig> origs;
    static int dragMode = 0;                                           // 0 none, 1 move, 2 resize
    static std::pair<int, int> dragNote(-1, -1);
    static double grabT = 0.0;
    static int grabP = 0;
    static bool marquee = false, marqueeMoved = false, marqueeAdd = false, rulerDrag = false;
    static ImVec2 marqueeStart;
    if (std::find(g_multiMidiIds.begin(), g_multiMidiIds.end(), activeId) == g_multiMidiIds.end()) activeId = g_multiMidiIds.front();
    ImGui::SetNextWindowSize(ImVec2(S(1000), S(700)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(280), S(120)), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("MIDI Editor - several clips###midi_multi_editor", &open)) {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;
        if (ImGui::IsWindowAppearing()) {
            double lo = 1e300;
            for (int id : g_multiMidiIds) if (const MidiClip* mc = findMidiClip(id)) lo = std::min(lo, mc->start);
            scrollX = (float)std::max(0.0, lo - 0.25);
            sel.clear();
        }
        {
            if (midiZoomSlider("##multi_zoom", pps))
                scrollX = zoomAroundPlayhead(pps, g_play, std::max(1.f, ImGui::GetWindowWidth() - S(120)));
            ImGui::SameLine();
            if (!sel.empty() && ImGui::SmallButton("Delete notes")) {
                std::map<int, std::vector<int>> per;
                for (const auto& s : sel) per[s.first].push_back(s.second);
                for (auto& [id, list] : per) {
                    MidiClip* mc = findMidiClip(id);
                    if (!mc) continue;
                    std::sort(list.rbegin(), list.rend());
                    for (int ni : list) if (ni >= 0 && ni < (int)mc->notes.size()) mc->notes.erase(mc->notes.begin() + ni);
                }
                sel.clear();
            }
            {   // selected notes: pitch / start / duration boxes like the single MIDI editor (adding notes is not possible here)
                for (auto it = sel.begin(); it != sel.end();) {
                    const MidiClip* sm = findMidiClip(it->first);
                    it = (!sm || it->second < 0 || it->second >= (int)sm->notes.size()) ? sel.erase(it) : std::next(it);
                }
                if (sel.size() == 1) {
                    MidiClip* smc = findMidiClip(sel.begin()->first);
                    MidiNote& selected = smc->notes[(size_t)sel.begin()->second];
                    ImGui::Text("Selected note:");
                    ImGui::SameLine(); ImGui::Text("%s", midiPitchName(selected.pitch));
                    ImGui::SameLine(); ImGui::SetNextItemWidth(S(100)); ImGui::InputInt("##multi_pitch", &selected.pitch, 0, 0);
                    selected.pitch = std::min(127, std::max(0, selected.pitch));
                    ImGui::SameLine(); ImGui::SetNextItemWidth(S(120)); ImGui::InputDouble("##multi_start", &selected.start, 0.0, 0.0, "+%.2f s");
                    if (ImGui::IsItemHovered()) helpTip("Start, relative to the start of its MIDI clip");
                    selected.start = std::max(0.0, selected.start);
                    {   // the same start on the timeline
                        double absStart = smc->start + selected.start;
                        ImGui::SameLine(); ImGui::SetNextItemWidth(S(130));
                        if (ImGui::InputDouble("##multi_start_abs", &absStart, 0.0, 0.0, "@%.2f s")) selected.start = std::max(0.0, absStart - smc->start);
                        if (ImGui::IsItemHovered()) helpTip("Start on the timeline (the start before it is relative to its MIDI clip)");
                    }
                    ImGui::SameLine(); ImGui::SetNextItemWidth(S(120)); ImGui::InputDouble("##multi_dur", &selected.dur, 0.0, 0.0, "%.2f s");
                    if (ImGui::IsItemHovered()) helpTip("Duration of the note");
                    selected.dur = std::max(0.001, selected.dur);
                    ImGui::SameLine(); ImGui::Text("Vel"); ImGui::SameLine(0, S(4)); ImGui::SetNextItemWidth(S(70)); ImGui::InputInt("##multi_velocity", &selected.velocity, 0, 0);
                    selected.velocity = std::min(127, std::max(1, selected.velocity));
                } else if (sel.size() > 1) {
                    ImGui::Text("%d notes selected", (int)sel.size());
                    ImGui::SameLine();
                    const bool qButton = ImGui::SmallButton("Q##multi_note_sync");
                    const bool qKey = !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
                        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Q, false);
                    if (qButton || qKey) {   // align notes to the earliest timeline start
                        historyFlush();
                        double first = 1e300;
                        for (const auto& sp : sel) if (const MidiClip* sm = findMidiClip(sp.first)) first = std::min(first, sm->start + sm->notes[(size_t)sp.second].start);
                        for (const auto& sp : sel) if (MidiClip* sm = findMidiClip(sp.first)) sm->notes[(size_t)sp.second].start = std::max(0.0, first - sm->start);
                        historyFlush();
                    }
                    if (ImGui::IsItemHovered()) helpTip("Q: align selected note starts to the earliest, within their clip boundaries.");
                } else {
                    ImGui::TextDisabled("%d clips in this roll. Click a note to select it and edit it here, drag to box-select (Ctrl adds), drag notes to move, drag a right edge to resize, Del deletes. Notes cannot be added here.", (int)g_multiMidiIds.size());
                }
            }
        }
        midiPitchSlider("##multi_pitch_range", pitchLow);
        ImGui::SameLine();
        const int pitchHigh = pitchLow + kMidiVisiblePitches - 1;
        const float keyboardW = S(64), rowH = S(kMidiRowH), rollH = rowH * (float)kMidiVisiblePitches;
        ImGui::BeginChild("##multi_roll", ImVec2(0, std::max(S(360), rollH + S(26))), true, ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 rollMin(origin.x, origin.y), rollMax(origin.x + available.x, origin.y + rollH + S(24));
        const float pianoX = origin.x + keyboardW, pianoRight = rollMax.x, rulerY = origin.y, pitchY = origin.y + S(24);
        ImGui::InvisibleButton("##multi_canvas", ImVec2(available.x, rollH + S(24)));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && io.KeyCtrl && io.MouseWheel != 0.f) {
            pps = std::min(kPpsMax, std::max(kPpsMin, pps * (1.f + 0.12f * io.MouseWheel)));
            scrollX = zoomAroundPlayhead(pps, g_play, std::max(1.f, pianoRight - pianoX));
        } else if (hovered && (io.MouseWheelH != 0.f || (io.KeyShift && io.MouseWheel != 0.f))) {
            const float wheel = io.MouseWheelH != 0.f ? io.MouseWheelH : io.MouseWheel;
            scrollX = std::max(0.f, scrollX - wheel * 60.f / std::max(1.f, pps));
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushClipRect(rollMin, rollMax, true);
        dl->AddRectFilled(rollMin, rollMax, IM_COL32(22, 23, 26, 255));
        dl->AddRectFilled(ImVec2(origin.x, pitchY), ImVec2(pianoX, pitchY + rollH), IM_COL32(31, 32, 36, 255));
        dl->AddRectFilled(ImVec2(pianoX, rulerY), ImVec2(pianoRight, pitchY), IM_COL32(31, 32, 36, 255));
        const double viewStart = std::max(0.0, (double)scrollX), viewEnd = viewStart + (pianoRight - pianoX) / std::max(1.f, pps);
        auto timeX = [&](double t) { return pianoX + (float)(t - viewStart) * pps; };
        auto xTime = [&](float x) { return viewStart + (double)(x - pianoX) / std::max(1.f, pps); };
        for (long long k = (long long)std::floor(viewStart / 0.25); k * 0.25 <= viewEnd; ++k) {
            const float x = timeX(k * 0.25);
            const bool major = (k % 4) == 0;
            dl->AddLine(ImVec2(x, pitchY), ImVec2(x, pitchY + rollH), major ? IM_COL32(58, 59, 66, 255) : IM_COL32(38, 39, 44, 255));
            if (major) { char label[24]; std::snprintf(label, sizeof(label), "%.1fs", k * 0.25); dl->AddText(ImVec2(x + S(3), rulerY + S(4)), IM_COL32(174, 175, 183, 255), label); }
        }
        for (int pitch = pitchHigh; pitch >= pitchLow; --pitch) {
            const float y0 = pitchY + (pitchHigh - pitch) * rowH;
            const bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10);
            dl->AddRectFilled(ImVec2(pianoX, y0), ImVec2(pianoRight, y0 + rowH), black ? IM_COL32(27, 28, 32, 255) : IM_COL32(35, 36, 40, 255));
            dl->AddLine(ImVec2(origin.x, y0 + rowH), ImVec2(pianoRight, y0 + rowH), IM_COL32(48, 49, 55, 255));
            dl->AddText(ImVec2(origin.x + S(6), y0 + S(1)), pitch % 12 == 0 ? IM_COL32(210, 211, 218, 255) : IM_COL32(138, 140, 148, 255), midiPitchName(pitch));
        }
        // the extent of every clip: a coloured bar on the ruler (the active clip is thicker)
        for (int id : g_multiMidiIds) {
            int li = 0;
            MidiClip* mc = findMidiClip(id, &li);
            const Clip* tg = mc ? findClip(mc->clipId) : nullptr;
            if (!mc || !tg) continue;
            const ImU32 edge = trackColor(li, 220, clipColorValue(g_proj.tracks[(size_t)li], *tg));
            const float a = std::max(pianoX, timeX(mc->start)), b = std::min(pianoRight, timeX(mc->start + mc->dur));
            if (b > a) dl->AddRectFilled(ImVec2(a, rulerY + S(18)), ImVec2(b, rulerY + S(18) + (id == activeId ? S(5) : S(2.5f))), edge);
            if (timeX(mc->start) >= pianoX && timeX(mc->start) <= pianoRight) dl->AddLine(ImVec2(timeX(mc->start), pitchY), ImVec2(timeX(mc->start), pitchY + rollH), edge, S(1));
            if (timeX(mc->start + mc->dur) >= pianoX && timeX(mc->start + mc->dur) <= pianoRight) dl->AddLine(ImVec2(timeX(mc->start + mc->dur), pitchY), ImVec2(timeX(mc->start + mc->dur), pitchY + rollH), edge, S(1));
        }
        for (int id : g_multiMidiIds) {
            int li = 0;
            MidiClip* mc = findMidiClip(id, &li);
            const Clip* tg = mc ? findClip(mc->clipId) : nullptr;
            if (!mc || !tg) continue;
            const uint32_t rgb = clipColorValue(g_proj.tracks[(size_t)li], *tg);
            for (int ni = 0; ni < (int)mc->notes.size(); ++ni) {
                const MidiNote& n = mc->notes[(size_t)ni];
                if (n.pitch < pitchLow || n.pitch > pitchHigh) continue;
                const float y0 = pitchY + (pitchHigh - n.pitch) * rowH + S(1), y1 = y0 + rowH - S(2);
                const float x0 = timeX(mc->start + n.start), x1 = timeX(mc->start + n.start + n.dur);
                if (x1 < pianoX || x0 > pianoRight) continue;
                const bool selected = sel.count({id, ni}) > 0;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x0 + S(5), x1), y1), trackColor(li, selected ? 255 : (id == activeId ? 230 : 170), rgb), S(2));
                dl->AddRect(ImVec2(x0, y0), ImVec2(std::max(x0 + S(5), x1), y1), selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), S(2));
                char vel[16];
                std::snprintf(vel, sizeof(vel), "%d", n.velocity);
                if (x1 - x0 > S(24)) dl->AddText(ImVec2(x0 + S(3), y0), IM_COL32(255, 255, 255, 230), vel);
            }
        }
        {   // playhead
            if (hovered && ImGui::IsMouseClicked(0) && mouse.x >= pianoX && mouse.y >= rulerY && mouse.y < pitchY) rulerDrag = true;
            if (rulerDrag) {
                if (ImGui::IsMouseDown(0)) { g_play = std::max(0.0, xTime(std::min(pianoRight, std::max(pianoX, mouse.x)))); g_resync = 2; }
                else rulerDrag = false;
            }
            const float px = timeX(g_play);
            if (px >= pianoX && px <= pianoRight) {
                dl->AddLine(ImVec2(px, rulerY), ImVec2(px, pitchY + rollH), IM_COL32(255, 90, 80, 255), S(2));
                dl->AddTriangleFilled(ImVec2(px - S(5), rulerY), ImVec2(px + S(5), rulerY), ImVec2(px, rulerY + S(9)), IM_COL32(255, 90, 80, 255));
            }
        }
        auto pitchAt = [&](float y) { return std::min(pitchHigh, std::max(pitchLow, pitchHigh - (int)((y - pitchY) / rowH))); };
        if (hovered && ImGui::IsMouseClicked(0) && mouse.x >= pianoX && mouse.y >= pitchY && mouse.y <= pitchY + rollH) {
            const int pitch = pitchAt(mouse.y);
            const double time = xTime(mouse.x);                                   // absolute
            const bool additive = io.KeyCtrl || io.KeyShift;
            std::pair<int, int> hit(-1, -1);
            for (int gi = (int)g_multiMidiIds.size() - 1; gi >= 0 && hit.first < 0; --gi) {
                const MidiClip* mc = findMidiClip(g_multiMidiIds[(size_t)gi]);
                if (!mc) continue;
                for (int ni = (int)mc->notes.size() - 1; ni >= 0; --ni) {
                    const MidiNote& n = mc->notes[(size_t)ni];
                    if (n.pitch == pitch && time >= mc->start + n.start && time <= mc->start + n.start + n.dur) { hit = {mc->id, ni}; break; }
                }
            }
            if (hit.first >= 0) {
                if (additive) { if (sel.count(hit)) sel.erase(hit); else sel.insert(hit); }
                else if (!sel.count(hit)) { sel.clear(); sel.insert(hit); }
                if (sel.count(hit)) {
                    const MidiClip* mc = findMidiClip(hit.first);
                    const MidiNote& n = mc->notes[(size_t)hit.second];
                    dragMode = mouse.x >= timeX(mc->start + n.start + n.dur) - S(8) ? 2 : 1;
                    if (dragMode == 2) { sel.clear(); sel.insert(hit); }
                    origs.clear();
                    for (const auto& s : sel) if (const MidiClip* sm = findMidiClip(s.first)) if (s.second >= 0 && s.second < (int)sm->notes.size()) origs.push_back({s.first, s.second, sm->notes[(size_t)s.second].start, sm->notes[(size_t)s.second].pitch});
                    dragNote = hit; grabT = time; grabP = pitch;
                    activeId = hit.first;
                }
            } else {
                if (!additive) sel.clear();
                marquee = true; marqueeMoved = false; marqueeAdd = additive; marqueeStart = mouse;
            }
        }
        if (dragMode != 0 && ImGui::IsMouseDown(0)) {
            if (dragMode == 2) {
                MidiClip* mc = findMidiClip(dragNote.first);
                if (mc && dragNote.second >= 0 && dragNote.second < (int)mc->notes.size()) {
                    MidiNote& n = mc->notes[(size_t)dragNote.second];
                    n.dur = std::max(0.001, clampTimelineTime(xTime(mouse.x) - mc->start - n.start));
                }
            } else if (mouse.x >= pianoX && mouse.y >= pitchY && mouse.y <= pitchY + rollH) {
                double dt = xTime(mouse.x) - grabT;
                int dp = pitchAt(mouse.y) - grabP;
                for (const Orig& o : origs) { dt = std::max(dt, -o.start); dp = std::min(127 - o.pitch, std::max(-o.pitch, dp)); }
                for (const Orig& o : origs) {
                    MidiClip* mc = findMidiClip(o.id);
                    if (!mc || o.idx >= (int)mc->notes.size()) continue;
                    mc->notes[(size_t)o.idx].start = std::max(0.0, o.start + dt);
                    mc->notes[(size_t)o.idx].pitch = o.pitch + dp;
                }
            }
        }
        if (dragMode != 0 && !ImGui::IsMouseDown(0)) { dragMode = 0; origs.clear(); }
        if (marquee) {
            if (std::fabs(mouse.x - marqueeStart.x) + std::fabs(mouse.y - marqueeStart.y) > S(3)) marqueeMoved = true;
            const float qx0 = std::min(mouse.x, marqueeStart.x), qx1 = std::max(mouse.x, marqueeStart.x);
            const float qy0 = std::min(mouse.y, marqueeStart.y), qy1 = std::max(mouse.y, marqueeStart.y);
            if (ImGui::IsMouseDown(0)) {
                if (marqueeMoved) {
                    dl->AddRectFilled(ImVec2(qx0, qy0), ImVec2(qx1, qy1), IM_COL32(237, 128, 38, 40));
                    dl->AddRect(ImVec2(qx0, qy0), ImVec2(qx1, qy1), IM_COL32(237, 128, 38, 220));
                }
            } else {
                if (!marqueeMoved) {
                    // a plain click on an empty cell only clears the selection: notes cannot be added in the multi editor
                } else {
                    for (int id : g_multiMidiIds) {
                        const MidiClip* mc = findMidiClip(id);
                        if (!mc) continue;
                        for (int ni = 0; ni < (int)mc->notes.size(); ++ni) {
                            const MidiNote& n = mc->notes[(size_t)ni];
                            if (n.pitch < pitchLow || n.pitch > pitchHigh) continue;
                            const float ny0 = pitchY + (pitchHigh - n.pitch) * rowH, ny1 = ny0 + rowH;
                            if (timeX(mc->start + n.start + n.dur) >= qx0 && timeX(mc->start + n.start) <= qx1 && ny1 >= qy0 && ny0 <= qy1) sel.insert({id, ni});
                        }
                    }
                }
                marquee = false;
            }
        }
        if (!sel.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            std::map<int, std::vector<int>> per;
            for (const auto& s : sel) per[s.first].push_back(s.second);
            for (auto& [id, list] : per) {
                MidiClip* mc = findMidiClip(id);
                if (!mc) continue;
                std::sort(list.rbegin(), list.rend());
                for (int ni : list) if (ni >= 0 && ni < (int)mc->notes.size()) mc->notes.erase(mc->notes.begin() + ni);
            }
            sel.clear();
        }
        ImGui::PopClipRect();
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) { g_multiMidiOpen = false; sel.clear(); }
}

void drawMidiEditor() {
    // One piano roll per MIDI clip. Note times are stored relative to the MIDI clip start.
    // (g_midiSelectedTrackId / g_midiDragTrackId hold the clip id.)
    for (size_t laneIndex = 0; laneIndex < g_proj.tracks.size(); ++laneIndex)
    for (MidiClip& mc : g_proj.tracks[laneIndex].midiClips) {
        Track& track = g_proj.tracks[laneIndex];
        if (!mc.editorOpen) continue;
        const Clip* tgtPtr = findClip(mc.clipId);
        if (!tgtPtr) { mc.editorOpen = false; continue; }
        const Clip& tgt = *tgtPtr;
        const int trackId = mc.id;
        ImGui::SetNextWindowSize(ImVec2(S(1000), S(760)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(S(250), S(100)), ImGuiCond_FirstUseEver);
        winMaxPrepare(200000 + mc.id);
        const std::string title = "MIDI Editor - " + (mc.name.empty() ? tgt.name : mc.name) + "###midi_editor_clip_" + std::to_string(mc.id);
        bool open = mc.editorOpen;
        if (ImGui::Begin(title.c_str(), &open)) {
            winMaxTitleButton(200000 + mc.id);
            if (ImGui::IsWindowAppearing()) {
                g_midiScrollX = (float)std::max(0.0, mc.start - 0.25);
            }
            ImGuiIO& io = ImGui::GetIO();
            const ImVec2 mouse = io.MousePos;
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Name");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(S(150));
                inputStrHint("##midi_clip_name", tgt.name.c_str(), mc.name);
                if (ImGui::IsItemHovered()) helpTip("MIDI clip name. Changing this does not change its instrument.");
                ImGui::SameLine();
                const bool wasMuted = mc.muted;
                if (wasMuted) pushBtnOn(ImVec4(0.75f, 0.24f, 0.20f, 1.f));
                if (ImGui::Button("M##midi_editor_mute")) mc.muted = !mc.muted;
                if (wasMuted) ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip("Mute only this MIDI clip.");
                ImGui::SameLine();
                // The instrument is routing, separate from the editable clip name.
                static std::unordered_map<int, std::string> nameBuf;
                std::string& typed = nameBuf[mc.id];
                static std::unordered_map<int, bool> nameEditing;
                if (!nameEditing[mc.id]) typed = tgt.name;
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.94f, 0.60f, 0.28f, 1.f), "Instrument");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(S(150));
                const bool changed = inputStr("##midi_target_name", typed, 0, nameFilter);
                nameEditing[mc.id] = ImGui::IsItemActive();
                if (ImGui::IsItemHovered()) helpTip("Name of the Csound instrument that receives these notes (p4 = pitch, p5 = velocity)");
                int found = 0;
                for (const Track& tr : g_proj.tracks)
                    for (const Clip& c : tr.clips) if (c.name == typed) { found = c.id; break; }
                if (changed && found) mc.clipId = found;
                if (!found) { ImGui::SameLine(); ImGui::TextDisabled("no instrument with this name"); }
            }
            if (midiZoomSlider("##midi_zoom", g_midiPps))
                g_midiScrollX = zoomAroundPlayhead(g_midiPps, g_play, std::max(1.f, ImGui::GetWindowWidth() - S(120)));
            ImGui::SameLine(); ImGui::SetNextItemWidth(S(95));
            double clipStart = mc.start;
            if (ImGui::InputDouble("Start (s)##midi_editor_clip", &clipStart, 0.0, 0.0, "%.3f")) mc.start = clip_timing::nonnegative(clipStart, mc.start);
            ImGui::SameLine(); ImGui::SetNextItemWidth(S(95));
            double clipDur = mc.dur;
            if (ImGui::InputDouble("Duration (s)##midi_editor_clip", &clipDur, 0.0, 0.0, "%.3f")) mc.dur = clip_timing::duration(clipDur, mc.dur, 0.01);
            if (ImGui::IsItemHovered()) helpTip("Shortening the clip can cut note playback. Note data is kept.");

            const bool mine = g_midiSelectedTrackId == trackId;
            for (auto it = g_midiSel.begin(); mine && it != g_midiSel.end();)
                it = (*it < 0 || *it >= (int)mc.notes.size()) ? g_midiSel.erase(it) : std::next(it);
            const size_t selCount = mine ? g_midiSel.size() : 0;
            if (selCount == 1) {
                MidiNote& selected = mc.notes[(size_t)*g_midiSel.begin()];
                ImGui::Text("Selected note:");
                ImGui::SameLine(); ImGui::Text("%s", midiPitchName(selected.pitch));
                ImGui::SameLine(); ImGui::SetNextItemWidth(S(100)); ImGui::InputInt("##midi_pitch", &selected.pitch, 0, 0);
                selected.pitch = std::min(127, std::max(0, selected.pitch));
                ImGui::SameLine(); ImGui::SetNextItemWidth(S(120)); ImGui::InputDouble("##midi_start", &selected.start, 0.0, 0.0, "+%.2f s");
                if (ImGui::IsItemHovered()) helpTip("Start, relative to the clip start");
                selected.start = std::max(0.0, selected.start);
                {   // the same start on the timeline
                    double absStart = mc.start + selected.start;
                    ImGui::SameLine(); ImGui::SetNextItemWidth(S(130));
                    if (ImGui::InputDouble("##midi_start_abs", &absStart, 0.0, 0.0, "@%.2f s")) selected.start = std::max(0.0, absStart - mc.start);
                    if (ImGui::IsItemHovered()) helpTip("Start on the timeline (the start above is relative to the MIDI clip)");
                }
                ImGui::SameLine(); ImGui::SetNextItemWidth(S(120)); ImGui::InputDouble("##midi_dur", &selected.dur, 0.0, 0.0, "%.2f s");
                selected.dur = std::max(0.001, selected.dur);
                ImGui::SameLine(); ImGui::Text("Vel"); ImGui::SameLine(0, S(4)); ImGui::SetNextItemWidth(S(70)); ImGui::InputInt("##midi_velocity", &selected.velocity, 0, 0);
                selected.velocity = std::min(127, std::max(1, selected.velocity));
                ImGui::SameLine();
            } else if (selCount > 1) {
                ImGui::Text("%d notes selected", (int)selCount);
                ImGui::SameLine();
                const bool qButton = ImGui::SmallButton("Q##midi_note_sync");
                const bool qKey = !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Q, false);
                if (qButton || qKey) {
                    historyFlush();
                    double first = 1e300;
                    for (int idx : g_midiSel) first = std::min(first, mc.notes[(size_t)idx].start);
                    for (int idx : g_midiSel) mc.notes[(size_t)idx].start = first;
                    historyFlush();
                }
                if (ImGui::IsItemHovered()) helpTip("Q: align selected note starts to the earliest.");
                ImGui::SameLine();
            } else {
                ImGui::TextDisabled("Double-click an empty cell to add a note, click to select, drag on empty space to box-select (Ctrl adds). Drag notes to move, drag the right edge to resize.");
            }
            if (selCount > 0 && ImGui::SmallButton(selCount > 1 ? "Delete notes" : "Delete note")) {
                for (auto it = g_midiSel.rbegin(); it != g_midiSel.rend(); ++it)
                    if (*it >= 0 && *it < (int)mc.notes.size()) mc.notes.erase(mc.notes.begin() + *it);
                g_midiSel.clear();
                g_midiSelectedNote = -1;
                g_midiSelectedTrackId = -1;
            }

            // Playhead keys: - back, + forward; Shift = fast (works while this editor window has focus)
            if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                const double step = io.KeyShift ? 2.0 : 0.25;
                const bool back = ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true);
                const bool fwd = ImGui::IsKeyPressed(ImGuiKey_Equal, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true);
                if (back != fwd) {
                    g_play = std::max(0.0, g_play + (fwd ? step : -step));
                    g_resync = 2;
                    const double vs = g_midiScrollX, ve = vs + (ImGui::GetContentRegionAvail().x - S(64)) / std::max(1.f, g_midiPps);
                    if (g_play < vs) g_midiScrollX = (float)std::max(0.0, g_play - 0.25);
                    else if (g_play > ve) g_midiScrollX = (float)std::max(0.0, g_play - (ve - vs) + 0.25);
                }
            }

            midiPitchSlider("##midi_pitch_range", g_midiPitchLow);
            ImGui::SameLine();
            const int pitchLow = g_midiPitchLow;
            const int pitchHigh = pitchLow + kMidiVisiblePitches - 1;
            const float keyboardW = S(64);
            const float rowH = S(kMidiRowH);
            const float rollH = rowH * (float)kMidiVisiblePitches;
            const float rollHeight = std::max(S(360), rollH + S(26));
            ImGui::BeginChild("##midi_roll", ImVec2(0, rollHeight), true, ImGuiWindowFlags_HorizontalScrollbar);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const ImVec2 rollMin(origin.x, origin.y);
            const ImVec2 rollMax(origin.x + available.x, origin.y + rollH + S(24));
            const float pianoX = origin.x + keyboardW;
            const float pianoRight = rollMax.x;
            const float rulerY = origin.y;
            const float pitchY = origin.y + S(24);
            ImGui::InvisibleButton("##midi_roll_canvas", ImVec2(available.x, rollH + S(24)));
            const bool canvasHovered = ImGui::IsItemHovered();
            if (canvasHovered && io.KeyCtrl && io.MouseWheel != 0.f) {
                g_midiPps = std::min(kPpsMax, std::max(kPpsMin, g_midiPps * (1.f + 0.12f * io.MouseWheel)));
                g_midiScrollX = zoomAroundPlayhead(g_midiPps, g_play, std::max(1.f, pianoRight - pianoX));
            } else if (canvasHovered && (io.MouseWheelH != 0.f || (io.KeyShift && io.MouseWheel != 0.f))) {
                const float wheel = io.MouseWheelH != 0.f ? io.MouseWheelH : io.MouseWheel;
                g_midiScrollX = std::max(0.f, g_midiScrollX - wheel * 60.f / std::max(1.f, g_midiPps));
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::PushClipRect(rollMin, ImVec2(rollMax.x, rollMax.y), true);
            dl->AddRectFilled(rollMin, rollMax, IM_COL32(22, 23, 26, 255));
            dl->AddRectFilled(ImVec2(origin.x, pitchY), ImVec2(pianoX, pitchY + rollH), IM_COL32(31, 32, 36, 255));
            dl->AddRectFilled(ImVec2(pianoX, rulerY), ImVec2(pianoRight, pitchY), IM_COL32(31, 32, 36, 255));

            const double viewStart = std::max(0.0, (double)g_midiScrollX);
            const double viewEnd = viewStart + (pianoRight - pianoX) / std::max(1.f, g_midiPps);
            auto timeX = [&](double time) { return pianoX + (float)(time - viewStart) * g_midiPps; };
            auto xTime = [&](float x) { return viewStart + (double)(x - pianoX) / std::max(1.f, g_midiPps); };
            const double grid = 0.25;
            const long long first = (long long)std::floor(viewStart / grid);
            for (long long k = first; k * grid <= viewEnd; ++k) {
                const double time = k * grid;
                const float x = timeX(time);
                const bool major = (k % 4) == 0;
                dl->AddLine(ImVec2(x, pitchY), ImVec2(x, pitchY + rollH),
                            major ? IM_COL32(58, 59, 66, 255) : IM_COL32(38, 39, 44, 255));
                if (major) {
                    char label[24];
                    std::snprintf(label, sizeof(label), "%.1fs", time);
                    dl->AddText(ImVec2(x + S(3), rulerY + S(4)), IM_COL32(174, 175, 183, 255), label);
                }
            }

            for (int pitch = pitchHigh; pitch >= pitchLow; --pitch) {
                const int row = pitchHigh - pitch;
                const float y0 = pitchY + row * rowH;
                const bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10);
                const ImU32 rowColor = black ? IM_COL32(27, 28, 32, 255) : IM_COL32(35, 36, 40, 255);
                dl->AddRectFilled(ImVec2(pianoX, y0), ImVec2(pianoRight, y0 + rowH), rowColor);
                dl->AddLine(ImVec2(origin.x, y0 + rowH), ImVec2(pianoRight, y0 + rowH), IM_COL32(48, 49, 55, 255));
                dl->AddText(ImVec2(origin.x + S(6), y0 + S(1)),
                            pitch % 12 == 0 ? IM_COL32(210, 211, 218, 255) : IM_COL32(138, 140, 148, 255),
                            midiPitchName(pitch));
            }

            {
                const float clipX0 = timeX(mc.start), clipX1 = timeX(mc.start + mc.dur);
                if (clipX0 > pianoX) dl->AddRectFilled(ImVec2(pianoX, pitchY), ImVec2(std::min(clipX0, pianoRight), pitchY + rollH), IM_COL32(0, 0, 0, 120));
                if (clipX1 < pianoRight) dl->AddRectFilled(ImVec2(std::max(clipX1, pianoX), pitchY), ImVec2(pianoRight, pitchY + rollH), IM_COL32(0, 0, 0, 120));
                const ImU32 edge = trackColor((int)laneIndex, 200, clipColorValue(track, tgt));
                if (clipX0 >= pianoX && clipX0 <= pianoRight) dl->AddLine(ImVec2(clipX0, rulerY), ImVec2(clipX0, pitchY + rollH), edge, S(2));
                if (clipX1 >= pianoX && clipX1 <= pianoRight) dl->AddLine(ImVec2(clipX1, rulerY), ImVec2(clipX1, pitchY + rollH), edge, S(2));
            }
            for (int ni = 0; ni < (int)mc.notes.size(); ++ni) {
                const MidiNote& note = mc.notes[(size_t)ni];
                if (note.pitch < pitchLow || note.pitch > pitchHigh) continue;
                const int row = pitchHigh - note.pitch;
                const float y0 = pitchY + row * rowH + S(1);
                const float y1 = y0 + rowH - S(2);
                const float x0 = timeX(mc.start + note.start), x1 = timeX(mc.start + note.start + note.dur);
                if (x1 < pianoX || x0 > pianoRight) continue;
                const bool selected = g_midiSelectedTrackId == trackId && g_midiSel.count(ni) > 0;
                const ImU32 noteColor = trackColor((int)laneIndex, selected ? 255 : 220, clipColorValue(track, tgt));
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x0 + S(5), x1), y1), noteColor, S(2));
                dl->AddRect(ImVec2(x0, y0), ImVec2(std::max(x0 + S(5), x1), y1),
                            selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), S(2));
                char velocity[16];
                std::snprintf(velocity, sizeof(velocity), "%d", note.velocity);
                if (x1 - x0 > S(24)) dl->AddText(ImVec2(x0 + S(3), y0), IM_COL32(255, 255, 255, 230), velocity);
            }

            {   // playhead: shown in the editor; click or drag the ruler to move it
                static bool rulerDrag = false;
                if (canvasHovered && ImGui::IsMouseClicked(0) && mouse.x >= pianoX && mouse.y >= rulerY && mouse.y < pitchY) rulerDrag = true;
                if (rulerDrag) {
                    if (ImGui::IsMouseDown(0)) { g_play = std::max(0.0, xTime(std::min(pianoRight, std::max(pianoX, mouse.x)))); g_resync = 2; }
                    else rulerDrag = false;
                }
                const float px = timeX(g_play);
                if (px >= pianoX && px <= pianoRight) {
                    dl->AddLine(ImVec2(px, rulerY), ImVec2(px, pitchY + rollH), IM_COL32(255, 90, 80, 255), S(2));
                    dl->AddTriangleFilled(ImVec2(px - S(5), rulerY), ImVec2(px + S(5), rulerY), ImVec2(px, rulerY + S(9)), IM_COL32(255, 90, 80, 255));
                }
            }
            auto pitchAt = [&](float y) { return std::min(pitchHigh, std::max(pitchLow, pitchHigh - (int)((y - pitchY) / rowH))); };
            auto selectOnly = [&](int ni) {
                g_midiSelectedTrackId = trackId;
                g_midiSel.clear();
                g_midiSel.insert(ni);
                g_midiSelectedNote = ni;
            };
            auto refreshPrimary = [&]() {
                g_midiSelectedNote = g_midiSel.empty() ? -1 : *g_midiSel.begin();
                if (g_midiSel.empty()) g_midiSelectedTrackId = -1;
            };
            if (canvasHovered && ImGui::IsMouseClicked(0) && mouse.x >= pianoX && mouse.y >= pitchY && mouse.y <= pitchY + rollH) {
                const int pitch = pitchAt(mouse.y);
                const double time = std::max(0.0, xTime(mouse.x) - mc.start);  // clip-relative
                const bool additive = io.KeyCtrl || io.KeyShift;
                int hit = -1;
                for (int ni = (int)mc.notes.size() - 1; ni >= 0; --ni) {
                    const MidiNote& note = mc.notes[(size_t)ni];
                    if (note.pitch == pitch && time >= note.start && time <= note.start + note.dur) { hit = ni; break; }
                }
                if (g_midiSelectedTrackId != trackId) { g_midiSel.clear(); g_midiSelectedNote = -1; }
                if (hit >= 0) {
                    g_midiSelectedTrackId = trackId;
                    bool stillSelected = true;
                    if (additive) {
                        if (g_midiSel.count(hit)) { g_midiSel.erase(hit); stillSelected = false; }
                        else g_midiSel.insert(hit);
                        refreshPrimary();
                        if (!g_midiSel.empty()) g_midiSelectedTrackId = trackId;
                    } else if (!g_midiSel.count(hit)) {
                        selectOnly(hit);
                    }
                    if (stillSelected) {
                        const MidiNote& note = mc.notes[(size_t)hit];
                        const float noteEndX = timeX(mc.start + note.start + note.dur);
                        g_midiDragMode = mouse.x >= noteEndX - S(8) ? 2 : 1;
                        if (g_midiDragMode == 2) selectOnly(hit);
                        g_midiMoveOrig.clear();
                        for (int idx : g_midiSel)
                            if (idx >= 0 && idx < (int)mc.notes.size())
                                g_midiMoveOrig.push_back({idx, mc.notes[(size_t)idx].start, mc.notes[(size_t)idx].pitch});
                        g_midiDragTrackId = trackId;
                        g_midiDragNote = hit;
                        g_midiDragTimeOffset = time;       // grab position (clip-relative)
                        g_midiDragPitchOffset = pitch;     // grab pitch
                    }
                } else {
                    // empty cell: click adds a note, drag box-selects (like automation dots)
                    if (!additive) { g_midiSel.clear(); g_midiSelectedNote = -1; g_midiSelectedTrackId = -1; }
                    g_midiMarquee = true;
                    g_midiMarqueeClip = trackId;
                    g_midiMarqueeStart = mouse;
                    g_midiMarqueeMoved = false;
                    g_midiMarqueeAdd = additive;
                    {   // double-click on an empty cell adds a note; one click only selects / clears
                        const double nowT = ImGui::GetTime();
                        g_midiMarqueeDbl = ImGui::IsMouseDoubleClicked(0) ||
                            (nowT - g_midiEmptyPressT < 0.5 && std::fabs(mouse.x - g_midiEmptyPressPos.x) < S(16) && std::fabs(mouse.y - g_midiEmptyPressPos.y) < S(16));
                        g_midiEmptyPressT = g_midiMarqueeDbl ? -10.0 : nowT;
                        g_midiEmptyPressPos = mouse;
                    }
                }
            }

            if (g_midiDragTrackId == trackId && ImGui::IsMouseDown(0) && g_midiDragNote >= 0 && g_midiDragNote < (int)mc.notes.size()) {
                if (g_midiDragMode == 2) {
                    MidiNote& note = mc.notes[(size_t)g_midiDragNote];
                    note.dur = std::max(0.001, clampTimelineTime(xTime(mouse.x) - mc.start - note.start));
                } else if (mouse.x >= pianoX && mouse.y >= pitchY && mouse.y <= pitchY + rollH) {
                    double dt = (xTime(mouse.x) - mc.start) - g_midiDragTimeOffset;
                    int dp = pitchAt(mouse.y) - g_midiDragPitchOffset;
                    for (const MidiOrig& o : g_midiMoveOrig) {
                        dt = std::max(dt, -o.start);
                        dp = std::min(127 - o.pitch, std::max(-o.pitch, dp));
                    }
                    for (const MidiOrig& o : g_midiMoveOrig) {
                        if (o.idx < 0 || o.idx >= (int)mc.notes.size()) continue;
                        mc.notes[(size_t)o.idx].start = std::max(0.0, o.start + dt);
                        mc.notes[(size_t)o.idx].pitch = o.pitch + dp;
                    }
                }
            }
            if (g_midiDragTrackId == trackId && !ImGui::IsMouseDown(0)) {
                g_midiDragTrackId = -1;
                g_midiDragNote = -1;
                g_midiDragMode = 0;
                g_midiMoveOrig.clear();
            }

            if (g_midiMarquee && g_midiMarqueeClip == trackId) {
                if (std::fabs(mouse.x - g_midiMarqueeStart.x) + std::fabs(mouse.y - g_midiMarqueeStart.y) > S(3)) g_midiMarqueeMoved = true;
                const float qx0 = std::min(mouse.x, g_midiMarqueeStart.x), qx1 = std::max(mouse.x, g_midiMarqueeStart.x);
                const float qy0 = std::min(mouse.y, g_midiMarqueeStart.y), qy1 = std::max(mouse.y, g_midiMarqueeStart.y);
                if (ImGui::IsMouseDown(0)) {
                    if (g_midiMarqueeMoved) {
                        dl->AddRectFilled(ImVec2(qx0, qy0), ImVec2(qx1, qy1), IM_COL32(237, 128, 38, 40));
                        dl->AddRect(ImVec2(qx0, qy0), ImVec2(qx1, qy1), IM_COL32(237, 128, 38, 220));
                    }
                } else {
                    if (!g_midiMarqueeMoved && g_midiMarqueeDbl) {
                        MidiNote note;
                        note.start = clampTimelineTime(std::max(0.0, xTime(g_midiMarqueeStart.x) - mc.start));
                        note.dur = 0.5;
                        note.pitch = pitchAt(g_midiMarqueeStart.y);
                        note.velocity = 100;
                        mc.notes.push_back(note);
                        selectOnly((int)mc.notes.size() - 1);
                    } else if (g_midiMarqueeMoved) {
                        if (g_midiSelectedTrackId != trackId) g_midiSel.clear();
                        for (int ni = 0; ni < (int)mc.notes.size(); ++ni) {
                            const MidiNote& note = mc.notes[(size_t)ni];
                            if (note.pitch < pitchLow || note.pitch > pitchHigh) continue;
                            const float ny0 = pitchY + (pitchHigh - note.pitch) * rowH, ny1 = ny0 + rowH;
                            const float nx0 = timeX(mc.start + note.start), nx1 = timeX(mc.start + note.start + note.dur);
                            if (nx1 >= qx0 && nx0 <= qx1 && ny1 >= qy0 && ny0 <= qy1) g_midiSel.insert(ni);
                        }
                        g_midiSelectedTrackId = trackId;
                        refreshPrimary();
                        if (!g_midiSel.empty()) g_midiSelectedTrackId = trackId;
                    }
                    g_midiMarquee = false;
                }
            }
            if (g_midiSelectedTrackId == trackId && !g_midiSel.empty() &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                for (auto it = g_midiSel.rbegin(); it != g_midiSel.rend(); ++it)
                    if (*it >= 0 && *it < (int)mc.notes.size()) mc.notes.erase(mc.notes.begin() + *it);
                g_midiSel.clear();
                g_midiSelectedTrackId = -1;
                g_midiSelectedNote = -1;
            }
            ImGui::PopClipRect();
            ImGui::EndChild();
        }
        ImGui::End();
        mc.editorOpen = open;
    }
}

}  // namespace

void appOpenProjectPath(const char* path) {
    if (path && *path) openProjectPath(path);
}

// ---------------------------------------------------------------- public
void appSetupStyle(float dpi) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4; s.PopupRounding = 4; s.FrameRounding = 3; s.GrabRounding = 3; s.ScrollbarRounding = 3;
    s.FramePadding = ImVec2(9, 5); s.ItemSpacing = ImVec2(8, 6); s.WindowBorderSize = 1;
    // Hide ImGui's title-bar collapse triangle on all CAW windows. Users can
    // still double-click the title bar to collapse/expand standard windows.
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.ScaleAllSizes(dpi);
    g_ui = dpi;

    const ImVec4 accent(0.93f, 0.50f, 0.15f, 1.f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]              = ImVec4(0.88f, 0.88f, 0.90f, 1.f);
    c[ImGuiCol_TextDisabled]      = ImVec4(0.48f, 0.48f, 0.52f, 1.f);
    c[ImGuiCol_WindowBg]          = ImVec4(0.09f, 0.09f, 0.10f, 1.f);
    c[ImGuiCol_ChildBg]           = ImVec4(0.09f, 0.09f, 0.10f, 1.f);
    c[ImGuiCol_PopupBg]           = ImVec4(0.03f, 0.03f, 0.04f, 0.99f);    // clearly darker than the windows: combo lists / menus read as their own layer, not part of the panel
    c[ImGuiCol_Border]            = ImVec4(0.45f, 0.45f, 0.50f, 0.65f);    // a clearly visible hairline so popups and panels keep a crisp edge on any background
    c[ImGuiCol_FrameBg]           = ImVec4(0.14f, 0.14f, 0.16f, 1.f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.19f, 0.19f, 0.22f, 1.f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.22f, 0.22f, 0.26f, 1.f);
    c[ImGuiCol_TitleBg]           = ImVec4(0.07f, 0.07f, 0.08f, 1.f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.12f, 0.12f, 0.14f, 1.f);
    c[ImGuiCol_Button]            = ImVec4(0.20f, 0.20f, 0.23f, 1.f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(0.28f, 0.28f, 0.32f, 1.f);
    c[ImGuiCol_ButtonActive]      = accent;
    c[ImGuiCol_Header]            = ImVec4(0.93f, 0.50f, 0.15f, 0.35f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.93f, 0.50f, 0.15f, 0.55f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.93f, 0.50f, 0.15f, 0.75f);
    c[ImGuiCol_SliderGrab]        = accent;
    c[ImGuiCol_SliderGrabActive]  = ImVec4(1.00f, 0.62f, 0.25f, 1.f);
    c[ImGuiCol_CheckMark]         = accent;
    c[ImGuiCol_TextSelectedBg]    = ImVec4(0.93f, 0.50f, 0.15f, 0.40f);
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.07f, 0.07f, 0.08f, 1.f);
    c[ImGuiCol_Separator]         = ImVec4(0.20f, 0.20f, 0.23f, 1.f);
}

void appSetCodeFonts(ImFont* regular, ImFont* bold, ImFont* italic) {
    g_codeFonts[0] = regular;
    g_codeFonts[1] = bold ? bold : regular;
    g_codeFonts[2] = italic ? italic : regular;
}

void appSetMarkFonts(ImFont* ui, ImFont* code) {
    g_markUiFont = ui;
    g_markCodeFont = code;
}

void appPrepareLogo() {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    // Preferred: render the vector logo at the exact pixel size of each place that shows it.
    {
        int sizes[kLogoVariantCount] = {};
        std::vector<unsigned char> renders[kLogoVariantCount];
        bool vectorOk = true;
        for (int i = 0; i < kLogoVariantCount && vectorOk; ++i) {
            sizes[i] = std::max(16, (int)std::lround(S(kLogoSizes[i])));
            vectorOk = rasterizeLogo(sizes[i], renders[i]);
        }
        if (vectorOk) {
            int ids[kLogoVariantCount];
            for (int i = 0; i < kLogoVariantCount; ++i) ids[i] = atlas->AddCustomRectRegular(sizes[i], sizes[i]);
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
            bool packed = pixels && width > 0 && height > 0;
            for (int i = 0; i < kLogoVariantCount && packed; ++i) {
                const ImFontAtlasCustomRect* rect = atlas->GetCustomRectByIndex(ids[i]);
                if (!rect || !rect->IsPacked()) { packed = false; break; }
                for (int y = 0; y < sizes[i]; ++y)
                    std::memcpy(pixels + ((size_t)(rect->Y + y) * (size_t)width + rect->X) * 4u,
                                &renders[i][(size_t)y * sizes[i] * 4u], (size_t)sizes[i] * 4u);
                g_logoVariants[i].px = sizes[i];
                atlas->CalcCustomRectUV(rect, &g_logoVariants[i].uv0, &g_logoVariants[i].uv1);
            }
            g_logoVariantsReady = packed ? kLogoVariantCount : 0;
            return;
        }
    }
    // Fallback (vector logo could not be parsed): the old 512 px bitmap.
    const int id = atlas->AddCustomRectRegular(512, 512);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
    const ImFontAtlasCustomRect* rect = atlas->GetCustomRectByIndex(id);
    if (!pixels || !rect || !rect->IsPacked() || width <= 0 || height <= 0) return;
    size_t pixel = 0;
    for (size_t i = 0; i + 1 < sizeof(kLogoRle); i += 2) {
        const unsigned count = kLogoRle[i], color = kLogoRle[i + 1];
        for (unsigned n = 0; n < count && pixel < 512u * 512u; ++n, ++pixel) {
            const size_t x = pixel % 512u, y = pixel / 512u;
            unsigned char* dst = pixels + ((size_t)(rect->Y + y) * (size_t)width + rect->X + x) * 4u;
            std::memcpy(dst, kLogoPalette + color * 4u, 4u);
        }
    }
    if (pixel != 512u * 512u) return;
    atlas->CalcCustomRectUV(rect, &g_logoUv0, &g_logoUv1);
    g_logoReady = true;
}

bool appNeedsResponsiveFrames() {
    return g_playing || g_timelineRun || g_renderActive || g_testProc.active() || g_testSound.running();
}

// An engine is started but nothing is playing: 30 fps is enough for meters and status, saves CPU/GPU.
bool appEnginesIdleRunning() {
    return g_scEngineOn || g_liveCsound || g_pdEngineOn || g_mixedPd;
}

void appDrawStartupSplash() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##startup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = vp->WorkPos, size = vp->WorkSize;
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 23, 28, 255));
    const float iconSize = S(240), left = origin.x + (size.x - iconSize) * .5f, top = origin.y + size.y * .5f - S(170);
    drawLogo(dl, ImVec2(left, top), iconSize);
    auto centerText = [&](const char* text, float y, ImU32 color, float scale) {
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * scale;
        const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text);
        dl->AddText(font, fontSize, ImVec2(origin.x + (size.x - extent.x) * .5f, y), color, text);
    };
    centerText("CAW - Code Audio Workstation", top + iconSize + S(18), IM_COL32(248, 244, 237, 255), 1.35f);
    centerText("0.0.34 beta", top + iconSize + S(52), IM_COL32(220, 221, 225, 255), 1.f);
    ImGui::End();
}

// A SuperCollider installed on the PC must never be used: ignore old saved paths that point into one.
bool isSystemSuperColliderPath(const std::string& path) {
    if (!plat::isWindows || path.empty()) return false;
    std::string lower = path;
    for (char& c : lower) { if (c == '/') c = '\\'; c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (lower.find("supercollider") == std::string::npos) return false;
    if (lower.find(".caw_deps") != std::string::npos) return false;
    if (lower.find("\\caw\\supercollider\\") != std::string::npos) return false;   // CAW's own copy
    for (const char* var : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        std::string root = plat::getEnv(var);
        if (root.empty()) continue;
        for (char& c : root) { if (c == '/') c = '\\'; c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        if (lower.compare(0, root.size(), root) == 0) return true;
    }
    return false;
}

void appInit() {
    const auto began = std::chrono::steady_clock::now();
    g_pdLibraryPath = plat::bundledLibpd();
    g_scLangPath = plat::superColliderSclang();
    const auto scConfig = scConfigPath();
    if (!scConfig.empty()) {
        std::ifstream in(scConfig, std::ios::binary);
        if (in) {
            std::string saved; std::getline(in, saved);
            std::error_code ec;
            if (!saved.empty() && !isSystemSuperColliderPath(saved) && std::filesystem::is_regular_file(std::filesystem::u8path(saved), ec) && !ec)
                g_scLangPath = saved;
        }
    }
    const auto scSynthConfig = scSynthConfigPath();
    if (!scSynthConfig.empty()) {
        std::ifstream in(scSynthConfig, std::ios::binary);
        if (in) {
            std::string saved; std::getline(in, saved);
            std::error_code ec;
            if (!saved.empty() && !isSystemSuperColliderPath(saved) && std::filesystem::is_regular_file(std::filesystem::u8path(saved), ec) && !ec)
                g_scSynthPath = saved;
        }
    }
    const auto pdConfig = pdConfigPath();
    if (!pdConfig.empty()) {
        std::ifstream in(pdConfig, std::ios::binary);
        if (in) {
            std::string saved; std::getline(in, saved);
            std::error_code ec;
            if (!saved.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(saved), ec) && !ec)
                g_pdLibraryPath = saved;
        }
    }
    for (float& v : g_specSm) v = -120.f;
    historyReset();
    g_savedSnap = serializeProject(g_proj);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    logLine("[caw] Startup initialization: " + std::to_string(ms) + " ms (audio scan deferred).");
    logLine("[caw] Ready. File > New project starts an empty project, File > Open project loads one.");
}

// The help panel: shows the explanation of whatever the mouse is over (all the old hover pop-ups), with an on / off box.
float helpPanelHeight() { return g_helpOn ? S(66) : 0.f; }
void drawHelpPanel() {
    if (!g_helpOn) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.14f, 1.f));
    ImGui::BeginChild("##helppanel", ImVec2(0, helpPanelHeight()), false);
    ImGui::SetCursorPos(ImVec2(S(8), S(3)));
    ImGui::PushTextWrapPos(0.f);
    if (g_helpShown.empty()) ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.f), "Hover over a control for help.");
    else ImGui::TextUnformatted(g_helpShown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void appFrame(float dt, bool visible) {
    struct FrameTimer {   // CAW_FRAMELOG=1: log every UI frame that takes longer than 20 ms
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~FrameTimer() {
            if (!cawEnv("CAW_FRAMELOG")) return;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 20.0) logLine("[caw] SLOW UI FRAME: " + std::to_string((int)ms) + " ms");
        }
    } frameTimer;
    ImGuiIO& io = ImGui::GetIO();
    static const float baseFontScale = io.FontGlobalScale;
    io.FontGlobalScale = baseFontScale * g_proj.cs.programFontSize / 17.f;
    pollProcesses(dt);
    pollTestAndScan(dt);
    if (!visible) {
        // Keep transport, export completion, recording, autosave and plugin
        // learning alive without rebuilding an invisible arrangement.
        updateMeters(dt);
        updateRecording();
        vstui::serviceLearn(g_proj);
        if (g_playing && g_useCsoundApi) g_apiProc.setMasterGain(g_proj.cs.masterGain);
        if (g_scEngineOn) g_scEngine.setGain(g_proj.cs.masterGain);
        historyTick(dt);
        tickAutosave(dt);
        return;
    }
    const bool globalKeys = !io.WantTextInput && !g_textEditorFocused;  // code editors keep their own Ctrl+Z
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Q, false)) appRequestClose();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveProject(false);
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) openProject();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) newProject();
    if (globalKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) redo(); else undo();
    }
    if (globalKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    // Space = play / stop the timeline. Never while a code editor window has the keyboard focus: a click on
    // a title bar or scrollbar left the editor focused and space then started playback while typing.
    // Ctrl+Space always works.
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        const bool plain = !io.KeyCtrl && !io.WantTextInput && !g_codeTyping && !g_textEditorFocused;
        const bool withCtrl = io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
        if (plain || withCtrl) toggleTimelinePlay();
    }
    updateMeters(dt);
    if (g_scEngineOn) g_scEngine.setGain(g_proj.cs.masterGain);
    updateRecording();
    {   // idle layout (meters of the current project while Csound is off)
        static int tick = 0;
        if (!g_liveCsound && (tick++ % 30) == 0) g_idleMeters = meterLayout(g_proj);
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleVar(2);
    drawMainMenuBar();
    drawTransport();
    g_helpShown = g_helpNext; g_helpNext.clear();
    const float belowH = (g_showMonitor ? S(g_monitorH + kMonitorSplitterH) : 0.f) + helpPanelHeight();
    const float autoMeterW = std::min(320.f, std::max(160.f, 40.f + masterChannels(g_proj.cs) * 28.f));
    const float meterW = g_viewMeter ? S(g_meterW > 0.f ? g_meterW : autoMeterW) : 0.f;
    drawArrange(belowH, meterW);
    if (g_viewMeter) {
        ImGui::SameLine(0, 0);
        drawMeterPanel(dt, meterW, belowH);
    }
    if (g_showMonitor) {
        drawMonitorSplitter();
        drawMonitor(dt);
    }
    drawHelpPanel();
    ImGui::End();

    if (g_confirmRenderOverwrite) {
        ImGui::OpenPopup("Confirm audio export");
        g_confirmRenderOverwrite = false;
    }
    if (ImGui::BeginPopupModal("Confirm audio export", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_renderWarnVst) ImGui::TextWrapped("CAW saves exposed writable VST parameter values, including unmapped ones. Plugin-specific state, samples and preset data may not be restored. Check your export.");
        if (g_renderWillOverwrite) {
            ImGui::TextWrapped("This file already exists: %s", renderFilePath().string().c_str());
            ImGui::TextUnformatted("Replace it after the new export succeeds?");
        }
        if (ImGui::Button(g_renderWillOverwrite ? "Replace" : "Export anyway")) { ImGui::CloseCurrentPopup(); startRenderNow(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (g_askSaveOnClose) { ImGui::OpenPopup("Save changes?###save_on_close"); g_askSaveOnClose = false; }
    if (ImGui::BeginPopupModal("Save changes?###save_on_close", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The project has unsaved changes. Save them before closing?");
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(S(110), 0))) {
            saveProject(false);
            if (serializeProject(g_proj) == g_savedSnap) { g_quitRequested = true; ImGui::CloseCurrentPopup(); }   // not saved (dialog cancelled / error): stay open
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save", ImVec2(S(110), 0))) { g_quitRequested = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_askPdOnClose) { ImGui::OpenPopup("Unsaved PD patches###pd_quit"); g_askPdOnClose = false; }
    if (ImGui::BeginPopupModal("Unsaved PD patches###pd_quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Some PD patch edits are unsaved. Save them in their clip windows before exiting.");
        if (ImGui::Button("Keep editing")) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Discard patch edits and exit")) {
            g_pdPatchBuffers.clear();
            ImGui::CloseCurrentPopup();
            appRequestClose();
        }
        ImGui::EndPopup();
    }

    drawSettings();
    drawAudioExport();
    drawScoreEditor();
    drawScoreExportWindow();
    drawCodeExportWindow();
    drawScoreMapWindow();
    drawEditor();
    drawAudioPool();
    drawIncludePool();
    drawMidiEditor();
    drawMultiMidiEditor();
    vstui::drawWindows(g_proj);
    if (g_playing && g_useCsoundApi) g_apiProc.setMasterGain(g_proj.cs.masterGain);
    historyTick(dt);
    tickAutosave(dt);
}

// Close button / Ctrl+Q / File > Exit: with unsaved changes the program first asks to save.
void appRequestClose() {
    for (const auto& item : g_pdPatchBuffers) {
        if (item.second.dirty()) { g_askPdOnClose = true; return; }
    }
    vstui::captureParameters(g_proj);
    if (serializeProject(g_proj) != g_savedSnap) g_askSaveOnClose = true;
    else g_quitRequested = true;
}

// True after Ctrl+Q / File > Exit: stops the engine and the renderers, then the main loop ends.
bool appQuitRequested() {
    if (!g_quitRequested) return false;
    g_pdTimelineEngine.stop();
    g_pdPreview.stop();
    stopScEngine();
    g_scEngine.stop();
    stopPlayback();
    stopVstEngine(true);
    return true;
}
