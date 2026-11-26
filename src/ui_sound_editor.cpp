#include "app_shared.h"
#include "ui_texture.h"
#include "wav_writer.h"
#include "undo_files.h"

#include <unordered_map>

// src/ui_sound_editor.cpp - the SOUND EDITOR window of a sample clip (right-click the clip).
//
// A window of its own with its own timeline: a high-resolution waveshape and / or spectrogram of the
// clip's audio FILE, zoomable from the whole file down to single samples (bit by bit, with the sample
// points drawn), plus editing: select a range, cut / copy / paste / delete / silence / fade / gain /
// normalize / reverse / invert / trim, undo and redo, and save the result as a WAV (over the original
// or as a copy the clip then points to).
//
// The editor works on its own copy of the samples (interleaved floats). The waveshape is drawn from a
// min / max pyramid (so the whole file is as cheap to draw as one screen) and from the raw samples
// when zoomed in. The spectrogram is ONE stable per-file STFT (audio_track.h's editorSpecEnsure /
// editorSpectroImage: the window follows the file and the settings, never the zoom, so a harmonic
// keeps its color at every zoom level), sampled per pixel into an RGBA image and uploaded to a GPU
// texture (ui_texture.h) - one quad per frame, like the lanes' sample editor.

namespace {

constexpr int kPyrBase = 64;        // frames per bin on the first pyramid level
constexpr double kMinSpp = 1.0 / 64.0;   // deepest zoom: 64 pixels per sample

struct Pyr { std::vector<float> mn, mx; int binFrames = kPyrBase; };

struct SndClipboard { std::vector<float> smp; int ch = 0; };
SndClipboard g_sndClipboard;
bool g_sndFocusSeen = false;   // a sound editor window had the keyboard focus this/last frame (app.cpp skips the project undo then)

struct SoundEd {
    int clipId = 0;
    bool open = true;
    bool focusNext = false;
    int view = 0;                   // 0 waveshape, 1 spectrogram, 2 both
    int editCh = -1;                // the channel the volume-type edits apply to (-1 = all channels)
    // the audio (an editable copy)
    std::vector<float> smp;
    int ch = 2, sr = 48000;
    bool dirty = false;
    int revision = 0;
    std::string path, status;
    // undo / redo
    struct Snap { std::vector<float> smp; long long selA = 0, selB = 0, cursor = 0; };
    std::vector<Snap> undo, redo;
    // view
    double viewStart = 0.0;         // first visible frame
    double spp = 0.0;               // frames per pixel (0 = fit the whole file on the first draw)
    float fitW = 0.f;               // the canvas width the current "whole file" fit was made for
    float ampZoom = 1.f;
    long long selA = 0, selB = 0, cursor = 0;   // selection (frames, unordered) and the cursor
    long long anchor = 0;
    // the Y drag (volume control on the selected samples): the pre-drag samples are kept so the
    // drag previews live and lands as ONE undo step
struct VolDrag { bool active = false; std::vector<float> smp; float y0 = 0.f; int ch = -1; } vol;
    bool rulerScrub = false;   // a click / drag on the ruler moves the PROJECT playhead (no playback)
    // min / max pyramid per channel
    int pyrRev = -1;
    std::vector<std::vector<Pyr>> pyr;
    // spectrogram: Audacity's Preferences > Spectrograms. Analysis = window size / type / zero
    // padding; view = scale, min / max frequency, gain / range (dB), frequency gain, color scheme.
    // Analysis settings rebuild the cached STFT; a view setting only re-renders the same analysis,
    // so the colors stay stable while zooming. Defaults: window 1024, Hann, zero padding 1x,
    // Mel scale, Inferno colors, gain 20 dB, range 80 dB, frequency gain 0.
    int spWin = 1024;            // 0 = auto (follows the file length), else 256..16384
    int spWinType = 5;           // 0 none (rectangular), 1 blackman, 2 gauss a=2.5, 3 a=3.5, 4 a=4.5, 5 hann, 6 hamming
    int spZeroPad = 1;           // 1 or 2: the FFT runs over N*zeroPad points (finer frequency detail)
    int spScale = 2;             // 0 linear, 1 logarithmic, 2 mel (Audacity's default: Mel)
    int spColor = 3;             // 0 roseus, 1 classic, 2 spectral, 3 inferno, 4 grayscale (default: Inferno)
    float spGainDb = 20.f;       // Audacity "Gain (dB)": the level at the TOP of the color scale
    float spRangeDb = 80.f;      // Audacity "Range (dB)": dB from the top down to the color floor
    float spFreqGain = 0.f;      // Audacity "Frequency Gain (dB/oct)": default 0 (flat)
    float spMinHz = 0.f, spMaxHz = 20000.f;
    void* specTex = nullptr;     // the GPU texture of the current view (ui_texture.h)
    long long specTexKey = -1;
    // edit parameters
    float gainDb = 6.f, normDb = -1.f;
    bool askDiscard = false;
    long long frames() const { return ch > 0 ? (long long)(smp.size() / (size_t)ch) : 0; }
    bool hasSel() const { return selA != selB; }
    long long selLo() const { return std::min(selA, selB); }
    long long selHi() const { return std::max(selA, selB); }
    ~SoundEd() { if (specTex) uitex::destroy(specTex); }   // the GPU texture must not leak on any close path
};

std::vector<std::unique_ptr<SoundEd>> g_sndEds;
int g_sndDiscardId = -1;   // the editor whose close is waiting for the discard question

// ------------------------------------------------------------------ helpers
std::string fmtTime(double sec) {
    if (sec < 0) sec = 0;
    const int m = (int)(sec / 60.0);
    const double s = sec - m * 60.0;
    char b[48];
    std::snprintf(b, sizeof b, "%d:%06.3f", m, s);
    return b;
}

std::string lowerExt(const std::string& p) {
    const size_t d = p.find_last_of('.');
    const size_t sl = p.find_last_of("/\\");
    if (d == std::string::npos || (sl != std::string::npos && d < sl)) return "";
    std::string e = p.substr(d + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

void buildPyramid(SoundEd& e) {
    e.pyr.assign((size_t)e.ch, {});
    const long long F = e.frames();
    for (int c = 0; c < e.ch; ++c) {
        std::vector<Pyr>& levels = e.pyr[(size_t)c];
        Pyr l0;
        l0.binFrames = kPyrBase;
        const size_t n0 = (size_t)std::max(1LL, (F + kPyrBase - 1) / kPyrBase);
        l0.mn.assign(n0, 0.f); l0.mx.assign(n0, 0.f);
        for (size_t b = 0; b < n0; ++b) {
            const long long f0 = (long long)b * kPyrBase, f1 = std::min(F, f0 + kPyrBase);
            float lo = 1e9f, hi = -1e9f;
            for (long long f = f0; f < f1; ++f) {
                const float v = e.smp[(size_t)f * (size_t)e.ch + (size_t)c];
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
            if (lo > hi) lo = hi = 0.f;
            l0.mn[b] = lo; l0.mx[b] = hi;
        }
        levels.push_back(std::move(l0));
        while (levels.back().mn.size() > 1) {
            const Pyr& prev = levels.back();
            Pyr nx;
            nx.binFrames = prev.binFrames * 2;
            const size_t n = (prev.mn.size() + 1) / 2;
            nx.mn.assign(n, 0.f); nx.mx.assign(n, 0.f);
            for (size_t i = 0; i < n; ++i) {
                const size_t a = i * 2, b = std::min(prev.mn.size() - 1, a + 1);
                nx.mn[i] = std::min(prev.mn[a], prev.mn[b]);
                nx.mx[i] = std::max(prev.mx[a], prev.mx[b]);
            }
            levels.push_back(std::move(nx));
        }
    }
    e.pyrRev = e.revision;
}

// min / max of channel c over the frame range [f0, f1)
void columnRange(const SoundEd& e, int c, double f0, double f1, float& lo, float& hi) {
    const long long F = e.frames();
    long long a = (long long)std::floor(f0), b = (long long)std::ceil(f1);
    a = std::max(0LL, std::min(F - 1, a));
    b = std::max(a + 1, std::min(F, b));
    const double span = f1 - f0;
    lo = 1e9f; hi = -1e9f;
    if (span < (double)kPyrBase * 2.0 || e.pyr.empty()) {
        for (long long f = a; f < b; ++f) {
            const float v = e.smp[(size_t)f * (size_t)e.ch + (size_t)c];
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
    } else {
        const std::vector<Pyr>& levels = e.pyr[(size_t)c];
        size_t k = 0;
        while (k + 1 < levels.size() && (double)levels[k + 1].binFrames <= span) ++k;
        const Pyr& L = levels[k];
        const long long b0 = a / L.binFrames, b1 = std::min((long long)L.mn.size(), (b + L.binFrames - 1) / L.binFrames);
        for (long long i = b0; i < std::max(b0 + 1, b1) && i < (long long)L.mn.size(); ++i) {
            lo = std::min(lo, L.mn[(size_t)i]); hi = std::max(hi, L.mx[(size_t)i]);
        }
    }
    if (lo > hi) lo = hi = 0.f;
}

double maxSpp(const SoundEd& e, float width) {
    return std::max(1.0, (double)e.frames() / std::max(1.f, width));
}

void clampView(SoundEd& e, float width) {
    const double F = (double)e.frames();
    e.spp = std::max(kMinSpp, std::min(maxSpp(e, width), e.spp));
    e.viewStart = std::max(0.0, std::min(std::max(0.0, F - (double)width * e.spp), e.viewStart));
}

void zoomAt(SoundEd& e, double factor, double anchorFrame, float anchorPx, float width) {
    const double ns = std::max(kMinSpp, std::min(maxSpp(e, width), e.spp * factor));
    e.spp = ns;
    e.viewStart = anchorFrame - (double)anchorPx * ns;
    clampView(e, width);
}

// nice step >= target from a 1-2-5 series
double niceStep(double target) {
    double base = 1.0;
    while (base > target) base /= 10.0;          // base <= target < base * 10
    while (base * 10.0 <= target) base *= 10.0;
    for (double m : {1.0, 2.0, 5.0, 10.0}) if (base * m >= target) return base * m;
    return base * 10.0;
}

void snapshot(SoundEd& e) {
    SoundEd::Snap s;
    s.smp = e.smp; s.selA = e.selA; s.selB = e.selB; s.cursor = e.cursor;
    e.undo.push_back(std::move(s));
    e.redo.clear();
    // keep the memory bounded: at most 40 steps and about 600 MB of snapshots
    size_t bytes = 0;
    for (const auto& u : e.undo) bytes += u.smp.size() * sizeof(float);
    while (e.undo.size() > 1 && (e.undo.size() > 40 || bytes > (size_t)600 * 1024 * 1024)) {
        bytes -= e.undo.front().smp.size() * sizeof(float);
        e.undo.erase(e.undo.begin());
    }
}

void changed(SoundEd& e) { ++e.revision; e.dirty = true; }

void doUndo(SoundEd& e, bool redo) {
    auto& from = redo ? e.redo : e.undo;
    auto& to = redo ? e.undo : e.redo;
    if (from.empty()) return;
    SoundEd::Snap cur;
    cur.smp = e.smp; cur.selA = e.selA; cur.selB = e.selB; cur.cursor = e.cursor;
    SoundEd::Snap s = std::move(from.back());
    from.pop_back();
    to.push_back(std::move(cur));
    e.smp = std::move(s.smp); e.selA = s.selA; e.selB = s.selB; e.cursor = s.cursor;
    changed(e);
}

// the frames an edit works on: the selection, or the whole file when nothing is selected
void editRange(const SoundEd& e, long long& a, long long& b) {
    const long long F = e.frames();
    if (e.hasSel()) { a = std::max(0LL, e.selLo()); b = std::min(F, e.selHi()); }
    else { a = 0; b = F; }
}
// the channel span an edit touches: the chosen channel only, or every channel
inline int chanFrom(const SoundEd& e) { return e.editCh >= 0 ? e.editCh : 0; }
inline int chanTo(const SoundEd& e) { return e.editCh >= 0 ? e.editCh + 1 : e.ch; }

void opDelete(SoundEd& e) {
    if (!e.hasSel()) return;
    const long long a = std::max(0LL, e.selLo()), b = std::min(e.frames(), e.selHi());
    if (b <= a) return;
    snapshot(e);
    e.smp.erase(e.smp.begin() + (std::ptrdiff_t)(a * e.ch), e.smp.begin() + (std::ptrdiff_t)(b * e.ch));
    e.cursor = a; e.selA = e.selB = a;
    changed(e);
}
void opCopy(SoundEd& e, bool cut) {
    if (!e.hasSel()) return;
    const long long a = std::max(0LL, e.selLo()), b = std::min(e.frames(), e.selHi());
    if (b <= a) return;
    g_sndClipboard.smp.assign(e.smp.begin() + (std::ptrdiff_t)(a * e.ch), e.smp.begin() + (std::ptrdiff_t)(b * e.ch));
    g_sndClipboard.ch = e.ch;
    if (cut) opDelete(e);
    e.status = cut ? "Cut to the clipboard." : "Copied to the clipboard.";
}
void opPaste(SoundEd& e) {
    if (g_sndClipboard.smp.empty() || g_sndClipboard.ch <= 0) return;
    // adapt the channel count of the clipboard to this file
    std::vector<float> in;
    const size_t frames = g_sndClipboard.smp.size() / (size_t)g_sndClipboard.ch;
    in.resize(frames * (size_t)e.ch);
    for (size_t f = 0; f < frames; ++f)
        for (int c = 0; c < e.ch; ++c) {
            const int sc = std::min(c, g_sndClipboard.ch - 1);
            in[f * (size_t)e.ch + (size_t)c] = g_sndClipboard.smp[f * (size_t)g_sndClipboard.ch + (size_t)sc];
        }
    snapshot(e);
    long long at = std::max(0LL, std::min(e.frames(), e.cursor));
    if (e.hasSel()) {   // replaces the selection
        const long long a = std::max(0LL, e.selLo()), b = std::min(e.frames(), e.selHi());
        e.smp.erase(e.smp.begin() + (std::ptrdiff_t)(a * e.ch), e.smp.begin() + (std::ptrdiff_t)(b * e.ch));
        at = a;
    }
    e.smp.insert(e.smp.begin() + (std::ptrdiff_t)(at * e.ch), in.begin(), in.end());
    e.selA = at; e.selB = at + (long long)frames; e.cursor = e.selB;
    changed(e);
}
void opTrim(SoundEd& e) {
    if (!e.hasSel()) return;
    const long long a = std::max(0LL, e.selLo()), b = std::min(e.frames(), e.selHi());
    if (b <= a) return;
    snapshot(e);
    std::vector<float> keep(e.smp.begin() + (std::ptrdiff_t)(a * e.ch), e.smp.begin() + (std::ptrdiff_t)(b * e.ch));
    e.smp = std::move(keep);
    e.selA = 0; e.selB = e.frames(); e.cursor = 0; e.viewStart = 0.0;
    changed(e);
}
void opSilence(SoundEd& e) {
    long long a, b; editRange(e, a, b);
    if (b <= a) return;
    snapshot(e);
    for (long long f = a; f < b; ++f)
        for (int c = chanFrom(e); c < chanTo(e); ++c) e.smp[(size_t)f * (size_t)e.ch + (size_t)c] = 0.f;
    changed(e);
}
void opFade(SoundEd& e, bool fadeIn) {
    long long a, b; editRange(e, a, b);
    if (b - a < 2) return;
    snapshot(e);
    const double n = (double)(b - a);
    for (long long f = a; f < b; ++f) {
        const double t = ((double)(f - a) + 0.5) / n;
        const float g = (float)(fadeIn ? t : 1.0 - t);
        for (int c = chanFrom(e); c < chanTo(e); ++c) e.smp[(size_t)f * (size_t)e.ch + (size_t)c] *= g;
    }
    changed(e);
}
void opGain(SoundEd& e, float db) {
    long long a, b; editRange(e, a, b);
    if (b <= a) return;
    snapshot(e);
    const float g = std::pow(10.f, db / 20.f);
    for (long long f = a; f < b; ++f)
        for (int c = chanFrom(e); c < chanTo(e); ++c) e.smp[(size_t)f * (size_t)e.ch + (size_t)c] *= g;
    changed(e);
}
void opNormalize(SoundEd& e, float targetDb) {
    long long a, b; editRange(e, a, b);
    if (b <= a) return;
    float pk = 0.f;
    for (long long f = a; f < b; ++f)
        for (int c = chanFrom(e); c < chanTo(e); ++c) pk = std::max(pk, std::fabs(e.smp[(size_t)f * (size_t)e.ch + (size_t)c]));
    if (pk < 1e-9f) { e.status = "Nothing to normalize: the range is silent."; return; }
    snapshot(e);
    const float g = std::pow(10.f, targetDb / 20.f) / pk;
    for (long long f = a; f < b; ++f)
        for (int c = chanFrom(e); c < chanTo(e); ++c) e.smp[(size_t)f * (size_t)e.ch + (size_t)c] *= g;
    changed(e);
}
void opReverse(SoundEd& e) {
    long long a, b; editRange(e, a, b);
    if (b - a < 2) return;
    snapshot(e);
    for (long long i = a, j = b - 1; i < j; ++i, --j)
        for (int c = 0; c < e.ch; ++c) std::swap(e.smp[(size_t)i * (size_t)e.ch + (size_t)c], e.smp[(size_t)j * (size_t)e.ch + (size_t)c]);
    changed(e);
}
void opInvert(SoundEd& e) {
    long long a, b; editRange(e, a, b);
    if (b <= a) return;
    snapshot(e);
    for (long long f = a; f < b; ++f)
        for (int c = chanFrom(e); c < chanTo(e); ++c) e.smp[(size_t)f * (size_t)e.ch + (size_t)c] = -e.smp[(size_t)f * (size_t)e.ch + (size_t)c];
    changed(e);
}

bool writeWav(const SoundEd& e, const std::string& path, std::string& err) {
    namespace fs = std::filesystem;
    const std::string tmp = path + ".caw_tmp";
    CawWavWriter w;
    if (!w.open(tmp, e.sr, e.ch, 2, err)) return false;   // 32-bit float: the edit is stored without any loss
    const size_t F = (size_t)e.frames();
    const size_t chunk = 65536;
    for (size_t off = 0; off < F; off += chunk) {
        const size_t n = std::min(chunk, F - off);
        if (!w.write(e.smp.data() + off * (size_t)e.ch, n, 1.0, 1.f, err)) return false;
    }
    if (!w.finish(err)) return false;
    std::error_code ec;
    fs::remove(fs::u8path(path), ec);
    fs::rename(fs::u8path(tmp), fs::u8path(path), ec);
    if (ec) { err = "could not replace the file: " + ec.message(); return false; }
    return true;
}

// point the clip at the (new) file content: the whole file becomes the clip's source
void applyToClip(SoundEd& e, const std::string& newPath, bool lengthChanged) {
    Clip* c = findClip(e.clipId);
    if (!c) return;
    historyFlush();
    c->audioFile = newPath;
    const double dur = (double)e.frames() / (double)std::max(1, e.sr);
    const double rate = std::max(0.05, (double)c->audioRate);
    c->audioSegs.clear();
    c->audioSegs.push_back({0.0, dur});
    c->dur = std::max(0.05, dur * rate);
    if (lengthChanged) c->audioGainPts.clear();   // their times were for the old length
    cawaudio::forgetAudio(newPath);
    audioKillClipVoices(c->id);
    g_audioVoiceSig.clear();
    e.path = newPath;
    e.dirty = false;
}

void saveEditor(SoundEd& e, bool asCopy) {
    Clip* c = findClip(e.clipId);
    if (!c) return;
    namespace fs = std::filesystem;
    std::string target = e.path;
    if (asCopy || lowerExt(e.path) != "wav") {
        std::string suggested = e.path;
        const size_t d = suggested.find_last_of('.');
        const size_t sl = suggested.find_last_of("/\\");
        if (d != std::string::npos && (sl == std::string::npos || d > sl)) suggested = suggested.substr(0, d);
        suggested += "_edit.wav";
        target = plat::chooseFile(true, "Save the edited sound", "WAV audio", {"wav"}, suggested);
        if (target.empty()) return;
        if (lowerExt(target) != "wav") target += ".wav";
    } else {
        // In-place save of a .wav clip: never overwrite the original file (undo must be able to
        // restore it). Save to a versioned copy next to it and point the clip at that copy via
        // applyToClip - the project snapshot then carries the whole edit, so undo/redo works.
        namespace fs = std::filesystem;
        const fs::path srcPath = fs::u8path(e.path);
        std::string dir = srcPath.parent_path().u8string(), stem = srcPath.stem().u8string();
        if (dir.empty()) dir = ".";
        if (stem.empty()) stem = "sound";
        int n = 1;
        std::error_code ec;
        do {
            target = dir + "/" + stem + ".cawedit" + std::to_string(n++) + ".wav";
        } while (fs::exists(fs::u8path(target), ec) && n < 10000);
    }
    const long long oldFrames = [&] {
        std::string er;
        const auto old = audioClipData(*c, er);
        return old ? (long long)(old->samples.size() / (size_t)std::max(1, old->channels)) : -1LL;
    }();
    std::string err;
    if (!writeWav(e, target, err)) { e.status = "Save failed: " + err; logLine("[caw] Sound editor: save failed: " + err); return; }
    applyToClip(e, target, oldFrames != e.frames());
    e.status = "Saved " + target;
    logLine("[caw] Sound editor: saved " + target);
}

void drawRuler(SoundEd& e, ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, IM_COL32(30, 31, 36, 255));
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), IM_COL32(12, 12, 14, 255));
    const float w = b.x - a.x;
    const double target = 90.0 * e.spp;   // frames between two labels
    const bool sampleUnits = target < (double)e.sr * 0.02;
    double stepFrames;
    if (sampleUnits) stepFrames = std::max(1.0, niceStep(target));
    else stepFrames = niceStep(target / (double)e.sr) * (double)e.sr;
    dl->PushClipRect(a, b, true);
    const double f0 = e.viewStart, f1 = e.viewStart + (double)w * e.spp;
    const double minorStep = stepFrames / 5.0;
    if (minorStep / e.spp >= 6.0)
        for (double f = std::ceil(f0 / minorStep) * minorStep; f <= f1; f += minorStep) {
            const float x = a.x + (float)((f - e.viewStart) / e.spp);
            dl->AddLine(ImVec2(x, b.y - S(4)), ImVec2(x, b.y), IM_COL32(120, 122, 132, 255));
        }
    for (double f = std::ceil(f0 / stepFrames) * stepFrames; f <= f1; f += stepFrames) {
        const float x = a.x + (float)((f - e.viewStart) / e.spp);
        dl->AddLine(ImVec2(x, b.y - S(10)), ImVec2(x, b.y), IM_COL32(190, 192, 200, 255));
        char lab[48];
        if (sampleUnits) std::snprintf(lab, sizeof lab, "%lld", (long long)std::llround(f));
        else std::snprintf(lab, sizeof lab, "%s", fmtTime(f / (double)e.sr).c_str());
        dl->AddText(ImVec2(x + S(3), a.y + S(2)), IM_COL32(200, 202, 210, 255), lab);
    }
    dl->PopClipRect();
}
void drawWaveLane(SoundEd& e, ImDrawList* dl, int c, ImVec2 a, ImVec2 b, uint32_t rgb, float width) {
    const float mid = 0.5f * (a.y + b.y), amp = 0.5f * (b.y - a.y) * 0.94f * e.ampZoom;
    const int cr = (int)((rgb >> 16) & 0xFF), cg = (int)((rgb >> 8) & 0xFF), cb = (int)(rgb & 0xFF);
    dl->AddRectFilled(a, b, IM_COL32((int)(cr * 0.16f), (int)(cg * 0.16f), (int)(cb * 0.16f), 255));
    // amplitude guides
    for (float lv : {1.f, 0.5f, -0.5f, -1.f}) {
        const float y = mid - lv * amp;
        if (y > a.y && y < b.y) dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 22));
    }
    dl->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), IM_COL32(255, 255, 255, 60));
    dl->AddText(ImVec2(a.x + S(4), a.y + S(2)), IM_COL32(255, 255, 255, 110), e.ch > 1 ? (c == 0 ? "L" : c == 1 ? "R" : "") : "");
    const long long F = e.frames();
    dl->PushClipRect(a, b, true);
    const ImU32 fill = IM_COL32(cr, cg, cb, 255);
    const ImU32 edge = IM_COL32(std::min(255, cr + 70), std::min(255, cg + 70), std::min(255, cb + 70), 255);
    if (e.spp >= 1.0) {
        // one column per pixel: min / max of the frames under it
        const int px = (int)std::ceil(width);
        for (int x = 0; x < px; ++x) {
            const double f0 = e.viewStart + (double)x * e.spp, f1 = f0 + e.spp;
            if (f0 >= (double)F) break;
            float lo, hi;
            columnRange(e, c, f0, f1, lo, hi);
            float yHi = mid - hi * amp, yLo = mid - lo * amp;
            if (yLo - yHi < 1.f) { const float m = 0.5f * (yHi + yLo); yHi = m - 0.5f; yLo = m + 0.5f; }
            dl->AddRectFilled(ImVec2(a.x + (float)x, yHi), ImVec2(a.x + (float)x + 1.f, yLo), fill);
        }
    } else {
        // zoomed in past one sample per pixel: the samples themselves - a line through them and a dot on each
        const double pxPerFrame = 1.0 / e.spp;
        const long long fa = std::max(0LL, (long long)std::floor(e.viewStart) - 1);
        const long long fb = std::min(F - 1, (long long)std::ceil(e.viewStart + (double)width * e.spp) + 1);
        const bool dots = pxPerFrame >= 6.0;
        ImVec2 prev(0, 0);
        bool havePrev = false;
        for (long long f = fa; f <= fb; ++f) {
            const float v = e.smp[(size_t)f * (size_t)e.ch + (size_t)c];
            const ImVec2 p(a.x + (float)(((double)f - e.viewStart) / e.spp), mid - v * amp);
            if (dots) dl->AddLine(ImVec2(p.x, mid), p, IM_COL32(cr, cg, cb, 110), 1.f);   // a stem from zero to the sample
            if (havePrev) dl->AddLine(prev, p, edge, 1.5f);
            prev = p; havePrev = true;
        }
        if (dots)
            for (long long f = fa; f <= fb; ++f) {
                const float v = e.smp[(size_t)f * (size_t)e.ch + (size_t)c];
                dl->AddCircleFilled(ImVec2(a.x + (float)(((double)f - e.viewStart) / e.spp), mid - v * amp), 3.f, IM_COL32(255, 255, 255, 235));
            }
    }
    dl->PopClipRect();
}


// ------------------------------------------------------------------ spectrogram
// The engine lives in audio_track.h: editorSpecEnsure computes ONE stable STFT for the whole file
// (window / type / zero padding from the settings, hop = window/8, fixed log-frequency rows,
// progressive with an 8 ms budget per frame), editorSpectroImage samples it per pixel for the
// current view (the y-axis scale / frequency range / threshold / range / frequency gain / colormap
// are applied while resampling, so colors never drift with the view). Here we only upload that
// RGBA image to a GPU texture once per view change (one quad per frame) and draw it; without a
// texture backend the same image is drawn as draw-list cells.

static void sedTexRelease(SoundEd& e) {
    if (e.specTex) uitex::destroy(e.specTex);
    e.specTex = nullptr;
    e.specTexKey = -1;
}

static cawaudio::EditorSpecSettings sedSpecSettings(const SoundEd& e) {
    cawaudio::EditorSpecSettings s;
    s.win = e.spWin;
    s.winType = e.spWinType;
    s.zeroPad = e.spZeroPad;
    s.chan = e.editCh < 0 ? 0 : e.editCh + 1;   // -1 / 0 = the mono mix, else the 1-based channel
    s.scale = e.spScale;
    s.color = e.spColor;
    s.minDb = e.spGainDb - e.spRangeDb;         // Audacity mapping: the color floor sits Range below the Gain level
    s.rangeDb = e.spRangeDb;
    s.freqGainDb = e.spFreqGain;
    s.minHz = e.spMinHz;
    s.maxHz = e.spMaxHz;
    return s;
}

void drawSpectro(SoundEd& e, ImDrawList* dl, ImVec2 a, ImVec2 b, float width) {
    dl->AddRectFilled(a, b, IM_COL32(10, 10, 12, 255));
    if (e.smp.empty()) return;
    const float height = b.y - a.y;
    const cawaudio::EditorSpecSettings sps = sedSpecSettings(e);
    // settings key: a proper hash mix (the old shift-xor overflowed - e.g. colors 0 / 2 / 4 all
    // shifted to the same value, so switching the scheme changed NOTHING on screen)
    auto mix = [](long long h, long long v) {
        uint64_t x = (uint64_t)h;
        x ^= (uint64_t)v + 0x9E3779B97F4A7C15ULL + (x << 6) + (x >> 2);
        return (long long)x;
    };
    const long long spsAnalysis = mix(mix(mix(0x9E3779B97F4A7C15LL, sps.win), sps.winType), mix(mix(sps.zeroPad, sps.chan), 0x5ED1));
    const long long spsView = mix(mix(mix(mix(mix(mix(spsAnalysis, (long long)std::llround(sps.minHz * 1000.0)), (long long)std::llround(sps.maxHz * 1000.0)),
        (long long)std::llround(sps.minDb * 100.0)), (long long)std::llround(sps.rangeDb * 100.0)), (long long)std::llround(sps.freqGainDb * 100.0)),
        mix((long long)sps.scale, (long long)sps.color));
    const long long fileKey = (long long)e.revision ^ 0x5EDC01 ^ (long long)std::hash<std::string>{}(e.path) ^ (long long)cawaudio::fileStamp(e.path) ^ spsAnalysis;
    // the view range in SECONDS (the engine works in seconds; viewStart / spp are frames)
    const double v0Sec = e.viewStart / (double)e.sr;
    const double v1Sec = (e.viewStart + (double)width * e.spp) / (double)e.sr;
    const long long key = fileKey ^ spsView ^ (long long)std::llround(v0Sec * 1e6)
        ^ ((long long)std::llround(v1Sec * 1e6) << 1)
        ^ ((long long)(int)width << 2) ^ ((long long)(int)height << 3);
    // the image is built in 16 px steps: dragging the window edge then stretches the quad a few
    // pixels (linear-filtered, invisible) instead of re-rendering per frame
    const int qw = std::max(16, ((int)width + 15) / 16 * 16), qh = std::max(16, ((int)height + 15) / 16 * 16);
    const cawaudio::EditorSpectroImg& img = cawaudio::editorSpectroImage(e.smp.data(), e.ch, e.sr, e.smp.size() / (size_t)std::max(1, e.ch),
        key, fileKey, v0Sec, v1Sec, qw, qh, sps);
    bool texOk = false;
    if (img.key == key && img.rgba.size() == (size_t)(qw * qh)) {
        if (e.specTexKey != key) {
            sedTexRelease(e);
            e.specTex = uitex::createRgba(img.w, img.h, img.rgba.data());
            e.specTexKey = e.specTex ? key : -1;
        }
        if (e.specTex) { dl->AddImage(e.specTex, a, b); texOk = true; }
    }
    if (!texOk) {   // no texture backend: draw the same image as cells (budgeted)
        if (img.w > 0 && img.h > 0) {
            const int step = std::max(1, (int)std::ceil(std::sqrt((double)img.w * (double)img.h / 100000.0)));
            const float cw = width / (float)img.w, chh = height / (float)img.h;
            dl->PushClipRect(a, b, true);
            for (int y = 0; y < img.h; y += step)
                for (int x = 0; x < img.w; x += step) {
                    const uint32_t col = img.rgba[(size_t)y * (size_t)img.w + (size_t)x];
                    if (!col) continue;
                    dl->AddRectFilled(ImVec2(a.x + (float)x * cw, a.y + (float)y * chh), ImVec2(a.x + (float)(x + step) * cw + 0.6f, a.y + (float)(y + step) * chh + 0.6f), col);
                }
            dl->PopClipRect();
        }
    }
    // frequency anchors over the chosen scale / range
    {
        const double fMax = std::min(std::max((double)sps.maxHz, 10.0), (double)e.sr * 0.5);
        const double fMin = std::min(std::max((double)sps.minHz, 1.0), fMax - 1.0);
        char lb[24];
        for (const double hz : {100.0, 1000.0, 5000.0, 10000.0}) {
            if (hz <= fMin || hz >= fMax) continue;
            double u;   // 0 at the bottom, 1 at the top
            if (sps.scale == 0) u = hz / fMax;
            else if (sps.scale == 2) {
                const double mMax = 2595.0 * std::log10(1.0 + fMax / 700.0), mMin = 2595.0 * std::log10(1.0 + fMin / 700.0);
                u = (2595.0 * std::log10(1.0 + hz / 700.0) - mMin) / std::max(1e-9, mMax - mMin);
            } else u = std::log(hz / fMin) / std::log(fMax / fMin);
            const float y = b.y - (float)u * height;
            std::snprintf(lb, sizeof lb, hz >= 1000.0 ? "%g kHz" : "%g Hz", hz >= 1000.0 ? hz / 1000.0 : hz);
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 40));
            dl->AddText(ImVec2(a.x + S(4), y - ImGui::GetFontSize() - 1.f), IM_COL32(255, 255, 255, 150), lb);
        }
    }
}

// ------------------------------------------------------------------ spectrogram settings ('+' dropdowns)
struct SedItem { const char* label; int value; };
static void sedPlusCombo(const char* id, const char* curLabel, const SedItem* items, int n, int& value) {
    char btn[128]; std::snprintf(btn, sizeof btn, "+  %s##%s", curLabel, id);
    if (ImGui::SmallButton(btn)) ImGui::OpenPopup(id);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", curLabel);
    if (ImGui::BeginPopup(id)) {
        for (int i = 0; i < n; ++i)
            if (ImGui::MenuItem(items[i].label, nullptr, value == items[i].value)) value = items[i].value;
        ImGui::EndPopup();
    }
}
// The spectrogram settings POPUP (opened from the toolbar button; never crams the toolbar rows).
// Layout follows Audacity's Preferences > Spectrograms: Scale group, Colors group, Algorithm group.
void drawSpecSettings(SoundEd& e) {
    if (!ImGui::BeginPopup("##se_specset")) return;
    const float w = S(300);
    ImGui::TextUnformatted("Scale");
    ImGui::Separator();
    ImGui::SetNextItemWidth(w);
    {
        static const char* scales[] = { "Mel", "Logarithmic", "Linear" };   // Audacity's order, Mel first (its default)
        static const int scaleVals[] = { 2, 1, 0 };
        int sc = 1;
        for (int k = 0; k < 3; ++k) if (e.spScale == scaleVals[k]) sc = k;
        if (ImGui::BeginCombo("##ss_scale", scales[sc])) {
            for (int k = 0; k < 3; ++k)
                if (ImGui::MenuItem(scales[k], nullptr, e.spScale == scaleVals[k])) e.spScale = scaleVals[k];
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("How the vertical axis maps frequency. Mel is Audacity's default: an ear-inspired scale; Logarithmic gives octaves equal room; Linear gives hertz equal room.");
    }
    ImGui::SetNextItemWidth(w * 0.48f);
    if (ImGui::DragFloat("Min frequency (Hz)", &e.spMinHz, 5.f, 0.f, 4000.f, "%.0f"))
        e.spMinHz = std::max(0.f, std::min(e.spMinHz, e.spMaxHz - 50.f));
    ImGui::SetNextItemWidth(w * 0.48f);
    if (ImGui::DragFloat("Max frequency (Hz)", &e.spMaxHz, 20.f, 2000.f, 24000.f, "%.0f"))
        e.spMaxHz = std::min(24000.f, std::max(e.spMaxHz, e.spMinHz + 50.f));
    if (ImGui::IsItemHovered()) helpTip("The frequency range the view spans (the top of the view is the max). 20000 Hz = Audacity's default.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Colors");
    ImGui::Separator();
    ImGui::SetNextItemWidth(w * 0.48f);
    if (ImGui::SliderFloat("Gain (dB)", &e.spGainDb, -10.f, 60.f, "%.0f")) {}
    if (ImGui::IsItemHovered()) helpTip("The level at the TOP of the color scale. Audacity's default is 20 dB.");
    ImGui::SetNextItemWidth(w * 0.48f);
    if (ImGui::SliderFloat("Range (dB)", &e.spRangeDb, 20.f, 120.f, "%.0f")) {}
    if (ImGui::IsItemHovered()) helpTip("How many decibels below the top span the whole colormap. Audacity's default is 80 dB. A small range exaggerates differences, a large range compresses them.");
    ImGui::SetNextItemWidth(w * 0.48f);
    if (ImGui::SliderFloat("Frequency gain (dB/oct)", &e.spFreqGain, -10.f, 30.f, "%.0f")) {}
    if (ImGui::IsItemHovered()) helpTip("Lifts the highs: dB per octave above 1 kHz. Audacity's default is 0 (flat).");
    ImGui::SetNextItemWidth(w);
    {
        static const char* colors[] = { "Roseus", "Classic", "Spectral", "Inferno", "Grayscale" };
        if (ImGui::BeginCombo("##ss_color", colors[std::max(0, std::min(4, e.spColor))])) {
            for (int k = 0; k < 5; ++k)
                if (ImGui::MenuItem(colors[k], nullptr, e.spColor == k)) e.spColor = k;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("The colormap: quiet = dark, loud = bright. Roseus is Audacity's default pink; Classic is the old Audacity look.");
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Algorithm");
    ImGui::Separator();
    ImGui::TextDisabled("Frequencies");
    {
        static const int winVals[] = { 0, 256, 512, 1024, 2048, 4096, 8192, 16384 };
        static const char* winNames[] = { "Auto (follows the file length)", "256", "512", "1024", "2048", "4096", "8192", "16384" };
        int wi = 3;
        for (int k = 0; k < 8; ++k) if (e.spWin == winVals[k]) wi = k;
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("Window size", winNames[wi])) {
            for (int k = 0; k < 8; ++k)
                if (ImGui::MenuItem(winNames[k], nullptr, e.spWin == winVals[k])) e.spWin = winVals[k];
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("The FFT window in samples. Audacity's default is 1024. Bigger = finer frequency detail, blurrier time.");
    }
    {
        static const char* types[] = { "None (rectangular)", "Blackman", "Gaussian (a=2.5)", "Gaussian (a=3.5)", "Gaussian (a=4.5)", "Hann", "Hamming" };
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("Window type", types[std::max(0, std::min(6, e.spWinType))])) {
            for (int k = 0; k < 7; ++k)
                if (ImGui::MenuItem(types[k], nullptr, e.spWinType == k)) e.spWinType = k;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("The analysis window's shape. Audacity's default is Hann.");
    }
    {
        static const char* pads[] = { "1x", "2x (finer frequency)" };
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("Zero padding", pads[e.spZeroPad >= 2 ? 1 : 0])) {
            if (ImGui::MenuItem(pads[0], nullptr, e.spZeroPad < 2)) e.spZeroPad = 1;
            if (ImGui::MenuItem(pads[1], nullptr, e.spZeroPad >= 2)) e.spZeroPad = 2;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("Doubles the FFT length with a zero tail - finer frequency interpolation. Audacity's default is 1x.");
    }
    ImGui::Separator();
    ImGui::TextDisabled("Analysis settings (window / padding / channel) rebuild the analysis; view settings re-render instantly.");
    ImGui::EndPopup();
}


// ------------------------------------------------------------------ the project on the canvas ruler
// No extra time bar: the project timeline lives on the editor's own ruler. Every clip that uses
// this file shows as a band over its source span (the clip that opened the editor is brighter),
// the project's loop region maps onto it, and the project playhead is a yellow marker moving with
// the transport. A click on the ruler moves the PROJECT playhead to the corresponding project
// time (mapped through the clip) - it never starts playback, exactly like the project's own ruler.

// source seconds -> project seconds through the clip that opened the editor (-1 = no clip there)
static double sndSrcToProj(const SoundEd& e, double srcSec) {
    const Clip* c = findClip(e.clipId);
    if (!c || c->audioSegs.empty()) return -1.0;
    const double rate = std::max(0.05, (double)c->audioRate);
    double proj = (double)c->start;
    for (const AudioSeg& s : c->audioSegs) {
        if (srcSec >= s.srcOffset - 1e-9 && srcSec <= s.srcOffset + s.dur + 1e-9)
            return proj + (srcSec - s.srcOffset) * rate;
        proj += s.dur * rate;
    }
    return -1.0;
}

// project seconds -> source seconds through the clip that opened the editor (-1 = outside)
static double sndProjToSrc(const SoundEd& e, double projSec) {
    const Clip* c = findClip(e.clipId);
    if (!c || c->audioSegs.empty()) return -1.0;
    const double rate = std::max(0.05, (double)c->audioRate);
    double local = (projSec - (double)c->start) / rate;   // source seconds into the seg list
    if (local < 0.0) return -1.0;
    for (const AudioSeg& s : c->audioSegs) {
        if (local <= s.dur) return s.srcOffset + local;
        local -= s.dur;
    }
    return -1.0;
}

void drawRulerProject(SoundEd& e, ImDrawList* dl, ImVec2 a, ImVec2 b, float bodyY) {
    const float bandY0 = b.y - S(6), bandY1 = b.y - S(1);
    bool any = false;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 7) continue;
        for (const Clip& c : t.clips) {
            if (c.audioFile != e.path || c.audioSegs.empty()) continue;
            double s0 = 1e30, s1 = -1e30;
            for (const AudioSeg& sg : c.audioSegs) { s0 = std::min(s0, sg.srcOffset); s1 = std::max(s1, sg.srcOffset + sg.dur); }
            const float x0 = a.x + (float)((s0 * (double)e.sr - e.viewStart) / e.spp);
            const float x1 = a.x + (float)((s1 * (double)e.sr - e.viewStart) / e.spp);
            if (x1 < a.x || x0 > b.x) continue;
            const bool mine = c.id == e.clipId;
            dl->AddRectFilled(ImVec2(std::max(a.x, x0), bandY0), ImVec2(std::min(b.x, x1), bandY1), mine ? IM_COL32(240, 150, 60, 235) : IM_COL32(150, 150, 160, 90), S(2));
            any = any || mine;
        }
    }
    if (any) {
        if (g_proj.loopEnabled && g_proj.loopEnd - g_proj.loopStart >= 0.01) {
            const double ls0 = sndProjToSrc(e, g_proj.loopStart), ls1 = sndProjToSrc(e, g_proj.loopEnd);
            if (ls0 >= 0.0 && ls1 >= 0.0 && ls1 > ls0) {
                const float x0 = a.x + (float)((ls0 * (double)e.sr - e.viewStart) / e.spp);
                const float x1 = a.x + (float)((ls1 * (double)e.sr - e.viewStart) / e.spp);
                dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, bodyY), IM_COL32(80, 220, 140, 18));   // a whisper of green over the whole shape
                dl->AddRectFilled(ImVec2(x0, bandY0 - S(2)), ImVec2(x1, bandY0), IM_COL32(80, 220, 140, 170));
            }
        }
        // the project playhead across the WHOLE shape (ruler + wave + spectrogram), like Audacity
        const double ps = sndProjToSrc(e, g_play);
        if (ps >= 0.0) {
            const float x = a.x + (float)((ps * (double)e.sr - e.viewStart) / e.spp);
            if (x >= a.x - 2 && x <= b.x + 2) {
                dl->AddLine(ImVec2(x, a.y), ImVec2(x, bodyY), IM_COL32(255, 225, 110, 255), S(1.6f));
                dl->AddLine(ImVec2(x - S(4), a.y), ImVec2(x + S(4), a.y), IM_COL32(255, 225, 110, 255), S(2.f));
            }
        }
        dl->AddText(ImVec2(b.x - ImGui::CalcTextSize("project").x - S(6), a.y + S(2)), IM_COL32(255, 170, 90, 200), "project");
    }
}
// ------------------------------------------------------------------ volume ruler (dB)
// A thin dB scale on the wave view's left edge: where 0 dBFS and the dB ticks sit for the current
// amplitude zoom. Drawn OVER the wave (a gutter would move the hit-testing); like a DAW's side scale.
// The dB scale lives in its own GUTTER column left of the wave (it never overlaps the samples).
// Called per lane: the ticks use that lane's amplitude mapping, drawn inside the gutter's x range.
void drawDbRuler(SoundEd& e, ImDrawList* dl, ImVec2 laneA, ImVec2 laneB, ImVec2 gA, ImVec2 gB) {
    const float gw = gB.x - gA.x;
    const float mid = 0.5f * (laneA.y + laneB.y), amp = 0.5f * (laneB.y - laneA.y) * 0.94f * e.ampZoom;
    dl->AddRectFilled(gA, gB, IM_COL32(16, 16, 19, 255));
    dl->AddLine(ImVec2(gB.x, gA.y), ImVec2(gB.x, gB.y), IM_COL32(70, 74, 84, 160));
    static const float ticks[] = {0.f, -3.f, -6.f, -12.f, -18.f, -24.f, -36.f, -48.f, -60.f, -80.f};
    dl->PushClipRect(gA, gB, true);
    for (const float db : ticks) {
        const float val = std::pow(10.f, db / 20.f);
        const float yHi = mid - val * amp;
        const float yLo = mid + val * amp;
        for (float y : {yHi, yLo}) {
            if (y <= gA.y + 2.f || y >= gB.y - 2.f) continue;
            const bool zero = db == 0.f;
            dl->AddLine(ImVec2(gA.x, y), ImVec2(gB.x - (zero ? 0.f : S(6)), y), zero ? IM_COL32(255, 200, 90, 200) : IM_COL32(160, 165, 175, 130), 1.f);
            char lb[10];
            std::snprintf(lb, sizeof lb, zero ? "0dB" : "%g", db);
            const float ty = y - ImGui::GetFontSize() * 0.5f;
            if (ty > gA.y && ty + ImGui::GetFontSize() < gB.y)
                dl->AddText(ImVec2(gA.x + S(2), ty), zero ? IM_COL32(255, 210, 120, 235) : IM_COL32(170, 175, 185, 200), lb);
        }
    }
    dl->PopClipRect();
}

void drawEditorBody(SoundEd& e, uint32_t clipRgb) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const long long F = e.frames();

    // ---- toolbar 1: view, spectrogram settings, channel, zoom, history, save
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("View");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(130));
    const char* views[] = {"Waveshape", "Spectrogram", "Both"};
    caw_ui::combo("##se_view", &e.view, views, 3);
    if (e.view != 0) {
        ImGui::SameLine();
        if (ImGui::Button("Spec settings")) ImGui::OpenPopup("##se_specset");
        if (ImGui::IsItemHovered()) helpTip("Scale, colors, algorithm: the spectrogram settings (like Audacity's Preferences > Spectrograms).");
    }
    drawSpecSettings(e);   // the popup (opens from the button above)
    // the timeline follow (the same modes the project transport uses; the F shortcut toggles on / off)
    ImGui::SameLine(0, S(14));
    {
        static const char* followNames[] = {"No follow", "Follow: page", "Follow: timeline"};
        ImGui::SetNextItemWidth(S(150));
        int fv = g_tlFollow;
        if (caw_ui::combo("##se_follow", &fv, followNames, 3)) g_tlFollow = fv;
        if (ImGui::IsItemHovered()) helpTip("Follow the project playhead while the timeline plays (Cubase's auto-scroll): \"page\" jumps a whole page each time the playhead leaves the view; \"timeline\" keeps the playhead at the middle of the view and the content moves under it, frame by frame. Shortcut: F.");
    }
    // the channel the volume-type edits (Gain / Fades / Silence / Normalize / Invert, the Y drag)
    // apply to: a small button shows the current choice, the popup lists the channels
    ImGui::SameLine(0, S(14));
    {
        char chLbl[24];
        if (e.editCh < 0) std::snprintf(chLbl, sizeof chLbl, "Ch: all");
        else if (e.ch == 2) std::snprintf(chLbl, sizeof chLbl, e.editCh == 0 ? "Ch: L" : "Ch: R");
        else std::snprintf(chLbl, sizeof chLbl, "Ch: %d", e.editCh);
        if (ImGui::Button(chLbl)) ImGui::OpenPopup("##se_chan_list");
    }
    if (ImGui::IsItemHovered()) helpTip("Choose the channel to edit and to analyze: the volume-type edits and the Y drag then touch only this channel (the spectrogram analyzes it alone). \"all\" = the mix.");
    if (ImGui::BeginPopup("##se_chan_list")) {
        if (ImGui::MenuItem("All channels", nullptr, e.editCh < 0)) e.editCh = -1;
        for (int c = 0; c < e.ch; ++c) {
            char nm[16];
            std::snprintf(nm, sizeof nm, e.ch == 2 ? (c == 0 ? "L (left)" : "R (right)") : "Channel %d", c);
            if (ImGui::MenuItem(nm, nullptr, e.editCh == c)) e.editCh = c;
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const float availW = std::max(100.f, ImGui::GetContentRegionAvail().x);
    (void)availW;
    float canvasW = std::max(100.f, ImGui::GetContentRegionAvail().x);   // the real canvas width is measured below; this keeps the zoom buttons sane
    if (ImGui::Button("Zoom in")) zoomAt(e, 0.5, e.viewStart + canvasW * 0.5 * e.spp, canvasW * 0.5f, canvasW);
    ImGui::SameLine();
    if (ImGui::Button("Zoom out")) zoomAt(e, 2.0, e.viewStart + canvasW * 0.5 * e.spp, canvasW * 0.5f, canvasW);
    ImGui::SameLine();
    if (ImGui::Button("Full")) { e.spp = maxSpp(e, canvasW); e.viewStart = 0.0; e.fitW = canvasW; }
    ImGui::SameLine(0, S(10));
    if (ImGui::Button("Y+")) e.ampZoom = std::min(32.f, e.ampZoom * 1.25f);
    if (ImGui::IsItemHovered()) helpTip("Vertical zoom: the wave gets taller.");
    ImGui::SameLine();
    if (ImGui::Button("Y-")) e.ampZoom = std::max(0.25f, e.ampZoom / 1.25f);
    if (ImGui::IsItemHovered()) helpTip("Vertical zoom: the wave gets flatter.");
    ImGui::SameLine(0, S(10));
    ImGui::BeginDisabled(!e.hasSel());
    if (ImGui::Button("To selection") && e.hasSel()) {
        e.spp = std::max(kMinSpp, (double)(e.selHi() - e.selLo()) / (double)canvasW);
        e.viewStart = (double)e.selLo();
        clampView(e, canvasW);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Sample level")) {   // down to single samples around the cursor
        const double mid = e.viewStart + canvasW * 0.5 * e.spp;
        const double c = e.hasSel() ? (double)e.selLo() : (e.cursor > 0 ? (double)e.cursor : mid);
        e.spp = 1.0 / 16.0;
        e.viewStart = c - canvasW * 0.5 * e.spp;
        clampView(e, canvasW);
    }
    ImGui::SameLine(0, S(18));
    ImGui::BeginDisabled(e.undo.empty());
    if (ImGui::Button("Undo")) doUndo(e, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(e.redo.empty());
    if (ImGui::Button("Redo")) doUndo(e, true);
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(18));
    ImGui::BeginDisabled(!e.dirty);
    if (ImGui::Button("Save")) saveEditor(e, false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        helpTip("Write the edited sound over its WAV file (32-bit float). Every clip that uses this file changes with it. Non-WAV files are saved as a new WAV.");
    ImGui::SameLine();
    if (ImGui::Button("Save copy...")) saveEditor(e, true);
    if (ImGui::IsItemHovered()) helpTip("Write the edited sound to a NEW WAV file; this clip then uses the copy and the original stays untouched.");

    // ---- toolbar 2: edits
    const bool sel = e.hasSel();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Cut")) opCopy(e, true);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) opCopy(e, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_sndClipboard.smp.empty());
    if (ImGui::Button("Paste")) opPaste(e);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Delete")) opDelete(e);
    ImGui::SameLine();
    if (ImGui::Button("Trim")) opTrim(e);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) helpTip("Keep only the selection.");
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(14));
    if (ImGui::Button("Silence")) opSilence(e);
    ImGui::SameLine();
    if (ImGui::Button("Fade in")) opFade(e, true);
    ImGui::SameLine();
    if (ImGui::Button("Fade out")) opFade(e, false);
    ImGui::SameLine();
    if (ImGui::Button("Reverse")) opReverse(e);
    ImGui::SameLine();
    if (ImGui::Button("Invert")) opInvert(e);
    ImGui::SameLine(0, S(14));
    ImGui::SetNextItemWidth(S(80));
    ImGui::InputFloat("##se_gain", &e.gainDb, 0.f, 0.f, "%.1f dB");
    e.gainDb = std::max(-60.f, std::min(30.f, e.gainDb));
    ImGui::SameLine();
    if (ImGui::Button("Gain")) opGain(e, e.gainDb);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(80));
    ImGui::InputFloat("##se_norm", &e.normDb, 0.f, 0.f, "%.1f dB");
    e.normDb = std::max(-60.f, std::min(0.f, e.normDb));
    ImGui::SameLine();
    if (ImGui::Button("Normalize")) opNormalize(e, e.normDb);
    if (ImGui::IsItemHovered()) helpTip("Scale the selection (or the whole file when nothing is selected) so its peak reaches the dB value on the left.");

    // ---- info line
    {
        const double sec = (double)F / (double)std::max(1, e.sr);
        char info[288];
        char chInfo[24];
        std::snprintf(chInfo, sizeof chInfo, e.editCh < 0 ? "all ch" : e.ch == 2 ? (e.editCh == 0 ? "L" : "R") : "ch%d", e.editCh);
        if (e.hasSel())
            std::snprintf(info, sizeof info, "Selection %s - %s  (%lld samples, %s)   |   editing %s   |   Length %s   %d Hz   %d ch   %s   |   zoom %.3g samples / pixel",
                fmtTime((double)e.selLo() / e.sr).c_str(), fmtTime((double)e.selHi() / e.sr).c_str(), e.selHi() - e.selLo(),
                fmtTime((double)(e.selHi() - e.selLo()) / e.sr).c_str(), chInfo, fmtTime(sec).c_str(), e.sr, e.ch, e.dirty ? "EDITED" : "saved", e.spp);
        else
            std::snprintf(info, sizeof info, "Cursor %s (sample %lld)   |   editing %s   |   Length %s   %d Hz   %d ch   %s   |   zoom %.3g samples / pixel",
                fmtTime((double)e.cursor / e.sr).c_str(), e.cursor, chInfo, fmtTime(sec).c_str(), e.sr, e.ch, e.dirty ? "EDITED" : "saved", e.spp);
        ImGui::TextDisabled("%s", info);
        if (!e.status.empty()) { ImGui::SameLine(0, S(16)); ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.f), "%s", e.status.c_str()); }
    }

    // ---- canvas geometry (the project timeline lives ON the canvas ruler below: no extra time bar)
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float rulerH = S(24), overviewH = S(46), gap = S(4);
    const float bodyH = std::max(S(120), avail.y - rulerH - overviewH - gap * 2.f);
    // the dB ruler gets its own gutter column left of the wave (the wave is never overlapped)
    const float dbGutter = (e.view != 1) ? S(46) : 0.f;
    const ImVec2 org0 = ImGui::GetCursorScreenPos();
    if (dbGutter > 0.f) ImGui::SetCursorScreenPos(ImVec2(org0.x + dbGutter, org0.y));
    const ImVec2 org(org0.x + dbGutter, org0.y);
    const float width = std::max(50.f, avail.x - dbGutter);
    if (e.spp <= 0.0) { e.spp = maxSpp(e, width); e.viewStart = 0.0; e.fitW = width; }
    // re-fit when the fit zoom's width is stale (the scrollbar appears / the window is resized):
    // otherwise the whole-file view keeps an old spp and no longer spans the file exactly
    if (e.fitW != width && std::fabs(e.spp - maxSpp(e, e.fitW)) <= maxSpp(e, e.fitW) * 1e-9) { e.spp = maxSpp(e, width); e.viewStart = 0.0; e.fitW = width; }
    // follow the project playhead in the editor view too (the same Cubase modes as the project)
    if (g_timelineRun && g_tlFollow > 0) {
        const double ps = sndProjToSrc(e, g_play);
        if (ps >= 0.0) {
            const double psFrame = ps * (double)e.sr;
            const double viewF = (double)width * e.spp;
            if (g_tlFollow == 1) {   // page
                if (psFrame >= e.viewStart + viewF) e.viewStart = psFrame;
                else if (psFrame < e.viewStart) e.viewStart = std::max(0.0, psFrame - viewF * 0.05);
            } else {                 // move by timeline: the playhead IS the middle of the view, frame by frame
                e.viewStart = std::max(0.0, psFrame - viewF * 0.5);
            }
        }
    }
    clampView(e, width);
    if (e.pyrRev != e.revision) buildPyramid(e);

    const ImVec2 rA(org.x, org.y), rB(org.x + width, org.y + rulerH);
    const ImVec2 cA(org.x, org.y + rulerH), cB(org.x + width, org.y + rulerH + bodyH);

    // the canvas input - ONE item from the top of the ruler down to the bottom of the body: the
    // ruler row is clickable (it moves the project playhead), the body selects / pans
    ImGui::SetCursorScreenPos(rA);
    ImGui::InvisibleButton("##se_canvas", ImVec2(width, rulerH + bodyH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    auto frameAt = [&](float x) { return std::max(0LL, std::min(F, (long long)std::llround(e.viewStart + (double)(x - cA.x) * e.spp))); };
    // the selection's volume handle (a +- grip at the top of the selection): drag it up / down to
    // scale the selected samples live. Zoomed in deep this is literally moving the bits in Y.
    ImVec2 handleA(0, 0), handleB(0, 0);
    bool handleVisible = false, overHandle = false;
    if (e.hasSel()) {
        const float sx0 = cA.x + (float)(((double)e.selLo() - e.viewStart) / e.spp);
        const float sx1 = cA.x + (float)(((double)e.selHi() - e.viewStart) / e.spp);
        if (sx1 > cA.x && sx0 < cB.x) {
            const float cxm = std::max(cA.x + S(10), std::min(cB.x - S(10), 0.5f * (std::max(sx0, cA.x) + std::min(sx1, cB.x))));
            handleA = ImVec2(cxm - S(9), cA.y + S(3));
            handleB = ImVec2(cxm + S(9), cA.y + S(19));
            handleVisible = true;
            overHandle = hovered && io.MousePos.x >= handleA.x && io.MousePos.x <= handleB.x && io.MousePos.y >= handleA.y && io.MousePos.y <= handleB.y;
        }
    }
    if (overHandle && ImGui::IsMouseClicked(0) && !e.vol.active) {
        e.vol.active = true;
        e.vol.smp = e.smp;   // the pre-drag samples: live preview + one undo step on release
        e.vol.y0 = io.MousePos.y;
        e.vol.ch = e.editCh;
    }
    if (e.vol.active) {
        if (ImGui::IsMouseDown(0)) {
            const float dy = e.vol.y0 - io.MousePos.y;   // up = louder
            const float g = std::exp(dy * 0.0115f);      // ~12 dB per 100 px
            const long long a = std::max(0LL, e.selLo()), b = std::min(F, e.selHi());
            for (long long f = a; f < b; ++f)
                for (int c = e.vol.ch >= 0 ? e.vol.ch : 0; c < (e.vol.ch >= 0 ? e.vol.ch + 1 : e.ch); ++c)
                    e.smp[(size_t)f * (size_t)e.ch + (size_t)c] = e.vol.smp[(size_t)f * (size_t)e.ch + (size_t)c] * g;
            char dbTxt[32];
            std::snprintf(dbTxt, sizeof dbTxt, "%+.1f dB", 20.f * std::log10(std::max(1e-4f, g)));
            dl->AddText(ImVec2(handleB.x + S(6), handleA.y), IM_COL32(255, 230, 120, 255), dbTxt);
        } else {   // release: the pre-drag state becomes ONE undo step
            SoundEd::Snap s;
            s.smp = std::move(e.vol.smp);
            s.selA = e.selA; s.selB = e.selB; s.cursor = e.cursor;
            e.undo.push_back(std::move(s));
            e.redo.clear();
            e.vol.active = false;
            e.vol.smp.clear();
            changed(e);
        }
    }
    if (hovered && io.MouseWheel != 0.f) {
        if (e.view == 1) {
            // spectrogram view: a plain mouse wheel must NOT zoom (the user's touchpad two-finger
            // pinch arrives as Ctrl+wheel - that is the zoom gesture here, or Ctrl+mouse scroll).
            // A plain wheel just pans the view.
            if (io.KeyCtrl) {
                const double at = e.viewStart + (double)(io.MousePos.x - cA.x) * e.spp;
                zoomAt(e, std::pow(0.8, (double)io.MouseWheel), at, io.MousePos.x - cA.x, width);
            } else {
                e.viewStart -= (double)io.MouseWheel * width * 0.15 * e.spp;
                clampView(e, width);
            }
        } else if (io.KeyShift) {
            e.viewStart -= (double)io.MouseWheel * width * 0.15 * e.spp;
            clampView(e, width);
        } else {
            // EVERY wheel = horizontal (time) zoom at the mouse. A touchpad pinch arrives as
            // Ctrl+wheel, so the old Ctrl = vertical zoom hijacked the two-finger zoom: Ctrl
            // now zooms in time like a plain wheel (vertical zoom lives on the Y+ / Y- buttons).
            const double at = e.viewStart + (double)(io.MousePos.x - cA.x) * e.spp;
            zoomAt(e, std::pow(0.8, (double)io.MouseWheel), at, io.MousePos.x - cA.x, width);
        }
    }
    const bool overRuler = io.MousePos.y <= cA.y + 1.f;   // the ruler row: the project playhead lives there
    if (hovered && overRuler && ImGui::IsMouseClicked(1)) {   // right-click on the ruler: set the loop, like the project's ruler
        const double proj = sndSrcToProj(e, (double)frameAt(io.MousePos.x) / (double)e.sr);
        if (proj >= 0.0) {
            if (!g_loopSelecting) {
                g_loopAnchor = proj;
                g_loopSelecting = true;
                g_proj.loopStart = g_proj.loopEnd = proj;
                g_proj.loopEnabled = false;
            } else if (std::fabs(proj - g_loopAnchor) >= 0.01) {
                g_proj.loopStart = std::min(g_loopAnchor, proj);
                g_proj.loopEnd = std::max(g_loopAnchor, proj);
                g_proj.loopEnabled = true;
                g_loopSelecting = false;
            }
            if (timelinePlaying()) g_resync = 2;
            if (timelinePlaying() && g_pdTimelineMode) pdTransport(true);
        }
    }
    if (hovered && overRuler) helpTip(g_loopSelecting ? "Right-click again to set the other loop boundary." :
        "Click = move the project playhead here (no playback). Right-click once = loop start, right-click again = loop end.");
    if (hovered && ImGui::IsMouseDoubleClicked(0) && !overRuler) { e.selA = 0; e.selB = F; e.cursor = 0; }
    else if (!e.vol.active && ImGui::IsItemActivated() && io.MouseDown[0] && !overHandle) {
        if (overRuler) {   // the ruler maps into project time: one click puts the timeline line there (no playback)
            e.rulerScrub = true;
            const double proj = sndSrcToProj(e, (double)frameAt(io.MousePos.x) / (double)e.sr);
            if (proj >= 0.0) seekTimeline(proj);
        } else {
            const long long f = frameAt(io.MousePos.x);
            if (io.KeyShift && (e.hasSel() || e.cursor != f)) { e.anchor = e.hasSel() ? e.selA : e.cursor; e.selA = e.anchor; e.selB = f; }
            else { e.anchor = f; e.selA = e.selB = f; }
            e.cursor = f;
        }
    }
    if (e.rulerScrub) {
        // the ruler scrub owns the mouse: keep mapping to project time while the button is held
        if (active && io.MouseDown[0]) {
            const double proj = sndSrcToProj(e, (double)frameAt(io.MousePos.x) / (double)e.sr);
            if (proj >= 0.0) seekTimeline(proj);
        } else e.rulerScrub = false;
    } else if (e.vol.active) {
        // the volume drag owns the mouse: no edge scroll, no selection change while it runs
    } else if (active && io.MouseDown[2]) {   // middle button: pan
        e.viewStart -= (double)io.MouseDelta.x * e.spp;
        clampView(e, width);
    } else if (active && io.MouseDown[0]) {
        // dragging past the edge scrolls the view
        if (io.MousePos.x < cA.x) { e.viewStart -= (double)(cA.x - io.MousePos.x) * 0.15 * e.spp; clampView(e, width); }
        else if (io.MousePos.x > cB.x) { e.viewStart += (double)(io.MousePos.x - cB.x) * 0.15 * e.spp; clampView(e, width); }
        const long long f = frameAt(io.MousePos.x);
        e.selA = e.anchor; e.selB = f;
        e.cursor = f;
    }

    // ---- paint: ruler, lanes, selection, cursor
    drawRuler(e, dl, rA, rB);
    dl->PushClipRect(cA, cB, true);
    {
        const bool both = e.view == 2;
        const float waveH = e.view == 0 ? bodyH : (both ? bodyH * 0.58f : 0.f);
        if (e.view != 1) {
            const ImVec2 gA(org0.x, cA.y), gB(org0.x + dbGutter - 1.f, cB.y);
            if (e.editCh >= 0) {   // one channel chosen: it gets the whole wave area
                const float y1 = e.view == 2 ? cA.y + waveH : cB.y;
                drawWaveLane(e, dl, e.editCh, ImVec2(cA.x, cA.y), ImVec2(cB.x, y1 - 1.f), clipRgb, width);
                drawDbRuler(e, dl, ImVec2(cA.x, cA.y), ImVec2(cB.x, y1 - 1.f), gA, ImVec2(gB.x, y1 - 1.f));
            } else {
                const float laneH = waveH / (float)std::max(1, e.ch);
                for (int c = 0; c < e.ch; ++c)
                    drawWaveLane(e, dl, c, ImVec2(cA.x, cA.y + laneH * (float)c), ImVec2(cB.x, cA.y + laneH * (float)(c + 1) - 1.f), clipRgb, width);
                for (int c = 0; c < e.ch; ++c)
                    drawDbRuler(e, dl, ImVec2(cA.x, cA.y + laneH * (float)c), ImVec2(cB.x, cA.y + laneH * (float)(c + 1) - 1.f),
                                ImVec2(gA.x, cA.y + laneH * (float)c), ImVec2(gB.x, cA.y + laneH * (float)(c + 1) - 1.f));
            }
        }
        if (e.view != 0) drawSpectro(e, dl, ImVec2(cA.x, cA.y + waveH), cB, width);
    }
    if (e.hasSel()) {
        const float sx0 = cA.x + (float)(((double)e.selLo() - e.viewStart) / e.spp);
        const float sx1 = cA.x + (float)(((double)e.selHi() - e.viewStart) / e.spp);
        dl->AddRectFilled(ImVec2(sx0, cA.y), ImVec2(sx1, cB.y), IM_COL32(255, 255, 255, 44));
        dl->AddLine(ImVec2(sx0, cA.y), ImVec2(sx0, cB.y), IM_COL32(255, 255, 255, 170));
        dl->AddLine(ImVec2(sx1, cA.y), ImVec2(sx1, cB.y), IM_COL32(255, 255, 255, 170));
        if (handleVisible) {   // the +- grip: drag it up / down = the selection's volume
            const ImU32 hc = e.vol.active ? IM_COL32(255, 220, 90, 255) : (overHandle ? IM_COL32(255, 255, 255, 240) : IM_COL32(255, 255, 255, 170));
            dl->AddRectFilled(handleA, handleB, IM_COL32(24, 24, 30, 230), S(3));
            dl->AddRect(handleA, handleB, hc, S(3));
            const float gy = 0.5f * (handleA.y + handleB.y);
            dl->AddLine(ImVec2(handleA.x + S(3), gy - S(3)), ImVec2(handleB.x - S(3), gy - S(3)), hc, 1.4f);
            dl->AddLine(ImVec2(handleA.x + S(3), gy), ImVec2(handleB.x - S(3), gy), hc, 1.4f);
            dl->AddLine(ImVec2(handleA.x + S(3), gy + S(3)), ImVec2(handleB.x - S(3), gy + S(3)), hc, 1.4f);
        }
    } else {
        const float cx = cA.x + (float)(((double)e.cursor - e.viewStart) / e.spp);
        dl->AddLine(ImVec2(cx, cA.y), ImVec2(cx, cB.y), IM_COL32(255, 220, 90, 230), 1.5f);
    }
    drawRulerProject(e, dl, rA, rB, cB.y);   // the project on top of everything: clip bands + loop on the ruler, the playhead across the whole shape
    if (hovered) {   // a hair line under the mouse and its position
        const float mx = io.MousePos.x;
        dl->AddLine(ImVec2(mx, cA.y), ImVec2(mx, cB.y), IM_COL32(255, 255, 255, 45));
        const long long f = frameAt(mx);
        char tip[96];
        std::snprintf(tip, sizeof tip, "%s   sample %lld", fmtTime((double)f / e.sr).c_str(), f);
        dl->AddText(ImVec2(std::min(mx + S(8), cB.x - S(180)), cA.y + S(4)), IM_COL32(255, 255, 255, 200), tip);
    }
    dl->PopClipRect();
    dl->AddRect(cA, cB, IM_COL32(12, 12, 14, 255));

    // ---- overview strip: the whole file, the view as a window on it (click / drag to move)
    const ImVec2 oA(org.x, cB.y + gap), oB(org.x + width, cB.y + gap + overviewH);
    ImGui::SetCursorScreenPos(oA);
    ImGui::InvisibleButton("##se_overview", ImVec2(width, overviewH));
    if (ImGui::IsItemActive() && io.MouseDown[0]) {
        const double center = (double)(io.MousePos.x - oA.x) / (double)width * (double)F;
        e.viewStart = center - (double)width * e.spp * 0.5;
        clampView(e, width);
    }
    dl->AddRectFilled(oA, oB, IM_COL32(22, 22, 26, 255));
    {
        const float mid = 0.5f * (oA.y + oB.y), amp = 0.5f * overviewH * 0.9f;
        const int px = (int)width;
        const int cr = (int)((clipRgb >> 16) & 0xFF), cg = (int)((clipRgb >> 8) & 0xFF), cb = (int)(clipRgb & 0xFF);
        for (int x = 0; x < px; ++x) {
            const double f0 = (double)x / (double)px * (double)F, f1 = (double)(x + 1) / (double)px * (double)F;
            float lo = 1e9f, hi = -1e9f;
            for (int c = 0; c < e.ch; ++c) { float l, h; columnRange(e, c, f0, f1, l, h); lo = std::min(lo, l); hi = std::max(hi, h); }
            if (lo > hi) lo = hi = 0.f;
            float yH = mid - hi * amp, yL = mid - lo * amp;
            if (yL - yH < 1.f) { yH = mid - 0.5f; yL = mid + 0.5f; }
            dl->AddRectFilled(ImVec2(oA.x + (float)x, yH), ImVec2(oA.x + (float)x + 1.f, yL), IM_COL32(cr, cg, cb, 200));
        }
        const float vx0 = oA.x + (float)(e.viewStart / (double)std::max(1LL, F)) * width;
        const float vx1 = oA.x + (float)((e.viewStart + (double)width * e.spp) / (double)std::max(1LL, F)) * width;
        dl->AddRectFilled(ImVec2(vx0, oA.y), ImVec2(std::max(vx1, vx0 + 2.f), oB.y), IM_COL32(255, 255, 255, 36));
        dl->AddRect(ImVec2(vx0, oA.y), ImVec2(std::max(vx1, vx0 + 2.f), oB.y), IM_COL32(255, 255, 255, 190));
        if (e.hasSel()) {
            const float sx0 = oA.x + (float)((double)e.selLo() / (double)std::max(1LL, F)) * width;
            const float sx1 = oA.x + (float)((double)e.selHi() / (double)std::max(1LL, F)) * width;
            dl->AddRectFilled(ImVec2(sx0, oA.y), ImVec2(std::max(sx1, sx0 + 1.f), oB.y), IM_COL32(255, 220, 90, 70));
        }
    }
    dl->AddRect(oA, oB, IM_COL32(12, 12, 14, 255));
    ImGui::SetCursorScreenPos(ImVec2(org.x, oB.y));
    ImGui::Dummy(ImVec2(width, 1.f));
    ImGui::TextDisabled("Wheel / pinch = time zoom at the mouse   Shift+wheel = scroll   Y+ / Y- = vertical zoom   middle drag = pan   drag = select   double-click = select all   drag the +- handle on the selection = volume   spectrogram view: wheel = pan, pinch / Ctrl+wheel = zoom   ruler click = move the project playhead");
}

}  // namespace

// ------------------------------------------------------------------ public
bool soundEditorFocused() { return g_sndFocusSeen; }   // a sound editor window had the keyboard focus (the app skips the project undo then)

void openSoundEditor(int clipId, int view) {
    for (auto& up : g_sndEds)
        if (up->clipId == clipId) { up->view = std::max(0, std::min(2, view)); up->open = true; up->focusNext = true; return; }
    Clip* c = findClip(clipId);
    if (!c || c->audioFile.empty()) return;
    std::string err;
    const cawaudio::AudioHandle data = audioClipData(*c, err);
    if (!data || data->samples.empty()) { logLine("[caw] Sound editor: could not read " + c->audioFile + (err.empty() ? std::string() : ": " + err)); return; }
    auto e = std::make_unique<SoundEd>();
    e->clipId = clipId;
    e->view = std::max(0, std::min(2, view));
    e->smp = data->samples;
    e->ch = std::max(1, data->channels);
    e->sr = std::max(8000, data->sr);
    e->path = c->audioFile;
    e->focusNext = true;
    e->cursor = e->selA = e->selB = 0;
    g_sndEds.push_back(std::move(e));
}

void drawSoundEditors() {
    g_sndFocusSeen = false;
    for (size_t i = 0; i < g_sndEds.size();) {
        SoundEd& e = *g_sndEds[i];
        int laneIndex = -1;
        Clip* clip = findClip(e.clipId, &laneIndex);
        if (!clip || !e.open) {
            if (clip && e.dirty && !e.open && g_sndDiscardId < 0) { e.open = true; e.askDiscard = true; g_sndDiscardId = e.clipId; }
            else if (clip && e.dirty && !e.open) e.open = true;   // another question is open: stay open   // closing with unsaved edits: ask first
            else { g_sndEds.erase(g_sndEds.begin() + (std::ptrdiff_t)i); continue; }
        }
        // the clip's file changed outside the editor (another save): the editor keeps its own copy
        const std::string title = "Sound Editor - " + clip->name + (e.dirty ? " *" : "") + "###sound_editor_" + std::to_string(e.clipId);
        ImGui::SetNextWindowSize(ImVec2(S(1180), S(660)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(S(160), S(90)), ImGuiCond_FirstUseEver);
        if (e.focusNext) { ImGui::SetNextWindowFocus(); e.focusNext = false; }
        winMaxPrepare(900000 + e.clipId);
        bool open = true;
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
            winMaxTitleButton(900000 + e.clipId);
            const uint32_t clipRgb = clipColorValue(g_proj.tracks[(size_t)laneIndex], *clip);
            // keyboard: only while this window has the focus and no text box is being edited
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) g_sndFocusSeen = true;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
                if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) doUndo(e, false);
                if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) doUndo(e, true);   // the same redo shortcut the project uses
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doUndo(e, true);
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) opCopy(e, false);
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) opCopy(e, true);
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) opPaste(e);
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && e.dirty) saveEditor(e, false);
                if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) { e.selA = 0; e.selB = e.frames(); }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete)) opDelete(e);
            }
            drawEditorBody(e, clipRgb);
        }
        ImGui::End();
        if (!open) e.open = false;
        if (e.askDiscard) {
            ImGui::OpenPopup("Discard the edits?###se_discard");
            e.askDiscard = false;
            e.open = true;
        }
        ++i;
    }
    // the discard dialog (one at a time: it concerns the editor that asked)
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Discard the edits?###se_discard", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This sound has edits that are not saved.");
        if (ImGui::Button("Discard and close")) {
            for (auto it = g_sndEds.begin(); it != g_sndEds.end(); ++it)
                if ((*it)->clipId == g_sndDiscardId) { g_sndEds.erase(it); break; }
            g_sndDiscardId = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing")) { g_sndDiscardId = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    } else if (g_sndDiscardId >= 0 && !ImGui::IsPopupOpen("Discard the edits?###se_discard")) {
        g_sndDiscardId = -1;   // the popup was dismissed some other way
    }
}
