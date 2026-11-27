#include "app.h"
#include "caw_audio_bus.h"
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
#include "timeline_edit.h"   // clip_automation_move + clip_timing + timeline_loop + timeline_zoom + midi_clip_ops + audio_channels (phase-1 merge)
#include "code_export.h"
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
#include "undo_files.h"
#include "score_svg.h"
#include "TextEditor.h"
#include "csound_opcodes.h"
#include "marks_gfx.h"
#include "vst_ui.h"
#include "vst_lang.h"
#include "vst_solo.h"
#include "monitor_view.h"   // monitor_math + monitor_fft + monitor_capture + monitor_frame + monitor_parse + monitor_bands + monitor_peaks + monitor_osc + monitor_work (phase-1 merge)
#include "audio_track.h"
#include "master_fx.h"

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


#include "app_shared.h"


// ---------------------------------------------------------------- state
// Session-only choices: what this session loads and what the combined Run button starts.
// Unchecked means "do not load / run it" - tracks of every kind can still be added and edited.
// CAW's 4th engine: a dedicated output stream, opened only when no other stream carries audio.
// MANUAL (Run-button) auditions only: the button shows "Stop clip" / the active color for these.
// Instances the TIMELINE launched never touch the button - it stays a plain "Run clip" while the
// transport plays the clip (the user asked for the Run button to be separate from the transport).

// Sample-track rings for VSTe inputs (engine 4): the sample engine's callback pushes each track's
// post-track-gain stereo mix, the rack pops one block per consuming input.


// Audio-thread dry delay for latency compensation: a circular buffer the hook pushes blocks into
// and reads back with a lag (the consuming VSTe's reported latency). Only touched by the audio thread.
void scRealtimeSource(void* context, float* output, unsigned long frames, int channels) noexcept {
    if (context) static_cast<ScEngine*>(context)->process(output, frames, channels);
    {
        // Play diagnostics: the SuperCollider engine's own level at this point (after its process,
        // before the sample engine adds anything). While a SuperCollider track is frozen its clips
        // do not run, so this must be silence - a non-zero value names the SC engine as the noise.
        static unsigned s_diagScN = 0;
        if ((++s_diagScN & 31) == 0) {
            const size_t n = (size_t)frames * (size_t)channels;
            double acc = 0.0;
            for (size_t i = 0; i < n; ++i) { const double v = output[i]; acc += v * v; }
            g_diagScRms.store((float)std::sqrt(n ? acc / (double)n : 0.0), std::memory_order_relaxed);
        }
    }
    // The per-track FX-only suppression happens inside ScEngine::process (the slot suppress mask:
    // a consumed slot never mixes into the device output), so no whole-mix zeroing here - with two
    // SuperCollider tracks, an FX-only VSTe consuming one of them must not silence the other.
    // The sample engine renders BEFORE the rack mixes: the VSTe input ring then holds THIS block
    // when the effect pops it (insert mode stays phase-tight; FX-only hears no extra latency).
    // NO g_sampleOutput guard here: every CAW stream carries this hook now (one stream at a
    // time), including the sample engine's own stream - with the guard, a sample-stream carrier
    // rendered nothing and the sample source went silent ("no source, only FX").
    {
        // Dry/wet alignment in mix mode: a latency-reporting VSTe delivers its output later than
        // the raw source. The ring/feed still gets the raw block (the effect must see it now); the
        // DRY that reaches the output is delayed by the effect's reported latency so they line up.
        static std::vector<float> smpScratch;
        static DryDelay smpDelay;
        const int compFrames = g_sampleDryLatencyComp.load(std::memory_order_relaxed);
        if (compFrames > 0 && !g_sampleRawSuppress.load(std::memory_order_acquire) && channels <= 8) {
            if ((int)smpScratch.size() < (int)(frames * (size_t)channels)) smpScratch.assign((size_t)frames * (size_t)channels, 0.f);
            else std::fill(smpScratch.begin(), smpScratch.end(), 0.f);   // the engine render ACCUMULATES: every block starts from silence
            smpDelay.ensure(frames, channels);
            cawaudio::engine().render(smpScratch.data(), (int)frames, channels);   // the per-track feed/ring push fires inside (raw audio)
            smpDelay.push(smpScratch.data(), frames);
            const long long total = smpDelay.written();
            for (unsigned long i = 0; i < frames; ++i) {
                float fr[8] = {};
                smpDelay.frameAt(total - (long long)compFrames - (long long)frames + (long long)i, fr);
                for (int c = 0; c < channels; ++c) output[(size_t)i * (size_t)channels + (size_t)c] += fr[c];
            }
        } else {
            cawaudio::engine().render(output, (int)frames, channels);   // sample tracks mix here (unless the sample engine runs its own stream)
            if (g_sampleRawSuppress.load(std::memory_order_acquire))
                std::fill(output, output + (size_t)frames * (size_t)channels, 0.f);   // a master-mode ("FX only") VSTe consumes this sample track: the effect's output replaces the raw mix
            static unsigned s_diagCawN = 0;
            if ((++s_diagCawN & 31) == 0) {
                const size_t n = (size_t)frames * (size_t)channels;
                double acc = 0.0;
                for (size_t i = 0; i < n; ++i) { const double v = output[i]; acc += v * v; }
                g_diagCawRms.store((float)std::sqrt(n ? acc / (double)n : 0.0), std::memory_order_relaxed);
            }
        }
    }
    if (g_vstEngineOn) {
        // A manual "Run clip" audition sounds without the transport: the effects wired to that
        // sample track must keep processing it (the idle gate otherwise stops them after 2.5 s).
        if (cawaudio::engine().manualAuditionActive()) vstSolo().keepAwake();
        vstSolo().renderMix(output, (int)frames, channels);   // SEPARATED ENGINES: the 4th engine always owns the VST graph
    }
}

void attachScRealtimeSource(CawAudioSettings& settings) {
    settings.realtimeSource = &scRealtimeSource;
    settings.realtimeSourceContext = &g_scEngine;
}

// Buffer by resolved patch path so edits survive closing a clip window or
// switching projects; two clips pointing to the same file share one buffer.
// A button that shows a state (on / off): the hover and the pressed colours equal the button colour, so hovering never hides the state.
void pushBtnOn(ImVec4 c) {
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
}

// Vector logo (res/logo.svg) drawn once per size it is shown at, so it is never scaled: About, welcome window, splash.
// hasClips(): defined in app_internal.h


// automation editing
bool isPenTool() { return g_tool == Tool::Pen || g_tool == Tool::Pen2; }


// box-select of MIDI regions (drag on empty space in a MIDI lane, like the Csound clips)
// One box-select for everything: Csound clips and MIDI regions. A double-click that turns into a drag box-selects;
// a double-click without a drag still creates a clip (kind 1 = Csound clip, 2 = MIDI region).

// undo / redo: whole-project snapshots, committed when an edit is finished


// monitor panels: display order and width (unscaled px, 0 = automatic) of every panel; drag a title to reorder, drag between panels to resize
// Control periods per scope point for the chosen time window, and the window that really results (whole control periods only).














// Console: every line knows its importance, its language and (when the message names one) its clip.


// ---- data printed by the running Csound engine (CawMon instrument): master + lane meters, scope, spectrum bands


float S(float v) { return v * g_ui; }


// ---- help panel: what used to be pop-up tooltips is shown in a panel at the bottom of the program




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






// ---- folder groups of the track panel: the tracks of a folder sit next to each other; a collapsed folder hides them (the sound is not affected)

bool trackHiddenByFolder(const Track& t) { const TrackFolder* f = findFolder(t.folderId); return f && f->collapsed; }



// Put track `idx` into folder `folderId` (0 = out of any folder) and move it next to the other tracks of that folder.




// Keep lanes visible while they contain MIDI clips, even if the instrument toggle was switched off.
bool trackHasMidi(const Track& track) {
    if (!track.midiClips.empty()) return true;
    for (const Clip& c : track.clips) if (parseClip(c).midiEnabled) return true;
    return false;
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




uint32_t storedRgb(uint32_t rgb) { return 0x01000000u | (rgb & 0x00FFFFFFu); }


uint32_t clipColorValue(const Track& lane, const Clip& clip) {
    return clip.color ? clip.color : lane.color;
}


// Short explanation shown on a clip: the Info text, else the first comment line of its code.
std::string clipDescription(const Clip& c) {
    if (!c.note.empty()) return c.note;
    if (!c.audioFile.empty()) {   // audio clip: file name + the sampler / stretch / reverse state
        std::string d = cawaudio::fileTitle(c.audioFile);
        if (c.audioRate != 1.0f) d += "  x" + std::to_string(c.audioRate).substr(0, 4);
        if (c.audioReverse) d += "  rev";
        if (c.midiEnabled) d += "  MIDI";
        if (!c.audioGainPts.empty()) d += "  vol";
        return d;
    }
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






Clip* findClip(int clipId, int* laneIndex )
{
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti)
        for (Clip& c : g_proj.tracks[ti].clips)
            if (c.id == clipId) { if (laneIndex) *laneIndex = (int)ti; return &c; }
    return nullptr;
}


MidiClip* findMidiClip(int id, int* laneIndex )
{
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti)
        for (MidiClip& c : g_proj.tracks[ti].midiClips)
            if (c.id == id) { if (laneIndex) *laneIndex = (int)ti; return &c; }
    return nullptr;
}




// Drop the MIDI clips that played a Csound clip which no longer exists.


// The MIDI editor of a Csound clip: its first MIDI clip, or a new one placed under the Csound clip.


// Enabling MIDI creates one region for this clip if it does not already have one.


Clip* selectedClip(int* laneIndex )
{
    if (g_selTrack < 0 || g_selTrack >= (int)g_proj.tracks.size()) return nullptr;
    Track& t = g_proj.tracks[(size_t)g_selTrack];
    if (g_selClip < 0 || g_selClip >= (int)t.clips.size()) return nullptr;
    if (laneIndex) *laneIndex = g_selTrack;
    return &t.clips[(size_t)g_selClip];
}


// Copy a clip into a brand-new, independent instrument (own name, color and id).


// Duplicate every selected clip as another use of the SAME instrument (same name, same code).
// The copies are placed right after the selection.





std::string chooseAudioPath();










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
        "(\n"
        "{ SinOsc.ar(~cawget.(\"input1\"), 0, 0.1) ! 2 }.play(target: ~cawgroup);\n"
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


// The code a new clip of a VST track starts with (CAW code).
// A VSTe clip must NOT touch P1..P8 by default: P1..P8 are whichever parameters the user picked,
// so driving one from the code on every new clip hijacks the plugin (the old default pinned the
// first picked parameter - e.g. Pitch - to the level of the input, which was always 0).

int addVstTrack(bool instrument) {
    const int idx = addTrack(std::string(instrument ? "VSTi" : "VSTe") + std::to_string(++g_trackCounter), "");
    g_proj.tracks[(size_t)idx].kind = instrument ? 3 : 4;
    g_proj.tracks[(size_t)idx].code = instrument
        ? "# plays a note every half second, P1 follows a slow lfo\nevery 0.5 {\n  note 48 + int(rnd(12)), 100, 0.4\n}\nP1 = uni(lfo(0.2))\n"
        : kVsteClipCode;
    vstui::openWindow(g_proj.tracks[(size_t)idx].id);
    return idx;
}



std::string chooseAudioPath();




// An audio track (kind 7, the "sample" tracks): its clips hold an audio file. The lane has the same
// fixed height as every other track; the big waveshape / spectrogram is the sound editor window
// (right-click the clip). The engine is started by the Run sample button (like Run VST).
int addAudioTrack() {
    const int idx = addTrack("Sample" + std::to_string(++g_trackCounter), "");
    g_proj.tracks[(size_t)idx].kind = 7;
    g_proj.tracks[(size_t)idx].height = kFixedTrackH;
    return idx;
}


// The compiled CAW code of a sample clip, cached by clip id ONLY: the code applies on Run
// (applySampleClip erases the entry to force a recompile), never while typing and never by
// the playhead reaching the clip - live-coding works like every other clip.
// The console shows CAW-code problems too (once per clip and error text: re-compiles never spam).



// Give a fresh audio clip its file (choose + decode). Returns false when the user cancels.
bool initAudioClip(Clip& c, const std::string& presetPath )
{
    const std::string path = presetPath.empty() ? chooseAudioPath() : presetPath;
    if (path.empty()) return false;
    c.audioFile = path;
    std::string error;
    const cawaudio::AudioHandle data = audioClipData(c, error);
    if (!data || data->samples.empty()) {
        logLine("[caw] Audio clip: could not read " + path + (error.empty() ? std::string() : ": " + error));
        c.audioFile.clear();
        return false;
    }
    c.audioSegs.clear();
    c.audioSegs.push_back({0.0, data->duration()});
    c.dur = std::max(0.05, data->duration());
    c.audioRate = 1.0f;
    c.audioReverse = false;
    c.audioGain = 1.0f;
    c.audioGainPts.clear();
    c.name = cawaudio::fileTitle(path);
    ensureUniqueClipName(c);
    c.code =
        "# CAW code of this sample clip\n"
        "# rate = 1        # clip length scale (0.25 .. 4): 0.5 -> half as long (double speed), 2 -> twice as long, e.g.  rate = smooth(lfo(0.2) * 0.3 + 1)\n"
        "# gain = 1        # volume 0..2, e.g.  gain = smooth(line(1, 0))  fades out\n"
        "# normalize = 0   # 1 = scale the file to full level before the gain\n"
        "# reverse = 0     # 1 = the clip plays backwards (read when the clip starts)\n"
        "# routing: out1 = in1    # out<n> = in<m>: output n plays input m; a mono file on both outputs:  out1 = in1  out2 = in1\n"
        "# granular: every 0.5 {  play: rnd(10), 0.2, 1  }   # play: startpoint (s), duration (s), pitch/speed (negative = reverse)\n"
        "#                                            # a 4th value chooses the grain's OUTPUT channel (1..8): every 0.5 { play: rnd(10), 0.2, 1, rnd(7) + 1 } throws every grain to a random channel\n"
        "# seed 0   -> rnd() gives new numbers every run (like Csound); seed 5 -> the same sequence every run\n";
    logLine("[caw] Sample clip: " + path + " (" + std::to_string(data->duration()) + " s)");
    // live-coding: the fresh template is the clip's applied state - compile it now
    {
        auto prog = std::make_shared<const vstscript::Program>(vstscript::Compiler().compile(c.code));
        logScriptError("sample clip", c.id, c.name, *prog);
        g_sampleScripts[c.id] = prog;
    }
    return true;
}


// Projects saved with the old VSTe default keep its exact text in every VSTe clip: upgrade that
// one string to the new default, so those clips stop driving the first picked parameter.
// Hand-written code never matches and is left untouched.
void upgradeVsteDefaultClips() {
    const std::string oldCode = vstlang::trim(kVsteClipCodeOld);
    auto upgrade = [&](std::string& code) { if (vstlang::trim(code) == oldCode) code = kVsteClipCode; };
    for (Track& t : g_proj.tracks) {
        if (t.kind != 4) continue;
        upgrade(t.code);
        for (Clip& c : t.clips) upgrade(c.code);
    }
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


Track* findTrackById(int id, int* index )
{
    for (size_t i = 0; i < g_proj.tracks.size(); ++i)
        if (g_proj.tracks[i].id == id) { if (index) *index = (int)i; return &g_proj.tracks[i]; }
    return nullptr;
}






// Sort a lane by time while keeping the selected points selected.
// Insert a dot at its time position (after dots with the same time), so a lane that is sorted stays sorted.




ParsedTrack pdParsedForUi(const Track& track,const Clip* only)
{
    ParsedTrack parsed;
    if (track.kind != 6) parsed = only ? parseClipForTrack(*only, track) : parseTrack(track);
    else {
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
    }
    // Sample tracks always offer the built-in "volume" lane (0..2 linear, applied live to the engine).
    if (track.kind == 7 && !std::any_of(parsed.params.begin(), parsed.params.end(),
                                       [](const CawParam& p) { return p.chn == "volume"; }))
        parsed.params.push_back(CawParam{"volume", 0.0, 2.0, .001, 1.0});
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







// the new dots are selected, so the drag handles show up right away


// Ramer-Douglas-Peucker on the value axis: keeps the shape with as few dots as possible.



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




// depth (start -> end, 1 = as drawn), time stretch (1 = as drawn), morph (-1 triangle .. 0 as drawn .. +1 square), all from the original dots


WaveHandles waveHandlePos(float x0, float x1, float yTop, float yBot, float yMid, float ly0, float ly1, float xMax) {
    WaveHandles h;
    const float cx = (x0 + x1) * 0.5f;
    h.depth = ImVec2(cx, std::min(ly1 - S(8), std::max(ly0 + S(8), yTop - S(11))));
    h.morph = ImVec2(cx, std::min(ly1 - S(8), std::max(ly0 + S(8), yBot + S(11))));
    h.stretch = ImVec2(std::min(xMax - S(8), x1 + S(13)), std::min(ly1 - S(8), std::max(ly0 + S(8), yMid)));
    return h;
}



// live Rec: one value of the clip's channel "rec" arrived from the engine monitor




// Deletes the selected dots of EVERY lane (a box can select dots in several lanes at once).


void endChildTight() {  // EndChild without the extra item spacing below it
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
}


// ------------------------------------------------ std::string <-> ImGui

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


bool inputStr(const char* label, std::string& s, ImGuiInputTextFlags flags, ImGuiInputTextCallback chain )
{
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




// ---- diagnostics: which clip, which line of its code, which time on the timeline

const Clip* clipByName(const std::string& name) {
    for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) if (c.name == name) return &c;
    return nullptr;
}

// Puts the clip, its line and its start times on a log line. An error with a line also gets a red mark in the code editor of the clip.



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




void diagClearAll() { g_diagMarks.clear(); g_errCtx = ErrCtx(); }

void diagClearClip(const std::string& name) { if (const Clip* c = clipByName(name)) g_diagMarks.erase(c->id); g_errCtx = ErrCtx(); }








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














void splitCaw(const std::string& code, std::string& patch, std::string& caw);

std::string csoundExePath();


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
    upgradeVsteDefaultClips();   // a VSTe clip's old default code drove the first picked parameter
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
    // live-coding: the loaded code IS the clip's applied state - compile it now so the first
    // play does not pick up anything typed after the load
    g_sampleScripts.clear();
    for (Track& t : g_proj.tracks) for (const Clip& c : t.clips) if (t.kind == 7) {
        auto prog = std::make_shared<const vstscript::Program>(vstscript::Compiler().compile(c.code));
        logScriptError("sample clip", c.id, c.name, *prog);
        g_sampleScripts[c.id] = prog;
    }
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





// ONE CAW stream at a time: exclusive drivers (ASIO / WDM-KS) fail a second open, and two CAW
// streams would render the shared hook twice (engines run at double speed). Whoever opens the
// next stream becomes the carrier; the shared hook mixes every engine into it.



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

void stopPdPatch() { g_pdPreview.stop(); g_pdPlayingClip = 0; g_pdRunLeft = 0.0; restoreScSoloOutput(); }


// The library is a machine setting, not part of a portable project.

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




// Csound, SuperCollider and libpd each ship DLLs with the same names (libsndfile-1.dll, libwinpthread-1.dll ...), and a
// process keeps only one copy per name. Load the newest copy of every shared name first, before any engine loads its
// own, so a newer engine (Csound 7) never ends up with an older copy that SuperCollider brought along.








int scTrackSlot(int trackId) {
    int slot = 0;
    for (const Track& track : g_proj.tracks) if (track.kind == 5) {
        if (track.id == trackId) return slot < ScEngine::kFirstBusSlot ? slot : -1;   // the last slots carry the audio-bus sends
        ++slot;
    }
    return -1;
}













// The code snapshot the timeline launches from: taken when the transport (re)starts, updated only
// by Run / Alt+Enter. Code typed into an editor NEVER reaches a launch on its own.

// Launch / stop SuperCollider clips from the playhead. The timeline only PLAYS: when the playhead
// ENTERS a clip's window it is (re)launched fresh for the remaining duration - even if a manual Run
// instance is sounding (the timeline owns the window, like Csound restarting at a loop). It never
// re-evaluates code on its own: new code is applied only by the Run button.






// CAW hub (caws/cawr): deliver shared bus values to the SuperCollider language.
// Csound channels and the PD instances are handled on their own engine threads;
// sclang is fed from here. SC clips read the values as ~cawr[\name].


// SuperCollider automation: the language has no per-tick engine feed like the PD cawget-<name>
// receivers, so while the timeline rolls CAW evaluates every SuperCollider lane at the playhead
// and mirrors the value into the language (~cawr, read by ~cawget) over the /caw/hub OSC path.
// Lane values stay in the lane's own range (0..1 unless the code declares a range), exactly like
// the PD feed; the shared caws/cawr hub itself is not touched.


// Every hub channel name used by the clip code (caws sends / cawr reads). The runner
// resolves the matching "caws/<name>" Csound channels against this list. PD patches
// need no declaration: their caws outputs arrive through the control queue.


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
    if (freezeActive()) return;   // a freeze render mutates mute / solo temporarily: never an undo step, never a snapshot
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
    // File safety: a restored snapshot may reference a frozen WAV that an unfreeze moved aside.
    for (const Track& t : g_proj.tracks)
        if (t.frozen && !t.freezePath.empty()) cawUndoRestoreFile(t.freezePath);
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









// ---------------------------------------------------------------- score (SVG) : export, editor, 3-minute example

// ---- spectrogram of the rendered audio file (shown in the score when "Spectrum" is on)

// The render file: a relative name lives next to the saved project (or in the work folder while the project is unsaved).



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


// A combo as wide as its longest entry (plus the arrow), never narrower than minW: the text always fits the box, whatever the font size.

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






// Stroke width of a line in the score (straight lines, steps, dashed, curve, outlines) and the shape ratio of the mirror / wedge shapes.
// The dots, bars, dashes and break points of the other shapes scale with the stroke width.




// What a clip is in the score. Lanes, notes and automation are settings of the lanes, not of the clip.




// Read-only patch-board overview. Static code matches show potential routes,
// not proof that a conditional statement actually executes.












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
// Runs (or reuses) the probe for the resolved Csound. Returns true when abs OR sqrt compiles.


// Meter / scope / spectrum frames: the CawMon instrument prints text lines (CAWM / CAWS / CAWL / CAWR / CAWC <first index> <values>)
// on Csound's output. They are held back by the output latency (plus the sync offset from Settings), so the meters move with the sound.

#ifdef _WIN32
using SockLen = int;

#else
using SockLen = socklen_t;

#endif




void updateSpectrumDerived() {
    if (g_fftDb.size() != (size_t)monitor_fft::bins) return;
    // The real time between two analyzer updates (the pipeline is throttled): the peak glide
    // speed limit is applied against it, so the marker's on-screen speed is exact.
    static std::chrono::steady_clock::time_point lastDeriv{};
    const auto nowDeriv = std::chrono::steady_clock::now();
    double dt = 0.033;
    if (lastDeriv.time_since_epoch().count() != 0)
        dt = std::min(0.25, std::max(0.005, std::chrono::duration<double>(nowDeriv - lastDeriv).count()));
    lastDeriv = nowDeriv;
    if (g_specStyle == 3)
        g_columnBands = monitor_bands::analyze(g_fftDb, g_rawSampleRate, monitor_bands::kColumnBandsPerOctave, monitor_fft::size);
    if (g_specStyle == 0) {
        // The SAME data in both views (user correction): every detected peak is shown. Only the
        // motion differs - Better glides calmly, Realistic follows as fast as the raw display.
        // Switching between the modes restarts the tracker: markers carrying positions (and
        // momentum) from the other mode would glide slowly toward the current tones after the
        // switch, which reads as a portamento sweep across the whole spectrum.
        static double lastGlideLimit = -1.0;
        const double glideLimit = g_specBetter ? 1000.0 : 12000.0;
        if (lastGlideLimit >= 0.0 && glideLimit != lastGlideLimit) monitor_peaks::resetTracking();
        lastGlideLimit = glideLimit;
        g_tonePeaks = monitor_peaks::findTracked(g_fftDb, g_rawSampleRate, monitor_fft::size,
                                                 glideLimit, dt, 1000000);
    }
}








// A monitor line of Csound's output; the tag may sit anywhere in the line. Returns false for every other line.




// Csound rejected a math opcode at parse time ("Unable to find opcode entry for 'abs'" / "unknown
// opcode 'abs'"). Remember WHICH name so the failed run can heal the generator: the finish handler
// flips the matching flag and the next build writes |x| via sqrt(x*x) or plain sums instead.




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


// Display smoothing of every meter (called once per frame).


// ---- Run: the project is the Csound engine (one CSD, every clip is an instrument). It stays alive until Stop, like CsoundQt's Run.
// ---- Play: every clip has a start time on the timeline = p2 of its score line. Play sends those lines (from the playhead on) to the running Csound.



// ---- App -> running Csound: numbered command files (no ports, no stdin, no tables).
// A command = "mode rows" (mode 1 = replaces the running timeline, 0 = plays next to it) and one row of numbers per event (delay in seconds from now).
// Csound reads command n when the serial file says n.

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




// ---------------------------------------------------------------- CAW's 4th engine: the independent VST rack
// The instrument tracks render in the app (see vst_solo.h) - like a DAW playing a VST while
// sharing the soundcard. Notes from MIDI clips are scheduled by CAW itself (no Csound needed);
// when Csound runs, its note ring (clip code) and parameter channels are forwarded in.
// What the VST engine loads: instruments AND effects, whatever Csound is doing - so a Csound
// start / stop never changes the plugin set (destroying and re-creating instances is what left a
// plugin mute).


// The Csound clip tap channels (engine 0) the Csound worker must capture per ksmps cycle for the
// 4th engine's VST rack. SEPARATED ENGINES: Csound never loads VST plugins - it only publishes
// these taps, so VST-side churn (editor opens, plugin reloads) can never reach Csound's audio.



// The MIDI-clip notes of the instrument tracks as one absolute-time schedule. A VSTi track's
// own MIDI clips go straight to its plugin; MIDI clips on any other track still route through
// their target clip (clipId) - when that clip lives on a VSTi track, its notes drive that plugin.


// The clip code of the VSTi tracks for the VST-only path (no Csound engine): the VST engine
// interprets the CAW language itself, so `note / cc / msg` lines play without Csound. While
// Csound runs, it translates and forwards the same code, so this list stays empty there.


// A dedicated output stream only when NO other stream is carrying audio (the engine's, PD's,
// SC's all get the mix through the realtime hook instead). Only while the VST engine runs.


// Any VST track - instrument OR effect. The 4th engine hosts both: an effect's parameters, picks
// and overrides must reach its own instance too, or the effect runs at its loaded defaults (often
// wet=0 / bypass) and "the plugin's own knobs do nothing" while Csound is off.
bool trackIsVst(int trackId) {
    for (const Track& t : g_proj.tracks) if (t.id == trackId) return t.kind == 3 || t.kind == 4;
    return false;
}


// The set of VST track ids that should keep sounding right now (live M / S). Recomputed whenever a
// mute or solo flag changes and pushed to the running engine so the VSTe reacts without a restart.


// What the 4th engine processes: live M / S. Effects take part too - it is the effect rack in
// every mode (SEPARATED ENGINES: this engine always hosts the VST graph, Csound never does).
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
        // FX only: the consumed sources' ONLY path is through the effect. The master send of every
        // clip the enabled inputs name is replaced whenever the rack is in the signal path - also
        // while the VSTe is explicitly muted (muting the VSTe then silences the whole chain: the
        // raw source must not leak back to the master, and its meter stays gray).
        const bool consumeSources = vt.vstOutMode == 0 && (vtActive || (vt.muted && !anySolo));
        for (const Track& t : g_proj.tracks) {
            if (t.kind == 5 || t.kind == 6) continue;
            for (const Clip& c : t.clips) {
                bool mute = false;
                if (consumeSources)
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
    vstui::pushVstParamOverride = [](int trackId, int stage, int slot, double value) {
        g_apiProc.pushVstParamOverride(trackId, stage, slot, value);
        if (g_vstEngineOn && trackIsVst(trackId)) vstSolo().pushParamOverride(trackId, slot, value, stage);   // the 4th engine's own copy: instrument OR effect
    };
    vstui::pushVstPickRefresh = [](int trackId, const std::vector<int>& pick) {
        g_apiProc.refreshVstPicks(trackId, pick);
        if (g_vstEngineOn && trackIsVst(trackId)) vstSolo().pushPickUpdate(trackId, pick);
    };
    vstui::pushVstParamLive = [](int trackId, int stage, int paramIndex, double value) {
        g_apiProc.pushVstParamValue(trackId, stage, paramIndex, value);
        if (g_vstEngineOn && trackIsVst(trackId)) vstSolo().pushParamValue(trackId, stage, paramIndex, value);
    };
    vstui::pushVstStateLive = [](int trackId, int stage, const std::string& blob, const std::string& originPath, const std::string& originClassId) {
        // SEPARATED ENGINES: the editor instance's full state (Kontakt's loaded .nki) reaches the
        // 4th engine's instance through the STATE WORKER (vst_solo.h): setState runs neither on
        // the audio thread nor on the UI thread - a sampler loading a patch must not deadlock
        // either. The offline render gets the blob through collectVstJobs, so exports carry it too.
        if (g_vstEngineOn) vstSolo().pushState(trackId, stage, blob, originPath, originClassId);
    };
#ifdef CAW_VST3
    // ONE INSTANCE PER PLUGIN: the live VST engine renders the plugin object loaded in the panel.
    // Called on the rack's init / loader thread; slots() is UI-owned, so the lookup is guarded.
    vstSharedProvider() = [](int key, const std::string& path, const std::string& classId) -> std::shared_ptr<caw::IPlugin> {
        std::lock_guard<std::mutex> lock(vstui::slotsMutex());
        const auto it = vstui::slots().find(key);
        if (it == vstui::slots().end() || !it->second.plugin) return {};
        if (it->second.loadedPath != path) return {};
        if (!classId.empty() && !it->second.loadedClassId.empty() && it->second.loadedClassId != classId) return {};
        return it->second.plugin;
    };
    vstui::editorFeedSource = [](int key) -> std::shared_ptr<caw::IPlugin> {
        const int trackId = key / 8, stage = key % 8;
        // SEPARATED ENGINES: the 4th engine always hosts the VST graph, so its instances hear the
        // real audio and the real notes. The editor tick replays the engine ring (processIdle) so
        // the plugin's own GUI runs at realtime speed instead of looking dead. Called from the
        // editor worker thread: the rack lookup is shared_ptr-safe; an engine that is off has no
        // graph and simply returns nothing.
        // The key decodes to (trackId, stage): each stage's editor window is fed from ITS stage's
        // engine instance, so a chain stage's EQ analyzer draws what that stage really processes.
        return vstSolo().pluginOfTrackStage(trackId, stage % 8);
    };
    // The meter lane layout asks the loaded plugin how many outputs it really has: a 4-channel
    // instrument gets 4 meters on its track (the master fold follows the audio setting).
    vstTrackOutputChannels = [](int trackId) -> int {
        // The routing text wins: one meter per OUTPUT line the text uses (max out index).
        for (const Track& t : g_proj.tracks) if (t.id == trackId && !t.vstRouteCode.empty()) {
            const int mx = t.routeMaxOut();
            return mx > 0 ? mx : 2;
        }
        // One meter per TRACK channel: the track's channel format (the user sets it in the VST panel,
        // like a DAW track). Never guessed from the plugin - plugins that follow the host report the
        // host's width, which made the meters and the master join flip.
        for (const Track& t : g_proj.tracks) if (t.id == trackId) return std::max(1, std::min(8, t.vstTrackChannels()));
        return 2;
    };
#endif
    vstui::pushVstOutModeLive = [](int trackId, int mode) {
        g_apiProc.setVstOutModeLive(trackId, mode);
        if (g_vstEngineOn) vstSolo().setOutMode(trackId, mode);   // the effect may run in the VST engine
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());   // replace <-> mix, now
        updateScSlotSuppress();   // replace <-> mix changes which SC track slots are consumed
        ensureSampleEngineForVstInputs();   // a sample or locked source may feed the effect now
    };
    vstui::pushVstOutWideLive = [](int trackId, int channels) {
        // TRACK CHANNELS changed: the running engines take the new width with the wiring push, the
        // meter layouts are rebuilt now (the idle one at once, the live one on its next refresh).
        g_apiProc.setVstOutWideLive(trackId, channels > 2 ? 1 : 0);
        g_apiProc.setVstChannelsLive(trackId, channels);
        if (g_vstEngineOn) { vstSolo().setOutWide(trackId, channels > 2 ? 1 : 0); vstSolo().setWiring(collectVstEngineJobs()); }
        // The meter SLOTS are positions in one list the running Csound monitor writes (fixed at Run).
        // Rebuilding the live list here shifted every later track onto the wrong slots: a Csound
        // source after (or before) this VSTe went dark. Only the idle layout follows; VST meters
        // are read by track id (g_vstMeterDb), so their bar count changes without touching the list.
        if (!g_liveCsound) g_idleMeters = meterLayout(g_proj, g_projectPath);
    };
    vstui::pushVstInputMasks = [](const std::map<int, unsigned>& masks) {
        g_apiProc.setVstInputMasks(masks);
        if (g_vstEngineOn) vstSolo().setInputMasks(masks);
        // The checkbox also changes the slot's configured state, so the jobs are re-pointed too:
        // an input that was off at Run can be switched ON live and start feeding at once.
        if (g_langVst && g_liveCsound && g_useCsoundApi) pushVstTapsToCsound();   // SEPARATED: taps only - the VST graph lives in the 4th engine
        if (g_vstEngineOn) vstSolo().setWiring(collectVstEngineJobs());
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());   // an input switched off stops replacing
        updateScSlotSuppress();   // an input switched on / off changes the consumed SC track slots
        ensureSampleEngineForVstInputs();   // the input may name a sample or locked source now
    };
    vstui::pushVstWiring = []() {   // an input mode / name changed: re-route the effect now, no Run
        if (g_langVst && g_liveCsound && g_useCsoundApi) pushVstTapsToCsound();   // SEPARATED: taps only - the VST graph lives in the 4th engine
        if (g_vstEngineOn) vstSolo().setWiring(collectVstEngineJobs());
        if (g_liveCsound && g_useCsoundApi) g_apiProc.setControlChannels(vstGateValues());
        updateScSlotSuppress();   // the input names changed: recompute the consumed SC track slots
        ensureSampleEngineForVstInputs();   // the new input may name a sample or locked source
    };
    // A .vst3 finished loading into the editor: the engine applies it NOW (not up to 0.5 s later)
    // while it runs. The engine itself is only started by its own button (Run VST / the combined
    // Run button) - Play and a plugin pick never start it. A re-picked plugin also gets a new
    // attempt after a failed load.
    // The editor tick feeds every open plugin window at ~60 Hz (realtime speed). Sound being
    // audible only changes ONE thing: the full-state capture (taken once after the user edits
    // something) runs under the engine's render pause (withRenderPaused) - the engine instance is
    // idle for that moment and the audio keeps playing from the FIFO, so the EQ curve / preset
    // drawn in the editor still reaches the engine while you listen. No timer polls getState().
    vstui::editorPlaybackBusy = []() { return g_timelineRun || g_runLeft > 0.0; };
    // Audible right now? Only then must the capture pause the engine's rendering (see above).
    vstui::editorStateCaptureBlocked = []() { return g_timelineRun || g_runLeft > 0.0; };
    vstui::editorStateCaptureGuard = [](const std::function<void()>& fn) { return vstSolo().withRenderPaused(fn); };
    vstui::engineRestartBusy = []() { return vstSolo().engineRestarting(); };
    vstui::notifyPluginLoaded = [](int trackId) {
        const Track* t = nullptr;
        for (const Track& o : g_proj.tracks) if (o.id == trackId && (o.kind == 3 || o.kind == 4)) { t = &o; break; }
        if (!t) return;
        g_vstFailedSig.clear(); g_vstFailedRetryT = 0.0;
        if (g_langVst && g_liveCsound && g_useCsoundApi)
            pushVstTapsToCsound();   // SEPARATED: Csound never reloads plugins - the 4th engine loads the new .vst3 by itself
        if (!g_vstEngineOn) return;   // the engine is off: the plugin waits for Run VST
        for (int st = 0; st <= t->vstChainCount(); ++st)   // every chain stage's editor instance is prepared too
            vstui::prepareEditorPlugin(vstui::slotKey(trackId, st), pdAudioSettings().sampleRate, 512);   // SEPARATED: the editor format follows the audio device (the VST engine's own format) - never Csound's ksmps. Both VSTi and VSTe: an unprepared editor instance never processes, so its GUI (EQ waves, meters) stays dead.
        vstEngineCheckJobs(10.0f);   // past the 0.5 s gate: the changed plugin set is applied on this frame
    };
    // Values the engine's plugins are actually using: the Csound rack first, then the 4th engine
    // (clip code writes P1..P8 there too, so the P sliders and the plugin GUI must follow it).
    vstui::takeVstEngineParams = [](int trackId, double out[8]) {
        if (g_apiProc.vstEngineParams(trackId, out)) return true;
        return g_vstEngineOn && vstSolo().params(trackId, out);
    };
    g_apiProc.setScAudioRing(&g_scRing);            // VSTe "SuperCollider" inputs read this
    g_apiProc.setTrackRings(
        [](int trackId) -> CawScRing* { return sampleRingOf(trackId); },        // engine 4: sample tracks
        [](int slot) -> CawScRing* { return scTrackRingOf(slot); });            // engine 2: one ring per SC track slot
    g_apiProc.setScRawSuppress(&g_scRawSuppress);   // master-mode VSTe replaces the raw SC mix
}


// ---------------------------------------------------------------- the VST engine (own Run / Stop button)
// Loads the instrument plugins, schedules the MIDI clips from the playhead and mixes the sound into
// whatever stream is open (or opens its own). It does not need Csound; when Csound runs too, the
// notes written by clip code (note / cc / msg) and the P1..P8 channels are forwarded into it.




// Everything that must follow a successful load: the clip code, the schedule and the output
// stream. ONE function for every path (Run VST, a plugin picked later, a retry), so the order in
// which the user does things can never leave the engine half-started.


// Load / reload the instruments and make them play. A failed load is retried every 5 s (and at
// once when the plugin is picked again) instead of leaving the engine silent until Run VST is
// pressed once more.






// The 0.5 s check: the clip-code mode and the instrument set. It runs from vstEngineTick (per
// frame, gated) and at once from notifyPluginLoaded (dt >= 0.5), so a plugin picked in the panel
// is loaded on the very frame - and a plugin picked BEFORE Run VST behaves exactly like one
// picked after it.


// Every frame: the engine's log, the dedicated stream, and a reload when a plugin was loaded / removed.


// Live VSTE input on/off: one bitmask per VSTe track, bit i = input slot i feeds the plugin.


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




// Play from time t: the timeline events (clip start times) from the playhead on. stopFirst: silence what sounds now (play, seek).
// Without it (live edit) what sounds keeps sounding and only the events still to come are replaced.


// The Run button of a clip editor: the clip plays next to the timeline.


























// How long a sound needs from the moment the engine starts it until it comes out of the speakers (hardware buffer -B plus the sync offset from Settings).
// The timeline sends events this much EARLIER, so the sound is heard when the playhead is at the event, not one buffer later.


// ---------------------------------------------------------------- audio tracks (kind 7)
// Decoded file, sampler notes, whole-clip playback, cut / join and the volume automation live in
// audio_track.h. The timeline tick schedules the voices, the shared audio callback mixes them.



// The output channels of a sample clip: "as the source" = one channel per file channel (mono ->
// output 1, stereo -> 1-2 ...); custom = the clip's code (out1 = in1, per block, live).







// The master chain, installed on every CAW device stream (see audio_output.h): the master FX
// rack processes the final mix in order, then the master gain scales it once for the device.
// NOTE: a command-line Csound opens its own device and keeps its own CSD gain; the master FX
// only covers CAW's streams.




// Run of a sample clip: apply the code. Timeline playing and the playhead inside the window:
// the sounding sample restarts with the new code. Transport stopped: audition once for the
// clip duration (Run sample must be on). Timeline playing but outside the window: just apply -
// the next window entry uses the new code.
// The track volume slider (0..2 linear). Applied to the sample engine voices; the volume
// AUTOMATION lane multiplies on top, live (engine track gain, no restarts).
float trackVolumeOf(int trackId) {
    for (const Track& t : g_proj.tracks) if (t.id == trackId) return t.volume;
    return 1.f;
}




// The track's volume automation at the playhead (the "volume" lane, 0..2). 1 = no lane or no points.
float trackVolumeAutomation(const Track& t, double time) {
    const auto lane = t.automation.find("volume");
    if (lane == t.automation.end() || lane->second.empty()) return 1.f;
    double lo = 0.0, hi = 2.0, initial = 1.0;
    const auto range = t.automationRanges.find("volume");
    if (range != t.automationRanges.end()) {
        lo = range->second.min;
        hi = range->second.max > lo ? range->second.max : lo + 1.0;
        initial = range->second.value;
    }
    const double v = autoValueAt(lane->second, time, initial);
    return (float)std::max(lo, std::min(hi, v));
}


// The timeline for audio clips: voices follow the window membership of each clip, sampler notes
// (MIDI clips on the MIDI lane) trigger the clip like a sample. Called every tick frame while
// the transport plays; restart = transport start / loop wrap / seek.


// The audio track engine can be the only thing playing: then IT is the playhead clock (audio-rate
// accurate, like the VST engine). The tick captures the anchor on resync.



// Trim helpers: the audio segments stay in step when the clip edges are dragged.

void audioCapRight(Clip& c) {   // the duration cannot exceed the source the segments cover
    if (c.audioSegs.empty()) return;
    const double rate = std::max(0.05, (double)c.audioRate);
    c.dur = std::min(std::max(0.05, c.dur), audioSegsTotal(c) * rate);   // the code's rate is the length scale: source fills dur/rate seconds
}


// Cut: split an audio clip at the playhead into two clips (each keeps its own segment).


// Join: merge the selected audio clips of one lane (same file) into the first one.



// The timeline: the playhead moves; when Csound is running (Run) it gets the clips of the timeline from the playhead on, in ONE command,
// and starts each clip at its start time. It never starts Csound by itself: that is the job of the "Run" button.





// First PD timeline milestone: one non-overlapping patch at a time. The UI
// clock triggers starts/stops, so timing and loops are not sample accurate.
// The full text goes to the console log; the transport bar has room for a short one.

// One plan for Play and for live editing. Csound clips only count while the Csound engine runs.
// includeMuted builds the plan the PD engine loads: every clip, with the muted ones flagged, so
// M / S is applied live instead of rebuilding the engine (and its device stream) on every mute.


// Retiming an already-loaded clip must not stop the engine or the transport.



// Rebuild the isolated runtime on edit. If playing, stop the transport first
// rather than silently continuing with stale or mismatched clips.




// Opcodes found in the code (opcode MyUdo, ...) or reported by "csound -z"; the editors refresh their colors when this grows.

void addExtraOpcode(const std::string& name) {
    if (name.size() >= 2 && g_extraOpcodes.insert(name).second) ++g_langVersion;
}


// user-defined opcodes:  opcode Name, aa, kk
void scanUserOpcodes(const std::string& code) {
    static const std::regex udo(R"re((?:^|\n)[ \t]*opcode[ \t]+([A-Za-z_]\w*))re");
    for (std::sregex_iterator it(code.begin(), code.end(), udo), end; it != end; ++it) addExtraOpcode((*it)[1].str());
}


// "Run clip": run only this clip (no audio), so syntax / init errors show in the console with the line number of the clip.






// "Run" of a clip turns into "Stop" while the clip plays in the engine or its silent test runs.
bool clipRunning(const Clip& clip) { return (g_runLeft > 0.0 && g_runName == clip.name) || (g_testProc.active() && g_test.name == clip.name); }




// ---- LIVE CODE (VSTi / VSTe and Csound clips) -------------------------------------------------
// The clip editor used to only change the text: the running instruments kept the code Csound
// compiled at Run, so every edit needed a full restart of every engine. Now the edited clip's
// instrument is rebuilt from the CURRENT project and compiled into the running Csound between two
// audio blocks (csoundCompileOrc). Csound keeps already-playing instances on the old definition,
// so a clip that is playing is restarted at its current position - only that clip, nothing else.

// Compiles this clip's new instrument into the running engine. Returns false when there is no
// engine to receive it (no API engine, or the instrument could not be built).


// A code change is heard at once: a playing clip is stopped and started again at the playhead, so
// the new definition runs from here. Other clips, lanes and engines keep going untouched.




// SUPERCOLLIDER-STYLE LIVE CODING (live.csound.com): edits are NEVER applied while typing. The clip
// editor marks the clip dirty on every keystroke; the code is pushed into the running engine only on
// an explicit trigger - Alt+Enter (the focused clip editor) or the Run button. applyClipCodeNow is
// that trigger for Csound / VSTi / VSTe / SuperCollider clips. Pure Data is handled in its editor
// (saveAndApply needs the in-memory patch buffer).
// SuperCollider: the Run button is SEPARATED from the project play state. Project stopped: the clip
// is evaluated for its full duration (audition). Project playing: the code is just APPLIED - the
// sounding instance is re-evaluated for its remaining window, a silent clip picks the code up at
// its next window entry (the timeline never re-evaluates on its own).



// Called by the clip editors on every text change. It only MARKS the clip dirty now - the recompile
// waits for Alt+Enter / Run (SuperCollider-style), never happens on a typing pause.


// "line 12" in a Csound message -> "instr Lead1, line 5 of the clip" (lines outside any clip stay as they are)


// One output line of Csound (play, render, run clip): the line number of the orchestra becomes clip + clip line, and the clip's start time is added.
// The line number often comes on its own line right after the error ("line 12:"), so a line that follows an error belongs to it.








// ---- MIDI clip tools (the Csound clips are never touched)
// Split a MIDI clip in two at timeline time t. Notes crossing the cut are split too.


// Merge the selected MIDI clips of a lane into one (a single selected clip joins with the next one of the same instrument).


// Join tool: with MIDI regions selected it joins them right away; with nothing selected it arms the tool,
// and the next click on a MIDI region joins that region with the next one.


// Play button: only plays the timeline clips (moves the playhead and sends the clips to the RUNNING Csound).
// It never starts Csound. Use "Run" for that. Without a running engine the playhead just moves, silently.
bool timelinePlaying() { return g_timelineRun; }






// ---- show / hide all automation and MIDI lanes (Alt+H / Alt+J / Alt+M / Alt+N)









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


// ---------------------------------------------------------------- arrange view
// ---- marker / graph track items on the timeline



// draws one item in the box (x0,top)-(x1,bot); `rgb` is the item colour. Size (percent) shrinks shapes and boxes inside the row (the score shows the full size),
// Font size changes the text.


// Tracks use the original 6 px LEDs; only the master uses taller LEDs.


// Q aligns selected starts, without a grid or changes to clip durations.








// ---------------------------------------------------------------- master monitor
// Cubic interpolation between p1 and p2 (t = 0..1), p0 and p3 are the neighbours.



// Console error -> the clip: opens its editor, puts the cursor on the line, scrolls the timeline to where the clip starts.




// Master level meter in the right panel: one bar per output channel (nchnls). Click it to reset the clip lights.






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


bool devCombo(const char* label, int& idx, const std::vector<AudioDevice>& list, const std::string& tag )
{
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


// Reads one "name = number" header line. Option lines (they start with '-') are handled separately.


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






// ---------------------------------------------------------------- instrument editor


// ---- maximize / restore for the editor windows

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


// Every window without its own numeric id still gets the maximize box + double-click: a stable key
// is derived from the window title (the same string must be passed to both helpers).
static int winKeyOf(const char* title) {
    uint32_t h = 2166136261u;
    for (const char* p = title; *p; ++p) { h ^= (uint32_t)(unsigned char)*p; h *= 16777619u; }
    return 700000000 + (int)(h % 100000000u);
}
void winMaxPrepareT(const char* title) { winMaxPrepare(winKeyOf(title)); }
void winMaxTitleButtonT(const char* title) { winMaxTitleButton(winKeyOf(title)); }

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
    // double-click on the title bar = maximize / restore (like every OS window). The chrome on the
    // right (the maximize box and the close button) is excluded so clicks there stay single actions.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(0) && ImGui::GetIO().MousePos.x < x0
        && ImGui::GetIO().MousePos.y >= pos.y && ImGui::GetIO().MousePos.y <= pos.y + ImGui::GetFrameHeight()) {
        if (w.maxed) { w.maxed = false; w.restore = true; }
        else { w.pos = pos; w.size = size; w.maxed = true; }
    }
}


// Source editor bound to one std::string; each clip (and each lane template) has its own instance.
// Returns true when the user edited the text this frame.


bool colorPicker(const char* id, uint32_t& color, uint32_t fallback) {
    const uint32_t v = color ? color : fallback;
    float rgb[3] = {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f};
    if (!ImGui::ColorEdit3(id, rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) return false;
    color = 0x01000000u | ((uint32_t)std::lround(rgb[0] * 255.f) << 16) |
            ((uint32_t)std::lround(rgb[1] * 255.f) << 8) | (uint32_t)std::lround(rgb[2] * 255.f);
    return true;
}



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








std::string poolInstrName(const std::string& code) {
    static const std::regex re(R"re((?:^|\n)[ \t]*instr[ \t]+([A-Za-z_][A-Za-z_0-9]*|[0-9]+))re");
    std::smatch match;
    return std::regex_search(code, match, re) ? match[1].str() : "(unnamed)";
}










// MIDI on/off in a clip editor. ON creates the clip's MIDI track (lane + region).
// OFF removes it, and asks first because the notes of the region are deleted.








// The Master FX window: 8 VST effects, processed IN ORDER on the master mix of every CAW
// audio stream (after the engines, before the master gain). The FX button sits under the
// master meter.




// Match the project timeline's logarithmic zoom and playhead-centered focus.


// Same 110-pixel travel as the zoom slider, rotated to scroll pitch vertically.


// ONE piano roll for several MIDI clips (select the MIDI clips in the timeline, press Enter). Every clip keeps its own notes and colour;
// new notes go to the clip chosen in "Add notes to". Note times are stored relative to their MIDI clip, like in the single editor.





void appOpenProjectPath(const char* path) {
    if (path && *path) openProjectPath(path);
}


// Files dropped from Explorer that are not projects: queued with the drop point; the program
// adds them as sample clips on the sample lane under the point.
void appQueueFileDrop(const char* path, int x, int y) {
    if (!path || !*path) return;
    g_dropPath = path; g_dropX = x; g_dropY = y; g_dropPending = true;
}


bool appTakeQueuedFileDrop(std::string& path, int& x, int& y) {
    if (!g_dropPending) return false;
    g_dropPending = false;
    path = g_dropPath;
    x = g_dropX;
    y = g_dropY;
    return true;
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
    // Native VST editor windows share this thread's Win32 message pump. Keep it
    // responsive while an editor is visible so plugin paint/timer-driven graphics
    // (for example animated EQ/filter curves) are not starved by the idle 5 fps mode.
    return g_playing || g_timelineRun || g_renderActive || g_testProc.active() ||
           g_testSound.running() || vstui::hasVisiblePluginEditor();
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


// Reads a saved engine path (~/.caw_libpd_path, ~/.caw_sclang_path, ~/.caw_scsynth_path, also on
// Windows in %USERPROFILE%). The file may have been written by a build script or by hand: strip a
// trailing "\r", spaces and quotes, and expand a leading "~/". Empty = nothing usable.
static std::string readSavedPath(const std::filesystem::path& file) {
    if (file.empty()) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::string saved; std::getline(in, saved);
    auto isTrim = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\''; };
    while (!saved.empty() && isTrim(saved.back())) saved.pop_back();
    size_t b = 0; while (b < saved.size() && isTrim(saved[b])) ++b;
    saved = saved.substr(b);
    if (saved.rfind("~/", 0) == 0) { const std::string home = plat::getEnv("HOME"); if (!home.empty()) saved = home + saved.substr(1); }
    return saved;
}

void appInit() {
    const auto began = std::chrono::steady_clock::now();
    logLine("[caw] CAW build: " + std::string(caw_brand::buildTag) +
            " (normal sample-to-VSTe input, arbitrary callback blocks, direct source-through-effect lock).");
    // the master chain (master FX + master gain) sits on the final device mix of every CAW stream
    g_masterChainFn = &masterChainRender;
    g_masterChainEnabled.store(true, std::memory_order_relaxed);
    g_apiProc.setHostGainExternal(true);   // the engine must not apply the master gain twice
    // VSTe "track/clips" inputs can name a sample track: the sample engine hands each track's
    // post-track-gain mix (up to 8 channels: a 4ch file or the code's out1..out8 routing) to a
    // per-track ring the rack pops (engine 4). The rack folds it (RX stereo) or takes it 1:1 (RX multi).
    cawaudio::engine().setFeed([](int trackId, const float* interleaved, int frames, int channels) {
        if (trackId <= 0 || !interleaved || frames <= 0 || channels < 1) return;
        sampleRingOf(trackId)->pushStride2(interleaved, (size_t)frames, std::min(8, channels));   // lock-free ring push (render lock is held)
        // CAW AUDIO BUSES: every sample track also streams its stereo mix into the bus "smp_<track name>"
        // (an audio file as a modulator / an analysis source for Csound, PD or SuperCollider).
        static std::map<int, std::unique_ptr<cawbus::Port>> ports;   // audio thread only
        std::string name;
        { std::unique_lock<std::mutex> l(cawbus::sampleTrackNames().m, std::try_to_lock);
          if (!l.owns_lock()) return;
          const auto it = cawbus::sampleTrackNames().byKey.find(trackId);
          if (it == cawbus::sampleTrackNames().byKey.end()) return;
          name = it->second; }
        auto& p = ports[trackId];
        if (!p) p = std::make_unique<cawbus::Port>();
        p->rename(name);
        static std::vector<float> lr;
        if (lr.size() < (size_t)frames * 2) lr.resize((size_t)frames * 2);
        for (int i = 0; i < frames; ++i) { lr[(size_t)i * 2] = interleaved[(size_t)i * channels]; lr[(size_t)i * 2 + 1] = interleaved[(size_t)i * channels + (channels > 1 ? 1 : 0)]; }
        cawbus::send(*p, lr.data(), (size_t)frames);
    });
    // VSTe "SuperCollider" inputs: the embedded SC engine hands each TRACK SLOT's raw stereo mix to
    // a per-slot ring the rack pops (engine 2). Every SuperCollider track has its own stream now -
    // a VSTe hears its own track, not the whole SC mix.
    g_scEngine.setTrackFeed([](int slot, const float* interleaved, int frames, int stride) {
        if (!interleaved || frames <= 0) return;
        if (slot >= 0 && slot < ScEngine::kFirstBusSlot) {   // CAW AUDIO BUSES: each SuperCollider track's raw mix as "sc_<track name>"
            static std::array<std::unique_ptr<cawbus::Port>, ScEngine::maxTrackSlots> ports;   // audio thread only
            std::string name;
            { std::unique_lock<std::mutex> l(cawbus::scSlotNames().m, std::try_to_lock);
              if (l.owns_lock()) { const auto it = cawbus::scSlotNames().byKey.find(slot); if (it != cawbus::scSlotNames().byKey.end()) name = it->second; } }
            if (!name.empty()) {
                auto& p = ports[(size_t)slot];
                if (!p) p = std::make_unique<cawbus::Port>();
                p->rename(name);
                static std::vector<float> lr;
                if (lr.size() < (size_t)frames * 2) lr.resize((size_t)frames * 2);
                for (int i = 0; i < frames; ++i) { lr[(size_t)i * 2] = interleaved[(size_t)i * stride]; lr[(size_t)i * 2 + 1] = interleaved[(size_t)i * stride + (stride > 1 ? 1 : 0)]; }
                cawbus::send(*p, lr.data(), (size_t)frames);
            }
        }
        if (CawScRing* ring = scTrackRingOf(slot)) ring->pushStride2(interleaved, (size_t)frames, stride);   // the ring is stereo: channels 0/1 of the widened slot
    });
    g_pdLibraryPath = plat::bundledLibpd();
    g_scLangPath = plat::superColliderSclang();
    // Saved engine paths (Settings > Programs, or written by the engine build scripts). A path that
    // is set but unusable is reported in the console, so "CAW cannot find it" is never silent.
    auto usable = [](const std::string& path, const char* what, const char* file) -> bool {
        if (path.empty()) return false;
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::u8path(path), ec) && !ec) return true;
        logLine(std::string("[caw] ") + what + ": the saved path in ~/" + file + " does not exist: " + path);
        return false;
    };
    {
        const std::string saved = readSavedPath(scConfigPath());
        if (!saved.empty() && isSystemSuperColliderPath(saved))
            logLine("[caw] SuperCollider: the saved sclang is a normal SuperCollider install (no CAW driver) - ignored: " + saved);
        else if (usable(saved, "SuperCollider", ".caw_sclang_path")) g_scLangPath = saved;
    }
    {
        const std::string saved = readSavedPath(scSynthConfigPath());
        if (!saved.empty() && isSystemSuperColliderPath(saved))
            logLine("[caw] SuperCollider: the saved libscsynth is a normal SuperCollider install - ignored: " + saved);
        else if (usable(saved, "SuperCollider", ".caw_scsynth_path")) g_scSynthPath = saved;
    }
    {
        const std::string saved = readSavedPath(pdConfigPath());
        if (usable(saved, "Pure Data", ".caw_libpd_path")) g_pdLibraryPath = saved;
    }
    // Nothing saved: CAW's own build folders (tools/unix/build-*.sh), so a fresh build is found
    // even without the settings files.
    if (!plat::isWindows) {
        const std::string home = plat::getEnv("HOME");
        std::error_code ec;
        const char* pdName = plat::isMac ? "libpd.dylib" : "libpd.so";
        if (g_pdLibraryPath.empty() && !home.empty()) {
            const auto f = std::filesystem::u8path(home) / ".caw_deps" / "libpd" / pdName;
            if (std::filesystem::is_regular_file(f, ec)) g_pdLibraryPath = f.u8string();
        }
        const auto scDir = std::filesystem::u8path(home) / ".caw_deps" / "supercollider" / "runtime-x64" / "SuperCollider";
        const char* scName = plat::isMac ? "libscsynth.dylib" : "libscsynth.so";
        if (!home.empty() && (g_scLangPath.empty() || g_scLangPath == "sclang") && std::filesystem::is_regular_file(scDir / "sclang", ec)) g_scLangPath = (scDir / "sclang").u8string();
        if (!home.empty() && g_scSynthPath.empty() && std::filesystem::is_regular_file(scDir / scName, ec)) g_scSynthPath = (scDir / scName).u8string();
    }
    logLine("[caw] Engine paths: Pure Data = " + (g_pdLibraryPath.empty() ? std::string("(not found)") : g_pdLibraryPath) +
            " | sclang = " + (g_scLangPath.empty() ? std::string("(not found)") : g_scLangPath) +
            " | libscsynth = " + (g_scSynthPath.empty() ? std::string("(auto)") : g_scSynthPath));
    for (float& v : g_specSm) v = -120.f;
    historyReset();
    g_savedSnap = serializeProject(g_proj);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    logLine("[caw] Startup initialization: " + std::to_string(ms) + " ms (audio scan deferred).");
    logLine("[caw] Ready. File > New project starts an empty project, File > Open project loads one.");
}


// The help panel: shows the explanation of whatever the mouse is over (all the old hover pop-ups), with an on / off box.



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
        // Keep transport, export completion, recording, autosave, plugin
        // learning and the VST editor instances alive without rebuilding an
        // invisible arrangement.
        updateMeters(dt);
        freezeTick();   // the freeze render pass keeps running while the window is minimized
        updateRecording();
        vstui::serviceLearn(g_proj);
        vstui::captureParameters(g_proj);   // the editor idle tick + state poll keep running while minimized
        if (g_playing && g_useCsoundApi) g_apiProc.setMasterGain(g_proj.cs.masterGain);
        if (g_scEngineOn) g_scEngine.setGain(1.f);   // the master chain applies the master gain on the device buffer (no double gain)
        historyTick(dt);
        tickAutosave(dt);
        return;
    }
    const bool globalKeys = !io.WantTextInput && !g_textEditorFocused;  // code editors keep their own Ctrl+Z
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Q, false)) appRequestClose();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !freezeActive()) saveProject(false);   // a freeze render holds temporary mutes: no save while it runs
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) openProject();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) newProject();
    if (globalKeys && !soundEditorFocused() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {   // a focused sound editor undoes itself
        if (io.KeyShift) redo(); else undo();
    }
    if (globalKeys && !soundEditorFocused() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    if (globalKeys && io.KeyAlt && !io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) seekTimeline(0.0);   // Alt+Left = go to the beginning
    if (globalKeys && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false)) {   // F: follow the playhead on / off
        if (g_tlFollow == 0) g_tlFollow = g_tlFollowLast > 0 ? g_tlFollowLast : 2;
        else { g_tlFollowLast = g_tlFollow; g_tlFollow = 0; }
        logLine("[caw] Timeline follow: " + std::string(g_tlFollow == 0 ? "off" : g_tlFollow == 1 ? "page" : "timeline (playhead centered)") + ".");
    }
    // Space = play / stop the timeline. Never while a code editor window has the keyboard focus: a click on
    // Space play / stop: works whenever the user is NOT typing (io.WantTextInput covers active
    // text widgets; g_codeTyping covers the code editors' own text areas). A clip window that is
    // merely SELECTED (focused, but the text cursor is not in its code area) does not block it -
    // clicking the window's title bar / fields leaves the window focused and Space still works.
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        const bool plain = !io.KeyCtrl && !io.WantTextInput && !g_codeTyping;
        const bool withCtrl = io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
        if (plain || withCtrl) toggleTimelinePlay();
    }
    updateMeters(dt);
    freezeTick();   // the freeze render pass (a modal dialog shows its progress while it runs)
    if (g_scEngineOn) g_scEngine.setGain(1.f);   // the master chain applies the master gain on the device buffer (no double gain)
    updateRecording();
    {   // idle layout (meters of the current project while Csound is off)
        static int tick = 0;
        if (!g_liveCsound && (tick++ % 30) == 0) g_idleMeters = meterLayout(g_proj, g_projectPath);
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
        if (g_renderWillOverwrite) {
            ImGui::TextWrapped("This file already exists: %s", renderFilePath().string().c_str());
            ImGui::TextUnformatted("Replace it after the new export succeeds?");
        }
        if (ImGui::Button(g_renderWillOverwrite ? "Replace" : "Export anyway")) { ImGui::CloseCurrentPopup(); startRenderNow(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The DSP-switch asks (user request): offline rendering wants the DSP off, real-time wants it
    // on. One modal serves all three cases; the action button performs the switch and continues.
    if (g_askDspOffExport || g_askDspOnExport || g_askDspOffLock) ImGui::OpenPopup("##dsp_switch_ask");
    if (ImGui::BeginPopupModal("##dsp_switch_ask", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_askDspOnExport) {
            ImGui::TextUnformatted("Real-time rendering needs the DSP on.");
            ImGui::TextWrapped("Real-time export captures the LIVE mix, so the checked engines (DSP) must be running.");
            ImGui::Spacing();
            if (ImGui::Button("Turn DSP on and export", ImVec2(S(200), 0))) {
                ImGui::CloseCurrentPopup(); g_askDspOnExport = false;
                // The full DSP start (what the DSP button does when it is off): the button then reads on.
                if (g_langCsound) startEngine();
                if (g_langSc && !g_scEngineOn) startScEngine();
                if (g_langVst && !g_vstEngineOn) startVstEngine();
                if (g_langAudio && !g_sampleEngineOn) { g_sampleEngineOn = true; ensureSampleOutput(); logLine("[caw] Sample engine running: the sample clips play with Play."); }
                startRenderNow();
            }
        } else if (g_askDspOffLock) {
            ImGui::TextUnformatted("Offline locking wants the DSP off.");
            ImGui::TextWrapped("Locking a VST track renders it offline without the live engine. Turn the DSP off first - a live plugin instance is what has been faulting.");
            ImGui::Spacing();
            if (ImGui::Button("Turn DSP off and lock", ImVec2(S(200), 0))) {
                ImGui::CloseCurrentPopup();
                const int track = g_askDspOffLockTrack;
                g_askDspOffLock = false; g_askDspOffLockTrack = -1;
                // The full DSP stop (what the DSP button does when everything runs): the button then reads off.
                stopScEngine();
                stopVstEngine();
                if (g_sampleEngineOn) { g_sampleEngineOn = false; cawaudio::engine().clearAll(); g_audioVoiceSig.clear(); closeSampleOutput(); logLine("[caw] Sample engine stopped."); }
                stopPlayback();
                if (track >= 0 && track < (int)g_proj.tracks.size()) freezeTrackBegin(track);
            }
        } else {
            ImGui::TextUnformatted("Offline rendering wants the DSP off.");
            ImGui::TextWrapped("Offline export renders outside the live engine. Turn the DSP off first - a live plugin instance is what has been faulting.");
            ImGui::Spacing();
            if (ImGui::Button("Turn DSP off and export", ImVec2(S(200), 0))) {
                ImGui::CloseCurrentPopup(); g_askDspOffExport = false;
                stopScEngine();
                stopVstEngine();
                if (g_sampleEngineOn) { g_sampleEngineOn = false; cawaudio::engine().clearAll(); g_audioVoiceSig.clear(); closeSampleOutput(); logLine("[caw] Sample engine stopped."); }
                stopPlayback();
                startRenderNow();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(110), 0))) { ImGui::CloseCurrentPopup(); g_askDspOffExport = g_askDspOnExport = g_askDspOffLock = false; g_askDspOffLockTrack = -1; }
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
    drawSoundEditors();
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
    vstui::stopEditorWorker();   // the editor housekeeping thread joins before anything it touches is torn down
    vstui::captureParameters(g_proj);
    if (serializeProject(g_proj) != g_savedSnap) g_askSaveOnClose = true;
    else g_quitRequested = true;
}


// True after Ctrl+Q / File > Exit: stops the engine and the renderers, then the main loop ends.
bool appQuitRequested() {
    if (!g_quitRequested) return false;
    cancelRender();   // a running (offline or real-time) export joins its writer thread and restores the transport
    g_pdTimelineEngine.stop();
    g_pdPreview.stop();
    stopScEngine();
    g_scEngine.stop();
    stopPlayback();
    stopVstEngine(true);
    return true;
}

