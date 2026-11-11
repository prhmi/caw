#pragma once
#include "model.h"

// Where one clip's code sits in a generated orchestra, to turn Csound's "line N" back into "instr Lead1, clip line K".
struct OrcSpan {
    std::string name;                  // instrument (clip) name
    int first = 0;                     // orchestra line (counted from <CsInstruments>, 1-based) of the first body line
    std::vector<int> lineMap;          // body line (0-based) -> clip line (1-based, 0 = generated line)
};
struct PdSend { std::string name, to, chn; };   // Pure Data clip: {send: "pitch", to: "Lead1", chn: "input1"}

// Result of preprocessing one track's code.
struct ParsedTrack {
    std::string body;                  // instrument body with <caw> removed and "caw" opcode translated
    std::vector<CawParam> params;      // automation parameters declared in <caw>
    bool midiEnabled = false;           // <caw> midi: "on"
    bool launcher = false;              // the code starts copies of itself (dawrun / schedule "this"): the clip's x field is then the copy count
    std::vector<int> lineMap;          // body line (0-based) -> line of the clip code (1-based, 0 = generated); kept in step with the meter taps
    std::vector<PdSend> pdSends;       // Pure Data clips: values the patch sends to a Csound instrument's <caw> channel
    int outChannels = 0;                // output channels found in out / outs / outq / outch ... (only when taps are on)
};

std::string channelName(const std::string& instr, const std::string& chn);  // "Lead1_input1"
// taps = true (engine run only): every out / outs / outq / outch line also feeds a meter bus, gaCawT_<instr>_<channel>.
ParsedTrack parseCode(const std::string& instrName, const std::string& code, bool taps = false, int runs = 1);  // one instrument's code
// Automation value of a lane at `time` (linear between the points; `fallback` before the first point).
double autoValueAt(const std::vector<AutoPoint>& points, double time, double fallback);
// MIDI for Pure Data / SuperCollider clips: the notes of every MIDI clip aimed at the clip, in absolute timeline seconds (sorted by start,
// pitch and velocity clamped, a note never runs past its MIDI clip). The clip is rendered once with these notes baked in (wav from 0 s).
constexpr double kMidiTail = 2.0;                                                // seconds rendered after the last note (release time)
std::vector<MidiNote> clipMidiNotes(const Project& p, int clipId);
double midiWavLength(const Project& p, int clipId);                              // end of the last note + kMidiTail
std::string pdWavPath(const std::string& name);

// ---- Pure Data / SuperCollider clips: the clip's x box = number of copies, the <caw> p-lines give every copy its p4, p5 ... values
// (same lines as for a Csound clip: {p: 5, from: 0, to: 1}  {p: 6, from: 400, +100}).
// Returns p-number -> one value per copy. Lines whose numbers are not plain numbers (Csound maths) are skipped.
std::map<int, std::vector<double>> cawCopyValues(const std::string& clipCode, int copies);
// Pure Data: number of [inlet] / [inlet~] objects on the top level of the patch text. Inlets are numbered by their x position,
// left to right. p4 goes to the RIGHT-most inlet, p5 to the one left of it, ... (Pd sends right to left, so p4 arrives first).
int pdInletCount(const std::string& patch);
// SuperCollider: "var p4 = \\p4.ir(0), p5 = ...;" for every p-field the code (or a <caw> p-line) uses, plus indx / iRun when used.
std::string scParamDeclarations(const std::string& code, const std::map<int, std::vector<double>>& copyValues);                                  // the wav a Pure Data clip is rendered to
ParsedTrack parseClip(const Clip& c, bool taps = false);                       // a clip = one independent instr
// Lane summary: union of every clip's <caw> channels (plus the lane template), midiEnabled if any clip uses MIDI.
// body is empty; use parseClip for real instrument code.
ParsedTrack parseTrack(const Track& t);

// Meter layout shared by the CSD builder and the UI: the first masterChannels() indices are the master outputs, then every lane that has
// output channels gets `channels` consecutive entries (a mono instrument = 1, stereo = 2, quad = 4 ... max 8).
// Master meter = one entry per output channel (nchnls), so indices 0 .. masterChannels-1 are the master outputs.
inline int masterChannels(const CsoundSettings& s) { return s.nchnls < 1 ? 1 : (s.nchnls > 64 ? 64 : s.nchnls); }
struct LaneMeterInfo { int trackId = 0; int first = 0; int channels = 0; };
std::vector<LaneMeterInfo> meterLayout(const Project& p);
constexpr int kMeterBands = 32;     // spectrum bands sent by the monitor instrument (25 Hz .. 18 kHz, logarithmic)
constexpr int kScopePoints = 64;    // scope points per channel

// Csound options of a live run as generated from the settings fields (used while "options" is empty).
std::string defaultOptions(const CsoundSettings& s);
// Reads -odac / -iadc / -+rtaudio / -b / -B out of s.options back into the settings fields.
void syncSettingsFromOptions(CsoundSettings& s);
// Writes the settings fields (device, backend, buffers) into the matching tokens of s.options.
void syncOptionsFromSettings(CsoundSettings& s);
std::string buildHeader(const CsoundSettings& s);                            // sr, ksmps, nchnls, 0dbfs, A4
// mode: "dac" (realtime) or "render" (the output file is given on the csound command line with -o)
std::string buildOptions(const CsoundSettings& s, const std::string& mode);
// fromTime: start playing from this position (seconds); clips are shifted so the CSD starts at 0
// spans (optional): filled with the position of every clip's code in the orchestra.
std::string buildCsd(const Project& p, const std::string& mode, double fromTime = 0.0, std::vector<OrcSpan>* spans = nullptr);

// One-clip test CSD (no audio output).
std::string buildClipTestCsd(const Project& p, const Clip& clip, OrcSpan& span);
