#pragma once
#include "model.h"
#include <filesystem>

// Where one clip's code sits in a generated orchestra, to turn Csound's "line N" back into "instr Lead1, clip line K".
struct OrcSpan {
    std::string name;                  // instrument (clip) name
    int first = 0;                     // line of the first body line in the .csd file (1-based, the way Csound counts)
    std::vector<int> lineMap;          // body line (0-based) -> clip line (1-based, 0 = generated line)
};

// Result of preprocessing one track's code.
struct ParsedTrack {
    std::string body;                  // instrument body with <caw> removed and cawget translated
    std::vector<CawParam> params;      // automation parameters declared in <caw>
    bool midiEnabled = false;           // <caw> midi: "on"
    bool launcher = false;              // the code starts copies of itself (dawrun / schedule "this"): the clip's x field is then the copy count
    std::vector<int> lineMap;          // body line (0-based) -> line of the clip code (1-based, 0 = generated); kept in step with the meter taps
    int scoreSig = 0;                   // the code sends to the channel "score": 0 none, 1 number (k / i), 2 signal (a), 3 string
    bool hasRec = false;                // the code sends to the channel "rec" (chnset kVal, "rec"): recorded into an automation lane (Capture / Rec)
    std::vector<std::string> writeChannels; // named automation destinations emitted by cawset
    int outChannels = 0;                // output channels found in out / outs / outq / outch ... (only when taps are on)
};

std::string channelName(const std::string& instr, const std::string& chn);  // "Lead1_input1"
// File the score probe (Render audio) writes: per clip the measured amplitude / frequency and everything sent to the channel "score".
std::string scoreProbePath();
// taps = true (engine run only): every out / outs / outq / outch line also feeds a meter bus, gaCawT_<instr>_<channel>.
ParsedTrack parseCode(const std::string& instrName, const std::string& code, bool taps = false, int runs = 1);  // one instrument's code
// Automation value of a lane at `time` (linear between the points; `fallback` before the first point).
double autoValueAt(const std::vector<AutoPoint>& points, double time, double fallback);
// Snap an automation value to the declared minimum + N * step, then clamp it.
double quantizeAutomation(double value, const CawParam& param);
// M / S of an automation lane: a muted lane (or any lane that is not soloed while another lane of the track is soloed) does not play,
// the channel stays at its default value, and the lane is not drawn in the score.
bool autoLaneActive(const Track& t, const std::string& channel);
// MIDI notes of the MIDI clips aimed at a clip, in absolute timeline seconds (sorted by start, pitch and velocity clamped, a note never runs past its MIDI clip).
bool clipHasMidiNotes(const Project& p, int clipId);                                // the clip has MIDI notes in a MIDI clip
std::vector<MidiNote> clipMidiNotes(const Project& p, int clipId);
// Folder for the files CAW makes while it runs (generated .csd files): a CAW folder in the system temp folder.
std::filesystem::path workDir();
ParsedTrack parseClip(const Clip& c, bool taps = false);                       // a clip = one independent instr
ParsedTrack parseClipForTrack(const Clip& c, const Track& t, bool taps = false);
std::string resolveCawFiles(const Project& p, const std::string& code);
// Lane summary: union of every clip's <caw> channels (plus the lane template), midiEnabled if any clip uses MIDI.
// body is empty; use parseClip for real instrument code.
ParsedTrack parseTrack(const Track& t);

// Meter layout shared by the CSD builder and the UI: the first masterChannels() indices are the master outputs, then every lane that has
// output channels gets `channels` consecutive entries (a mono instrument = 1, stereo = 2, quad = 4 ... max 8).
// Master meter = one entry per output channel (nchnls), so indices 0 .. masterChannels-1 are the master outputs.
inline int masterChannels(const CsoundSettings& s) { return s.nchnls < 1 ? 1 : (s.nchnls > 64 ? 64 : s.nchnls); }
struct LaneMeterInfo { int trackId = 0; int first = 0; int channels = 0; };
std::vector<LaneMeterInfo> meterLayout(const Project& p);
// Clips (unique names, project order) whose code sends to the channel "rec": the engine monitor streams them to the app for the live Rec button.
std::vector<std::string> recClipNames(const Project& p);
struct CawWriteTarget { std::string clip, channel; };
std::vector<CawWriteTarget> cawWriteTargets(const Project& p);
constexpr int kMeterBands = 64;     // spectrum bands sent by the monitor instrument (25 Hz .. 18 kHz, logarithmic)
constexpr int kScopePoints = 128;    // scope points per channel

// Csound options of a live run as generated from the settings fields (used while "options" is empty).
std::string defaultOptions(const CsoundSettings& s);
// Reads -odac / -iadc / -+rtaudio / -b / -B out of s.options back into the settings fields.
void syncSettingsFromOptions(CsoundSettings& s);
// Writes the settings fields (device, backend, buffers) into the matching tokens of s.options.
void syncOptionsFromSettings(CsoundSettings& s);
std::string buildHeader(const CsoundSettings& s);                            // sr, ksmps, nchnls, 0dbfs, A4
// mode: "engine" (Run: real time, instruments only, the timeline triggers them over stdin) or "render" (the output file is given on the csound command line with -o)
std::string buildOptions(const CsoundSettings& s, const std::string& mode);
// fromTime: start playing from this position (seconds); clips are shifted so the CSD starts at 0
// spans (optional): filled with the position of every clip's code in the orchestra.
// Set by the app: returns the Csound device number of the Windows default sound card (or -1). Used when the output device is "System default".
extern int (*g_cawDefaultDevice)(bool capture);
// Set by the app before every Play: control periods per scope point (the scope time window).
extern int g_cawScopeDiv;
extern int g_cawScopePeak;

// Run / Play link without port, stdin or tables: numbered command files (caw_cmd_<n>.txt + caw_cmd_serial.txt) that the running Csound reads when the serial changes.
// Clips and automation channels are referred to by their number in these lists (fixed when Run is pressed).
std::vector<std::string> engineClipNames(const Project& p);      // unique clip (instrument) names, project order
std::string engineFilePrefix();                                  // "<temp>/CAW/caw_cmd_": the files are <prefix>serial.txt and <prefix><n>.txt
std::vector<std::string> engineChannelNames(const Project& p);   // unique automation channel names (clip_channel), project order

// THE timeline: a clip is an instrument with a start time; this is the list of what starts when (the score of a render AND of the live Play).
// start = seconds after `from`. Clip events: start the instrument `name` for `dur` s with p4 = a, p5 = b (MIDI note: pitch, velocity).
// Automation events: ramp the channel `name` from a to b over `dur` s.
// includeSounding = true: events that started before `from` and still sound are included (started now, shortened). false: only what starts at or after `from`.
struct TimelineEvent { bool automation = false; std::string name; double start = 0.0, dur = 0.0, a = 0.0, b = 0.0; };
std::vector<TimelineEvent> timelineEvents(const Project& p, double from, bool includeSounding = true);
unsigned long long timelineSignature(const Project& p);          // changes when anything Play would start changes

std::string buildCsd(const Project& p, const std::string& mode, double fromTime = 0.0, std::vector<OrcSpan>* spans = nullptr);

// One-clip test CSD (no audio output).
std::string buildClipTestCsd(const Project& p, const Clip& clip, OrcSpan& span);
