#pragma once
#include "volume_limits.h"
#include <map>
#include <set>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

struct AutoPoint { double t; double v; bool line = false; };   // line: inside a Pen 2 stroke, no dot is shown (only the start and the end of the stroke have dots)            // time in seconds, value
struct MidiNote  { double start; double dur; int pitch = 60; int velocity = 100; int quarter = 0; };   // quarter (score sign only, no sound change): 0 none, 1 quarter sharp, 2 quarter flat, 3 koron // MIDI p4 note and p5 velocity

// One piece of source audio inside an audio clip: from srcOffset (source seconds), playing dur
// source seconds at the clip's rate. A cut makes two clips with one segment each; a join merges
// the segments of the clips into one (the timeline plays the segments one after the other).
struct AudioSeg {
    double srcOffset = 0.0;   // position in the source file (seconds)
    double dur = 0.0;         // length in SOURCE seconds (the clip time this takes = dur / rate)
};

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
    bool scoreShowLang = false;           // score: the coding language (Csound) is written next to the name
    int scoreStyle = 0;                   // score, sound material: 0 shape, 1 line, 2 grains (scattered), 3 block, 4 sliced wave (granular), 5 FM graph, 6 dots and lines (keys, leads)
    std::map<std::string, int> scoreDataStyle; // named streams: 0 default, 1 steps, 2 dots, 3 hidden, 4 audio spectrum
    std::string scoreLabel;               // score: text instead of the clip name (empty = name)
    double scoreFreqLo = 0.0;             // score: frequency band of this sound in Hz (0 / 0 = not set); makes the graphic lane a frequency axis
    double scoreFreqHi = 0.0;
    std::string note;                     // short explanation shown on the clip
    int runs = 1;                         // how many times the instr is triggered inside the clip (evenly spread)
    bool midiEnabled = false;             // MIDI switch in the clip editor
    std::string pFieldRules;              // copy/p-field rules, edited separately from Csound code
    int lang = 0;                         // always 0 = Csound (kept so old project files still load)
    // audio clip (track kind 7): the timeline plays the audio file of this clip
    std::string audioFile;                // absolute path of the audio file (WAV natively, other formats through FFmpeg)
    std::vector<AudioSeg> audioSegs;      // the source segments of the clip (source seconds); empty = invalid
    float audioRate = 1.0f;               // basic time-stretch: speed and pitch together (resample)
    bool audioReverse = false;            // the segments play backwards
    float audioGain = 1.0f;               // clip gain
    int audioView = 0;                    // 0 waveshape, 1 spectrogram
    int audioRoute = 0;                   // output routing: 0 = as the source (mono -> ch 1, stereo -> ch 1-2 ...), 1 = custom
    std::vector<int> audioOutMap;         // legacy saved routing (round65 projects); custom routing now lives in the clip's code (out1 = in1)
    std::vector<AutoPoint> audioGainPts;  // volume automation inside the clip (t = clip seconds, v = gain 0..2)
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
    std::string name;                     // empty = show the target instrument's name
    bool muted = false;                   // mute only this MIDI region
};

// ---- score (SVG score export). Everything here is saved in the project file.
struct ScoreMark { double t = 0.0; std::string label; std::string text; };   // section: label = "A", text = short description; marker: label = text on the line

// A lane of the printed score. Several Caw tracks (and single clips) can be assigned to one score lane.
struct ScoreLaneDef {
    int id = 0;
    std::string name;
    std::string sub;                      // small line under the name (instrument / sample file / speaker)
    int kind = 0;                         // 0 auto (staff when it has MIDI notes), 1 staff (5 lines), 2 graphic (no staff)
    int clef = 0;                         // 0 smart auto, 1 treble, 2 bass, 3 no clef, 4 treble 8va, 5 bass 8vb, 6 grand staff
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
    bool detailedDataLabels = false;      // full score: false = timing text, true = captured data and automation flow
    double lineWeight = 1.0;              // general line weight of the score (0.4 fine .. 3 heavy)
    double sysSec = 45.0;                 // seconds per system (line) in the full score 
    std::vector<ScoreMark> sections;      // rehearsal marks (boxed letters on the time ruler)
    std::vector<ScoreMark> markers;       // labelled vertical time lines through all lanes
};

// A note on a marker track: a comment / guide for the project (text on the timeline, from t for dur seconds).
struct TrackMark {
    double t = 0.0; double dur = 4.0; std::string text; uint32_t color = 0;
    // marker track: type 0 comment, 1 letter box (A, B, C...), 2 cue (Q, shape = Q style), 3 code (lang = Csound), 4 span bracket (horizontal line with 90 degree ends, shape 0 ends down + text above, 1 ends up + text below)
    // graph track: always a graphic-score shape (shape = index in markgfx::kShapeNames: grain cloud, glissando, swell...), no text
    // The start and the length of an item are changed on the timeline (drag it, drag its right edge).
    int type = 0;
    int shape = 0;
    int lang = 0;
    int size = 100;      // size of the box / shape in percent (40 .. 400)
    int ratio = 100;     // swell / fade shapes: curve of the edges in percent (30 .. 300, 100 = straight)
    int font = 0;        // font size of the text in the score and on the timeline (6 .. 40, 0 = default 9)
    bool border = false; // graph track: draw a thin border around the item in the score (the timeline always shows it with a light background)
    int scorePos = 0;    // in the score: 0 not shown, 1 above the lane, 2 below the lane (marker track: its lane is the lane with the same name; graph track: its own lane)
};

struct TrackFolder { int id = 0; std::string name; bool collapsed = false; uint32_t color = 0; };   // color 0 = default blue-grey, else 0xRRGGBB   // folder group in the track panel: hides / shows the tracks inside

struct AudioAsset {
    std::string name;                         // name shown in the Audio list
    std::string path;                         // external audio file path on this computer
};

struct IncludeAsset {
    std::string name;                         // include filename shown in the pool
    std::string path;                         // external source or score file path
    bool score = false;                       // false = orchestra include, true = score include
};

struct CawParam  { std::string chn; double min, max, step; double value; };  // value = start value of the channel

struct Track {
    int kind = 0;                                    // 0 Csound, 1 marker, 2 graph, 3 VSTi, 4 VSTe, 5 SuperCollider, 6 Pure Data
    int lang = 0;                                    // language of the template: always 0 = Csound
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
    bool templateMidiEnabled = false;
    std::string templatePFieldRules;
    std::vector<MidiClip> midiClips;                 // free MIDI regions that drive the Csound clips of this lane
    std::vector<Clip> clips;                         // each clip = its own instr + "i" event (p2=start, p3=dur)
    std::map<std::string, std::vector<AutoPoint>> automation;  // key = caw channel name
    std::map<std::string, CawParam> automationRanges; // range and initial value set from the lane
    std::map<std::string, float> automationHeights;   // resizable height per automation lane
    std::map<std::string, uint32_t> automationColors; // independent color per automation lane
    uint32_t color = 0;                              // lane RGB color, 0 uses the default palette
    float height = 76.f;                             // instrument-lane height in unscaled pixels
    int id = 0;                                      // stable editor-window identity
    bool muted = false;
    bool solo = false;
    float volume = 1.f;                              // track volume 0..2, linear (1 = unity); applied to the sample engine (other engines follow)
    bool deleteSelected = false;
    int folderId = 0;                                // 0 = not in a folder group, else the id of a TrackFolder of the project (the tracks of a folder sit next to each other)
    bool midiHidden = false;                         // the MIDI lane is hidden (data kept)
    std::set<std::string> hiddenAutomation;          // automation lanes that are hidden (data kept)
    bool editorOpen = false;                         // UI-only: lane template editor
    // score export (saved)
    int scoreKind = 0;                               // 0 auto (staff when the lane has MIDI notes), 1 staff, 2 graphic material, 3 hidden
    int scoreClef = 0;                               // 0 smart auto, 1 treble, 2 bass, 3 no clef, 4 treble 8va, 5 bass 8vb, 6 grand staff
    std::string scoreLabel;                          // name printed in the score (empty = lane name)
    std::string scoreMidiLabel;                      // lane name of the MIDI lane in the score (empty = same as the track)
    int scoreMidiShow = 0;                           // 0 every note, 1 overall contour, 2 hidden, 3 pitch range band, 4 note names, 5 fast-note clusters
    std::map<std::string, std::string> scoreAutoNames; // automation channel -> lane name in the score (missing / empty = same as the track)
    std::string scoreSub;                            // small line under the name, e.g. the sample file
    int scoreLane = 0;                               // 0 = own lane in the score, >0 = id of a ScoreLaneDef this track belongs to
    int scoreOverlap = 0;                            // tracks with the same score name share ONE lane: 0 overlap = their shapes are combined into one (same colour: one shape, different colours: both colours), 1 separated = drawn on top of each other at 50% opacity, not combined
    std::map<std::string, int> scoreAuto;            // automation channel -> 0 hidden, 1 own automation lane with the full curve, 2 compact contour lane; missing = 1 (every automation with points is in the score)
    // VSTi (kind 3) / VSTe (kind 4). The plugin file is kept in "code". Only the 8 picked parameters are visible to the code.
    std::vector<int> vstPick;                        // plugin parameter indexes of kP1..kP8 (up to 8)
    std::vector<double> vstInit;                     // value of each picked parameter when it was picked (start value of kP1..kP8)
    std::string vstPath;                             // the .vst3 file
    std::string vstClassId;                          // the plugin class inside it (32 hex); empty = first audio class
    std::string vstPatchState;                       // SEPARATED ENGINES: the plugin's full state (Kontakt's loaded .nki etc.), saved with the project (v58)
    std::map<uint32_t, double> vstParameterState;     // writable parameter IDs and normalized values
    // VSTe audio output (replaces the legacy vstOut / vstOutMaster pair for the UI):
    //   0 = master (replace): the VSTe output goes to the master, the source tracks' own master
    //       output is suppressed and their meters are drawn gray.
    //   1 = mix: the VSTe output goes to the master AND the source tracks keep sounding to the
    //       master; every meter keeps its default color.
    // Default = "FX only" (replace): a freshly added effect consumes its source, like a DAW's
    // insert slot. Switch the combo to "FX+Source" for a send-style parallel effect.
    int vstOutMode = 0;
    // The plugin's output width on this track (VSTi and VSTe): 0 = stereo (the plugin's channels
    // are mixed as stereo 1+3 -> L / 2+4 -> R, the same fold the master meter shows; the track has
    // 2 meters), 1 = as source (the plugin's own channels go 1:1: a 4-output plugin shows 4
    // meters and each channel folds into the master through the virtual soundcard rule).
    int vstOutWide = 0;
    // TRACK CHANNELS (like a DAW track's channel format): how many channels this VSTi / VSTe track
    // carries - 1, 2, 4, 6 or 8. The plugin is processed at this width, the track shows this many
    // meters, and its channels fold into the master by the k mod N rule. 0 = old project: use
    // vstOutWide (0 -> 2, 1 -> the plugin's own width when it is known, else 2).
    int vstChannels = 0;
    // Plugin-window sync options (per track, all chain stages). Both OFF by default = zero cost:
    //   vstParamPoll: the plugin does NOT report its knob moves (performEdit) - sweep its parameters
    //                 every 60 ms while its window is open (Kilohearts-class editors need this)
    //   vstWindowFeed: feed the engine's audio / notes into the plugin window (EQ analyzers, meters)
    bool vstParamPoll = false;
    bool vstWindowFeed = false;
    int vstTrackChannels() const { return vstChannels > 0 ? vstChannels : 2; }
    // VSTe EFFECT CHAIN: effects 2..8 run IN ORDER after the first effect (the input of effect
    // #k is the output of effect #k-1). Stage 0 is the first effect = the legacy single-plugin
    // fields above (vstPath / vstClassId / vstParameterState / vstPatchState). A stage that
    // cannot accept all of the previous stage's channels folds them at its own input width
    // (channel c plays on input c mod M; a 4-channel stage into a stereo one mixes 1+3 -> L /
    // 2+4 -> R). Stages are contiguous: removing one pulls the tail up.
    struct VstStage { std::string path, classId; std::map<uint32_t, double> parameterState; std::string patchState; bool bypass = false; };
    bool vstBypass0 = false;   // effect 1 (the first plugin) bypassed
    VstStage vstChain[7];
    // Which plugin a parameter slot P1..P8 drives: 0 = the first effect, 1..7 = chain stage.
    // Automation lanes and the P-slider path are keyed by the SLOT, so a slot can sit on any
    // stage of the chain.
    int vstSlotStage[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // OUTPUT ROUTING text (VSTi and VSTe, like a sample clip's routing): out<n> = in<m> says the
    // plugin's input channel m plays on the track's output line n. Empty = the Out stereo /
    // Out source combo decides the fold. Unlisted outputs stay silent; the default template is
    // the identity out1..out8 = in1..in8.
    std::string vstRouteCode;
    // Parse the routing text: outMap[k] = the input channel (0-based) that output k+1 plays
    // (-1 = silent). active = at least one out<n> line exists.
    static std::array<int, 8> routeOutMap(const std::string& code, bool& active) {
        std::array<int, 8> outMap{}; for (int k = 0; k < 8; ++k) outMap[(size_t)k] = -1;
        active = false;
        size_t pos = 0;
        while (pos < code.size()) {
            size_t end = code.find('\n', pos);
            if (end == std::string::npos) end = code.size();
            std::string line = code.substr(pos, end - pos);
            pos = end + 1;
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            auto issp = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            while (!line.empty() && issp(line.back())) line.pop_back();
            size_t b = 0;
            while (b < line.size() && issp(line[b])) ++b;
            if (b >= line.size() || line[b] != 'o' && line[b] != 'O') continue;
            if (line.compare(b, 3, "out") != 0 && line.compare(b, 3, "Out") != 0) continue;
            size_t d = b + 3;
            std::string num;
            while (d < line.size() && (std::isdigit((unsigned char)line[d]))) num += line[d++];
            if (num.empty()) continue;
            while (d < line.size() && issp(line[d])) ++d;
            if (d >= line.size() || line[d] != '=') continue;
            ++d;
            while (d < line.size() && issp(line[d])) ++d;
            std::string rhs;
            if (d + 2 < line.size() && (line.compare(d, 2, "in") == 0 || line.compare(d, 2, "In") == 0) && std::isdigit((unsigned char)line[d + 2]))
                d += 2;   // in<m>: skip the "in"
            while (d < line.size() && (std::isdigit((unsigned char)line[d]))) rhs += line[d++];
            if (rhs.empty()) continue;
            int on = 0, in = 0;
            try { on = std::stoi(num); in = std::stoi(rhs); } catch (...) { continue; }
            if (on < 1 || on > 8 || in < 1 || in > 8) continue;
            outMap[(size_t)on - 1] = in - 1;
            active = true;
        }
        return outMap;
    }
    int routeMaxOut() const {
        bool act = false;
        const auto m = routeOutMap(vstRouteCode, act);
        if (!act) return 0;
        int mx = 0;
        for (int k = 0; k < 8; ++k) if (m[(size_t)k] >= 0) mx = k + 1;
        return mx;   // 0 = no routing line produced an output (everything silent)
    }
    // How many chain stages this VSTe has (stages are contiguous; 0 = the single-plugin form).
    int vstChainCount() const {
        int n = 0;
        while (n < 7 && !vstChain[n].path.empty()) ++n;
        return n;
    }
    // The 8 VSTe input lines. Each line is an independent source with its own on/off switch:
    //   mode 0 = track (name = a track name), 1 = clips (name = a clip name), 2 = master (no name).
    // A source fills as many of the 8 plugin input channels as it has outputs (a stereo source
    // takes two lines), in slot order. Lines past 8 are dropped.
    struct VstInput { bool on = false; int mode = 0; std::string name; bool sidechain = false;
                     int receive = 0; };   // sidechain: this insert feeds the effect plugin's SIDECHAIN input bus instead of the summed main input.
                     // receive (wide track sources only): 0 = stereo (the virtual soundcard fold: ch k plays on plugin line k mod 2, so ch1+3 -> L, ch2+4 -> R),
                     // 1 = multichannel (the source's channels go 1:1 into the plugin's input lines - a stereo plugin then hears ch1 | ch2 un-folded)
    VstInput vstInputs[8];
    // Legacy fields, kept only so older project files (CAW_PROJECT < 53) still load. New code
    // reads vstOutMode / vstInputs instead; these are migrated on load and written back as the
    // same values for backward compatibility with older builds.
    std::string vstOut;                              // legacy: audio out bus name (empty = master)
    bool vstOutMaster = false;                       // legacy: with a bus, also send to master
    // Freeze (Cubase-style lock): the whole track is rendered ONCE into a WAV file on disk, then
    // the live engines stop processing it - the frozen WAV plays through the sample engine like a
    // clip instead (cheap CPU). While frozen the track is locked: clips, automations and the MIDI
    // lane cannot be edited until it is unfrozen (unfreeze removes the WAV, everything is back).
    bool frozen = false;                             // frozen: the WAV below is the sound, the live engines skip this track
    std::string freezePath;                          // absolute path of the rendered WAV (empty when not frozen)
    double freezeStart = 0.0;                        // timeline position the WAV begins at (the earliest clip start)
    double freezeEnd = 0.0;                          // timeline position the WAV ends at (the last clip end + a small tail)
    int vstInMode = 0;                               // legacy: 0 = bus sends, 1 = track, 2 = clips
    std::string vstInSource;                         // legacy: the track name (mode 1) or clip name (mode 2)
    int vstOutCh = 2;                                // legacy: output channel count 2 / 4 / 6 / 8 (now always 2)
    std::vector<std::string> vstPickName;            // automation lane name of each picked parameter (the clips of the track get one lane per name)
    int vstMidi = 2;                                 // VSTi: 0 = CAW MIDI clips, 1 = device MIDI, 2 = both
};

// A frozen track really plays (the WAV exists on disk and the engines skip it). Marker (1),
// graph (2) and VSTe (4) tracks cannot freeze: they carry no instrument sound of their own.
inline bool trackFrozen(const Track& t) { return t.frozen && !t.freezePath.empty(); }
inline bool trackCanFreeze(const Track& t) { return t.kind == 0 || t.kind == 3 || t.kind == 5 || t.kind == 6 || t.kind == 7; }

// General Csound engine settings (written into the generated CSD header / options)
struct CsoundSettings {
    int sr = 48000;
    int ksmps = 32;              // kr = sr / ksmps
    int nchnls = 2;
    int nchnlsIn = 0;            // 0 = not written (Csound then uses nchnls)
    double zdbfs = 1.0;          // 0dbfs
    double a4 = 440.0;
    int bufSW = 512;             // -b  software buffer (sample frames)
    int bufHW = 2048;            // -B  hardware buffer (sample frames)
    bool monitorFeed = true;     // engine run: Csound prints meter / scope / spectrum data for the monitor panels
    int syncMs = 0;              // extra delay (ms, can be negative) added to the sound sync of the timeline and the meters; tune it until the sound meets the playhead
    std::string rtaudio = "portaudio";
    std::string options;                     // typed Csound options for live runs, e.g. "-odac -+rtaudio=portaudio -b512 -B2048 -d" (empty = generated)
    int outDev = -1;             // -1 = system default
    int inDev = -1;

    // render + external csound program
    std::string csoundExe = "csound";       // full path to csound.exe, or just "csound" if it is on PATH
    std::string ffmpegExe;                  // optional path to an ffmpeg executable
    std::string renderFile = "render.wav";
    int renderFormat = 0;                   // 0..2 WAV, 3 FLAC 24-bit, 4 MP3 via ffmpeg
    int renderMode = 0;                     // 0 = offline (faster than real time), 1 = real-time capture
    double exportStart = 0.0;               // the export range start (seconds; 0 = project start)
    double exportEnd = 0.0;                 // the export range end (seconds; 0 = project end, auto)
    double tail = 2.0;                      // seconds of silence added after the last clip (reverb tails)
    bool autosaveEnabled = false;           // separate recovery file, never overwrites the project
    int autosaveMinutes = 5;
    float masterGain = 1.f;                 // linear output gain, shared by live host output and export
    int exportChannels = 2;                // output channels, independent of engine channel count
    // EXPORT SOURCE: 0 = as virtual (the master mixdown: the exported fold follows exportChannels),
    // 1 = as real routing (the virtual soundcard bus: the whole project at the audio settings
    // width N, every track at its routed positions - like the live master).
    int exportSource = 0;
    int exportConvert = 0;                 // 0 = off, else the target width the export folds to (1 / 2 / 4 = mono / stereo / quadro)
    bool exportConvertOn = false;          // the Convert checkbox (exportConvert holds the target width)
    bool exportSplit = false;              // true = each final channel exports as its own mono file (<name>_1, <name>_2, ...)
    int exportFlacBits = 24;               // lossless FLAC bit depth, not bitrate
    int exportMp3Kbps = 192;               // MP3 constant bitrate
    float programFontSize = 17.f;           // interface font, independent of the code editor
    float editorFontSize = 17.f;
    int editorFontStyle = 0;                 // 0 regular, 1 bold, 2 italic
};

struct Project {
    CsoundSettings cs;
    double loopStart = 0.0, loopEnd = 0.0;
    bool loopEnabled = false;
    std::vector<Track> tracks;
    std::vector<TrackFolder> folders;                // folder groups of the track panel
    std::string udoCode;                             // project-wide Csound UDO definitions, emitted before instruments
    std::vector<std::string> instrPool;              // complete project-wide instr ... endin definitions
    std::string orchestraCode;                       // project-wide global orchestra declarations
    std::vector<AudioAsset> audioPool;               // named audio-file references used by clip code
    std::vector<IncludeAsset> includes;               // external Csound source / score includes
    ScoreInfo score;
};
