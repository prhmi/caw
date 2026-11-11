#pragma once
#include <map>
#include <set>
#include <cstdint>
#include <string>
#include <vector>

struct AutoPoint { double t; double v; };            // time in seconds, value
struct MidiNote  { double start; double dur; int pitch = 60; int velocity = 100; }; // MIDI p4 note and p5 velocity

// One clip on a lane = one independent Csound instrument.
// Every clip owns its instr name, its code, its color and its MIDI notes.
struct Clip {
    double start = 0.0;                   // position on the timeline (seconds)
    double dur = 1.0;
    std::string name;                     // unique Csound instr name ("Lead1", "Lead2", ...)
    std::string code;                     // this clip's own instrument body + optional <caw> block
    uint32_t color = 0;                   // own RGB color, 0 = use the lane color
    std::vector<MidiNote> legacyNotes;    // only used while loading old project files (converted to MidiClips)
    std::vector<double> legacyCuts;       // same
    int id = 0;                           // stable editor-window identity
    bool muted = false;                   // this clip is skipped by the timeline
    int scoreLane = 0;                    // score: 0 = lane of the track, -1 = not in the score, >0 = id of a ScoreLaneDef
    int scoreMidi = 0;                    // score: how its notes (MIDI input or written notes) are shown: 0 every note, 1 overall up / down contour, 2 hidden, 3 pitch range band, 4 every note with its name
    std::string scoreWritten;             // score: notes written by hand for a clip whose notes are made inside the code, e.g. "C4 E4:2 - G4" (name[:relative length], - = rest). Only for the score, no sound.
    std::string scoreDynamic;             // score: dynamic marking under the clip: "mf", "pp<ff" (crescendo), "f>p" (diminuendo); empty = from the MIDI velocity
    int scoreStyle = 0;                   // score, sound material: 0 shape, 1 line, 2 grains, 3 block
    std::string scoreLabel;               // score: text instead of the clip name (empty = name)
    double scoreFreqLo = 0.0;             // score: frequency band of this sound in Hz (0 / 0 = not set); makes the graphic lane a frequency axis
    double scoreFreqHi = 0.0;
    std::string note;                     // short explanation shown on the clip
    int runs = 1;                         // how many times the instr is triggered inside the clip (evenly spread)
    int lang = 0;                         // 0 = Csound code, 1 = Pure Data patch, 2 = SuperCollider (1 and 2 are rendered to a wav, played by a Csound instrument)
    bool editorOpen = false;              // UI-only state, not serialized
};

// A MIDI clip lives in the MIDI lane and is completely free in time: it can be moved, resized, cut and joined
// without touching any Csound clip. It is only linked to a Csound clip (clipId) for the instrument name, the color,
// and as the receiver of its notes (p4 = pitch, p5 = velocity). Note times are relative to the MIDI clip start.
struct MidiClip {
    int id = 0;
    int clipId = 0;                       // Clip::id of the Csound clip (instrument) that receives these notes
    double start = 0.0;                   // timeline position (seconds), independent of the Csound clip
    double dur = 1.0;
    std::vector<MidiNote> notes;
    bool editorOpen = false;              // UI-only state, not serialized
};

// ---- score (SVG score export). Everything here is saved in the project file.
struct ScoreMark { double t = 0.0; std::string label; std::string text; };   // section: label = "A", text = short description; marker: label = text on the line

// A lane of the printed score. Several Caw tracks (and single clips) can be assigned to one score lane.
struct ScoreLaneDef {
    int id = 0;
    std::string name;
    std::string sub;                      // small line under the name (instrument / sample file / speaker)
    int kind = 0;                         // 0 auto (staff when it has MIDI notes), 1 staff (5 lines), 2 graphic (no staff)
    int clef = 0;                         // 0 auto, 1 treble, 2 bass, 3 none (five lines without a clef)
    uint32_t color = 0;                   // 0 = colour of the first track
};

struct ScoreInfo {
    int mode = 1;                         // 0 = electronics only (no 5-line staves at all), 1 = instruments + electronics
    int systemsPerPage = 0;               // full score: 0 = as many as fit on the page
    bool coverPage = true;                // full score: first page with lanes list, notes and key
    bool eventList = true;                // full score: last pages with a table of all clip texts
    bool spectrum = false;                // a spectrogram lane of the rendered audio file (Settings: render file) under the lanes
    std::string notes;                    // free text on the cover page (setup, instructions)
    std::vector<ScoreLaneDef> lanes;
    int nextLaneId = 1;
    std::string title;                    // empty = project file name
    std::string subtitle;
    std::string composer;
    bool mergeSameName = true;            // tracks that share the same name (or score name) are ONE lane of the score
    double tempo = 0.0;                   // beats per minute; 0 = time only (seconds), > 0 = bars: barlines, bar numbers, systems end on a barline
    int meterNum = 4;                     // time signature (used when tempo > 0)
    int meterDen = 4;
    double sysSec = 45.0;                 // seconds per system (line) in the full score (rounded to whole bars when tempo is set)
    std::vector<ScoreMark> sections;      // rehearsal marks (boxed letters on the time ruler)
    std::vector<ScoreMark> markers;       // labelled vertical time lines through all lanes
};

struct CawParam  { std::string chn; double min, max, step; double value; };  // value = start value of the channel

struct Track {
    std::string name;                                // lane name, also the base name for new clips
    std::string code;                                // template code copied into new clips on this lane
    std::vector<MidiClip> midiClips;                 // free MIDI regions that drive the Csound clips of this lane
    std::vector<Clip> clips;                         // each clip = its own instr + "i" event (p2=start, p3=dur)
    std::map<std::string, std::vector<AutoPoint>> automation;  // key = caw channel name
    std::map<std::string, float> automationHeights;   // resizable height per automation lane
    std::map<std::string, uint32_t> automationColors; // independent color per automation lane
    uint32_t color = 0;                              // lane RGB color, 0 uses the default palette
    float height = 76.f;                             // instrument-lane height in unscaled pixels
    int id = 0;                                      // stable editor-window identity
    bool muted = false;
    bool solo = false;
    bool deleteSelected = false;
    bool midiHidden = false;                         // the MIDI lane is hidden (data kept)
    std::set<std::string> hiddenAutomation;          // automation lanes that are hidden (data kept)
    bool editorOpen = false;                         // UI-only: lane template editor
    // score export (saved)
    int scoreKind = 0;                               // 0 auto (staff when the lane has MIDI notes), 1 staff, 2 graphic material, 3 hidden
    int scoreClef = 0;                               // 0 auto, 1 treble, 2 bass, 3 none
    std::string scoreLabel;                          // name printed in the score (empty = lane name)
    std::string scoreSub;                            // small line under the name, e.g. the sample file
    int scoreLane = 0;                               // 0 = own lane in the score, >0 = id of a ScoreLaneDef this track belongs to
    std::map<std::string, int> scoreAuto;            // automation channel -> 0 hidden, 1 own automation lane with the full curve, 2 compact contour lane; missing = 1 (every automation with points is in the score)
};

// General Csound engine settings (written into the generated CSD header / options)
struct CsoundSettings {
    int sr = 44100;
    int ksmps = 32;              // kr = sr / ksmps
    int nchnls = 2;
    int nchnlsIn = 0;            // 0 = not written (Csound then uses nchnls)
    double zdbfs = 1.0;          // 0dbfs
    double a4 = 440.0;
    int bufSW = 1024;            // -b  software buffer (sample frames)
    int bufHW = 4096;            // -B  hardware buffer (sample frames)
    bool monitorFeed = true;     // engine run: Csound prints meter / scope / spectrum data for the monitor panels
    std::string rtaudio = "portaudio";
    std::string options;                     // typed Csound options for live runs, e.g. "-odac -+rtaudio=portaudio -b1024 -B4096 -d" (empty = generated)
    int outDev = -1;             // -1 = system default
    int inDev = -1;

    // render + external csound program
    std::string csoundExe = "csound";       // full path to csound.exe, or just "csound" if it is on PATH
    std::string scExe = "sclang";         // SuperCollider language program (sclang.exe), or "sclang" if it is on PATH
    std::string pdExe = "pd";             // Pure Data program (pd.exe), or just "pd" if it is on PATH
    std::string renderFile = "render.wav";
    int renderFormat = 0;                   // 0 = WAV 16-bit, 1 = WAV 24-bit, 2 = WAV 32-bit float
    double tail = 2.0;                      // seconds of silence added after the last clip (reverb tails)
    float editorFontSize = 17.f;
    int editorFontStyle = 0;                 // 0 regular, 1 bold, 2 italic
};

struct Project {
    CsoundSettings cs;
    std::vector<Track> tracks;
    ScoreInfo score;
};
