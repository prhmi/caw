#pragma once
#include <map>
#include <set>
#include <cstdint>
#include <string>
#include <vector>

struct AutoPoint { double t; double v; bool line = false; };   // line: inside a Pen 2 stroke, no dot is shown (only the start and the end of the stroke have dots)            // time in seconds, value
struct MidiNote  { double start; double dur; int pitch = 60; int velocity = 100; int quarter = 0; };   // quarter (score sign only, no sound change): 0 none, 1 quarter sharp, 2 quarter flat, 3 koron // MIDI p4 note and p5 velocity

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
    std::string scoreWritten;             // score: notes written by hand for a clip whose notes are made inside the code, e.g. "C4 E4:2 - G4" (name[:relative length], - = rest). Only for the score, no sound.
    std::string scoreDynamic;             // score: dynamic marking under the clip: "mf", "pp<ff" (crescendo), "f>p" (diminuendo); empty = from the MIDI velocity
    int scoreRole = 0;                    // score: what the clip does: 0 generator, 1 effect, 2 both
    int scoreDetail = 0;                  // score: 0 OVERALL = the whole shape of the sound the clip makes (amplitude shape + frequency position, measured from the rendered clip); 1 DETAILED = additionally everything the code sends to the channel "score" (numbers, signals, strings), plus text, time and parameters
    bool scoreShowLang = false;           // score: the coding language (Csound / Pd / SC) is written next to the name
    int scoreStyle = 0;                   // score, sound material: 0 shape, 1 line, 2 grains (scattered), 3 block, 4 sliced wave (granular), 5 FM graph, 6 dots and lines (keys, leads)
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
    int mode = 1;                         // no longer used (kept for old project files): every track decides itself if it is an instrument (staff) or electronics (graphic)
    bool analyze = true;                  // Render audio also measures every clip (amplitude, frequency) and records the "score" channel for the score
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
    std::string copyright;
    bool mergeSameName = true;            // tracks that share the same name (or score name) are ONE lane of the score
    double lineWeight = 1.0;              // general line weight of the score (0.4 fine .. 3 heavy)
    double sysSec = 45.0;                 // seconds per system (line) in the full score 
    std::vector<ScoreMark> sections;      // rehearsal marks (boxed letters on the time ruler)
    std::vector<ScoreMark> markers;       // labelled vertical time lines through all lanes
};

// A note on a marker track: a comment / guide for the project (text on the timeline, from t for dur seconds).
struct TrackMark {
    double t = 0.0; double dur = 4.0; std::string text; uint32_t color = 0;
    // marker track: type 0 comment, 1 letter box (A, B, C...), 2 cue (Q, shape = Q style), 3 code (lang = Csound / Pd / SC colors), 4 span bracket (horizontal line with 90 degree ends, shape 0 ends down + text above, 1 ends up + text below)
    // graph track: always a graphic-score shape (shape = index in markgfx::kShapeNames: grain cloud, glissando, swell...), no text
    // The start and the length of an item are changed on the timeline (drag it, drag its right edge).
    int type = 0;
    int shape = 0;
    int lang = 0;
    int size = 100;      // size of the box / shape in percent (40 .. 400)
    int ratio = 100;     // swell / fade shapes: curve of the edges in percent (30 .. 300, 100 = straight)
    int font = 0;        // font size of the text in the score and on the timeline (6 .. 40, 0 = default 9)
    int scorePos = 0;    // in the score: 0 not shown, 1 above the lane, 2 below the lane (marker track: its lane is the lane with the same name; graph track: its own lane)
};

struct CawParam  { std::string chn; double min, max, step; double value; };  // value = start value of the channel

struct Track {
    int kind = 0;                                    // 0 = normal track (clips), 1 = marker track (comments / guidance), 2 = free automation (not linked to a clip: the Csound channel is the track NAME, read with chnget "Name")
    int lang = 0;                                    // language of the template: new clips on this track start as 0 Csound, 1 Pure Data, 2 SuperCollider
    double autoMin = 0.0, autoMax = 1.0;             // free automation (kind 2): value range
    std::vector<TrackMark> marks;                    // marker track (kind 1)
    std::map<std::string, int> scoreAutoShape;       // automation channel -> how the line looks in the score (see ScoreAutoShape names in the editor); missing = 0 curve
    std::map<std::string, int> scoreAutoMerge;       // graph line -> 0 combine (one shape with the other lines), 1 half combine (on top at 50%), 2 separated (own strip); missing = combine
    std::map<std::string, int> scoreAutoYSize;       // graph line -> height of its strip in the score, percent (50 .. 400, missing = 100)
    std::set<std::string> scoreAutoMuted, scoreAutoSolo;   // graph lines: M hides the line in the score, S shows only the soloed lines
    std::map<std::string, double> scoreAutoCurve;    // automation channel -> shape ratio 0.3 .. 3 for the mirror / wedge shapes (1 = linear)
    std::map<std::string, double> scoreAutoStroke;   // automation channel -> stroke width of the line in the score (0.3 .. 8); missing = default. The dots, bars, dashes and break points of the other shapes scale with it (ratio to the default width)
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
    std::string scoreMidiLabel;                      // lane name of the MIDI lane in the score (empty = same as the track)
    int scoreMidiShow = 0;                           // how the notes of the MIDI lane show: 0 every note, 1 overall up / down, 2 hidden, 3 pitch range band, 4 every note with its name
    std::map<std::string, std::string> scoreAutoNames; // automation channel -> lane name in the score (missing / empty = same as the track)
    std::string scoreSub;                            // small line under the name, e.g. the sample file
    int scoreLane = 0;                               // 0 = own lane in the score, >0 = id of a ScoreLaneDef this track belongs to
    int scoreOverlap = 0;                            // tracks with the same score name share ONE lane: 0 overlap = their shapes are combined into one (same colour: one shape, different colours: both colours), 1 separated = drawn on top of each other at 50% opacity, not combined
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
