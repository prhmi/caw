#include "app_shared.h"

// ---- engine identity of a track: one color + a short tag per track type, so the type is readable at a glance
static uint32_t engineRgb(int kind) {
    switch (kind) {
        case 0: return 0xEB8C3Cu;   // Csound            - orange
        case 1: return 0x8C96AAu;   // marker            - gray
        case 2: return 0x5A96F0u;   // graph             - blue
        case 3: return 0x9A6EE6u;   // VST instrument    - violet
        case 4: return 0x3CBEC8u;   // VST effect        - cyan
        case 5: return 0x5AC86Eu;   // SuperCollider     - green
        case 6: return 0xE15A6Eu;   // Pure Data         - red
        case 7: return 0xE6C846u;   // sample            - yellow
        default: return 0x8C96AAu;
    }
}
static const char* engineTag(int kind) {
    switch (kind) {
        case 0: return "CSD";
        case 1: return "MRK";
        case 2: return "GRF";
        case 3: return "VSTi";
        case 4: return "VSTe";
        case 5: return "SC";
        case 6: return "PD";
        case 7: return "SMP";
        default: return "?";
    }
}
static const char* engineName(int kind) {
    switch (kind) {
        case 0: return "Csound track";
        case 1: return "Marker track";
        case 2: return "Graph track";
        case 3: return "VST instrument (VSTi) track";
        case 4: return "VST effect (VSTe) track";
        case 5: return "SuperCollider track";
        case 6: return "Pure Data track";
        case 7: return "Sample (audio file) track";
        default: return "Track";
    }
}

// src/ui_timeline.cpp (phase 2: extracted from app.cpp; the shared state is app_state.h, the
//  internal declarations are app_internal.h - all included through app_shared.h.)

ImU32 trackColor(int i, int alpha, uint32_t custom )
{
    if (custom != 0)
        return IM_COL32((custom >> 16) & 0xFF, (custom >> 8) & 0xFF, custom & 0xFF, alpha);
    const uint32_t rgb = defaultTrackRgb(i);
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}
#include "app.h"
float automationHeight(const Track& track, const std::string& channel) {
    const auto it = track.automationHeights.find(channel);
    const float h = it == track.automationHeights.end() ? kAutomationLaneH : it->second;
    return track.kind == 2 ? std::max(h, 68.f) : h;   // a graph line has a second row: shape and lane
}
#include "app.h"
TrackFolder* findFolder(int id) {
    if (id == 0) return nullptr;
    for (TrackFolder& f : g_proj.folders) if (f.id == id) return &f;
    return nullptr;
}
#include "app.h"
void pruneFolders() {   // folders without tracks disappear
    for (size_t k = g_proj.folders.size(); k-- > 0;) {
        bool used = false;
        for (const Track& t : g_proj.tracks) if (t.folderId == g_proj.folders[k].id) used = true;
        if (!used) g_proj.folders.erase(g_proj.folders.begin() + (long)k);
    }
}
#include "app.h"
int folderMemberCount(int id) { int n = 0; for (const Track& t : g_proj.tracks) if (t.folderId == id) ++n; return n; }
#include "app.h"
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
#include "app.h"
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
#include "app.h"
bool autoShown(const Track& track, const std::string& channel) { return track.hiddenAutomation.count(channel) == 0 && !trackHiddenByFolder(track); }
#include "app.h"
bool midiShown(const Track& track) { return !track.midiHidden && !trackHiddenByFolder(track) && trackHasMidi(track); }
#include "app.h"
float automationHeightSum(const Track& track) {
    float total = 0.f;
    for (const auto& [channel, points] : track.automation) if (autoShown(track, channel)) total += automationHeight(track, channel);
    return total;
}
#include "app.h"
float automationLaneTop(const Track& track, float baseY, const std::string& channel) {
    float y = baseY;
    for (const auto& [name, points] : track.automation) {
        if (name == channel) break;
        if (autoShown(track, name)) y += S(automationHeight(track, name));
    }
    return y;
}
#include "app.h"
float midiLaneTop(const Track& track, float baseY) {
    return baseY + S(automationHeightSum(track));
}
#include "app.h"
uint32_t automationColor(const Track& track, const std::string& channel, int laneIndex) {
    const auto found = track.automationColors.find(channel);
    if (found != track.automationColors.end() && found->second != 0) return found->second;
    static const uint32_t defaults[] = {0x01F2A65Au, 0x015BC0DEu, 0x01B18BF0u, 0x0177C77Du, 0x01E66B87u, 0x01D6C855u};
    return defaults[(size_t)std::max(0, laneIndex) % (sizeof(defaults) / sizeof(defaults[0]))];
}
#include "app.h"
double clampTimelineTime(double t) {
    return std::max(0.0, t); // free timing, no hidden snapping
}
#include "app.h"
void renameClipGroup(const Clip& clip, const std::string& oldName) {
    if (oldName == clip.name) return;
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips)
            if (&c != &clip && c.name == oldName && c.code == clip.code) c.name = clip.name;
}
#include "app.h"
std::string nextClipName(const std::string& laneName) {
    std::string base = laneName;
    while (!base.empty() && base.back() >= '0' && base.back() <= '9') base.pop_back();
    fixName(base);
    for (int k = 1;; ++k) {
        const std::string candidate = base + std::to_string(k);
        if (!clipNameUsed(candidate, nullptr)) return candidate;
    }
}
#include "app.h"
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
#include "app.h"
int newMidiId() {
    int top = 0;
    for (const Track& t : g_proj.tracks) for (const MidiClip& c : t.midiClips) top = std::max(top, c.id);
    return top + 1;
}
#include "app.h"
void removeMidiClipsOf(int clipId) {
    for (Track& t : g_proj.tracks)
        t.midiClips.erase(std::remove_if(t.midiClips.begin(), t.midiClips.end(),
                                         [clipId](const MidiClip& c) { return c.clipId == clipId; }), t.midiClips.end());
}
#include "app.h"
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
#include "app.h"
void ensureMidiClipFor(int laneIndex, const Clip& clip) {
    if (laneIndex < 0 || laneIndex >= (int)g_proj.tracks.size()) return;
    if (midi_clip_ops::ensureForClip(g_proj, (size_t)laneIndex, clip.id))
        g_collapsedAuto.erase(g_proj.tracks[(size_t)laneIndex].id);
}
#include "app.h"
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
#include "app.h"
// TRACK FREEZE helper: is this clip's lane frozen (the clip cannot be edited or deleted)?
inline bool clipFrozenById(int clipId) {
    for (const Track& t : g_proj.tracks) {
        if (!trackFrozen(t)) continue;
        for (const Clip& c : t.clips) if (c.id == clipId) return true;
    }
    return false;
}

void duplicateSelectedClips() {
    std::vector<std::pair<int, Clip>> made;
    double lo = 1e300, hi = 0.0;
    for (int ti = 0; ti < (int)g_proj.tracks.size(); ++ti)
        for (const Clip& c : g_proj.tracks[(size_t)ti].clips)
            if (g_selClips.count(c.id)) {
                if (trackFrozen(g_proj.tracks[(size_t)ti])) continue;   // frozen: the lane is locked, its clips are not copied
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
#include "app.h"
void deleteSelectedMidiClips() {
    for (Track& t : g_proj.tracks)
        t.midiClips.erase(std::remove_if(t.midiClips.begin(), t.midiClips.end(),
                                         [](const MidiClip& c) { return g_selMidiClips.count(c.id) > 0; }), t.midiClips.end());
    g_selMidiClips.clear();
}
#include "app.h"
void deleteSelectedClips() {
    for (const int id : g_selClips) {
        if (clipFrozenById(id)) continue;   // frozen: the lane is locked, its clips cannot be deleted
        audioKillClipVoices(id); removeMidiClipsOf(id);   // audio: the voice stops with the clip
    }
    for (Track& t : g_proj.tracks)
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                     [](const Clip& c) { return g_selClips.count(c.id) > 0 && !clipFrozenById(c.id); }), t.clips.end());
    g_selClips.clear();
    g_selClip = -1;
}
#include "app.h"
void addClip(int laneIndex, double start, double dur, const std::string& code, const std::string& name )
{
    Track& lane = g_proj.tracks[(size_t)laneIndex];
    if (!hasClips(lane)) return;   // graph and marker tracks take no clips
    if (trackFrozen(lane)) { logLine("[caw] The track \"" + lane.name + "\" is frozen - unfreeze it to edit it."); return; }   // frozen: the lane is locked
    Clip c = makeClip(lane, start, dur, code);
    if (!name.empty()) { c.name = name; ensureUniqueClipName(c); }
    lane.clips.push_back(c);
}
#include "app.h"
void clearAutoSelection() {
    g_autoSel.clear(); g_autoSelMulti.clear();
    g_autoSelTrackId = -1;
    g_autoSelChannel.clear();
}
#include "app.h"
std::vector<AutoPoint>* selectedAutoLane() {
    Track* t = findTrackById(g_autoSelTrackId);
    if (!t) return nullptr;
    auto it = t->automation.find(g_autoSelChannel);
    return it == t->automation.end() ? nullptr : &it->second;
}
#include "app.h"
void validateAutoSelection() {
    std::vector<AutoPoint>* lane = selectedAutoLane();
    if (!lane) { clearAutoSelection(); return; }
    for (auto it = g_autoSel.begin(); it != g_autoSel.end();)
        it = (*it < 0 || *it >= (int)lane->size()) ? g_autoSel.erase(it) : std::next(it);
}
#include "app.h"
void insertAutoPointSorted(std::vector<AutoPoint>& points, const AutoPoint& point) {
    const auto at = std::upper_bound(points.begin(), points.end(), point.t,
                                     [](double t, const AutoPoint& p) { return t < p.t; });
    points.insert(at, point);
}
#include "app.h"
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
#include "app.h"
void ensureAutoLane(Track& t, const std::string& channel) {
    if (t.automation.find(channel) == t.automation.end()) {
        t.automation.emplace(channel, std::vector<AutoPoint>{});
        t.automationHeights[channel] = kAutomationLaneH;
    }
    t.hiddenAutomation.erase(channel);
    g_collapsedAuto.erase(t.id);
}
#include "app.h"
void replaceDotsInRange(std::vector<AutoPoint>& lane, double t0, double t1, const std::vector<AutoPoint>& fresh) {
    lane.erase(std::remove_if(lane.begin(), lane.end(), [&](const AutoPoint& p) { return p.t >= t0 - 1e-9 && p.t <= t1 + 1e-9; }), lane.end());
    lane.insert(lane.end(), fresh.begin(), fresh.end());
    std::stable_sort(lane.begin(), lane.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
}
#include "app.h"
void selectDotsInRange(const Track& t, const std::string& channel, double t0, double t1) {
    const auto it = t.automation.find(channel);
    if (it == t.automation.end()) return;
    g_autoSel.clear(); g_autoSelMulti.clear();
    g_autoSelTrackId = t.id; g_autoSelChannel = channel;
    for (int i = 0; i < (int)it->second.size(); ++i)
        if (it->second[(size_t)i].t >= t0 - 1e-9 && it->second[(size_t)i].t <= t1 + 1e-9) g_autoSel.insert(i);
}
#include "app.h"
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
#include "app.h"
void thinToLimit(std::vector<AutoPoint>& s, double eps, size_t maxPoints) {
    std::vector<AutoPoint> work = s;
    thinSamples(work, eps);
    for (int guard = 0; work.size() > maxPoints && guard < 30; ++guard) { eps = std::max(eps * 1.6, 1e-9); work = s; thinSamples(work, eps); }
    s.swap(work);
}
#include "app.h"
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
#include "app.h"
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
#include "app.h"
void reshapeDots(std::vector<AutoPoint>& lane, const std::vector<int>& idx, const std::vector<AutoPoint>& orig,
                 double dStart, double dEnd, double stretch, double morph, double lo, double hi,
                 double anchorT, double anchorSpan) {
    if (orig.empty() || orig.size() != idx.size()) return;
    const double kPi = 3.14159265358979323846;
    double vMin = orig[0].v, vMax = orig[0].v;
    for (const AutoPoint& q : orig) { vMin = std::min(vMin, q.v); vMax = std::max(vMax, q.v); }
    const double center = (vMin + vMax) * 0.5, half = (vMax - vMin) * 0.5;
    // anchorT / anchorSpan >= 0: several lanes are reshaped together - they share one time origin and one
    // depth ramp, so the lines stay aligned with each other. Otherwise the lane's own first / last dot.
    const double t0 = anchorT >= 0.0 ? anchorT : orig.front().t;
    const double span = anchorSpan >= 0.0 ? anchorSpan : orig.back().t - t0;
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
#include "app.h"
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
#include "app.h"
bool hasAutoSelection() {
    if (!g_autoSel.empty()) return true;
    for (const auto& kv : g_autoSelMulti) if (!kv.second.empty()) return true;
    return false;
}
#include "app.h"
// Every selected dot in every lane: the primary lane (g_autoSel) and the other lanes (g_autoSelMulti).
std::map<std::pair<int, std::string>, std::set<int>> allSelectedDots() {
    std::map<std::pair<int, std::string>, std::set<int>> all;
    for (const auto& [key, idx] : g_autoSelMulti) if (!idx.empty()) all[key].insert(idx.begin(), idx.end());
    if (!g_autoSel.empty() && g_autoSelTrackId >= 0) all[{g_autoSelTrackId, g_autoSelChannel}].insert(g_autoSel.begin(), g_autoSel.end());
    return all;
}
#include "app.h"
// Put a whole dot selection back: the given primary lane (if it has dots) becomes g_autoSel, the rest g_autoSelMulti.
void setAllSelectedDots(const std::map<std::pair<int, std::string>, std::set<int>>& all, int primaryTrack, const std::string& primaryChannel) {
    g_autoSel.clear(); g_autoSelMulti.clear();
    auto primary = all.find({primaryTrack, primaryChannel});
    if (primary == all.end() && !all.empty()) primary = all.begin();
    if (primary != all.end()) { g_autoSelTrackId = primary->first.first; g_autoSelChannel = primary->first.second; g_autoSel = primary->second; }
    for (const auto& [key, idx] : all) if (primary == all.end() || key != primary->first) if (!idx.empty()) g_autoSelMulti[key] = idx;
}
#include "app.h"
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
#include "app.h"
std::string timeText(double t) {
    const int m = (int)(t / 60.0);
    char b[64];
    std::snprintf(b, sizeof b, "%.2f s (%d:%04.1f)", t, m, t - m * 60.0);
    return b;
}
#include "app.h"
void toggleHideAllAutomation();
#include "app.h"
void showAllAutomation();
#include "app.h"
void toggleHideAllMidi();
#include "app.h"
void showAllMidi();
#include "app.h"
void drawLaneName(const char* id, std::string& name, const std::string& fallback) {
    ImGui::SetNextItemWidth(S(220));
    inputStr(id, name, 0);
    ImGui::SameLine();
    if (name.empty()) ImGui::TextDisabled("empty = %s", fallback.c_str());
    else ImGui::TextDisabled("name in the score");
}
#include "app.h"
float fitWidth(const char* sample, float minW )
{
    return std::max(minW, ImGui::CalcTextSize(sample).x + ImGui::GetStyle().FramePadding.x * 2.f + S(10));
}
#include "app.h"
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
#include "app.h"
void drawMidiScoreMenu(Track& t) {
    ImGui::TextDisabled("MIDI lane in the score");
    drawLaneName("##midi_name", t.scoreMidiLabel, t.scoreLabel.empty() ? t.name : t.scoreLabel);
    comboFit(S(220), "##midi_show", &t.scoreMidiShow,
                 "Every note\0Overall contour\0Not in the score\0Pitch range band\0Every note with its name\0Fast-note clusters\0");
    if (ImGui::IsItemHovered()) helpTip("Fast-note clusters group tightly spaced notes into compact alternative notehead clusters, with a count.");
}
#include "app.h"
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
#include "app.h"
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
#include "app.h"
void drawClipScoreMenu(Clip& c, Track* lane )
{
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
#include "app.h"
void drawScoreEditor() {
    if (!g_scoreOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(680), S(720)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(200), S(70)), ImGuiCond_FirstUseEver);
    winMaxPrepareT("Score");
    if (ImGui::Begin("Score", &g_scoreOpen, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Score");
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
#include "app.h"
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
#include "app.h"
void cutAudioClipAt(int trackIndex, int clipIndex, double t) {
    if (trackIndex < 0 || trackIndex >= (int)g_proj.tracks.size()) return;
    Track& lane = g_proj.tracks[(size_t)trackIndex];
    if (clipIndex < 0 || clipIndex >= (int)lane.clips.size()) return;
    Clip& c = lane.clips[(size_t)clipIndex];
    if (c.audioSegs.empty() || t <= c.start + 0.01 || t >= c.start + c.dur - 0.01) return;
    historyFlush();
    const double rate = std::max(0.05, (double)c.audioRate);
    const double cutSrc = (t - c.start) / rate;   // source seconds into the clip where the cut sits (the code's rate is the length scale)
    // left clip: segments up to cutSrc (the last one truncated)
    Clip left = c;
    left.audioSegs.clear();
    double acc = 0.0;
    for (const AudioSeg& s : c.audioSegs) {
        if (acc >= cutSrc) break;
        const double take = std::min(s.dur, cutSrc - acc);
        left.audioSegs.push_back({s.srcOffset, take});
        acc += take;
    }
    left.dur = std::max(0.05, t - c.start);
    std::vector<AutoPoint> leftPts, rightPts;
    for (const AutoPoint& p : c.audioGainPts) {
        if (p.t < t - c.start) leftPts.push_back(p);
        else rightPts.push_back({p.t - (t - c.start), p.v, p.line});
    }
    left.audioGainPts = leftPts;
    Clip right = c;
    right.audioSegs.clear();
    acc = 0.0;
    bool inRight = false;
    for (const AudioSeg& s : c.audioSegs) {
        if (acc >= cutSrc) { right.audioSegs.push_back(s); inRight = true; continue; }
        const double take = std::min(s.dur, cutSrc - acc);
        if (take < s.dur) { right.audioSegs.push_back({s.srcOffset + take, s.dur - take}); inRight = true; }
        acc += take;
    }
    right.start = t;
    right.dur = std::max(0.05, c.start + c.dur - t);
    right.audioGainPts = rightPts;
    c = left;
    // new id + unique name for the right half
    right.id = g_nextClipId++;
    right.name = c.name + std::to_string(++g_trackCounter);
    ensureUniqueClipName(right);
    lane.clips.insert(lane.clips.begin() + clipIndex + 1, right);
    if (lane.clips[(size_t)clipIndex].audioSegs.empty()) {   // degenerate cut: keep everything on the right
        lane.clips[(size_t)clipIndex] = right;
        lane.clips.erase(lane.clips.begin() + clipIndex + 1);
    }
    normalizeClips();
    logLine("[caw] Audio clip cut at " + std::to_string(t) + " s.");
}
#include "app.h"
bool audioClipsSelected() {
    for (const Track& lane : g_proj.tracks)
        if (lane.kind == 7)
            for (const Clip& c : lane.clips)
                if (g_selClips.count(c.id)) return true;
    return false;
}
#include "app.h"
void joinAudioSelectedClips() {
    for (Track& lane : g_proj.tracks) {
        std::vector<Clip*> picked;
        for (Clip& c : lane.clips) if (lane.kind == 7 && g_selClips.count(c.id)) picked.push_back(&c);
        if (picked.size() < 2) continue;
        std::sort(picked.begin(), picked.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
        const std::string& file = picked[0]->audioFile;
        bool ok = true;
        for (const Clip* c : picked) if (c->audioFile != file || c->audioSegs.empty()) { ok = false; break; }
        if (!ok) { logLine("[caw] Join: audio clips must use the same file."); return; }
        historyFlush();
        Clip& first = *picked[0];
        double end = first.start + first.dur;
        std::vector<Clip*> merged{&first};
        for (size_t k = 1; k < picked.size(); ++k) {
            Clip& c = *picked[k];
            if (c.start > end + 1e-6) { logLine("[caw] Join: audio clips must sit next to each other (no gap). Close the gap first."); return; }
            end = std::max(end, c.start + c.dur);
            merged.push_back(&c);
        }
        if (merged.size() < 2) return;
        Clip result = first;
        result.audioSegs.clear();
        for (const Clip* c : merged)
            for (const AudioSeg& s : c->audioSegs) result.audioSegs.push_back(s);
        result.dur = end - result.start;
        std::vector<AutoPoint> pts;
        for (size_t k = 0; k < merged.size(); ++k) {
            const double shift = merged[k]->start - result.start;
            for (const AutoPoint& p : merged[k]->audioGainPts) pts.push_back({p.t + shift, p.v, p.line});
        }
        std::stable_sort(pts.begin(), pts.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
        result.audioGainPts = pts;
        first = result;
        for (size_t k = 1; k < merged.size(); ++k)
            lane.clips.erase(std::remove_if(lane.clips.begin(), lane.clips.end(),
                [&](const Clip& c) { return c.id == merged[k]->id; }), lane.clips.end());
        normalizeClips();
        logLine("[caw] Audio clips joined: " + std::to_string(merged.size()) + " clips, " +
                std::to_string(result.audioSegs.size()) + " segments.");
        return;
    }
}
#include "app.h"
void markLiveCodeEdit(const Clip& clip) {
    g_liveCodeEdits[clip.id] = LiveCodeEdit{ImGui::GetTime(), true};
    g_clipCodeDirty.insert(clip.id);
}
#include "app.h"
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
#include "app.h"
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
#include "app.h"
void toggleTimelinePlay() {
    if (g_timelineRun) {
        g_timelineRun = false;
        g_pdResumeAfterReload = false;
        if (g_pdTimelineMode) {
            pdTransport(false);g_pdPlayingClip = 0; g_pdTimelineMode = false;
        }
        if (engineIsUp()) commandStopAll();
        if (g_liveCsound && g_useCsoundApi) g_apiProc.silenceVstTaps();
        if (g_scReady) stopAllScClips();
        if (g_vstEngineOn) vstSolo().halt();   // the VST engine stops scheduling and releases its notes
        cawaudio::engine().clearAll();   // audio track voices stop with the transport
        clearSampleRings();
        g_audioVoiceSig.clear();
        g_scTimelineSig = 0; g_scPlanT = 0.0; g_scLastPDTimelineTime = -1.0;
        mixedPdTransport(false);
    } else {
        // SEPARATED ENGINES: Play NEVER starts an engine any more. It only plays the timeline -
        // with no engine up the playhead just moves, silently. The VST engine is started by its
        // own button (Run VST / the combined Run button), Csound / PD / SC likewise.
        if (anyVstTrack() && !g_vstEngineOn) {
            playNotice(g_langVst
                ? "Play only plays the timeline - press Run VST (or the combined Run button) to hear the VST tracks."
                : "Enable VST in Settings > Engines and press Run VST to hear the VST tracks.",
                g_langVst ? "VST engine is off" : "Enable VST first");
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
            playNotice("Enable SuperCollider in Settings > Engines - the SC clips are skipped, the other engines play.", "Enable SuperCollider first");
        } else if (hasAudibleScClip && !g_scReady) {
            const std::string runSc = std::string("DSP");
            const std::string message = g_scEngineOn
                ? std::string("SuperCollider is still preparing - the SC clips are skipped for now.")
                : "The SC clips are skipped - press " + runSc + " to hear them. The timeline plays (the playhead moves) either way.";
            playNotice(message, g_scEngineOn ? "SuperCollider is preparing" : runSc);
            // The timeline PLAYS even when nothing else is up: the playhead moves (silently for the
            // SC clips until their engine runs). It used to refuse here, which locked the whole
            // transport whenever SuperCollider was the selected language but its engine was off.
        }
        {
            // Sample tracks AND locked tracks: the same rule - Play moves the playhead, the Run sample button starts the engine.
            bool hasSampleClip = false;
            for (const Track& track : g_proj.tracks) {
                if (track.kind != 7 || track.muted || (anyTrackSolo && !track.solo)) continue;
                for (const Clip& clip : track.clips) if (!clip.muted && !clip.audioFile.empty()) hasSampleClip = true;
            }
            // A locked (LKD) track plays its rendered WAV without any engine now - only the real sample clips need the sample engine.
            bool hasFrozenAudible = false;
            for (const Track& track : g_proj.tracks)
                if (trackFrozen(track) && !track.muted && !(anyTrackSolo && !track.solo)) { hasFrozenAudible = true; break; }
            if (hasSampleClip && !g_sampleEngineOn)
                playNotice("Press DSP to hear the sample clips. The timeline plays either way.", "Sample engine is off");
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
            if (pdTrack && !g_pdEngineOn && !g_mixedPd) playNotice("Press DSP to enable Pure Data clips.");
            else if (pdTrack && !pdClip) playNotice("The PD track has no clips. Double-click its lane to add one, then choose a .pd file.", "PD track has no clip: double-click its lane");
            else if (pdTrack) playNotice("All PD clips or PD tracks are muted (or another track is solo).");
        }
        if (audiblePdEvents && !g_mixedPd) {
            // A PD problem skips the PD timeline only: VSTi / Csound / SC still play. Play again
            // once PD is ready (Run PD preloads its clips; the VST engine keeps working meanwhile).
            if (g_pdLibraryPath.empty()) g_pdLibraryPath = plat::bundledLibpd();
            if (!g_langPd) {
                playNotice("Enable Pure Data in Settings > Engines - the PD clips are skipped, the other engines play.");
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
                    g_pdNeedsReload=true;playNotice("PD clips changed - they are skipped. Press DSP to preload them again.", "PD skipped: Run PD again");
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
            logLine(g_langCsound ? "[caw] Press DSP for the Csound clips."
                    : (!g_langPd && !g_langSc && !g_langVst) ? "[caw] Score only: no engine is enabled (Settings > Engines). The timeline and editors work."
                    : "[caw] No SuperCollider or Pure Data audio engine is running.");
    }
}
#include "app.h"
void seekTimeline(double t) {
    g_play = std::max(0.0, t);
    if (g_timelineRun) {
        g_resync = 2;
        if (g_pdTimelineMode) { g_pdPlayingClip = 0; vstEngineJoinTransport(); pdTransport(true); if (g_scReady) scheduleScTimeline(g_play); }
    }
}
#include "app.h"
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
#include "app.h"
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
#include "app.h"
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
#include "app.h"
void showAllMidi() {
    for (Track& t : g_proj.tracks) t.midiHidden = false;
    g_midiAllHidden = false;
}
#include "app.h"
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
    const bool vstStarting = g_vstEngineOn && !vstSolo().ready() && !g_vstIdleNoPlugin;   // idle without a plugin is not "starting"
    bool anySampleTrack = false;
    for (const Track& st : g_proj.tracks) if (st.kind == 7) anySampleTrack = true;
    const bool smpOn = g_sampleEngineOn || anySampleTrack;
    const bool smpReady = g_sampleEngineOn && (g_sampleOutput != nullptr || g_vstSoloOutput != nullptr || g_scSoloOutput != nullptr || scHasSharedAudioOwner());
    const bool anyStarting = csStarting || pdStarting || scStarting || vstStarting || (g_sampleEngineOn && !smpReady);
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
    const char smpStatus = g_sampleEngineOn ? (smpReady ? '1' : '~') : '0';
    (void)csStatus; (void)scStatus; (void)pdStatus; (void)vstStatus; (void)smpStatus;   // the pill is combined now; the flags stay for debug
    const bool anyReady = csReady || scReady || pdReady || vstReady || smpReady;
    if (g_renderActive) {
        std::snprintf(statusText, sizeof statusText, "Rendering...");
        statusCol = ImVec4(0.95f, 0.60f, 0.20f, 1.f);
    } else {
        // One combined status for all engines: DSP:on / DSP:off / DSP: starting...
        // "starting" = any engine is switched on but not ready yet (the waiting phase, e.g. a
        // plugin loading); "on" only when nothing is waiting and at least one engine is ready.
        std::snprintf(statusText, sizeof statusText, anyStarting ? "DSP: starting..." : (anyReady ? "DSP: on" : "DSP: off"));
        if (anyStarting) statusCol = ImVec4(0.95f, 0.72f, 0.30f, 1.f);
        else if (anyReady) statusCol = ImVec4(0.40f, 0.85f, 0.50f, 1.f);
    }
    if (g_playNoticeT > 0.0) {   // a reason why Play did nothing, shown where it cannot be missed
        g_playNoticeT -= ImGui::GetIO().DeltaTime;
        std::snprintf(statusText, sizeof statusText, "%.64s", g_playNotice.c_str());
        statusCol = ImVec4(0.95f, 0.60f, 0.20f, 1.f);
    }
    char engineText[96];
    // the sampling rate and the project's channel count sit left of the status pill (user request)
    std::snprintf(engineText, sizeof engineText, "%d Hz  |  %d ch", cs.sr, masterChannels(cs));
    const float dotW = S(14);
    const float statusW = dotW + ImGui::CalcTextSize(statusText).x + S(12);
    const float engineW = ImGui::CalcTextSize(engineText).x + S(16);
    const float rightW = statusW + engineW + S(12);
    if (totalW > S(640)) {
        const ImVec2 base = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = base.x + totalW - rightW + S(4);
        const float yMid = base.y + S(26);
        {
            const ImVec2 sz = ImGui::CalcTextSize(engineText);
            dl->AddText(ImVec2(x, yMid - sz.y * 0.5f), IM_COL32(150, 150, 158, 255), engineText);
            x += engineW;
        }
        const ImVec2 sz = ImGui::CalcTextSize(statusText);
        dl->AddRectFilled(ImVec2(x - S(6), yMid - S(13)), ImVec2(x + statusW, yMid + S(13)), IM_COL32(28, 29, 34, 255), S(13));
        dl->AddCircleFilled(ImVec2(x + S(5), yMid), S(4), ImGui::ColorConvertFloat4ToU32(statusCol));
        dl->AddText(ImVec2(x + dotW, yMid - sz.y * 0.5f), ImGui::ColorConvertFloat4ToU32(statusCol), statusText);
    }

    // ---- always visible: go to start, DSP, Play, Loop, M, S, follow, time (user order)
    if (ImGui::Button("|<")) seekTimeline(0.0);
    ImGui::SameLine();
    // ONE Run button for everything: every enabled engine's button also starts/stops the sample
    // engine, so a session with sample tracks shows a single Run button (the status pill's SMP
    // shows what is running). The sample engine gets its own button only in a pure sample session
    // (no Csound / PD / SC / VST language checked).
    bool anySample = false;
    for (const Track& t : g_proj.tracks) if (t.kind == 7) anySample = true;
    auto sampleRunStart = [&]() {
        if (g_langAudio && !g_sampleEngineOn) {   // "Audio" checked: the sample engine runs even with no sample track yet - a clip added later plays without re-running DSP
            g_sampleEngineOn = true;
            ensureSampleOutput();
            logLine("[caw] Sample engine running: the sample clips play with Play.");
        }
    };
    auto sampleRunStop = [&]() {
        if (g_sampleEngineOn) {
            int frozenCount = 0;
            for (const Track& t : g_proj.tracks) if (trackFrozen(t)) ++frozenCount;
            g_sampleEngineOn = false;
            cawaudio::engine().clearAll();
            g_audioVoiceSig.clear();
            closeSampleOutput();
            logLine("[caw] Sample engine stopped.");
            if (frozenCount > 0)
                logLine("[caw] Note: " + std::to_string(frozenCount) + " frozen track(s) play their rendered WAV through the sample engine - they are silent until you run Sample again (mute / fader still apply once it runs).");
        }
    };
    if (g_langCsound || g_liveCsound) {
        const bool csOn = g_liveCsound;
        const bool scOn = g_scEngineOn;
        const bool vstOn = g_vstEngineOn;
        const bool csPd = g_mixedPd || (g_langCsound && g_langPd);
        const bool vstWanted = g_langVst;   // checked = the VST engine runs, even with no .vst3 picked yet: it loads and starts by itself later
        const bool on = csOn;
        // The label is the list of engines this button starts, exactly as checked in Settings >
        // Languages ("Run Csound + VST", "Run Csound + PD + SC + VST", ...).
        // The button always reads DSP: what is running shows in the status pill and the green button state.
        const std::string label = "DSP";
        if (on) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(label.c_str())) {
            if (csOn) {
                bool added = false;
                if (g_langSc && !scOn) { startScEngine(); added = true; }
                if (vstWanted && !vstOn) { startVstEngine(); added = true; }
                if (g_langAudio && !g_sampleEngineOn) { sampleRunStart(); added = true; }
                if (!added) {
                    if (scOn) stopScEngine();
                    if (vstOn) stopVstEngine();
                    sampleRunStop();
                    stopPlayback();
                }
            } else {
                const bool startScAfterHost = g_langSc && !g_scEngineOn;
                // Open the Csound/PD-owned device stream first, then start SC
                // into that callback instead of opening and replacing the card twice.
                startEngine();
                if (startScAfterHost && g_liveCsound && !startScEngine()) stopPlayback();
                if (vstWanted && !g_vstEngineOn) startVstEngine();
                sampleRunStart();
            }
        }
        if (on) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) helpTip("%s", (g_langSc || g_langVst || anySample)
            ? "DSP runs the checked engines together (Csound with PD when enabled, SuperCollider, VST) and the sample tracks. If everything is already running, this button stops them all."
            : (csPd ? "DSP runs Csound and PD through one CAW audio output." : "DSP runs Csound for Csound timeline clips."));
        ImGui::SameLine();
    }
    if (g_langPd && !g_langCsound && !g_mixedPd) {
        const bool on = g_pdEngineOn;
        const bool scOn = g_scEngineOn;
        const bool vstOn = g_vstEngineOn;
        const bool vstWanted = g_langVst;   // checked = the VST engine runs, even with no .vst3 picked yet
        // Always reads DSP; what runs shows in the status pill and the green button state.
        const std::string label = "DSP";
        if (on) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(label.c_str())) {
            if (on) {
                bool added = false;
                if (g_langSc && !scOn) { startScEngine(); added = true; }
                if (vstWanted && !vstOn) { startVstEngine(); added = true; }
                if (g_langAudio && !g_sampleEngineOn) { sampleRunStart(); added = true; }
                if (!added) {
                    const bool hadSc = scOn;
                    if (hadSc) stopScEngine();
                    if (vstOn) stopVstEngine();
                    sampleRunStop();
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
                if (g_pdLibraryPath.empty()) playNotice(plat::isWindows ? "Choose libpd in Settings > Programs." : "libpd not found: build it with tools/unix/build-libpd.sh, or choose libpd.so in Settings > Programs.", "Choose libpd in Settings > Programs.");
                else if (!makePdPlan(plan, error, true)) playNotice(error, "PD clip problem: see the log");
                else {
                    if (!g_audioDevicesReady) refreshAudioDevices(false);
                    if (g_proj.cs.outDev == -2) playNotice("Select an audio output in Settings > Audio.");
                    else {
                        closeScSoloOutput();
                        g_pdTimelineEngine.setHub(&g_cawHub);   // CAW hub: PD-only mode shares the same caws/cawr bus
        handAudioCardToNextStream();   // PD's engine thread opens its own stream: exclusive drivers need the card free
                        if (!g_pdTimelineEngine.start(g_pdLibraryPath, plan, pdAudioSettings())) {
                            playNotice("PD preparation failed: " + g_pdTimelineEngine.status());
                            restoreScSoloOutput();
                        } else {
                            g_pdTimelineEvents=std::move(plan);g_pdNeedsReload=false;
                            g_pdPlanUpdateFailed=false;
                            g_idleMeters=meterLayout(g_proj,g_projectPath);
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
                            sampleRunStart();
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
        const bool vstWanted = g_langVst;   // checked = the VST engine runs, even with no .vst3 picked yet
        // Always reads DSP like the other engine buttons; what runs shows in the status pill and the green state.
        std::string label = "DSP";
        if (on || vstOn) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
        if (ImGui::Button(label.c_str())) {
            if (!on) {
                if (startScEngine() && vstWanted && !g_vstEngineOn) startVstEngine();
                sampleRunStart();
            } else if (vstWanted && !vstOn) {
                startVstEngine();
            } else {
                if (vstOn) stopVstEngine();
                sampleRunStop();
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
        if (ImGui::Button("DSP", ImVec2(0, 0))) {
            if (on) { stopVstEngine(); sampleRunStop(); }
            else { startVstEngine(); sampleRunStart(); }
        }
        if (on) ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            helpTip("DSP runs CAW's VST engine without Csound. Sample or locked sources feeding a VSTe start the sample engine automatically when Audio is enabled. Press Play afterwards.");
        ImGui::SameLine();
    }
    // The sample engine (audio tracks, kind 7): every other Run button starts/stops it too, so its
    // own button is only needed in a pure sample session (no Csound / PD / SC / VST language checked).
    {
        const bool ownSampleButton = anySample && !g_langCsound && !g_liveCsound && !g_langPd && !g_langSc && !g_langVst;
        if (ownSampleButton) {
            const bool on = g_sampleEngineOn;
            if (on) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
            // Always reads Run; what runs shows in the status pill (SMP) and the green button state.
            if (ImGui::Button("DSP")) {
                g_sampleEngineOn = !on;
                if (g_sampleEngineOn) { ensureSampleOutput(); logLine("[caw] Sample engine running: the sample clips play with Play."); }
                else { cawaudio::engine().clearAll(); g_audioVoiceSig.clear(); closeSampleOutput(); logLine("[caw] Sample engine stopped."); }
            }
            if (on) ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) helpTip("DSP runs the sample tracks: their audio clips play with Play (the playhead moves either way). A clip's CAW code drives its length (rate), volume (gain), direction (reverse) and routing (out1..out8). Samples do not need any other engine.");
            ImGui::SameLine();
        }
    }
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
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, std::max(0.f, (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f)));   // Button (not SmallButton): the label centers vertically, same height as the other transport buttons
    if (ImGui::Button("M##global_unmute")) {
        const int count = unmuteAllTracks();
        logLine("[caw] Unmuted all tracks" + std::string(count ? " (" + std::to_string(count) + (count == 1 ? " track)" : " tracks)") : " (none were muted)") + ".");
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        helpTip("Global mute: unmute EVERY muted track (Alt+M). Only the track mutes are cleared - clips and automation lanes keep their own mute. Solo stays.");
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, std::max(0.f, (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f)));   // Button (not SmallButton): the label centers vertically, same height as the other transport buttons
    if (ImGui::Button("S##global_unsolo")) {
        const int count = unsoloAllTracks();
        logLine("[caw] Cleared solo on all tracks" + std::string(count ? " (" + std::to_string(count) + (count == 1 ? " track)" : " tracks)") : " (none were solo)") + ".");
    }
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        helpTip("Global solo off: clear solo on EVERY track (Alt+S). Mutes stay as they are.");
    ImGui::SameLine();
    {
        static const char* followNames[] = {"None", "Page scroll", "Follow"};
        ImGui::SetNextItemWidth(S(104));
        int fv = g_tlFollow;
        if (caw_ui::combo("##tl_follow", &fv, followNames, 3)) g_tlFollow = fv;
        if (ImGui::IsItemHovered()) helpTip("Follow the playhead while the timeline plays (Cubase's auto-scroll): \"page\" jumps a whole page each time the playhead leaves the view; \"timeline\" keeps the playhead at the middle of the view and the content moves under it, frame by frame. Shortcut: F.");
    }
    ImGui::SameLine();   // the follow combo stays on the transport row (the tools follow here too)
    int ms = (int)(g_play * 1000.0);
    ImGui::AlignTextToFramePadding();
    ImGui::SetWindowFontScale(1.3f);   // the timer reads big (user request)
    ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "%02d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    ImGui::SetWindowFontScale(1.f);
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
    endChildTight();
    ImGui::PopStyleColor();
}
#include "app.h"
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
#include "app.h"
void drawShapePrims(ImDrawList* dl, int shape, float x0, float y0, float x1, float y1, ImU32 line, ImU32 fillSoft, double ratio )
{
    for (const markgfx::Prim& p : markgfx::shapePrims(shape, x0, y0, x1, y1, ratio)) {
        std::vector<ImVec2> v;
        for (const auto& q : p.pts) v.push_back(ImVec2((float)q.first, (float)q.second));
        if (p.fill && (p.solid || primsConvex(v))) dl->AddConvexPolyFilled(v.data(), (int)v.size(), p.solid ? line : fillSoft);
        if (v.size() >= 2) dl->AddPolyline(v.data(), (int)v.size(), line, p.closed ? ImDrawFlags_Closed : 0, S((float)p.sw));
    }
}
#include "app.h"
ImU32 codeTokColor(int kind) {
    static const ImU32 cols[] = { IM_COL32(225, 226, 232, 255), IM_COL32(197, 134, 192, 255), IM_COL32(181, 206, 168, 255),
                                  IM_COL32(206, 145, 120, 255), IM_COL32(106, 153, 85, 255), IM_COL32(86, 156, 214, 255) };
    return cols[std::min(5, std::max(0, kind))];
}
#include "app.h"
void drawLedMeter(ImDrawList* dl, float x0, float x1, float top, float bottom, float db, bool clip, bool gray, bool master )
{
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
#include "app.h"
void qSelectedClips() {
    if (g_selClips.size() + g_selMidiClips.size() < 2) return;
    historyFlush();
    clip_timing::sync(g_proj, g_selClips, g_selMidiClips);
    historyFlush();
}
#include "app.h"
void drawClipQMenu() {
    if (ImGui::MenuItem("Q", "Q", false, g_selClips.size() + g_selMidiClips.size() >= 2))
        qSelectedClips();
    if (ImGui::IsItemHovered()) helpTip("Align selected clip starts to the earliest selected clip.");
}
#include "app.h"
void drawChevron(ImDrawList* dl, float x, float y, float radius, bool right, ImU32 color) {
    caw_ui::disclosure(dl, ImVec2(x, y), radius, color, !right);
}
#include "app.h"
// Track lock dialogs (drawArrange): the engine-down warning and the VSTe render-and-lock planner.
static int g_freezeWarnTrack = -1;    // the track whose engine is down
static bool g_freezeWarnOpen = false; // p_open of the warning modal (syncs with click-outside closes)
static int g_freezeVsteAsk = -1;      // the rack track the dialog plans the render-and-lock for
static bool g_freezeVsteOpen = false; // p_open of the rack modal
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
    for (Track& tr : g_proj.tracks) tr.height = kFixedTrackH;   // every lane has the same fixed height (sample lanes too: the big shape lives in the sound editor)
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
    // follow the playhead while the timeline plays (Cubase's auto-scroll modes):
    // page = the view jumps one whole page each time the playhead leaves it; timeline = the playhead
    // stays pinned at ~1/3 of the view and the content slides under it.
    if (g_timelineRun && g_tlFollow > 0) {
        if (g_tlFollow == 1) {
            if ((double)g_play >= (double)g_scrollX + viewSec) g_scrollX = (float)g_play;
            else if ((double)g_play < (double)g_scrollX) g_scrollX = (float)std::max(minX, g_play - viewSec * 0.05);
        } else {
            // move by timeline: the playhead IS the middle of the view, moved frame by frame
            g_scrollX = (float)std::max(minX, g_play - viewSec * 0.5);
        }
    }
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
                for (auto& [key, sel] : g_autoSelMulti) {
                    Track* ot = findTrackById(key.first);
                    if (!ot) continue;
                    auto ol = ot->automation.find(key.second);
                    if (ol != ot->automation.end()) sortLaneKeepSelection(ol->second, sel);
                }
                g_wh.active = false;
            } else {
                consumed = true;
                float ly0 = 0.f, ly1 = 0.f;
                autoLaneGeometry(*whTrack, whTi, g_wh.channel, ly0, ly1);
                const float yC = yOfValue(g_wh.center, ly0, ly1, g_wh.lo, g_wh.hi);
                const float x0px = tx(g_wh.t0), x1px = tx(g_wh.t0 + g_wh.span);
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
                const double aT = g_wh.others.empty() ? -1.0 : g_wh.t0, aS = g_wh.others.empty() ? -1.0 : g_wh.span;
                reshapeDots(whLane->second, g_wh.idx, g_wh.orig, dS, dE, st, mo, g_wh.lo, g_wh.hi, aT, aS);
                const ParsedTrack whParsed = pdParsedForUi(*whTrack);
                const CawParam* whParam = hasClips(*whTrack) ? laneParameter(whParsed, g_wh.channel) : nullptr;
                if (whParam) for (int index : g_wh.idx)
                    if (index >= 0 && index < (int)whLane->second.size())
                        whLane->second[(size_t)index].v = quantizeAutomation(whLane->second[(size_t)index].v, *whParam);
                for (const auto& ol : g_wh.others) {   // every other selected lane: the same depth / stretch / shape
                    Track* ot = findTrackById(ol.trackId);
                    if (!ot || trackFrozen(*ot)) continue;
                    auto lane = ot->automation.find(ol.channel);
                    if (lane == ot->automation.end()) continue;
                    reshapeDots(lane->second, ol.idx, ol.orig, dS, dE, st, mo, ol.lo, ol.hi, g_wh.t0, g_wh.span);
                    for (int index : ol.idx)
                        if (index >= 0 && index < (int)lane->second.size())
                            lane->second[(size_t)index].v = laneQuantizeValue(*ot, ol.channel, lane->second[(size_t)index].v);
                }
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
                    g_wh.t0 = tMin; g_wh.span = tMax - tMin;
                    for (const auto& [key, sel] : g_autoSelMulti) {   // the other selected lanes join the reshape
                        if (sel.empty()) continue;
                        const Track* ot = findTrackById(key.first);
                        if (!ot || trackFrozen(*ot) || !autoShown(*ot, key.second)) continue;
                        const auto lane = ot->automation.find(key.second);
                        if (lane == ot->automation.end()) continue;
                        WaveHandleDrag::Lane ol;
                        ol.trackId = key.first; ol.channel = key.second;
                        for (int idx : sel) if (idx >= 0 && idx < (int)lane->second.size()) { ol.idx.push_back(idx); ol.orig.push_back(lane->second[(size_t)idx]); }
                        if (ol.idx.empty()) continue;
                        double l2, h2, i2; laneValueRange(*ot, key.second, l2, h2, i2);
                        ol.lo = l2; ol.hi = h2;
                        for (const AutoPoint& q : ol.orig) { g_wh.t0 = std::min(g_wh.t0, q.t); }
                        g_wh.others.push_back(std::move(ol));
                    }
                    for (const auto& ol : g_wh.others) for (const AutoPoint& q : ol.orig) g_wh.span = std::max(g_wh.span, q.t - g_wh.t0);
                    g_wh.span = std::max(g_wh.span, tMax - g_wh.t0);
                    g_wh.handleY = kind == 1 ? wh.depth.y : wh.morph.y; g_wh.startY = m.y;
                    consumed = true;
                }
            }
        }
    }
    if (inBody && !consumed && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) {
        for (int ti = 0; ti < nT && !consumed; ++ti) {
            if (hiddenTrack(ti)) continue;
            Track& track = g_proj.tracks[(size_t)ti];
            if (trackFrozen(track)) continue;   // frozen: the lane is locked, its automation cannot be edited
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
                // Is the pressed dot already part of the (possibly multi-lane) selection? Then the press moves
                // everything that is selected - the dots of every lane and the selected clips with them.
                bool grabSelected = false;
                if (hit >= 0) {
                    if (sameLane) grabSelected = g_autoSel.count(hit) > 0;
                    else { const auto mi = g_autoSelMulti.find({track.id, channel}); grabSelected = mi != g_autoSelMulti.end() && mi->second.count(hit) > 0; }
                }
                if (!sameLane) {
                    if (io.KeyCtrl || io.KeyShift || grabSelected) {   // additive / grab: this lane becomes the primary one, nothing is lost
                        const auto all = allSelectedDots();
                        g_autoSelMulti.clear();
                        for (const auto& [key, idx] : all) g_autoSelMulti[key] = idx;
                        g_autoSelTrackId = track.id; g_autoSelChannel = channel;
                        const auto own = all.find({track.id, channel});
                        g_autoSel = own == all.end() ? std::set<int>() : own->second;
                        g_autoSelMulti.erase({track.id, channel});
                    } else { g_autoSel.clear(); g_autoSelMulti.clear(); g_autoSelTrackId = track.id; g_autoSelChannel = channel; }
                }
                g_selTrack = ti;
                g_selClip = -1;
                consumed = true;
                if (!io.KeyCtrl && !io.KeyShift && !grabSelected) { g_selClips.clear(); g_selMidiClips.clear(); }
                if (grabSelected && !isPenTool() && ImGui::IsMouseClicked(0) && !ImGui::IsMouseDoubleClicked(0) &&
                    (!g_selClips.empty() || !g_selMidiClips.empty())) {
                    // dots AND clips are selected: drag them all together in time (the clip move carries the dots)
                    g_drag = Drag::Move;
                    g_dragAnchorT = xt(m.x);
                    g_dragOrig.clear();
                    for (const Track& tr : g_proj.tracks)
                        for (const Clip& sc : tr.clips)
                            if (g_selClips.count(sc.id)) g_dragOrig.push_back({sc.id, sc.start});
                    g_dragDots = allSelectedDots();
                    g_clipAutomationMove.capture(g_proj, g_selClips, &g_dragDots);
                    g_dragMidiOrig.clear();
                    for (const Track& tr : g_proj.tracks)
                        for (const MidiClip& mc2 : tr.midiClips)
                            if (g_selMidiClips.count(mc2.id)) g_dragMidiOrig.push_back({mc2.id, mc2.start});
                    break;
                }

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
                    } else if (io.KeyShift) {
                        g_autoSel.insert(hit);   // Shift adds the dot (and keeps every other lane), then it can be dragged with the rest
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
                if (!g_autoDragMoved) {   // until the mouse really moves: snapshot the other selected lanes
                    g_autoDragOrigMulti.clear();
                    for (const auto& [key, idx] : g_autoSelMulti) {
                        if (idx.empty()) continue;
                        const Track* ot = findTrackById(key.first);
                        if (!ot) continue;
                        const auto ol = ot->automation.find(key.second);
                        if (ol != ot->automation.end()) g_autoDragOrigMulti[key] = ol->second;
                    }
                }
                if (down && g_autoDragMoved && g_autoDragOrig.size() == points.size()) {
                    const double dt = io.KeyShift ? 0.0 : xt(m.x) - g_autoDragStartT;  // Shift = value only
                    const double dv = valueAtY(m.y, laneY0, laneY1, lo, hi) - g_autoDragStartV;
                    const double dvNorm = hi > lo ? dv / (hi - lo) : 0.0;   // other lanes move by the same share of their own range
                    for (const auto& [key, orig] : g_autoDragOrigMulti) {
                        Track* ot = findTrackById(key.first);
                        if (!ot || trackFrozen(*ot)) continue;
                        auto ol = ot->automation.find(key.second);
                        const auto sel = g_autoSelMulti.find(key);
                        if (ol == ot->automation.end() || sel == g_autoSelMulti.end() || ol->second.size() != orig.size()) continue;
                        double lo2, hi2, init2;
                        laneValueRange(*ot, key.second, lo2, hi2, init2);
                        for (int idx : sel->second) {
                            if (idx < 0 || idx >= (int)orig.size()) continue;
                            ol->second[(size_t)idx].t = clampTimelineTime(orig[(size_t)idx].t + dt);
                            ol->second[(size_t)idx].v = laneQuantizeValue(*ot, key.second,
                                std::min(hi2, std::max(lo2, orig[(size_t)idx].v + dvNorm * (hi2 - lo2))));
                        }
                    }
                    for (int idx : g_autoSel) {
                        if (idx < 0 || idx >= (int)points.size()) continue;
                        points[(size_t)idx].t = clampTimelineTime(g_autoDragOrig[(size_t)idx].t + dt);
                        points[(size_t)idx].v = laneQuantizeValue(*track, g_autoDragChannel,
                            std::min(hi, std::max(lo, g_autoDragOrig[(size_t)idx].v + dv)));
                    }
                }
                if (!down) {
                    sortLaneKeepSelection(points, g_autoSel);
                    for (auto& [key, sel] : g_autoSelMulti) {
                        Track* ot = findTrackById(key.first);
                        if (!ot) continue;
                        auto ol = ot->automation.find(key.second);
                        if (ol != ot->automation.end()) sortLaneKeepSelection(ol->second, sel);
                    }
                    g_autoDragOrigMulti.clear();
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
                        // the box may cover several lanes (of one track or of several tracks): every dot inside is picked
                        const float x0 = std::min(m.x, g_autoDragStartMouse.x), x1 = std::max(m.x, g_autoDragStartMouse.x);
                        const float y0 = std::min(m.y, g_autoDragStartMouse.y), y1 = std::max(m.y, g_autoDragStartMouse.y);
                        auto all = allSelectedDots();
                        for (int ti2 = 0; ti2 < nT; ++ti2) {
                            if (hiddenTrack(ti2)) continue;
                            const Track& tr2 = g_proj.tracks[(size_t)ti2];
                            if (trackFrozen(tr2)) continue;
                            for (const auto& [ch2, pts2] : tr2.automation) {
                                if (!autoShown(tr2, ch2)) continue;
                                float ly0 = 0.f, ly1 = 0.f;
                                autoLaneGeometry(tr2, ti2, ch2, ly0, ly1);
                                if (ly1 < y0 || ly0 > y1) continue;
                                double lo2, hi2, init2;
                                laneValueRange(tr2, ch2, lo2, hi2, init2);
                                for (int pi = 0; pi < (int)pts2.size(); ++pi) {
                                    const float px = tx(pts2[(size_t)pi].t);
                                    const float py = yOfValue(pts2[(size_t)pi].v, ly0, ly1, lo2, hi2);
                                    if (px >= x0 && px <= x1 && py >= y0 - S(4) && py <= y1 + S(4)) all[{tr2.id, ch2}].insert(pi);
                                }
                            }
                        }
                        setAllSelectedDots(all, track->id, g_autoDragChannel);
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
            if (trackFrozen(track)) continue;   // frozen: the lane is locked, the MIDI regions cannot be edited
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
                                g_dragDots = allSelectedDots();
                                g_clipAutomationMove.capture(g_proj, g_selClips, &g_dragDots);
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
            g_dragDots.clear();
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
            if (!g_dragOrig.empty() || !g_dragDots.empty()) {
                ClipAutomationMove::DotSelection moved;
                const int primaryTrack = g_autoSelTrackId; const std::string primaryChannel = g_autoSelChannel;
                g_clipAutomationMove.apply(g_proj, dt, nullptr, &moved);
                if (!g_dragDots.empty()) setAllSelectedDots(moved, primaryTrack, primaryChannel);
            }
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

    // ---- audio clip tools (kind 7): Pen 2 draws the volume automation on the waveshape, Cut splits the clip
    bool audioClipHandled = false;
    {   // a file dropped from Explorer: on a sample lane it becomes a sample clip at the drop point
        std::string dPath; int dx = 0, dy = 0;
        if (appTakeQueuedFileDrop(dPath, dx, dy)) {
            const int dti = laneOf((float)dy);
            if (dti >= 0 && g_proj.tracks[(size_t)dti].kind == 7 && !trackFrozen(g_proj.tracks[(size_t)dti])) {
                Track& dt = g_proj.tracks[(size_t)dti];
                Clip dc = makeClip(dt, clampTimelineTime(xt((float)dx)), 2.0, "");
                if (initAudioClip(dc, dPath)) {
                    dt.clips.push_back(dc);
                    g_selTrack = dti;
                    g_selClip = (int)dt.clips.size() - 1;
                    g_selClips.clear();
                    g_selClips.insert(dc.id);
                }
            } else if (dti >= 0) {
                logLine("[caw] Drop: that lane is not a sample lane - right-click > Add sample track, then drop the file there.");
            }
        }
    }
    if (inBody && !consumed && !markerHandled) {
        const int ati = laneOf(m.y);
        if (ati >= 0 && g_proj.tracks[(size_t)ati].kind == 7) {
            const int aci = clipAt(ati, m.x);
            if (aci >= 0) {
                Track& at = g_proj.tracks[(size_t)ati];
                Clip& ac = at.clips[(size_t)aci];
                const float gy0 = yTop[(size_t)ati] + S(2), gy1 = baseBottom[(size_t)ati] - S(2);
                auto gainY = [&](double v) { return gy1 - (float)std::max(0.0, std::min(2.0, v)) / 2.f * (gy1 - gy0); };
                auto pointX = [&](double t) { return tx(ac.start + std::max(0.0, std::min(ac.dur, t))); };
                if (g_tool == Tool::Cut && ImGui::IsMouseClicked(0)) {
                    cutAudioClipAt(ati, aci, clampTimelineTime(xt(m.x)));
                    audioClipHandled = true;
                    consumed = true;
                } else if (g_tool == Tool::Pen && m.y >= yTop[(size_t)ati] + S(2)) {
                    // find a point under the mouse (or make one), then drag it - Cubase style create + drag
                    int hit = -1;
                    for (int k = 0; k < (int)ac.audioGainPts.size(); ++k) {
                        const AutoPoint& p = ac.audioGainPts[(size_t)k];
                        if (std::fabs(pointX(p.t) - m.x) <= S(6) && std::fabs(gainY(p.v) - m.y) <= S(7)) { hit = k; break; }
                    }
                    if (ImGui::IsMouseClicked(0)) {
                        if (hit < 0) {
                            const double t = std::max(0.0, std::min(ac.dur, clampTimelineTime(xt(m.x)) - ac.start));
                            const double v = 2.0 * (double)(gy1 - m.y) / std::max(1.f, gy1 - gy0);
                            ac.audioGainPts.push_back({t, std::max(0.0, std::min(2.0, v)), false});
                            std::stable_sort(ac.audioGainPts.begin(), ac.audioGainPts.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
                            for (int k = 0; k < (int)ac.audioGainPts.size(); ++k)
                                if (ac.audioGainPts[(size_t)k].t == t) { hit = k; break; }
                        }
                        g_audioGainClip = ac.id;
                        g_audioGainIdx = hit;
                        audioClipHandled = true;
                        consumed = true;
                    }
                }
            }
        }
    }
    if (g_audioGainClip >= 0) {
        bool live = ImGui::IsMouseDown(0);
        Clip* gc = nullptr;
        if (live) {
            for (Track& t2 : g_proj.tracks)
                for (Clip& c2 : t2.clips)
                    if (c2.id == g_audioGainClip) { gc = &c2; break; }
        }
        if (!live || !gc || g_audioGainIdx < 0 || g_audioGainIdx >= (int)gc->audioGainPts.size()) {
            g_audioGainClip = -1;
        } else {
            AutoPoint& p = gc->audioGainPts[(size_t)g_audioGainIdx];
            p.t = std::max(0.0, std::min(gc->dur, clampTimelineTime(xt(m.x)) - gc->start));
            const float gy0 = 0.f, gy1 = 1.f;   // gain follows the mouse vertically within the clip body
            (void)gy0; (void)gy1;
            // recompute the band from the drawing code: y maps gain 0..2 over the waveform area
            int gti = -1; findClip(gc->id, &gti);
            if (gti >= 0) {
                const float y0 = yTop[(size_t)gti] + S(2), y1 = baseBottom[(size_t)gti] - S(2);
                p.v = std::max(0.0, std::min(2.0, 2.0 * (double)(y1 - m.y) / std::max(1.f, y1 - y0)));
            }
            std::stable_sort(gc->audioGainPts.begin(), gc->audioGainPts.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
            // double-click a point removes it (the sort keeps the indexes; re-find by proximity)
            if (ImGui::IsMouseDoubleClicked(0)) {
                gc->audioGainPts.erase(gc->audioGainPts.begin() + g_audioGainIdx);
                g_audioGainClip = -1;
            }
        }
    }

    // ---- mouse interaction
    if (inBody && !consumed && !markerHandled && !audioClipHandled) {
        int ti = laneOf(m.y);
        int ci = clipAt(ti, m.x);
        if (ti >= 0 && trackFrozen(g_proj.tracks[(size_t)ti])) {   // frozen: the lane is untouchable (no drag, no editor, no new clips)
            ti = -1;
            ci = -1;
        }
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
            // Ctrl/Shift-click or grabbing an already selected clip keeps the selected automation dots, so clips and
            // dots move together; a plain click elsewhere starts a new selection.
            const bool keepDots = ci >= 0 && ((io.KeyCtrl || io.KeyShift) || g_selClips.count(g_proj.tracks[(size_t)ti].clips[(size_t)ci].id));
            if (!keepDots) { g_autoSel.clear(); g_autoSelMulti.clear(); }
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
                    g_dragDots = allSelectedDots();
                    g_clipAutomationMove.capture(g_proj, g_selClips, &g_dragDots);
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
                if (trackFrozen(lane)) {   // frozen: the lane is locked, no new clips (the double-click ran on a lane that froze since)
                    g_marqDblKind = 0;
                } else if (g_marqDblKind == 1 && hasClips(lane)) {
                    // new clip = new, independent instrument (own name, code copy, color, MIDI)
                    if (lane.kind == 7) {
                        // audio lane: the new clip asks for its file (canceled: no clip is added)
                        Clip ac = makeClip(lane, g_marqT, 2.0, "");
                        if (initAudioClip(ac)) {
                            lane.clips.push_back(ac);
                            g_selTrack = g_marqLane; g_selClip = (int)lane.clips.size() - 1;
                            g_selClips.clear();
                            g_selClips.insert(lane.clips.back().id);
                        }
                    } else {
                    std::string code = lane.code;
                    if (g_selTrack == g_marqLane && g_selClip >= 0 && g_selClip < (int)lane.clips.size()) code = lane.clips[(size_t)g_selClip].code;
                    lane.clips.push_back(makeClip(lane, g_marqT, 2.0, code));
                    g_selTrack = g_marqLane; g_selClip = (int)lane.clips.size() - 1;
                    g_selClips.clear();
                    g_selClips.insert(lane.clips.back().id);
                    }
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
            g_dragDots.clear();
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
                    g_proj.tracks[(size_t)nt].kind != 7 && g_proj.tracks[(size_t)g_selTrack].kind != 7 &&
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
            if (!g_dragOrig.empty() || !g_dragDots.empty()) {
                ClipAutomationMove::DotSelection moved;
                const int primaryTrack = g_autoSelTrackId; const std::string primaryChannel = g_autoSelChannel;
                g_clipAutomationMove.apply(g_proj, dt, nullptr, &moved);
                if (!g_dragDots.empty()) setAllSelectedDots(moved, primaryTrack, primaryChannel);
            }
        } else if (g_drag == Drag::ResizeLeft && g_selTrack >= 0 && g_selTrack < nT && g_selClip >= 0 &&
                   g_selClip < (int)g_proj.tracks[(size_t)g_selTrack].clips.size()) {
            Clip& c = g_proj.tracks[(size_t)g_selTrack].clips[(size_t)g_selClip];
            const double ns = std::min(g_trimEnd - 0.001, std::max(0.0, xt(m.x)));
            const double delta = ns - g_trimStart;
            c.start = ns;
            c.dur = g_trimEnd - ns;
            if (g_proj.tracks[(size_t)g_selTrack].kind == 7) audioTrimLeft(c, delta);   // the source follows the edge
            (void)delta;
        } else if (g_drag == Drag::Resize && g_selTrack >= 0 && g_selTrack < nT && g_selClip >= 0 &&
                   g_selClip < (int)g_proj.tracks[(size_t)g_selTrack].clips.size()) {
            Clip& c = g_proj.tracks[(size_t)g_selTrack].clips[(size_t)g_selClip];
            c.dur = std::max(0.001, clampTimelineTime(xt(m.x) - c.start));
            if (g_proj.tracks[(size_t)g_selTrack].kind == 7) audioCapRight(c);   // not beyond the source the clip holds
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
            if (t.kind != 7) {   // sample clips draw their texts on top of the shape instead
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
            }
            if (t.kind == 7 && !c.audioFile.empty()) {   // sample clip: the shape fills the whole clip, the texts sit on top of it
                const float vx0 = a.x + S(2), vx1 = b.x - S(2);
                const float vy0 = a.y + S(2), vy1 = b.y - S(2);
                if (vx1 - vx0 > S(10) && vy1 - vy0 > S(10)) {
                    std::string err;
                    const cawaudio::AudioHandle data = audioClipData(c, err);
                    if (data && !data->samples.empty()) {
                        const int w = (int)(vx1 - vx0), h = (int)(vy1 - vy0);
                        long long key = 0x51DB;
                        auto hm = [&](long long x) { key ^= x + 0x9e3779b97f4a7c15LL + (key << 6) + (key >> 2); };
                        hm((long long)std::hash<std::string>{}(c.audioFile));
                        hm((long long)cawaudio::fileStamp(c.audioFile));
                        hm((long long)(c.audioRate * 1000.0));
                        hm(c.audioReverse ? 11 : 3);
                        hm((long long)(c.audioGain * 1000.0));   // the code's gain: the shape amplitude
                        hm((long long)(c.dur * 1000.0));
                        hm((long long)data->samples.size());
                        hm(w); hm(h); hm(c.audioView);
                        hm((long long)clipRgb);   // the spectrogram colors are cached: the clip color is part of the key
                        const auto& v = cawaudio::viewFor(c.id, key, data, c, w, h, g_pps, clipRgb);
                        auto drawWave = [&]() {   // Cubase / Ableton style: a dark body, the wave solid in the clip color
                            const float mid = 0.5f * (vy0 + vy1), amp = 0.5f * (vy1 - vy0) - S(1);
                            const int cr = (int)((clipRgb >> 16) & 0xFF), cg = (int)((clipRgb >> 8) & 0xFF), cb = (int)(clipRgb & 0xFF);
                            dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, vy1), IM_COL32((int)(cr * 0.26f), (int)(cg * 0.26f), (int)(cb * 0.26f), 255));
                            dl->AddLine(ImVec2(vx0, mid), ImVec2(vx1, mid), IM_COL32(255, 255, 255, 40));
                            const int cols = (int)v.wave.size();
                            if (cols <= 0) return;
                            const int step = std::max(1, cols / 1100);
                            for (int col = 0; col < cols; col += step) {
                                const float fx = vx0 + (float)col / (float)cols * (vx1 - vx0);
                                const cawaudio::WaveColumn& wc = v.wave[(size_t)col];
                                if (wc.hi <= wc.lo + 1e-5f) continue;
                                const float yHi = mid - wc.hi * amp, yLo = mid - wc.lo * amp;
                                dl->AddRectFilled(ImVec2(fx, yHi), ImVec2(fx + (vx1 - vx0) / (float)cols * step + 0.6f, yLo), IM_COL32(cr, cg, cb, 255));
                                // slightly brighter crest lines at the extremes, like the DAWs
                                const int br = std::min(255, cr + 60), bg = std::min(255, cg + 60), bb = std::min(255, cb + 60);
                                dl->AddLine(ImVec2(fx, yHi), ImVec2(fx + (vx1 - vx0) / (float)cols * step, yHi), IM_COL32(br, bg, bb, 255), S(1));
                                dl->AddLine(ImVec2(fx, yLo), ImVec2(fx + (vx1 - vx0) / (float)cols * step, yLo), IM_COL32(br, bg, bb, 255), S(1));
                            }
                        };
                        auto drawSpectro = [&]() {   // Audacity style: black background, gapless cells, colors precomputed in the view cache
                            if (v.spectroRgba.empty() || v.spCols <= 0 || v.spRows <= 0) return;
                            dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, vy1), IM_COL32(10, 10, 12, 255));
                            const float cw = (vx1 - vx0) / (float)v.spCols, chh = (vy1 - vy0) / (float)v.spRows;
                            // draw only the columns inside the clip rect (the timeline clips the rest)
                            const float cl0 = dl->GetClipRectMin().x, cl1 = dl->GetClipRectMax().x;
                            const int colA = std::max(0, (int)std::floor((cl0 - vx0) / cw) - 1);
                            const int colB = std::min(v.spCols - 1, (int)std::ceil((cl1 - vx0) / cw) + 1);
                            for (int col = colA; col <= colB; ++col) {
                                const float x = vx0 + (float)col * cw;
                                const uint32_t* cell = v.spectroRgba.data() + (size_t)col;
                                for (int r = 0; r < v.spRows; ++r) {
                                    const uint32_t rgba = cell[(size_t)r * (size_t)v.spCols];
                                    if (rgba == 0) continue;   // below the range: the dark backing shows
                                    const float y = vy0 + (float)r * chh;
                                    dl->PrimReserve(6, 4);   // PrimRect writes directly: the caller reserves first (this was the crash)
                                    dl->PrimRect(ImVec2(x - 0.3f, y - 0.3f), ImVec2(x + cw + 0.7f, y + chh + 0.7f), rgba);
                                }
                            }
                        };
                        if (c.audioView == 1) drawSpectro();
                        else drawWave();
                        if (!c.audioGainPts.empty()) {   // volume automation: the gain curve over the shape (0..2, unity = middle)
                            std::vector<ImVec2> poly;
                            for (const AutoPoint& p : c.audioGainPts) {
                                const float px = vx0 + (float)std::max(0.0, std::min(c.dur, p.t)) / std::max(1e-9, c.dur) * (vx1 - vx0);
                                const float py = vy1 - (float)std::max(0.0, std::min(2.0, p.v)) / 2.f * (vy1 - vy0);
                                poly.push_back(ImVec2(px, py));
                            }
                            if (poly.size() >= 2) dl->AddPolyline(poly.data(), (int)poly.size(), IM_COL32(250, 190, 60, 255), 0, S(1.5f));
                            for (const ImVec2& p : poly) { dl->AddCircleFilled(p, S(3), IM_COL32(250, 190, 60, 255)); dl->AddCircle(p, S(4.2f), IM_COL32(20, 20, 24, 255), 0, S(1.2f)); }
                        }
                        // the title and description ON TOP of the shape, on a soft backing so they stay readable
                        {
                            const std::string label = c.runs > 1 ? c.name + "  x" + std::to_string(c.runs) : c.name;
                            const float bw = std::min(vx1 - vx0, (float)label.size() * S(7) + S(14));
                            dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + bw, a.y + S(20)), IM_COL32(10, 10, 12, 130), S(4));
                            dl->AddText(ImVec2(a.x + S(7), a.y + S(3)), IM_COL32(255, 255, 255, 240), label.c_str());
                            std::string desc2 = clipDescription(c);
                            char info2[96];
                            if (desc2.empty()) {
                                std::snprintf(info2, sizeof info2, "%.2f s - %.2f s", c.start, c.start + c.dur);
                                desc2 = info2;
                            }
                            const float bw2 = std::min(vx1 - vx0, (float)desc2.size() * S(6) + S(14));
                            dl->AddRectFilled(ImVec2(a.x, a.y + S(20)), ImVec2(a.x + bw2, a.y + S(38)), IM_COL32(10, 10, 12, 110), 0);
                            dl->AddText(ImVec2(a.x + S(7), a.y + S(22)), IM_COL32(255, 255, 255, 170), desc2.c_str());
                        }
                    }
                }
            }
            if (c.muted) dl->AddRectFilled(a, b, IM_COL32(10, 10, 12, 150), S(4));
            dl->PopClipRect();
            if (x1 - x0 > S(46)) {   // small M button: mute this clip
                const ImVec2 ma(b.x - S(21), a.y + S(3)), mb(b.x - S(5), a.y + S(19));
                dl->AddRectFilled(ma, mb, c.muted ? IM_COL32(190, 60, 52, 255) : IM_COL32(0, 0, 0, 110), S(3));
                const ImVec2 ms = ImGui::CalcTextSize("M");   // the letter sits centered in the chip (x and y)
                dl->AddText(ImVec2((ma.x + mb.x) * 0.5f - ms.x * 0.5f, (ma.y + mb.y) * 0.5f - ms.y * 0.5f), IM_COL32(255, 255, 255, c.muted ? 255 : 170), "M");
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
                const ImVec2 ms = ImGui::CalcTextSize("M");   // the letter sits centered in the chip (x and y)
                dl->AddText(ImVec2((ma.x + mb.x) * 0.5f - ms.x * 0.5f, (ma.y + mb.y) * 0.5f - ms.y * 0.5f), IM_COL32(255, 255, 255, mc.muted ? 255 : 170), "M");
            }
        }
    }
    {   // TRACK FREEZE: frozen lanes draw dimmed and untouchable; the rendered WAV shows as one icy clip with its waveform
        for (int i = 0; i < nT; ++i) {
            if (hiddenTrack(i)) continue;
            const Track& t = g_proj.tracks[(size_t)i];
            if (!trackFrozen(t)) continue;
            const float y0 = yTop[(size_t)i], y1 = baseBottom[(size_t)i];
            if (y1 < bodyMin.y || y0 > bodyMax.y) continue;
            // the frozen WAV as a clip-shaped range with a small waveform (the sound that actually plays)
            if (t.freezeEnd > t.freezeStart) {
                float fx0 = tx(t.freezeStart), fx1 = tx(t.freezeEnd);
                if (fx1 >= bodyMin.x && fx0 <= bodyMax.x) {
                    fx0 = std::max(fx0, bodyMin.x);
                    fx1 = std::min(fx1, bodyMax.x);
                    const float fy0 = y0 + S(4), fy1 = y1 - S(4);
                    dl->AddRectFilled(ImVec2(fx0, fy0), ImVec2(fx1, fy1), IM_COL32(120, 170, 210, 55), S(4));
                    dl->AddRect(ImVec2(fx0, fy0), ImVec2(fx1, fy1), IM_COL32(150, 200, 235, 190), S(4), 0, S(1.2f));
                    Clip fc;                                   // only the path matters: the loader caches by file
                    fc.audioFile = t.freezePath;
                    std::string err;
                    const auto data = audioClipData(fc, err);
                    if (data && data->channels > 0 && !data->samples.empty()) {
                        const int ch = data->channels;
                        const size_t frames = data->samples.size() / (size_t)ch;
                        const double span = t.freezeEnd - t.freezeStart;
                        const float midY = (fy0 + fy1) * 0.5f;
                        const float half = std::max(S(2.f), (fy1 - fy0) * 0.5f - S(3.f));
                        for (float x = fx0 + 1.f; x < fx1; x += 2.f) {
                            double tt = xt(x);
                            tt = std::max(t.freezeStart, std::min(t.freezeEnd - 1e-6, tt));
                            const double pos = (tt - t.freezeStart) / span * (double)frames;
                            const size_t f0 = (size_t)std::max(0.0, pos);
                            const size_t f1 = std::min(frames, f0 + 256);
                            float mn = 1.f, mx = -1.f;
                            for (size_t f = f0; f < f1; ++f)
                                for (int c = 0; c < std::min(ch, 2); ++c) {
                                    const float v = data->samples[f * (size_t)ch + (size_t)c];
                                    mn = std::min(mn, v);
                                    mx = std::max(mx, v);
                                }
                            if (mn > mx) continue;
                            dl->AddLine(ImVec2(x, midY - mx * half), ImVec2(x, midY - mn * half), IM_COL32(165, 210, 240, 230), S(1.2f));
                        }
                    }
                    {   // the LOCKED caption sits centered in the icy range (only when the range is wide enough)
                        const ImVec2 ls = ImGui::CalcTextSize("LOCKED");
                        if (fx1 - fx0 > ls.x + S(12))
                            dl->AddText(ImVec2((fx0 + fx1) * 0.5f - ls.x * 0.5f, (fy0 + fy1) * 0.5f - ls.y * 0.5f), IM_COL32(175, 215, 245, 235), "LOCKED");
                    }
                }
            }
            // the untouchable look: a cool translucent veil over the whole lane (clips, automations, MIDI lane)
            dl->AddRectFilled(ImVec2(bodyMin.x, y0), ImVec2(bodyMax.x, y1), IM_COL32(120, 165, 205, 30));
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
        {   // the engine of the track: a faint tint over the header and a solid strip beside the track color
            const uint32_t er = engineRgb(t.kind);
            const int ecr = (int)((er >> 16) & 0xFF), ecg = (int)((er >> 8) & 0xFF), ecb = (int)(er & 0xFF);
            dl->AddRectFilled(ImVec2(r0.x + S(5), r0.y), r1, IM_COL32(ecr, ecg, ecb, 22));
            dl->AddRectFilled(ImVec2(r0.x + S(5), r0.y), ImVec2(r0.x + S(9), r1.y), IM_COL32(ecr, ecg, ecb, 235));
            if (ImGui::IsMouseHoveringRect(ImVec2(r0.x, r0.y), ImVec2(r0.x + S(9), r1.y)) && ImGui::IsWindowHovered()) helpTip("%s", engineName(t.kind));
        }
        if (t.muted) dl->AddRectFilled(ImVec2(r0.x + S(6), r0.y), ImVec2(r1.x, r1.y), IM_COL32(10, 10, 10, 34));
        if (trackFrozen(t))   // frozen: the header is tinted icy blue; the LOCKED caption is drawn right of the controls (below, next to the meter)
            dl->AddRectFilled(r0, r1, IM_COL32(110, 165, 205, 26));
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
        // VST tracks: the width of the plugin that is loaded NOW (the engine's layout was taken at Run,
        // before a later-loaded 4-out plugin existed). Their peaks come by track id, not by layout index.
        const int liveVstCh = (t.kind == 3 || t.kind == 4) && vstTrackOutputChannels ? std::max(1, std::min(8, vstTrackOutputChannels(t.id))) : 0;
        for (const LaneMeterInfo& lm : (g_liveCsound ? g_engineMeters : g_idleMeters))
            if (lm.trackId == t.id) { meterReserve = (liveVstCh > 0 ? liveVstCh : lm.channels) * S(8) + S(12); break; }
        if (t.kind == 5) meterReserve = 2 * S(8) + S(12);
        if (t.kind == 7) {   // sample tracks: one meter per output channel the clips use
            const auto ch = g_sampleMeterCh.find(t.id);
            meterReserve = (float)(ch == g_sampleMeterCh.end() ? 2 : ch->second) * S(8) + S(12);
        }
        {
            const std::vector<LaneMeterInfo>& layout = g_liveCsound ? g_engineMeters : g_idleMeters;
            for (const LaneMeterInfo& lm : layout) {
                if (lm.trackId != t.id) continue;
                const float barW = S(6), barGap = S(2);
                const int nBars = liveVstCh > 0 ? liveVstCh : lm.channels;
                const float totalW = nBars * barW + (nBars - 1) * barGap;
                const float mx1 = r1.x - S(8), mx0 = mx1 - totalW;
                const float top = y0 + S(5), bot = y1 - S(5);
                if (bot - top > S(4)) {
                    for (int c = 0; c < nBars; ++c) {
                        const float bx0 = mx0 + c * (barW + barGap), bx1 = bx0 + barW;
                        const size_t idx = (size_t)(lm.first + c);
                        float db = (g_liveCsound && idx < g_meterDb.size()) ? g_meterDb[idx] : -90.f;
                        bool clipped = idx < g_meterClipHold.size() && g_meterClipHold[idx] > 0.f;
                        if(t.kind==6 && c<8) {
                            auto it=g_pdMeterDb.find(t.id);
                            db=it==g_pdMeterDb.end()?-90.f:it->second[(size_t)c];clipped=db>=0.f;
                        }
                        const int vstMaxCh = 8;   // one bar per plugin output channel in both output modes
                        if ((t.kind == 3 || t.kind == 4) && c >= lm.channels) { db = -90.f; clipped = false; }   // beyond the Run-time layout: never read the next track's meter slot
                        if((t.kind==3 || t.kind==4) && (g_vstEngineOn || g_liveCsound) && c<vstMaxCh) {   // VST tracks: always by TRACK ID (both racks publish per-track peaks) - never by a slot index that a width change shifts
                            auto it=g_vstMeterDb.find(t.id);
                            db=it==g_vstMeterDb.end()?-90.f:it->second[(size_t)c];clipped=db>=0.f;
                        }
                        bool grayMeter = lm.csoundOnly || trackConsumedByMasterVste(g_proj, t);   // a master-mode (replace) VSTe suppresses this source's own master output: its meter goes gray
                        if (trackFrozen(t)) {   // locked: the WAV voice rides the sample engine - the level is live, gray like a consumed source (the engines behind the lock are stopped)
                            auto sit = g_sampleMeterDb.find(t.id);
                            db = (sit != g_sampleMeterDb.end() && c < (int)sit->second.size()) ? sit->second[(size_t)c] : -100.f;
                            clipped = db >= 0.f;
                            grayMeter = true;
                        }
                        drawLedMeter(dl, bx0, bx1, top, bot, db, clipped, grayMeter);
                    }
                }
                break;
            }
        }
        if (t.kind == 5) {
            const auto it = g_scMeterDb.find(t.id);
            const int channels = scTrackMeterChannels(t);   // the meter follows the clip code's channel count
            const float barW = S(6), gap = S(2), totalW = (float)channels * barW + (float)(channels - 1) * gap;
            const float mx1 = r1.x - S(8), mx0 = mx1 - totalW;
            const float top = y0 + S(5), bot = y1 - S(5);
            if (it == g_scMeterDb.end()) {
                g_scMeterDb[t.id].fill(-100.f);
            }
            std::array<float, ScEngine::slotStride> values{};
            values.fill(-100.f);
            if (it != g_scMeterDb.end()) values = it->second;
            if (trackFrozen(t)) {   // locked: the WAV voice rides the sample engine - live level, gray like a consumed source
                const auto sit = g_sampleMeterDb.find(t.id);
                for (int channel = 0; channel < 2; ++channel)
                    values[(size_t)channel] = (sit != g_sampleMeterDb.end() && channel < (int)sit->second.size()) ? sit->second[(size_t)channel] : -100.f;
            }
            if (bot - top > S(4)) {
                for (int channel = 0; channel < channels; ++channel) {
                    const float x0 = mx0 + (float)channel * (barW + gap);
                    const float x1 = x0 + barW;
                    drawLedMeter(dl, x0, x1, top, bot, values[(size_t)channel],
                                 values[(size_t)channel] >= 0.f,
                                 trackFrozen(t) || trackConsumedByMasterVste(g_proj, t));   // FX only: a consumed source's meter is gray like the other source tracks
                }
                ImGui::PushID(700000 + i);
                ImGui::SetCursorScreenPos(ImVec2(mx0, top));
                ImGui::InvisibleButton("##sc_engine_meter", ImVec2(totalW, bot - top));
                if (ImGui::IsItemHovered())
                    helpTip("Live per-channel peaks for this SC track. The meter follows the clip code's channel count (Out.ar arrays / ! N, up to %d channels). CAW routes the common ~cawgroup / Out.ar(0, ...) patterns to this track's isolated bus.", ScEngine::slotStride);
                ImGui::PopID();
            }
        }

        if (t.kind == 7) {
            const auto it = g_sampleMeterDb.find(t.id);
            const auto chIt = g_sampleMeterCh.find(t.id);
            const int channels = chIt == g_sampleMeterCh.end() ? 2 : std::max(1, chIt->second);
            const float barW = S(6), gap = S(2), totalW = (float)channels * barW + (float)(channels - 1) * gap;
            const float mx1 = r1.x - S(8), mx0 = mx1 - totalW;
            const float top = y0 + S(5), bot = y1 - S(5);
            if (bot - top > S(4)) {
                for (int channel = 0; channel < channels; ++channel) {
                    const float x0 = mx0 + (float)channel * (barW + gap);
                    const float x1 = x0 + barW;
                    float db = -100.f;
                    if (it != g_sampleMeterDb.end() && channel < (int)it->second.size()) db = it->second[(size_t)channel];
                    // FX only (master-replace) VSTe consumes this sample track: its own master output is replaced, so the meter goes gray (like the other source tracks).
                    // A locked track is gray too: the WAV voice rides the sample engine, the engines behind the lock are stopped.
                    drawLedMeter(dl, x0, x1, top, bot, db, db >= 0.f, trackConsumedByMasterVste(g_proj, t) || trackFrozen(t));
                }
                ImGui::PushID(700000 + i);
                ImGui::SetCursorScreenPos(ImVec2(mx0, top));
                ImGui::InvisibleButton("##sample_meter", ImVec2(totalW, bot - top));
                if (ImGui::IsItemHovered())
                    helpTip("Live peaks for this sample track, one bar per output channel (as the source, or the clip's custom routing).");
                ImGui::PopID();
            }
        }

        // frozen tracks: the LOCKED caption takes its own reserve between the controls and the meter
        // (meterReserve feeds the name width and rowRight, so nothing can slide under the caption)
        if (trackFrozen(t)) {
            meterReserve += ImGui::CalcTextSize("LOCKED").x + S(12);
            const ImVec2 ls = ImGui::CalcTextSize("LOCKED");
            dl->AddText(ImVec2(r1.x - meterReserve + S(6), (y0 + y1) * 0.5f - ls.y * 0.5f), IM_COL32(150, 200, 235, 190), "LOCKED");
        }

        const float fh = ImGui::GetFrameHeight();
        const float rowY1 = y0 + S(6);                              // row-1 top (the name input sits here)
        // Header controls use Button (never SmallButton - its label pins to the text baseline). The
        // strip's height is the project's t.height (y1 = yTop + S(t.height)) while the widgets' natural
        // height comes from the ImGui style (fh) - two different sources - so the buttons take what the
        // strip actually offers (never under one text line; taller tracks grow them back up to fh).
        const float row2Y = y0 + S(6) + fh + S(4);
        const float row2H = std::max(ImGui::GetFontSize(), std::min(fh, y1 - row2Y - S(4)));
        // ---- exact vertical label centering ----
        // ImGui cuts every text origin to the whole-pixel grid (ImFont::RenderText does IM_TRUNC on pos;
        // InputText draws its label at frame_bb.Min + FramePadding and the same truncation follows), so
        // a fractional FramePadding.y can never land a label dead-center: the sub-pixel part of
        // top + padding is simply cut off - that is why the labels never sat perfectly in center. The
        // fix: every control gets an integer top, an INTEGER FramePadding.y and an explicit height, so
        // top + padding is an exact integer (the truncation then changes nothing) and the label's ink
        // band - measured from the real glyphs (ImFontGlyph Y0/Y1 are text-top-relative) - lands where
        // it is aimed, with no rounding jitter from one track to the next.
        auto inkBand = [](const char* label, float& y0min, float& y1max) {
            ImFont* fnt = ImGui::GetFont();
            y0min = 1e9f; y1max = -1e9f;
            for (const char* p = label; *p && !(p[0] == '#' && p[1] == '#'); ++p)
                if (*p != ' ') {
                    const ImFontGlyph* g = fnt->FindGlyph((ImWchar)*p);
                    y0min = std::min(y0min, g->Y0);
                    y1max = std::max(y1max, g->Y1);
                }
            if (y0min > y1max) { y0min = fnt->Ascent - fnt->FontSize * 0.72f; y1max = fnt->Ascent; }   // no ink: cap-height fallback
        };
        float inkA0 = 0.f, inkA1 = 0.f;
        inkBand("A", inkA0, inkA1);                          // the cap band shared by M, S, LKD, SCO, III and the dB digits
        const float inkMidCap = (inkA0 + inkA1) * 0.5f;      // ink center below the text top; the row's label-line reference
        const float rowY1q = std::floor(rowY1 + 0.5f);       // whole-pixel tops: with integer paddings the text origins stay exact
        const float row2Yq = std::floor(row2Y + 0.5f);
        const float nameLabelTop = rowY1q + std::floor(ImGui::GetStyle().FramePadding.y);   // where the name's label really renders (top + padding, truncated)
        const float m2 = std::round((row2H - 2.f * inkMidCap) * 0.5f);       // integer row-2 padding: the cap ink lands dead-center
        const float row2Hq = 2.f * inkMidCap + 2.f * m2;                     // the exact ink-centered box height (row2H +- 1px)
        // the panel hides elements as it gets narrower; the meter stays, the name gives way
        // row 1 = name, III (VSTi / VSTe), volume; row 2 = (+), M, S, LKD, SCO
        const float rowRight = r1.x - meterReserve;   // the meter's (and the LOCKED caption's) left edge (+ a small gap)
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
            // row 1: the name, then Show (VSTi / VSTe only), then the volume box
            ImGui::SetCursorScreenPos(ImVec2(r0.x + S(16), rowY1q));   // whole-pixel top: the name's label origin is exact
            ImGui::SetNextItemWidth(nameW);
            const bool lockedName = trackFrozen(t);
            if (lockedName) ImGui::BeginDisabled();   // frozen: the track is locked (the name is part of what was rendered)
            if (inputStr("##name", t.name, 0, nameFilter)) g_selTrack = i;
            if (!ImGui::IsItemActive()) ensureUniqueInstrumentName(i);
            if (lockedName) ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) helpTip(t.kind == 2 ? "Name of the graph (its lane name in the score). Right-click the track for more." : t.kind == 1 ? "Name of the marker track. Right-click the track for more." : "Lane name. Every clip on the lane is its own instr (right-click a clip to rename it). Right-click the track for more.", t.name.c_str());
        }
        // row 1 continues right of the name. Each element fits independently (no cascade): a narrow
        // panel drops the Show button first, the volume stays as long as there is any room for it.
        // (prevRight / rowFull are the shared variables captured by fitsRow above.)
        prevRight = r0.x + S(16) + nameW;   // right edge of the last placed row-1 element
        if ((t.kind == 3 || t.kind == 4) && prevRight + S(8) + smallBtnW("III", S(7)) <= rowRight) {   // VSTi / VSTe: open the plugin window again
            ImGui::SetCursorScreenPos(ImVec2(std::floor(prevRight + S(8) + 0.5f), nameLabelTop - m2));   // integer top + integer padding: the label lands on the name's own label line
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(S(7)), m2));   // Button (not SmallButton): the label centers vertically
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.f));   // top-aligned text line: ImGui does not re-center it, so the ink lands exactly on the box center (padding is the integer ink offset)
            const bool winOpen = [&] {   // a plugin window of the track (effect 1 or a chain stage) is open: the button stays on
                for (int st = 0; st <= t.vstChainCount(); ++st) {
                    const auto it = vstui::slots().find(vstui::slotKey(t.id, st));
                    if (it != vstui::slots().end() && it->second.open) return true;
                }
                return false;
            }();
            bool btnOn = false;
            if (winOpen) {
                const uint32_t er = engineRgb(t.kind);
                btnOn = true;
                pushBtnOn(ImVec4(((er >> 16) & 0xFF) / 255.f, ((er >> 8) & 0xFF) / 255.f, (er & 0xFF) / 255.f, 1.f));
            }
            const bool showClicked = ImGui::Button("III##vst", ImVec2(0.f, row2Hq));   // explicit height: the cap ink sits exactly on the box center
            if (btnOn) ImGui::PopStyleColor(3);
            prevRight = ImGui::GetItemRectMax().x;
            ImGui::PopStyleVar(2);
            if (showClicked) vstui::openWindow(t.id);
            if (ImGui::IsItemHovered()) helpTip(winOpen ? "The plugin window is open - click to bring it up again" : "Open the plugin window: pick the 8 parameters, show the plugin interface");
        }
        {   // volume number box in whole dB: double-click to type. 0 dB = unity; stored as linear gain.
            const float volW = S(52);
            if (prevRight + S(8) + volW <= rowRight) {
                const float volPadY = std::round((row2H - ImGui::GetFontSize()) * 0.5f);   // integer: frame height (font + 2*pad) matches row2H
                ImGui::SetCursorScreenPos(ImVec2(std::floor(prevRight + S(8) + 0.5f), nameLabelTop - volPadY));   // integer top: the digits land on the name's own label line
                const float before = t.volume;
                const float dbf = t.volume <= 0.0005f ? -60.f : (float)(20.0 * std::log10(std::max(0.0005, (double)t.volume)));
                int dbi = (int)std::lround(std::max(-60.f, dbf));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, volPadY));
                if (dbIntBox("##trackvol_db", dbi, -60, kMaxVolumeDb, volW, " dB"))   // whole dB, typed (double-click, then type); -60 = silence, no practical upper limit
                    t.volume = dbi <= -60 ? 0.f : (float)std::pow(10.0, (double)dbi / 20.0);
                if (!std::isfinite(t.volume) || t.volume < 0.f) t.volume = 0.f;
                if (std::fabs(t.volume - before) > 1e-6f) {
                    if (t.kind == 7 || trackFrozen(t)) g_audioVoiceSig.clear();   // sample voices rebuild from the signature on the next tick (frozen tracks carry the fader live)
                    if (g_vstEngineOn && trackIsVst(t.id)) vstSolo().setTrackVolume(t.id, t.volume);   // the VST fader is live: applied at the master join on the next block
                }
                prevRight = ImGui::GetItemRectMax().x;
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered()) helpTip("Track volume in whole dB: double-click, then type (-60 = silence, 0 = unity, no upper limit). Applies to the sample engine voices and the VSTi / VSTe output at the master join; the volume lane multiplies on top.");
            }
        }
        {   // the engine tag (CSD / VSTi / VSTe / SC / PD / SMP ...): a small pill in the engine color, only where there is room
            const char* tag = engineTag(t.kind);
            const float tagScale = 0.78f;
            const ImVec2 ts = ImGui::CalcTextSize(tag);
            const float tw = ts.x * tagScale + S(10), th = ts.y * tagScale + S(4);
            if (prevRight + S(8) + tw <= rowRight) {
                const uint32_t er = engineRgb(t.kind);
                const float px = std::floor(prevRight + S(8) + 0.5f);
                const float py = std::floor(rowY1q + (fh - th) * 0.5f + 0.5f);
                dl->AddRectFilled(ImVec2(px, py), ImVec2(px + tw, py + th), IM_COL32((er >> 16) & 0xFF, (er >> 8) & 0xFF, er & 0xFF, 215), S(3));
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * tagScale, ImVec2(px + S(5), py + S(2)), IM_COL32(18, 18, 22, 255), tag);
            }
        }
        // row 2: (+) lanes, M, S, LKD (freeze / lock), sco - left to right; each one shows only if it
        // still ends before the meter, so they vanish one after another from the right
        prevRight = r0.x + S(18);
        rowFull = false;   // restart the fit cascade for this row
        if (r0.x + S(18) + smallBtnW("+", gst.FramePadding.x) <= rowRight) {
            const float plusW = smallBtnW("+", gst.FramePadding.x);   // same width the font "+" button had
            ImGui::SetCursorScreenPos(ImVec2(std::floor(r0.x + S(18) + 0.5f), row2Yq));
            // one (+) icon only: it opens the list that adds the automation lanes and the MIDI track.
            // (the old fold arrow was removed - the - on a lane removes the lane, so the pair covers everything)
            // the + works for every track kind: SuperCollider reads lanes with ~cawget, Pure Data with [r $0-cawget-name]
            // The + is DRAWN (two strokes) instead of printed from the font: a font glyph's ink box is rounded
            // to whole pixels (and the font scale moves it), so a symbol this small never sat exactly in the middle.
            const bool plusClicked = ImGui::Button("##plus", ImVec2(plusW, row2Hq));
            {
                const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
                const float span = std::max(4.f, std::round(ImGui::GetFontSize() * 0.5f));   // total arm length
                const float th = (((int)std::round(bmax.y - bmin.y)) % 2 == 0) ? 2.f : 1.f;   // stroke thickness: even box height -> 2px, odd -> 1px (the stroke then centers on whole pixels)
                const float cx = (bmin.x + bmax.x) * 0.5f, cy = (bmin.y + bmax.y) * 0.5f;
                const float x0 = std::round(cx - span * 0.5f), y0p = std::round(cy - th * 0.5f);
                const float cxi = std::round(cx - th * 0.5f), y0v = std::round(cy - span * 0.5f);
                const ImU32 pc = ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddRectFilled(ImVec2(x0, y0p), ImVec2(x0 + span, y0p + th), pc);        // horizontal stroke
                dl->AddRectFilled(ImVec2(cxi, y0v), ImVec2(cxi + th, y0v + span), pc);      // vertical stroke
            }
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
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(gst.FramePadding.x), m2));   // Button (not SmallButton): the label centers vertically
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.f));   // top-aligned text line: ImGui does not re-center it, so the ink lands exactly on the box center (padding is the integer ink offset)
                const bool mClicked = ImGui::Button("M", ImVec2(0.f, row2Hq));
                ImGui::PopStyleVar(2);
                prevRight = ImGui::GetItemRectMax().x;
                if (mClicked) { t.muted = !wasMuted; if (t.muted) t.solo = false; pushVstActiveLive(); }   // mute and solo are exclusive; push live so VSTe tracks react without a restart
                if (wasMuted) ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(t.muted ? "Unmute track" : "Mute track (turns solo off)");
            }
            if (fitsRow(smallBtnW("S", gst.FramePadding.x))) {
                ImGui::SameLine(0, S(8));
                const bool wasSolo = t.solo;
                if (wasSolo) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(gst.FramePadding.x), m2));   // Button (not SmallButton): the label centers vertically
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.f));   // top-aligned text line: ImGui does not re-center it, so the ink lands exactly on the box center (padding is the integer ink offset)
                const bool sClicked = ImGui::Button("S", ImVec2(0.f, row2Hq));
                ImGui::PopStyleVar(2);
                prevRight = ImGui::GetItemRectMax().x;
                if (sClicked) { t.solo = !wasSolo; if (t.solo) t.muted = false; pushVstActiveLive(); }   // mute and solo are exclusive; push live so VSTe tracks react without a restart
                if (wasSolo) ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(t.solo ? "Unsolo track" : "Solo track (turns mute off)");
            }
            if ((trackCanFreeze(t) || t.kind == 4) && fitsRow(smallBtnW("LKD", gst.FramePadding.x))) {   // freeze (Cubase-style lock): render the track (or the rack with its sources) into a WAV and lock it
                ImGui::SameLine(0, S(8));
                const bool wasFrozen = trackFrozen(t);
                if (wasFrozen) pushBtnOn(ImVec4(0.28f, 0.50f, 0.66f, 1.f));   // icy blue: the track is locked (untouchable)
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(gst.FramePadding.x), m2));   // Button (not SmallButton): the label centers vertically
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.f));   // top-aligned text line: ImGui does not re-center it, so the ink lands exactly on the box center (padding is the integer ink offset)
                const bool fClicked = ImGui::Button("LKD", ImVec2(0.f, row2Hq));
                ImGui::PopStyleVar(2);
                prevRight = ImGui::GetItemRectMax().x;
                if (fClicked && !freezeActive()) {
                    if (t.kind == 4) {   // the rack: its sound is the sources through the plugins - the dialog plans the render-and-lock
                        if (wasFrozen) freezeUnfreeze(i);
                        else { g_freezeVsteAsk = i; g_freezeVsteOpen = true; ImGui::OpenPopup("##freeze_vste_ask"); }
                    } else if (wasFrozen) {
                        freezeUnfreeze(i);
                    } else {
                        // The freeze renders OFFLINE for kinds 0 / 3 / 5 / 6 / 7 (no live engine needed);
                        // kind 5 needs sclang.exe + scsynth.exe (scNrtAvailable logs why when missing).
                        // Real-time render mode (Settings > Audio): the LIVE engines are the renderer,
                        // so kind 5 needs the SC engine up instead of the NRT tools.
                        const char* warnEng = "";
                        bool down = false;
                        if (t.kind == 5 && (g_proj.cs.renderMode == 1 ? !g_scReady : !scNrtAvailable())) { down = true; warnEng = g_proj.cs.renderMode == 1 ? "SuperCollider (DSP)" : "SuperCollider"; }
                        else if (t.kind == 0 && !g_langCsound && (!engineIsUp() || !g_useCsoundApi)) { down = true; warnEng = "Csound (DSP)"; }
                        if (down) {
                            g_freezeWarnTrack = i;
                            g_freezeWarnOpen = true;
                            ImGui::OpenPopup("##freeze_need_engine");
                        } else {
                            freezeTrackBegin(i);   // falls back to the real-time capture with a log line if the offline host is unavailable
                        }
                    }
                }
                if (wasFrozen) ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(wasFrozen
                    ? (t.kind == 4
                        ? "Unlock the rack: delete the rendered WAV - the sources play live through the plugins again (you can edit them)"
                        : "Unlock the track: delete the rendered WAV - the engines play the track live again (you can edit it)")
                    : (t.kind == 4
                        ? "Lock the rack: render its current sources through the plugins into one WAV, without freezing the sources first. Much less CPU; the sources are muted until you unlock the rack."
                        : "Lock the track (freeze): render the whole track into a WAV file and lock it. The engines stop processing the track (much less CPU) and the WAV plays instead. The track cannot be edited until you unlock it. Mute / solo and the volume fader keep working."));
            }
            if (fitsRow(smallBtnW("SCO", S(7)))) {
                ImGui::SameLine(0, S(8));
                { ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(S(7)), m2));   // Button (not SmallButton): the label centers vertically
                  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.f));   // top-aligned text line: ImGui does not re-center it, so the ink lands exactly on the box center (padding is the integer ink offset)
                  const bool scoClicked = ImGui::Button("SCO##trk", ImVec2(0.f, row2Hq));   // explicit height: the cap ink sits exactly on the box center
                  prevRight = ImGui::GetItemRectMax().x;
                  if (scoClicked) ImGui::OpenPopup("track_score_popup");
                  ImGui::PopStyleVar(2); }
                if (ImGui::IsItemHovered()) helpTip(t.kind == 2 ? "Score: the lane of the whole graph. Give it the same name as another track and the graph is drawn in that track's lane (overlap or separated)." : "Score: the name of this track lane (the same name = the same lane) and how it shows");
                if (ImGui::BeginPopup("track_score_popup")) { drawTrackScoreMenu(t); ImGui::EndPopup(); }
            }
        }
        ImGui::PopID();
        if (ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseClicked(0) && edgeTrack < 0) g_selTrack = i;
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r0, r1) && ImGui::IsMouseClicked(1)) { g_ctxTrack = i; g_selTrack = i; ImGui::OpenPopup("##track_ctx"); }

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
                            // the dots follow the new range: a 1200 dot under a new maximum of 700 becomes 700 (what
                            // the lane shows is what plays). Values inside the range are left exactly as they were.
                            const auto dots = t.automation.find(channel);
                            if (dots != t.automation.end())
                                for (AutoPoint& d : dots->second) d.v = std::min(p.max, std::max(p.min, d.v));
                        }
                        if (!(p.max > p.min) || !(p.step > 0))
                            ImGui::TextDisabled("Maximum must exceed minimum, and step must be positive.");
                        ImGui::TextDisabled("Dots outside the new range are clamped to it. Restart Csound to apply range changes to playback.");
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
                    const ImFontGlyph* ga = ImGui::GetFont()->FindGlyph((ImWchar)'A');
                    const float inkMidCapL = (ga->Y0 + ga->Y1) * 0.5f;   // the cap band's ink center below the text top
                    const float scoPadX = std::round(ImGui::GetStyle().FramePadding.x);
                    const float scoW = ImGui::CalcTextSize("SCO").x + scoPadX * 2.f;   // "SCO" (caps) is wider than the old fixed slot: auto width, same right edge the S(34) button ended at
                    const float scoPadY = std::round((ImGui::GetFrameHeight() - 2.f * inkMidCapL) * 0.5f);   // integer padding + explicit height: the cap ink lands dead-center
                    const float scoHq = 2.f * inkMidCapL + 2.f * scoPadY;
                    ImGui::SetCursorScreenPos(ImVec2(std::floor(bodyMin.x - S(48) - scoW + 0.5f), std::floor(laneTop + S(2) + 0.5f)));
                    const auto sm = t.scoreAuto.find(channel);
                    const int smode = sm == t.scoreAuto.end() ? 1 : sm->second;
                    if (smode > 0) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(scoPadX, scoPadY));
                    if (ImGui::Button("SCO##auto", ImVec2(0.f, scoHq))) ImGui::OpenPopup("auto_score_popup");
                    ImGui::PopStyleVar();
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
                if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(la, lb) && ImGui::IsMouseClicked(1) && edgeTrack < 0 && edgeAutomationTrack < 0) {
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
            const ImFontGlyph* ga = ImGui::GetFont()->FindGlyph((ImWchar)'A');
            const float inkMidCapM = (ga->Y0 + ga->Y1) * 0.5f;   // the cap band's ink center below the text top
            const float midiScoPadX = std::round(ImGui::GetStyle().FramePadding.x);
            const float midiScoW = ImGui::CalcTextSize("SCO").x + midiScoPadX * 2.f;   // "SCO" (caps): auto width instead of the old S(34) slot
            const float midiScoPadY = std::round((ImGui::GetFrameHeight() - 2.f * inkMidCapM) * 0.5f);   // integer padding + explicit height: the cap ink lands dead-center
            const float midiScoH = 2.f * inkMidCapM + 2.f * midiScoPadY;
            if (bodyMin.x - S(46) - midiScoW >= midiTitleEnd) {
                ImGui::PushID(i);
                ImGui::SetCursorScreenPos(ImVec2(std::floor(bodyMin.x - S(46) - midiScoW + 0.5f), std::floor(midiTop + S(2) + 0.5f)));   // same right edge the S(34) button ended at; whole-pixel top
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(midiScoPadX, midiScoPadY));
                if (ImGui::Button("SCO##midi_score", ImVec2(0.f, midiScoH))) ImGui::OpenPopup("midi_score_popup");
                ImGui::PopStyleVar();
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

    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(hdrMin, hdrMax) && ImGui::IsMouseClicked(1) && m.y >= yTop[(size_t)nT] && !ImGui::IsPopupOpen("##track_ctx")) { g_ctxTrack = -1; ImGui::OpenPopup("##track_ctx"); }
    if (ImGui::BeginPopup("##track_ctx")) {
        // Every kind of track can be added whatever the session list says: an unchecked engine
        // is only not RUN (code it now, enable the engine in Settings > Engines to hear it).
        if (ImGui::MenuItem("Add Csound track")) { g_selTrack = addLangTrack(0); g_selClip = -1; }
        if (!g_langCsound && ImGui::IsItemHovered()) helpTip("Csound is not enabled in Settings > Engines: the track and its code work, Run will not start Csound.");
        if (ImGui::MenuItem("Add SuperCollider track")) { g_selTrack = addNonCsoundTrack(true); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("Double-click the lane to add a SuperCollider code clip. Start SC with the transport Run button, then press Play.");
        if (ImGui::MenuItem("Add Pure Data track")) { g_selTrack = addNonCsoundTrack(false); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("Double-click the track to create a PD clip, then open the clip and load a .pd patch.");
        ImGui::Separator();
        if (ImGui::MenuItem("Add VSTi")) { g_selTrack = addVstTrack(true); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("A VST3 instrument with MIDI input and parameter automation.");
        if (ImGui::MenuItem("Add VSTe")) { g_selTrack = addVstTrack(false); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("A VST3 effect with audio input and parameter automation.");
        if (ImGui::MenuItem("Add sample track")) { g_selTrack = addAudioTrack(); g_selClip = -1; }
        if (ImGui::IsItemHovered()) helpTip("Clips that hold an audio file (WAV, or anything FFmpeg reads). Double-click the lane to add one, or drag a file from Explorer onto the lane. Its CAW code drives length (rate), volume (gain), direction (reverse) and routing. Start it with the Run sample button.");
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
        if (trackFrozen(g_proj.tracks[(size_t)del])) {   // a frozen track takes its rendered WAV with it
            std::error_code rmEc;
            std::filesystem::remove(std::filesystem::u8path(g_proj.tracks[(size_t)del].freezePath), rmEc);
        }
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
            if (g_proj.tracks[(size_t)laneIndex].kind == 7) {   // sample clip: the sound settings right in the menu
                Clip& ac = *clip;
                if (ac.audioSegs.empty() && !ac.audioFile.empty()) {
                    std::string err;
                    if (const auto data = audioClipData(ac, err)) { ac.audioSegs.push_back({0.0, data->duration()}); ac.dur = std::max(0.05, data->duration()); }
                }
                if (ImGui::MenuItem("Edit sample...")) { openSoundEditor(ac.id, ac.audioView); ImGui::CloseCurrentPopup(); }   // opens the editor in the clip's current view (waveshape / spectrogram)
                if (ImGui::IsItemHovered()) helpTip("A big window with its own timeline: zoom down to single samples, select, cut, fade, normalize and save the file. The editor opens in the clip's chosen view (waveshape / spectrogram below).");
                ImGui::Separator();
                int view = ac.audioView;
                const char* views[] = {"Waveshape", "Spectrogram"};
                ImGui::SetNextItemWidth(S(170));
                if (caw_ui::combo("View", &view, views, 2)) ac.audioView = view;
                if (ImGui::IsItemHovered()) helpTip("How the clip is drawn on the timeline. The spectrogram is colored with the clip color.");
                if (ImGui::Checkbox("Reverse", &ac.audioReverse)) { audioKillClipVoices(ac.id); }
                if (ImGui::IsItemHovered()) helpTip("Plays the clip's audio backwards. The code can also set  reverse = 1.");
                ImGui::Separator();
            }
            drawClipQMenu();
            ImGui::Separator();
            if (ImGui::MenuItem("Edit this clip's code")) { clip->editorOpen = true; ImGui::CloseCurrentPopup(); }
            if (ImGui::MenuItem("Add MIDI clip", nullptr, false, g_proj.tracks[(size_t)laneIndex].kind == 0 || g_proj.tracks[(size_t)laneIndex].kind == 3 || g_proj.tracks[(size_t)laneIndex].kind == 6 || g_proj.tracks[(size_t)laneIndex].kind == 7)) {
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
                audioKillClipVoices(g_ctxClipId);
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

    // TRACK FREEZE: the render pass runs as a modal - the user cannot edit while the pass rolls
    {
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(420), 0.f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.13f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.50f, 0.66f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        if (freezeActive() && !ImGui::IsPopupOpen("##freeze_progress")) ImGui::OpenPopup("##freeze_progress");
        if (ImGui::BeginPopupModal("##freeze_progress", nullptr, flags)) {
            if (freezeActive()) {
                ImGui::TextColored(ImVec4(0.55f, 0.78f, 0.92f, 1.f), "Freezing track \"%s\"...", freezeTrackName().c_str());
                if (freezeOffline())
                    ImGui::TextDisabled("OFFLINE render: silent, running as fast as the CPU allows (no playback). SC / PD / VSTe racks still render in real time.");
                else
                    ImGui::TextDisabled("Rendering the whole track into a WAV file. The playhead runs through the range once; the engines play only this track while it renders.");
                ImGui::Spacing();
                ImGui::ProgressBar(freezeProgress(), ImVec2(-FLT_MIN, S(16)));
                ImGui::Spacing();
                ImGui::TextDisabled("Freeze keeps the volume fader live; mute / solo keep working. Everything else is locked until you unfreeze the track.");
                ImGui::Separator();
                if (ImGui::Button("Cancel")) { freezeCancel(); ImGui::CloseCurrentPopup(); }
            } else {
                ImGui::CloseCurrentPopup();   // the pass finished (or was cancelled) elsewhere
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    {   // TRACK LOCK: the engine is down - a lock renders the track LIVE, so there is nothing to capture yet
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(440), 0.f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.11f, 0.09f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.66f, 0.50f, 0.25f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        if (g_freezeWarnOpen && !ImGui::IsPopupOpen("##freeze_need_engine")) ImGui::OpenPopup("##freeze_need_engine");
        if (ImGui::BeginPopupModal("##freeze_need_engine", &g_freezeWarnOpen, flags)) {
            if (g_freezeWarnTrack < 0 || g_freezeWarnTrack >= (int)g_proj.tracks.size()) {
                ImGui::CloseCurrentPopup();
                g_freezeWarnOpen = false;
            } else {
                const Track& wt = g_proj.tracks[(size_t)g_freezeWarnTrack];
                const char* engName = "";
                const char* runLabel = "";
                bool down = false;
                if (wt.kind == 0 && !engineIsUp()) { down = true; engName = "Csound (DSP)"; runLabel = "Run DSP"; }
                else if (wt.kind == 3 && !g_vstEngineOn) { down = true; engName = "The VST engine"; runLabel = "Run VST"; }
                else if (wt.kind == 5 && !g_scReady) { down = true; engName = "SuperCollider"; runLabel = "Run SuperCollider"; }
                else if (wt.kind == 6 && !g_pdEngineOn && !g_mixedPd) { down = true; engName = "Pure Data (DSP)"; runLabel = "Run DSP"; }
                else if (wt.kind == 7 && !g_sampleEngineOn) { down = true; engName = "The sample engine"; runLabel = "Run Sample"; }
                if (!down && wt.kind == 0 && !g_useCsoundApi) {   // the external csound process renders outside CAW: nothing to capture
                    ImGui::TextColored(ImVec4(0.92f, 0.68f, 0.30f, 1.f), "Csound can't render online in the external csound process mode.");
                    ImGui::TextDisabled("A lock renders the track live through CAW's Csound API engine. Switch CAW to its API engine (the external csound process plays outside CAW), run Csound again, then lock the track.");
                    ImGui::Spacing();
                    ImGui::TextDisabled("Nothing was changed.");
                    ImGui::Separator();
                    if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); g_freezeWarnOpen = false; }
                } else if (!down) {   // the engine came up while the dialog was open
                    ImGui::TextColored(ImVec4(0.55f, 0.78f, 0.92f, 1.f), "The engine is running now.");
                    ImGui::TextDisabled("Close this dialog and lock the track again.");
                    ImGui::Separator();
                    if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); g_freezeWarnOpen = false; }
                } else {
                    ImGui::TextColored(ImVec4(0.92f, 0.68f, 0.30f, 1.f), "%s can't render online.", engName);
                    ImGui::TextDisabled("A lock renders the track LIVE through its engine: run the engine first, then lock the track. Nothing was changed.");
                    ImGui::Separator();
                    if (ImGui::Button(runLabel)) {
                        if (wt.kind == 5) startScEngine();
                        else if (wt.kind == 3) startVstEngine();
                        else if (wt.kind == 7) {   // the sample engine has no heavy start: flip it on and open the stream
                            g_sampleEngineOn = true;
                            ensureSampleOutput();
                            logLine("[caw] Sample engine running: the sample clips play with Play.");
                        }
                        else startEngine();   // Csound (DSP), Pure Data (DSP): the shared DSP host stream
                        ImGui::CloseCurrentPopup();
                        g_freezeWarnOpen = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); g_freezeWarnOpen = false; }
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    {   // TRACK LOCK: render the VSTe's current sources through its plugins and lock only the rack
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(480), 0.f));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.10f, 0.13f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.50f, 0.66f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        if (g_freezeVsteOpen && !ImGui::IsPopupOpen("##freeze_vste_ask")) ImGui::OpenPopup("##freeze_vste_ask");
        if (ImGui::BeginPopupModal("##freeze_vste_ask", &g_freezeVsteOpen, flags)) {
            if (g_freezeVsteAsk < 0 || g_freezeVsteAsk >= (int)g_proj.tracks.size()) {
                ImGui::CloseCurrentPopup();
                g_freezeVsteOpen = false;
            } else {
                const Track& rack = g_proj.tracks[(size_t)g_freezeVsteAsk];
                ImGui::TextColored(ImVec4(0.55f, 0.78f, 0.92f, 1.f), "Render the effect of \"%s\" (lock the rack)", rack.name.c_str());
                ImGui::TextDisabled("Locking renders everything OFFLINE (fast - not the playing time): Csound / sample sources render through the Csound offline host, the rack and its VSTi / PD sources render in CAW's C++ VST / PD hosts. The source tracks are not frozen (except Csound / sample / SC sources, which render first). The sources stay muted until you unlock the rack.");
                const std::vector<int> sources = freezeVsteSources(g_freezeVsteAsk);
                if (!sources.empty()) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Sources:");
                    for (int si : sources) {
                        const Track& st = g_proj.tracks[(size_t)si];
                        const char* kn = st.kind == 0 ? "Csound" : st.kind == 3 ? "VSTi" : st.kind == 5 ? "SuperCollider" : st.kind == 6 ? "Pure Data" : "Sample";
                        if (trackFrozen(st)) ImGui::BulletText("%s  (%s, already locked - its WAV feeds the effect)", st.name.c_str(), kn);
                        else ImGui::BulletText("%s  (%s, rendered directly through the effect)", st.name.c_str(), kn);
                    }
                }
                std::string problem;
                const bool ready = freezeVsteReady(g_freezeVsteAsk, problem);
                if (!problem.empty()) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.92f, 0.68f, 0.30f, 1.f), "%s", problem.c_str());
                }
                ImGui::Separator();
                if (ready) {
                    if (ImGui::Button("Render sources through VSTe and lock rack")) {
                        // try/catch: an exception escaping the frame dies with the whole app with no
                        // report - whatever the offline/realtime setup hits is logged instead, and
                        // a half-started pass is cancelled cleanly.
                        try { freezeVsteRenderAll(g_freezeVsteAsk); }
                        catch (const std::exception& e) {
                            if (freezeActive()) freezeCancel();
                            logLine(std::string("[caw] Rack lock failed with an error: ") + e.what());
                        } catch (...) {
                            if (freezeActive()) freezeCancel();
                            logLine("[caw] Rack lock failed with an unknown error (out of memory?).");
                        }
                        ImGui::CloseCurrentPopup();
                        g_freezeVsteOpen = false;
                    }
                    if (ImGui::IsItemHovered()) helpTip("The current sources feed the plugins directly; only the effect's final output is written to a WAV. No source track is frozen.");
                    ImGui::SameLine();
                }
                if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); g_freezeVsteOpen = false; }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
}
#include "app.h"
float catmullRom(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t * t + (-p0 + 3.f * p1 - 3.f * p2 + p3) * t * t * t);
}
#include "app.h"
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
#include "app.h"
bool codeEditorBody(std::unordered_map<int, std::unique_ptr<TextEditor>>& editors, int key, std::string& code, float reserveBottom, int lang, bool diag )
{
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
        else if (lang == 3) {   // sample clip CAW code: plain with // comments - no Csound opcodes, no identifier tooltips
            auto plain = TextEditor::LanguageDefinition::CPlusPlus();
            plain.mName = "CAW sample code";
            plain.mKeywords.clear(); plain.mIdentifiers.clear();
            plain.mTokenRegexStrings.clear();
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
#include "app.h"
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
#include "app.h"
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
#include "app.h"
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
#include "app.h"
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
            if (lane.kind == 7) {
                const std::string title = "Sample clip - " + clip.name + "###audio_clip_" + std::to_string(clip.id);
                bool open = clip.editorOpen;
                ImGui::SetNextWindowSize(ImVec2(S(640), S(460)), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowPos(ImVec2(S(300), S(150)), ImGuiCond_FirstUseEver);
                winMaxPrepare(clip.id);
                if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
                    winMaxTitleButton(clip.id);
                    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;   // a code window has focus: Space types, never plays
                    ImGui::SetNextItemWidth(S(150));
                    const std::string nameBefore = clip.name;
                    if (inputStr("##audio_clip_name", clip.name, 0, nameFilter)) renameClipGroup(clip, nameBefore);
                    if (!ImGui::IsItemActive()) ensureUniqueClipName(clip);
                    ImGui::SameLine(); colorPicker("##audio_clip_color", clip.color, lane.color);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
                    inputStrHint("##audio_clip_note", "Describe this clip...", clip.note);
                    if (!clip.audioFile.empty()) {
                        std::string err;
                        const auto data = audioClipData(clip, err);
                        ImGui::TextDisabled("%s", clip.audioFile.c_str());
                        if (data) ImGui::TextDisabled("%.2f s source, %d Hz, %s, %d segment(s)", data->duration(), data->sr, data->channels == 1 ? "mono" : "stereo", (int)clip.audioSegs.size());
                        else if (!err.empty()) ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "File problem: %s", err.c_str());
                        if (clip.audioSegs.empty() && data) { clip.audioSegs.push_back({0.0, data->duration()}); clip.dur = std::max(0.05, data->duration()); }
                    }
                    int view = clip.audioView;
                    const char* views[] = {"Waveshape", "Spectrogram"};
                    ImGui::SetNextItemWidth(S(150));
                    if (caw_ui::combo("View##audio_ed", &view, views, 2)) clip.audioView = view;   // the (+) dropbox style like every dropdown in the project
                    if (ImGui::IsItemHovered()) helpTip("How the clip is drawn on the timeline. The spectrogram is colored with the clip color.");
                    ImGui::SameLine();
                    {   // output routing: "as the source" (mono -> 1, stereo -> 1-2 ...) or "custom"
                        // custom = set inside the CAW code:  out1 = in1   out2 = in1  (output n plays input m; 1-based)
                        int route = clip.audioRoute;
                        const char* routes[] = {"as the source", "custom"};
                        ImGui::SetNextItemWidth(S(140));
                        if (caw_ui::combo("Routing##audio_ed", &route, routes, 2)) {
                            clip.audioRoute = std::min(1, std::max(0, route));
                            audioKillClipVoices(clip.id);
                        }
                        if (ImGui::IsItemHovered()) helpTip("Where the clip's channels go. as the source: file channel 1 -> output 1, channel 2 -> output 2 ... custom: the clip's code routes them - out1 = in1 plays input 1 on output 1; a mono file on both outputs: out1 = in1, out2 = in1. The track meters follow.");
                    }
                    // Run: applies the code (like every other language: Run = apply new code).
                    const bool dirty = g_clipCodeDirty.count(clip.id) != 0;
                    pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                    if (ImGui::Button(dirty ? "Run clip *" : "Run clip")) applySampleClip(clip, lane.id);
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) helpTip("DSP off: Run plays this clip's CAW code once (the sample engine starts if it is off). DSP on: Run is an APPLY button only - the code is applied; a playing window restarts with the new code, nothing else is auditioned.");
                    ImGui::SameLine();
                    midiToggleButton((int)ti, clip, "audio");
                    if (ImGui::IsItemHovered()) helpTip("Sampler mode: the file is NOT played by the timeline any more - it only plays when the playhead reaches a MIDI note of this lane's MIDI lane (the note starts the file with its pitch as the speed; the velocity scales the volume). Draw notes on the MIDI lane below.");
                    ImGui::SameLine();
                    if (!clip.midiEnabled) ImGui::BeginDisabled();
                    if (ImGui::Button("Edit MIDI##audio")) openMidiEditorFor((int)ti, clip);
                    if (!clip.midiEnabled) ImGui::EndDisabled();
                    const ImGuiStyle& st = ImGui::GetStyle();
                    const float bottomRow = ImGui::GetFrameHeight() + st.ItemSpacing.y * 3.f + S(8);
                    if (g_codeFonts[0]) ImGui::PushFont(g_codeFonts[0]);
                    ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
                    const bool codeEdited = codeEditorBody(g_clipCodeEditors, clip.id, clip.code, bottomRow, 3, true);
                    ImGui::SetWindowFontScale(1.f);
                    if (g_codeFonts[0]) ImGui::PopFont();
                    if (codeEdited) g_clipCodeDirty.insert(clip.id);   // Run applies it (Run = apply new code)
                    ImGui::Separator();
                    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Start"); ImGui::SameLine(0, S(6));
                    ImGui::SetNextItemWidth(S(100));
                    if (ImGui::InputDouble("##audio_clip_start", &clip.start, 0.0, 0.0, "%.3f s")) clip.start = clampTimelineTime(clip.start);
                    ImGui::SameLine(0, S(12)); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Duration"); ImGui::SameLine(0, S(6));
                    ImGui::SetNextItemWidth(S(100));
                    if (ImGui::InputDouble("##audio_clip_duration", &clip.dur, 0.0, 0.0, "%.3f s")) clip.dur = std::max(0.05, clip.dur);
                    ImGui::SameLine(0, S(12)); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("End %.3f s", clip.start + clip.dur);
                    if (clip.midiEnabled) ImGui::TextDisabled("Sampler: every MIDI note plays the clip from its first segment (pitch shifts the speed, velocity scales the volume). Pen 2 on the waveshape draws volume automation, Cut splits, Join merges.");
                    else ImGui::TextDisabled("The code drives the sample: rate (clip length: 0.5 = half as long, 2 = twice as long), gain (the shape amplitude), reverse, out routing, granular play:. lfo / smooth / line / every / when all work. Pen 2 on the waveshape draws volume automation.");
                }
                ImGui::End();
                clip.editorOpen = open;
                continue;
            }
            if (lane.kind == 5) {
                const std::string title = "Code editor - " + clip.name + " (SuperCollider)###sc_clip_" + std::to_string(clip.id);
                bool open = clip.editorOpen;
                ImGui::SetNextWindowSize(ImVec2(S(640), S(440)), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowPos(ImVec2(S(300), S(150)), ImGuiCond_FirstUseEver);
                winMaxPrepare(clip.id);
                if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
                    winMaxTitleButton(clip.id);
                    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                        g_textEditorFocused = true;
                        // SUPERCOLLIDER-STYLE: Alt+Enter pushes the clip's code into the engine now (the TextEditor
                        // ignores Alt+Enter, so no newline is inserted - see TextEditor.cpp Enter handling).
                        if (ImGui::GetIO().KeyAlt && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
                            applyClipCodeNow(clip, lane);
                    }
                    ImGui::SetNextItemWidth(S(150));
                    const std::string nameBefore = clip.name;
                    if (inputStr("##sc_clip_name", clip.name, 0, nameFilter)) renameClipGroup(clip, nameBefore);
                    if (!ImGui::IsItemActive()) ensureUniqueClipName(clip);
                    ImGui::SameLine(); colorPicker("##sc_clip_color", clip.color, lane.color);
                    ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
                    inputStrHint("##sc_clip_note", "Describe this clip...", clip.note);

                    const bool dirty = g_clipCodeDirty.count(clip.id) != 0;
                    const bool manual = scManualRunning(clip.id);   // only MANUAL (Run-button) auditions light the button
                    if (manual) pushBtnOn(ImVec4(0.85f, 0.35f, 0.10f, 1.f));
                    else pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                    if (ImGui::Button(manual ? "Stop clip" : (dirty ? "Run clip *" : "Run clip"))) {
                        if (g_scPendingClipId == clip.id && !g_scReady) g_scPendingClipId = 0;
                        else if (dirty) applyScClip(clip);   // Run applies the edited code (timeline playing: re-evaluate the sounding instance)
                        else if (manual) stopScClip(clip.id);
                        else applyScClip(clip);
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) helpTip("DSP off: Run plays this SuperCollider clip once (it boots the engine and waits for it). DSP on: Run is an APPLY button only - the code becomes what the timeline uses from now on, nothing is auditioned. The timeline playing a clip never changes this button.");
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
                    if (codeEdited) { syncSharedCode(clip); g_clipCodeDirty.insert(clip.id); }   // SUPERCOLLIDER-STYLE: mark dirty - Alt+Enter / Run pushes it
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
                if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
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
                    if (ImGui::Button(running ? "  Stop  ##pd" : (g_clipCodeDirty.count(clip.id) ? "  Run * ##pd" : "  Run  ##pd"))) {
                        if (g_pdEngineOn || g_mixedPd) {
                            // DSP ON: Run is an APPLY button only - save the in-memory patch (and force the live
                            // replan while the transport plays, so the running clip reloads the changed patch).
                            // Never auditions, never restarts the engine.
                            for (auto& kv : g_pdPatchBuffers) if (kv.second.dirty()) pd_patch_editor::save(std::filesystem::u8path(kv.first), kv.second);
                            if (g_timelineRun && (g_pdTimelineMode || g_mixedPd)) g_pdPlanT = 1.0;   // the next frame re-plans; the engine reloads the changed patch
                            g_clipCodeDirty.erase(clip.id);
                        } else if (running) stopPdPatch();
                        else {
                            std::filesystem::path patch=std::filesystem::u8path(clip.code);
                            if(patch.is_relative()&&!g_projectPath.empty())patch=std::filesystem::u8path(g_projectPath).parent_path()/patch;
                            std::string error;
                            for (auto& kv : g_pdPatchBuffers) if (kv.second.dirty()) pd_patch_editor::save(std::filesystem::u8path(kv.first), kv.second);   // SUPERCOLLIDER-STYLE: flush in-memory patch edits to disk before starting, so Run uses the current code
                            if(clip.code.empty()) logLine("[caw] Select a .pd patch first.");
                            else if(startPdPatch(patch.u8string(),clip.id,error)) { g_pdRunLeft = clip.dur; g_clipCodeDirty.erase(clip.id); }   // SUPERCOLLIDER-STYLE: Run applies the code and plays the patch for the clip's duration, then stops
                            else logClipError("PureData", clip, error.empty()?"Could not start this patch.":error);
                        }
                    }
                    ImGui::PopStyleColor(3);
                    if(ImGui::IsItemHovered())helpTip("DSP off: Run plays this patch (the PD preview engine starts if the DSP is off). DSP on: Run is an APPLY button only - the patch is saved into the running engine (a playing clip reloads it), nothing is auditioned. Edits are applied with Alt+Enter or Run (SuperCollider-style: nothing is applied while typing).");
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
                            // SUPERCOLLIDER-STYLE: Alt+Enter pushes the edited patch to the running engine now
                            // (the TextEditor ignores Alt+Enter, so no newline is inserted).
                            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyAlt && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
                                if (saveAndApply()) g_clipCodeDirty.erase(clip.id);
                            }
                            struct LiveEdit { std::size_t hash = 0; double changedAt = 0.0, checkedAt = 0.0; bool tried = false; };
                            static std::map<int, LiveEdit> liveEdits;
                            LiveEdit& live = liveEdits[clip.id];
                            const double now = ImGui::GetTime();
                            const std::size_t h = std::hash<std::string>{}(buffer.readable);
                            if (h != live.hash) { live.hash = h; live.changedAt = now; live.tried = false; }
                            if (buffer.dirty()) {
                                g_clipCodeDirty.insert(clip.id);   // SUPERCOLLIDER-STYLE: dirty until Alt+Enter / Run - never applied while typing
                                if (!open) { live.tried = true; saveAndApply(); }   // closing the editor keeps the edits (a deliberate action, not "while typing")
                            } else {
                                g_clipCodeDirty.erase(clip.id);
                                if (now - live.checkedAt > 1.0) {   // the file was changed elsewhere (Pure Data): show it
                                    live.checkedAt = now;
                                    std::string disk, why;
                                    if (pd_patch_editor::read(file, disk, why) && disk != buffer.original) pd_patch_editor::load(file, buffer);
                                }
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
            // the window that is already open comes to the front instead. ONLY within the same track kind:
            // a sample clip and a Pure Data clip that happen to share a name are different things -
            // redirecting would open the WRONG editor (a sample clip showing a Pure Data window).
            if (!g_clipWinSeen.count(clip.id)) {
                int openId = -1;
                for (const Track& ot : g_proj.tracks) for (const Clip& oc : ot.clips)
                    if (oc.id != clip.id && ot.kind == lane.kind && oc.name == clip.name && oc.editorOpen && g_clipWinSeen.count(oc.id)) openId = oc.id;
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
            if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
                winMaxTitleButton(clip.id);
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                    g_textEditorFocused = true;
                    // SUPERCOLLIDER-STYLE: Alt+Enter pushes the clip's code into the running engine now
                    // (the TextEditor ignores Alt+Enter, so no newline is inserted).
                    if (ImGui::GetIO().KeyAlt && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
                        applyClipCodeNow(clip, lane);
                }
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
                const bool codeDirty = g_clipCodeDirty.count(clip.id) != 0;
                if (clipIsRunning) pushBtnOn(ImVec4(0.85f, 0.35f, 0.10f, 1.f));
                else pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
                if (ImGui::Button(clipIsRunning ? "  Stop  " : (codeDirty ? "  Run *  " : "  Run  "))) {
                    if (clipIsRunning) stopClipRun(clip);
                    else if (lane.kind == 3 || lane.kind == 4) applyClipCodeNow(clip, lane);   // VSTi / VSTe: Run pushes the edited code (no Csound round-trip)
                    else if (engineIsUp()) {   // DSP ON: Run is an APPLY button only - the code goes into the running engine, nothing plays
                        if (pushClipCodeToEngine(clip)) logLine("[caw] Run clip " + clip.name + ": applied to the running engine (the DSP is on - Play runs the clip).\n");
                        g_clipCodeDirty.erase(clip.id);
                    }
                    else runClip(clip);   // DSP OFF: Run plays the clip (it starts the DSP and fires when the engine is up)
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) helpTip(clipIsRunning ? "Stop this clip" : "DSP off: Run plays this clip once (it starts the DSP engine and fires when the engine is up). DSP on: Run is an APPLY button only - the edited code goes into the running engine, nothing plays. Alt+Enter pushes the edited code into the running engine without replaying (SuperCollider-style: nothing is applied while typing).");
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
            if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
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
    drawUdoPool();
    drawInstrPool();
    drawOrchestraPool();
    drawMasterFxWindow();
}
#include "app.h"
const char* midiPitchName(int pitch) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static char text[16];
    std::snprintf(text, sizeof(text), "%s%d", names[pitch % 12], pitch / 12 - 1);
    return text;
}
#include "app.h"
#include <cstdlib>
namespace {
struct DbEditState { ImGuiID id = 0; char buf[16] = {0}; char initial[16] = {0}; bool touched = false; int startFrame = 0; };
DbEditState g_dbEdit;
int dbEditCallback(ImGuiInputTextCallbackData* d) {
    // Until the user types / moves the caret / deletes, the WHOLE number stays selected (a click or a drag cannot
    // make a partial selection). After the first real edit the box behaves like a normal text field.
    DbEditState* st = (DbEditState*)d->UserData;
    if (!st->touched) {
        if (std::strcmp(d->Buf, st->initial) != 0 ||
            ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_Delete) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
            ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_End))
            st->touched = true;
        else
            d->SelectAll();
    }
    return 0;
}
}  // namespace
bool dbIntBox(const char* id, int& db, int minDb, int maxDb, float width, const char* suffix) {
    ImGui::PushID(id);
    const ImGuiID gid = ImGui::GetID("##dbint");
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    bool changed = false;
    if (g_dbEdit.id != gid) {
        // display mode: a plain frame with the centered number. One click does nothing; a double-click starts editing.
        const ImVec2 size(width, ImGui::GetFrameHeight());
        ImGui::InvisibleButton("##dbint", size);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dl->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), st.FrameRounding);
        if (st.FrameBorderSize > 0.f) dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), st.FrameRounding, 0, st.FrameBorderSize);
        char txt[32];
        std::snprintf(txt, sizeof txt, "%d%s", db, suffix ? suffix : "");
        const float tw = ImGui::CalcTextSize(txt).x;
        dl->AddText(ImVec2(std::floor(a.x + (width - tw) * 0.5f + 0.5f), a.y + st.FramePadding.y),   // integer-friendly origin: top + the caller's padding
                    ImGui::GetColorU32(ImGuiCol_Text), txt);
        if (hovered && ImGui::IsMouseDoubleClicked(0)) {
            g_dbEdit.id = gid;
            std::snprintf(g_dbEdit.buf, sizeof g_dbEdit.buf, "%d", db);
            std::snprintf(g_dbEdit.initial, sizeof g_dbEdit.initial, "%d", db);
            g_dbEdit.touched = false;
            g_dbEdit.startFrame = ImGui::GetFrameCount();
            ImGui::SetCursorScreenPos(pos);   // the text field takes the same place this frame
        }
    }
    if (g_dbEdit.id == gid) {
        const int age = ImGui::GetFrameCount() - g_dbEdit.startFrame;
        if (age <= 1) ImGui::SetKeyboardFocusHere();
        const float tw = ImGui::CalcTextSize(g_dbEdit.buf).x;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::max(st.FramePadding.x, std::floor((width - tw) * 0.5f)), st.FramePadding.y));
        ImGui::SetNextItemWidth(width);
        const bool enter = ImGui::InputText("##dbint", g_dbEdit.buf, sizeof g_dbEdit.buf,
            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
            dbEditCallback, &g_dbEdit);
        ImGui::PopStyleVar();
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
        const bool lostFocus = age > 3 && !ImGui::IsItemActive();
        if (enter || cancel || lostFocus) {
            if (!cancel) {
                char* end = nullptr;
                const long v = std::strtol(g_dbEdit.buf, &end, 10);
                if (end != g_dbEdit.buf) {   // a real number was typed (an empty box or a lone "-" changes nothing)
                    const int nv = (int)std::max((long)minDb, std::min((long)maxDb, v));
                    if (nv != db) { db = nv; changed = true; }
                }
            }
            g_dbEdit.id = 0;
        }
    }
    ImGui::PopID();
    return changed;
}
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
#include "app.h"
void midiPitchSlider(const char* id, int& pitchLow) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.15f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.19f, 0.22f, 0.24f, 1.f));
    ImGui::VSliderInt(id, ImVec2(S(18), S(110)), &pitchLow, 0, 127 - kMidiVisiblePitches + 1, "");
    if (ImGui::IsItemHovered()) helpTip("Scroll the visible pitch range.");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}
#include "app.h"
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
    winMaxPrepareT("MIDI Editor - several clips###midi_multi_editor");
    if (ImGui::Begin("MIDI Editor - several clips###midi_multi_editor", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("MIDI Editor - several clips###midi_multi_editor");
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
#include "app.h"
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
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
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
#include "app.h"
