#include "score_svg.h"
#include "csd_builder.h"
#include "marks_gfx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace {

// ------------------------------------------------------------------ small helpers
uint32_t paletteRgb(int index) {
    static const uint32_t p[] = {
        0xEC783C, 0x3CAAC8, 0x966EDC, 0x6EBE5A, 0xDC5A82, 0xD2B43C,
        0x4D7BE8, 0x2BA88D, 0xC653CE, 0xD16E42, 0x75AAE0, 0xA0A650 };
    if (index >= 0 && index < 12) return p[index];
    const float h = std::fmod(std::max(0, index) * 137.507764f, 360.f) / 60.f;
    const float x = 0.7f * (1.f - std::fabs(std::fmod(h, 2.f) - 1.f));
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = .7f; g = x; } else if (h < 2) { r = x; g = .7f; } else if (h < 3) { g = .7f; b = x; }
    else if (h < 4) { g = x; b = .7f; } else if (h < 5) { r = x; b = .7f; } else { r = .7f; b = x; }
    return ((uint32_t)std::lround((r + .17f) * 255) << 16) | ((uint32_t)std::lround((g + .17f) * 255) << 8) | (uint32_t)std::lround((b + .17f) * 255);
}

std::string hex(uint32_t rgb) { char b[16]; std::snprintf(b, sizeof b, "#%06X", rgb & 0xFFFFFF); return b; }
// weighted mix of "#RRGGBB" colours (used when graph lines are combined into one shape: the shape gets the mix of their colours)
std::string mixHexWeighted(const std::vector<std::pair<std::string, double>>& cols) {
    double r = 0, g = 0, b = 0, ws = 0;
    for (const auto& pc : cols) {
        unsigned v = 0;
        if (pc.first.size() != 7 || pc.first[0] != '#' || std::sscanf(pc.first.c_str() + 1, "%x", &v) != 1) continue;
        r += pc.second * ((v >> 16) & 255); g += pc.second * ((v >> 8) & 255); b += pc.second * (v & 255); ws += pc.second;
    }
    if (ws <= 0) return std::string();
    const auto q = [&](double x) { return (uint32_t)std::lround(std::min(255.0, std::max(0.0, x / ws))); };
    return hex((q(r) << 16) | (q(g) << 8) | q(b));
}
std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return b; }

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break; default: o += c;
        }
    }
    return o;
}

std::string clockText(double s) {
    const int sec = (int)(s + 0.5);
    char b[32]; std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    return b;
}

std::string durText(double s) {
    const int sec = (int)(s + 0.5);
    char b[48]; std::snprintf(b, sizeof b, "%d'%02d\"", sec / 60, sec % 60);
    return b;
}

std::string secText(double s) {
    char b[32];
    if (s >= 10) std::snprintf(b, sizeof b, "%.0fs", s); else std::snprintf(b, sizeof b, "%.1fs", s);
    return b;
}

double hash01(uint32_t x) {   // deterministic pseudo random 0..1
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return (x & 0xFFFFFF) / 16777215.0;
}

std::string pitchName(int p) {
    static const char* n[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "Bb", "B" };
    return std::string(n[((p % 12) + 12) % 12]) + std::to_string(p / 12 - 1);
}

int diatonic(int midi) {
    static const int sc[] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 6, 6 };   // A# is written as Bb (on the B line)
    return (midi / 12) * 7 + sc[((midi % 12) + 12) % 12];
}
bool isBlackKey(int midi) { const int k = ((midi % 12) + 12) % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

const char* dynamicFor(double velocity) {
    if (velocity < 32) return "pp";
    if (velocity < 52) return "p";
    if (velocity < 72) return "mp";
    if (velocity < 92) return "mf";
    if (velocity < 112) return "f";
    return "ff";
}

std::string fit(const std::string& s, double pxWidth, double fontPx) {
    const size_t maxChars = (size_t)std::max(0.0, std::floor(pxWidth / (fontPx * 0.56)));
    if (s.size() <= maxChars) return s;
    if (maxChars < 3) return "";
    return s.substr(0, maxChars - 2) + "..";
}
double textWidth(const std::string& s, double fontPx) { return s.size() * fontPx * 0.56; }

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// name used to decide that two tracks are the same score lane: trimmed, lower case
std::string normName(const std::string& s) {
    std::string o = trimmed(s);
    for (char& ch : o) ch = (char)std::tolower((unsigned char)ch);
    return o;
}

// short name for the next systems of the full score ("Clarinet in Bb" -> "Clar.")
std::string abbreviate(const std::string& name) {
    if (name.size() <= 7) return name;
    std::istringstream in(name);
    std::string w;
    in >> w;
    if (w.size() > 4) w = w.substr(0, 4);
    return w + ".";
}

// one pitch: C4, f#3, Bb2, e (octave 4), or a MIDI number
bool parsePitch(const std::string& tok, int& midi, int* quarter = nullptr) {
    if (quarter) *quarter = 0;
    if (tok.empty()) return false;
    if (std::isdigit((unsigned char)tok[0])) {
        char* end = nullptr;
        const long v = std::strtol(tok.c_str(), &end, 10);
        if (*end != 0 || v < 0 || v > 127) return false;
        midi = (int)v;
        return true;
    }
    static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
    const char l = (char)std::tolower((unsigned char)tok[0]);
    if (l < 'a' || l > 'g') return false;
    int acc = 0;
    size_t i = 1;
    // quarter-tone signs of the python score library: "q" quarter sharp (Qsharp), "qb" quarter flat (Qflat), "d" koron. Written right after the letter: Cq4, Eqb4, Bd3
    if (i < tok.size() && (tok[i] == 'q' || tok[i] == 'Q')) {
        if (i + 1 < tok.size() && (tok[i + 1] == 'b' || tok[i + 1] == 'B')) { if (quarter) *quarter = 2; i += 2; }
        else { if (quarter) *quarter = 1; i += 1; }
    } else if (i < tok.size() && tok[i] == 'd') { if (quarter) *quarter = 3; i += 1; }
    for (; i < tok.size() && !(quarter && *quarter) && (tok[i] == '#' || tok[i] == 'b' || tok[i] == 'B'); ++i) acc += tok[i] == '#' ? 1 : -1;
    int oct = 4;
    if (i < tok.size()) {
        char* end = nullptr;
        oct = (int)std::strtol(tok.c_str() + i, &end, 10);
        if (*end != 0) return false;
    }
    midi = (oct + 1) * 12 + base[l - 'a'] + acc;
    return midi >= 0 && midi <= 127;
}

std::string clipText(const Clip& c) {
    if (!c.note.empty()) return c.note;
    std::istringstream in(c.code);
    std::string line;
    if (std::getline(in, line)) {
        const size_t i = line.find_first_not_of(" \t\r");
        if (i != std::string::npos && line[i] == ';') {
            const size_t j = line.find_first_not_of("; \t", i);
            if (j != std::string::npos) { std::string r = line.substr(j); while (!r.empty() && (r.back() == '\r' || r.back() == ' ')) r.pop_back(); return r; }
        }
    }
    return "";
}

const Clip* findClip(const Track& t, int id) {
    for (const Clip& c : t.clips) if (c.id == id) return &c;
    return nullptr;
}

double projectEnd(const Project& p) {
    double e = 0;
    for (const Track& t : p.tracks) {
        for (const Clip& c : t.clips) e = std::max(e, c.start + c.dur);
        for (const MidiClip& m : t.midiClips) e = std::max(e, m.start + m.dur);
        for (const auto& a : t.automation)   // the automation of a graph can run longer than the clips
            if (!t.hiddenAutomation.count(a.first)) for (const AutoPoint& pt : a.second) e = std::max(e, pt.t);
    }
    return std::max(e, 1.0);
}

std::string hzText(double f) {
    char b[32];
    if (f >= 1000) std::snprintf(b, sizeof b, "%.3g kHz", f / 1000.0); else std::snprintf(b, sizeof b, "%.0f Hz", f);
    return b;
}

// ------------------------------------------------------------------ canvas: records primitives
typedef std::vector<std::pair<double, double>> Pts;

struct Canvas {
    double lw = 1.0;                  // general line weight of the score (Score editor)
    bool bw = false;
    std::vector<ScorePrim> prims;

    uint32_t rgba(const std::string& c, double alpha = 1.0) const {
        uint32_t r = 0, g = 0, b = 0;
        if (!bw && c.size() == 7 && c[0] == '#') { unsigned v = 0; std::sscanf(c.c_str() + 1, "%x", &v); r = (v >> 16) & 255; g = (v >> 8) & 255; b = v & 255; }
        const uint32_t a = (uint32_t)std::lround(std::min(1.0, std::max(0.0, alpha)) * 255.0);
        return (r << 24) | (g << 16) | (b << 8) | a;
    }
    static void parseDash(const char* d, ScorePrim& p) {
        if (d) { float a = 0, b = 0; if (std::sscanf(d, "%f,%f", &a, &b) >= 1) { p.dash0 = a; p.dash1 = b > 0 ? b : a; } }
    }

    void text(double x, double y, const std::string& s, double size, const std::string& fill = "#111111", bool bold = false, const char* anchor = "start", bool italic = false, bool mono = false) {
        if (s.empty()) return;
        ScorePrim p; p.kind = ScorePrim::Text; p.x = (float)x; p.y = (float)y; p.text = s; p.size = (float)size; p.fill = rgba(bw ? "#000000" : fill);
        p.bold = bold; p.italic = italic; p.mono = mono; p.anchor = !std::strcmp(anchor, "middle") ? 1 : !std::strcmp(anchor, "end") ? 2 : 0;
        prims.push_back(std::move(p));
    }
    void line(double x1, double y1, double x2, double y2, const std::string& c = "#000000", double sw = 0.6, double op = 1.0, const char* dash = nullptr) {
        ScorePrim p; p.kind = ScorePrim::Line; p.x = (float)x1; p.y = (float)y1; p.w = (float)x2; p.h = (float)y2; p.stroke = rgba(c, op); p.sw = (float)(sw * lw);
        parseDash(dash, p);
        prims.push_back(std::move(p));
    }
    void rect(double x, double y, double w, double h, const std::string& fill, double fo, const std::string& stroke, double sw = 1.0) {
        ScorePrim p; p.kind = ScorePrim::Rect; p.x = (float)x; p.y = (float)y; p.w = (float)std::max(w, 0.0); p.h = (float)h; p.sw = (float)(sw * lw);
        if (!fill.empty()) p.fill = rgba(fill, bw ? fo * 0.35 : fo);
        if (!stroke.empty()) p.stroke = rgba(stroke);
        prims.push_back(std::move(p));
    }
    void polygon(const Pts& pts, const std::string& c, double fo, double sw = 1.0, bool dashed = false) {
        ScorePrim p; p.kind = ScorePrim::Polygon; p.fill = rgba(c, bw ? fo * 0.35 : fo); p.stroke = rgba(c); p.sw = (float)(sw * lw);
        if (dashed) { p.dash0 = 3; p.dash1 = 2; }
        for (const auto& q : pts) { p.pts.push_back((float)q.first); p.pts.push_back((float)q.second); }
        prims.push_back(std::move(p));
    }
    void fillPoly(const Pts& pts, const std::string& c, double fo) {
        if (pts.size() < 3) return;
        ScorePrim p; p.kind = ScorePrim::Polygon; p.fill = rgba(c, bw ? fo * 0.35 : fo);
        for (const auto& q : pts) { p.pts.push_back((float)q.first); p.pts.push_back((float)q.second); }
        prims.push_back(std::move(p));
    }
    void polyline(const Pts& pts, const std::string& c, double sw = 1.0, const char* dash = nullptr, bool round = false) {
        if (pts.size() < 2) return;
        ScorePrim p; p.kind = ScorePrim::Polyline; p.stroke = rgba(c); p.sw = (float)(sw * lw); p.round = round;
        parseDash(dash, p);
        for (const auto& q : pts) { p.pts.push_back((float)q.first); p.pts.push_back((float)q.second); }
        prims.push_back(std::move(p));
    }
    // ---- ported from the python score library (def.py): note(), sharp(), bflat()
    // note head: two cubic curves (an almond shape); r = half of the staff line distance, like y_space / 2 in python
    void note(double x, double y, const std::string& c, double r0) {
        const double r = r0 * 1.1;
        Pts v; v.push_back({ x - r * 1.2, y + r * .6 });
        cubic(v, x - r * 1.2, y + r * .6, x - r * 1.7, y - r * .3, x + r * .3, y - r * 1.4, x + r * 1.2, y - r * .6);
        cubic(v, x + r * 1.2, y - r * .6, x + r * 1.7, y + r * .3, x - r * .3, y + r * 1.4, x - r * 1.2, y + r * .6);
        fillPoly(v, c, 1.0);
        polyline(v, c, std::max(0.6, r * 0.2), nullptr, true);
    }
    // sharp sign left of a note (x, y = middle of the note, ys = staff line distance)
    void sharp(double x, double y, double ys, const std::string& c, double sw = 0.9) {
        const double off = ys * 2.0 / 3.0, width = ys / 2;
        const double ybot = y + ys, ybot1 = y + ys / 2, ybot2 = y + ys / 9, ytop = y - ys, ytop2 = y - ys / 2, ytop1 = y - ys / 9;
        const double x1 = x - off - width - ys / 2, x2 = x - off - ys / 2, xmin = x1 - ys / 5, xmax = x2 + ys / 5;
        line(x1, ybot, x1, ytop, c, sw);
        line(x2, ybot, x2, ytop, c, sw);
        line(xmin, ybot1, xmax, ybot2, c, sw * 2.0);
        line(xmin, ytop1, xmax, ytop2, c, sw * 2.0);
    }
    // quarter sharp (python Qsharp): one vertical line and two slanted bars
    void qsharp(double x, double y, double ys, const std::string& c, double sw = 0.9) {
        const double off = ys * 0.8;
        const double ybot = y + ys, ybot1 = y + ys / 2, ybot2 = y + ys / 9, ytop = y - ys, ytop2 = y - ys / 2, ytop1 = y - ys / 9;
        const double x1 = x - off - ys * 0.5, xmin = x1 - ys / 5, xmax = x1 + ys / 5;
        line(x1, ybot, x1, ytop, c, sw);
        line(xmin - 1, ybot1, xmax + 1, ybot2, c, sw * 2.0);
        line(xmin - 1, ytop1, xmax + 1, ytop2, c, sw * 2.0);
    }
    // quarter flat (python Qflat): a flat with a wider curve
    void qflat(double x, double y, double ys, const std::string& c, double sw = 0.9) {
        const double width = ys * 0.5, off = ys;
        const double xl = x + ys * 0.5 - off - width, ytop = y - ys * 1.5, ybot = y + ys * 0.5, yend = y - ys * 0.25;
        Pts v; v.push_back({ xl, ytop }); v.push_back({ xl, ybot });
        const double qx = x - off * 2.15, qy = y - ys * 0.6;
        for (int i = 1; i <= 12; ++i) {
            const double t = i / 12.0, u = 1 - t;
            v.push_back({ u * u * xl + 2 * u * t * qx + t * t * xl, u * u * ybot + 2 * u * t * qy + t * t * yend });
        }
        polyline(v, c, sw, nullptr, true);
    }
    // koron (python koron): a stem with a flag
    void koron(double x, double y, double ys, const std::string& c, double sw = 0.9) {
        const double k = ys / 10.0, xs = x - 20 * k, yb = y + 25 * k;
        Pts v = { { xs, yb }, { xs, yb - 30 * k }, { xs + 7 * k, yb - 25 * k }, { xs, yb - 20 * k } };
        polyline(v, c, sw, nullptr, true);
    }
    // flat sign left of a note
    void flat(double x, double y, double ys, const std::string& c, double sw = 0.9) {
        const double width = ys / 2, off = ys;
        const double xl = x - off - width, ytop = y - ys * 1.5, ybot = y + ys * 0.5, yend = y - ys / 4;
        Pts v; v.push_back({ xl, ytop }); v.push_back({ xl, ybot });
        const double qx = x - off / 4, qy = y - ys * 3 / 4;
        for (int i = 1; i <= 12; ++i) {
            const double t = i / 12.0, u = 1 - t;
            v.push_back({ u * u * xl + 2 * u * t * qx + t * t * xl, u * u * ybot + 2 * u * t * qy + t * t * yend });
        }
        polyline(v, c, sw, nullptr, true);
    }
    void circle(double x, double y, double r, const std::string& fill, const std::string& stroke = "", double sw = 1.0) {
        ScorePrim p; p.kind = ScorePrim::Circle; p.x = (float)x; p.y = (float)y; p.w = (float)r; p.h = (float)r; p.sw = (float)(sw * lw);
        if (!fill.empty()) p.fill = rgba(fill);
        if (!stroke.empty()) p.stroke = rgba(stroke);
        prims.push_back(std::move(p));
    }
    static void cubic(Pts& out, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, int n = 14) {
        for (int i = 1; i <= n; ++i) {
            const double t = (double)i / n, u = 1 - t;
            out.push_back({ u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                            u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3 });
        }
    }
    void gClef(double x, double y, double ls) {
        auto P = [&](double dx, double dy) { return std::make_pair(x + ls * dx, y + ls * dy); };
        Pts v; v.push_back(P(1, 3));
        auto c = [&](std::pair<double, double> a, std::pair<double, double> b, std::pair<double, double> d) {
            const auto s = v.back(); cubic(v, s.first, s.second, a.first, a.second, b.first, b.second, d.first, d.second);
        };
        c(P(1.5, 2), P(3.2, 2.7), P(1.5, 3.5));
        c(P(.01, 4), P(.2, 2.8), P(1.5, 1));
        c(P(2, .2), P(1.5, .01), P(1.5, .01));
        c(P(.5, .1), P(2, 2), P(1.1, 4.5));
        polyline(v, "#000000", 1.6, nullptr, true);
    }
    void fClef(double x, double y, double ls) {
        auto P = [&](double dx, double dy) { return std::make_pair(x + ls * dx, y + ls * dy); };
        Pts v; v.push_back(P(.7, .8));
        auto c = [&](std::pair<double, double> a, std::pair<double, double> b, std::pair<double, double> d) {
            const auto s = v.back(); cubic(v, s.first, s.second, a.first, a.second, b.first, b.second, d.first, d.second);
        };
        c(P(.9, 1.1), P(.5, 1.3), P(.4, 1));
        c(P(.2, .5), P(.6, 0), P(1, .14));
        c(P(2, .5), P(1, 3), P(.33, 2.2));
        polyline(v, "#000000", 1.8, nullptr, true);
        circle(x + ls * 2, y + ls * 0.7, 1.2, "#000000");
        circle(x + ls * 2, y + ls * 1.3, 1.2, "#000000");
    }
    void slur(double x1, double x2, double y, const std::string& c) {
        Pts v; v.push_back({ x1, y });
        for (int i = 1; i <= 12; ++i) { const double t = i / 12.0; v.push_back({ x1 + (x2 - x1) * t, y - 9 * 4 * t * (1 - t) / 2 }); }
        polyline(v, c, 1.1);
    }
    // smooth curve through the points (quadratic through the mid points)
    void smoothPath(const Pts& pts, const std::string& c, double sw, const char* dash = nullptr) {
        if (pts.size() < 2) return;
        Pts v; v.push_back(pts[0]);
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const auto s = v.back();
            const double mx = (pts[i].first + pts[i + 1].first) / 2, my = (pts[i].second + pts[i + 1].second) / 2;
            for (int k = 1; k <= 8; ++k) {
                const double t = k / 8.0, u = 1 - t;
                v.push_back({ u * u * s.first + 2 * u * t * pts[i].first + t * t * mx, u * u * s.second + 2 * u * t * pts[i].second + t * t * my });
            }
        }
        v.push_back(pts.back());
        polyline(v, c, sw, dash, true);
    }
};

std::string primsToSvg(const ScorePage& page) {
    auto col = [](uint32_t c, std::string& attr, std::string& op) {
        char b[16]; std::snprintf(b, sizeof b, "#%02X%02X%02X", (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255); attr = b;
        const int a = c & 255; op = a >= 255 ? "" : std::to_string(a / 255.0).substr(0, 5);
    };
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << num(page.w) << "\" height=\"" << num(page.h) << "\" viewBox=\"0 0 "
      << num(page.w) << " " << num(page.h) << "\">\n<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    for (const ScorePrim& p : page.prims) {
        std::string sc, so, fc, fo;
        if (p.stroke & 255) col(p.stroke, sc, so);
        if (p.fill & 255) col(p.fill, fc, fo);
        auto strokeAttrs = [&](bool dashOk = true) {
            if (sc.empty()) { o << " stroke=\"none\""; return; }
            o << " stroke=\"" << sc << "\" stroke-width=\"" << p.sw << "\"";
            if (!so.empty()) o << " stroke-opacity=\"" << so << "\"";
            if (p.round) o << " stroke-linecap=\"round\" stroke-linejoin=\"round\"";
            if (dashOk && p.dash0 > 0) o << " stroke-dasharray=\"" << p.dash0 << "," << p.dash1 << "\"";
        };
        auto fillAttrs = [&]() {
            if (fc.empty()) { o << " fill=\"none\""; return; }
            o << " fill=\"" << fc << "\"";
            if (!fo.empty()) o << " fill-opacity=\"" << fo << "\"";
        };
        switch (p.kind) {
            case ScorePrim::Line:
                o << "<line x1=\"" << num(p.x) << "\" y1=\"" << num(p.y) << "\" x2=\"" << num(p.w) << "\" y2=\"" << num(p.h) << "\""; strokeAttrs(); o << "/>\n"; break;
            case ScorePrim::Rect:
                o << "<rect x=\"" << num(p.x) << "\" y=\"" << num(p.y) << "\" width=\"" << num(p.w) << "\" height=\"" << num(p.h) << "\""; fillAttrs(); strokeAttrs(); o << "/>\n"; break;
            case ScorePrim::Polygon:
            case ScorePrim::Polyline: {
                o << (p.kind == ScorePrim::Polygon ? "<polygon" : "<polyline") << " points=\"";
                for (size_t i = 0; i + 1 < p.pts.size(); i += 2) o << num(p.pts[i]) << "," << num(p.pts[i + 1]) << " ";
                o << "\""; fillAttrs(); strokeAttrs(); o << "/>\n"; break;
            }
            case ScorePrim::Ellipse:
                o << "<ellipse cx=\"" << num(p.x) << "\" cy=\"" << num(p.y) << "\" rx=\"" << num(p.w) << "\" ry=\"" << num(p.h) << "\" transform=\"rotate(" << num(p.angle) << " " << num(p.x) << " " << num(p.y) << ")\"";
                fillAttrs(); strokeAttrs(); o << "/>\n"; break;
            case ScorePrim::Circle:
                o << "<circle cx=\"" << num(p.x) << "\" cy=\"" << num(p.y) << "\" r=\"" << num(p.w) << "\""; fillAttrs(); strokeAttrs(); o << "/>\n"; break;
            case ScorePrim::Text: {
                o << "<text x=\"" << num(p.x) << "\" y=\"" << num(p.y) << "\" font-family=\"" << (p.mono ? "Consolas, 'Courier New', monospace" : "Georgia, 'Times New Roman', serif") << "\" font-size=\"" << num(p.size) << "\"";
                if (p.bold) o << " font-weight=\"bold\"";
                if (p.italic) o << " font-style=\"italic\"";
                o << " fill=\"" << (fc.empty() ? "#000000" : fc) << "\" text-anchor=\"" << (p.anchor == 1 ? "middle" : p.anchor == 2 ? "end" : "start") << "\">" << esc(p.text) << "</text>\n";
                break;
            }
        }
    }
    o << "</svg>\n";
    return o.str();
}

// ------------------------------------------------------------------ WAV -> spectrogram
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * 3.14159265358979323846 / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

// "C4 E4:2 - G3+D4" -> notes spread over `dur` seconds. name[:length] (length is relative, default 1), "-" or "r" = rest, "+" joins a chord.
std::vector<MidiNote> scoreWrittenNotes(const std::string& text, double dur) {
    struct Tok { std::vector<int> pitches, quarters; double w = 1.0; };
    std::vector<Tok> toks;
    std::istringstream in(text);
    std::string word;
    double total = 0.0;
    while (in >> word) {
        Tok t;
        const size_t colon = word.find(':');
        if (colon != std::string::npos) {
            char* end = nullptr;
            const double w = std::strtod(word.c_str() + colon + 1, &end);
            if (end != word.c_str() + colon + 1 && w > 0.0) t.w = std::min(64.0, w);
            word = word.substr(0, colon);
        }
        if (word == "-" || word == "r" || word == "R" || word == "_") { toks.push_back(t); total += t.w; continue; }
        size_t pos = 0;
        bool ok = !word.empty();
        while (ok && pos <= word.size()) {
            const size_t plus = word.find('+', pos);
            const std::string one = word.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
            int midi = 0, q = 0;
            if (!parsePitch(one, midi, &q)) { ok = false; break; }
            t.pitches.push_back(midi);
            t.quarters.push_back(q);
            if (plus == std::string::npos) break;
            pos = plus + 1;
        }
        if (!ok) continue;   // unknown word: ignored, so a typo never breaks the score
        toks.push_back(t);
        total += t.w;
    }
    std::vector<MidiNote> out;
    if (total <= 0.0 || dur <= 0.0) return out;
    double at = 0.0;
    for (const Tok& t : toks) {
        const double len = t.w / total * dur;
        for (size_t pi = 0; pi < t.pitches.size(); ++pi) {
            const int pitch = t.pitches[pi];
            MidiNote n;
            n.quarter = t.quarters[pi];
            n.start = at;
            n.dur = std::max(0.05, len * 0.94);
            n.pitch = pitch;
            n.velocity = 80;
            out.push_back(n);
        }
        at += len;
    }
    return out;
}

bool analyzeScoreWav(const std::string& path, ScoreSpectrum& out, std::string& error) {
    out = ScoreSpectrum();
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return (unsigned)((unsigned char)bytes[o] | ((unsigned char)bytes[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return (uint32_t)u16(o) | ((uint32_t)u16(o + 2) << 16); };
    if (bytes.size() < 44 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) { error = "not a WAV file (use a WAV render format)"; return false; }
    unsigned fmt = 0, ch = 0, bits = 0; uint32_t sr = 0; size_t dataOff = 0, dataLen = 0;
    for (size_t o = 12; o + 8 <= bytes.size();) {
        const std::string id = bytes.substr(o, 4);
        size_t len = u32(o + 4);
        if (id == "fmt " && o + 8 + 16 <= bytes.size()) {
            fmt = u16(o + 8); ch = u16(o + 10); sr = u32(o + 12); bits = u16(o + 22);
            if (fmt == 0xFFFE && len >= 26) fmt = u16(o + 8 + 24);
        } else if (id == "data") {
            dataOff = o + 8; dataLen = std::min(len, bytes.size() - dataOff); break;
        }
        o += 8 + len + (len & 1);
    }
    if (!ch || !sr || !dataLen || (fmt != 1 && fmt != 3)) { error = "unsupported WAV format"; return false; }
    const size_t bps = bits / 8;
    if (bps < 2 || bps > 4) { error = "unsupported WAV bit depth"; return false; }
    const size_t frames = dataLen / (bps * ch);
    if (frames < 2048) { error = "audio file is too short"; return false; }
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (unsigned c = 0; c < ch; ++c) {
            const size_t o = dataOff + (i * ch + c) * bps;
            double v = 0;
            if (fmt == 3 && bps == 4) { float fv; std::memcpy(&fv, bytes.data() + o, 4); v = fv; }
            else if (bps == 2) v = (int16_t)u16(o) / 32768.0;
            else if (bps == 3) { int32_t x = ((unsigned char)bytes[o] << 8) | ((unsigned char)bytes[o + 1] << 16) | ((unsigned char)bytes[o + 2] << 24); v = (x >> 8) / 8388608.0; }
            else { int32_t x = (int32_t)u32(o); v = x / 2147483648.0; }
            sum += v;
        }
        mono[i] = (float)(sum / ch);
    }
    const double duration = (double)frames / sr;
    double cps = 12.0;
    if (duration * cps > 6000.0) cps = 6000.0 / duration;
    const size_t N = sr >= 96000 ? 8192 : 4096;
    const int B = 40;
    const double fLo = 40.0, fHi = std::min(16000.0, sr * 0.45);
    const size_t cols = (size_t)std::ceil(duration * cps);
    const double hop = sr / cps;
    std::vector<double> win(N);
    for (size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / (double)N);
    std::vector<double> dbs(cols * (size_t)B);
    double maxDb = -200;
    std::vector<std::complex<double>> buf(N);
    for (size_t c = 0; c < cols; ++c) {
        const long start = (long)(c * hop) - (long)(N / 2);
        for (size_t i = 0; i < N; ++i) {
            const long idx = start + (long)i;
            buf[i] = (idx >= 0 && idx < (long)frames) ? mono[(size_t)idx] * win[i] : 0.0;
        }
        fft(buf);
        for (int b = 0; b < B; ++b) {
            const double f0 = fLo * std::pow(fHi / fLo, (double)b / B), f1 = fLo * std::pow(fHi / fLo, (double)(b + 1) / B);
            size_t k0 = (size_t)std::floor(f0 * N / sr), k1 = (size_t)std::ceil(f1 * N / sr);
            k0 = std::min(k0, N / 2 - 1); k1 = std::min(std::max(k1, k0 + 1), N / 2);
            double pw = 0;
            for (size_t k = k0; k < k1; ++k) pw += std::norm(buf[k]);
            pw /= (double)(k1 - k0);
            const double db = 10 * std::log10(pw + 1e-12);
            dbs[c * (size_t)B + (size_t)b] = db;
            maxDb = std::max(maxDb, db);
        }
    }
    out.bands = B; out.colsPerSec = cps; out.fLo = fLo; out.fHi = fHi; out.duration = duration;
    out.data.resize(dbs.size());
    for (size_t i = 0; i < dbs.size(); ++i) out.data[i] = (uint8_t)std::min(255.0, std::max(0.0, (dbs[i] - (maxDb - 70.0)) / 70.0 * 255.0));
    return true;
}

namespace {

// ------------------------------------------------------------------ lane model
struct Auto {
    std::string label;
    std::string track;
    const std::vector<AutoPoint>* pts = nullptr;
    int mode = 1;                    // 1 own lane with the full curve, 2 compact contour lane
    double lo = 0, hi = 0;           // range declared in the <caw> block (hi > lo), else the range of the data
    bool ranged = false;
    int shape = 0;                   // 0 curve, 1 steps, 2 lines, 3 dots, 4 dashed, 5 mirror smooth, 6 mirror boxes, 7 hairpin, 8 bars
    double ratio = 1.0;              // shape ratio of the mirror shapes (1 = linear)
    double stroke = 0.0;             // stroke width of the line (0 = default); the dots, bars, dashes and break points scale with it
    std::string color;               // colour of the automation lane in the editor (#RRGGBB), used for the line in the score
    bool graph = false;              // line of a graph track
    double ysize = 1.0;              // graph lines: height of the strip (1 = normal)
    bool overlay = false;            // extra layer of a combined shape: only the curve is drawn (no frame, labels or grid)
    int merge = 2;                   // graph lines: 0 combine (one strip with the other combined lines), 1 half combine (same strip, 50% on top), 2 separated (own strip)
};

struct SrcClip { const Track* t; const Clip* c; int row; };
struct SrcMidi { const Track* t; const MidiClip* m; const Clip* c; };

struct ScoreItem { double t, dur; std::string text, color; int kind, type, shape, lang, pos, size, font, ratio; bool border = false; };   // kind = track kind (1 marker, 2 graph)   // marker / graph track item shown above (1) or below (2) its lane

struct RLane {
    std::vector<ScoreItem> items;
    std::string name, sub, color;
    int kind = 0, clef = 0;
    bool staff = false, bass = false, axis = false, grandStaff = false;
    int octaveShift = 0;
    std::vector<const Track*> tracks;
    std::vector<std::string> rowKeys, rowNames;   // graphic lane: one row per track NAME (tracks with the same name share a row)
    bool dyn = false;                // a clip has a dynamic marking: the row gets room for it
    bool merged = false;             // several tracks with the same name were put together
    bool byName = false;             // lane made from score names: all its tracks share ONE row (their shapes overlap)
    bool separated = false;          // a track of the lane asks for 'separated': shapes drawn on top of each other at 50% instead of combined
    bool noClef = false;
    std::vector<SrcClip> clips;
    std::vector<SrcMidi> midi;
    std::vector<Auto> autos;
    int lo = 127, hi = 0;
    size_t notes = 0;
    bool used = false;
};

struct Event { int cue; double t, dur; std::string lane, clip, text, params; bool mark; };

double smoothAmp(double u) { return std::pow(std::min(1.0, std::min(u * 10, (1 - u) * 10)), 0.6); }

// ------------------------------------------------------------------ the renderer
struct Renderer {
    const Project& p;
    ScoreRenderOptions opt;
    std::string title;
    std::vector<RLane> lanes;
    std::vector<ScoreMark> sections, markers;
    std::map<const Clip*, int> cue;
    std::map<const Clip*, std::string> dataCache;
    std::deque<MidiClip> synth;      // written notes of clips (shown like MIDI)
    double W = 1500, L = 200, R = 50;   // W = page width
    double LW = 1500;                   // width a system is laid out with (the full score scales it down to fit the page)
    double end = 1, sysSec = 1, pps = 1;
    int systems = 1;
    bool detailed = false;
    int labelRows = 1;
    double tickStep = 5, labelStep = 30;
    const ScoreSpectrum* spec = nullptr;

    Renderer(const Project& proj, const ScoreRenderOptions& o, const std::string& fallbackTitle) : p(proj), opt(o), detailed(o.detailed) {
        title = p.score.title.empty() ? (fallbackTitle.empty() ? std::string("Untitled") : fallbackTitle) : p.score.title;
        end = projectEnd(p);
        sysSec = detailed ? std::min(600.0, std::max(10.0, p.score.sysSec)) : end;
        systems = std::max(1, (int)std::ceil(end / sysSec - 1e-9));
        labelRows = detailed ? 2 : 1;
        spec = (p.score.spectrum && o.spectrum && o.spectrum->valid()) ? o.spectrum : nullptr;
        auto sorted = [](std::vector<ScoreMark> v) { std::stable_sort(v.begin(), v.end(), [](const ScoreMark& a, const ScoreMark& b) { return a.t < b.t; }); return v; };
        sections = sorted(p.score.sections);
        markers = sorted(p.score.markers);
        setLayoutWidth(W);
        buildLanes();
        assignCues();
    }

    void setLayoutWidth(double w) {
        LW = w;
        pps = (LW - L - R) / sysSec;
        labelStep = 300.0;
        for (double c : { 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 }) { labelStep = c; if (c * pps >= 56) break; }
        tickStep = pps >= 7 ? 5.0 : 10.0;
    }

    std::map<const Clip*, const Track*> clipTrack;
    int midiShow(const Clip* c) const { const auto it = clipTrack.find(c); return it == clipTrack.end() ? 0 : it->second->scoreMidiShow; }
    // name of a clip in the score, with its role and language when asked for
    std::string clipTitle(const Clip& c) const {
        std::string s = c.scoreLabel.empty() ? c.name : c.scoreLabel;
        if (c.scoreShowLang) s += std::string("  [") + "Csound" + "]";
        if (c.scoreRole == 1) s += "  (fx)";
        else if (c.scoreRole == 2) s += "  (gen+fx)";
        return s;
    }


    // ---- measured clip data (Render audio -> probe file): amplitude / frequency of the clip and the data of the channel "score"
    struct ClipAna { bool ok = false; std::vector<double> t, amp, cent; double centLo = 0, centHi = 0; };
    std::map<const Clip*, ClipAna> anaCache;
    const ScoreClipProbe* probeOf(const Clip& c) const {
        if (!opt.probe) return nullptr;
        const auto it = opt.probe->find(c.name);
        return it == opt.probe->end() ? nullptr : &it->second;
    }
    const ClipAna& anaOf(const Clip& c) {
        const auto found = anaCache.find(&c);
        if (found != anaCache.end()) return found->second;
        ClipAna a;
        if (const ScoreClipProbe* pr = probeOf(c)) {
            double mx = 0;
            for (size_t i = 0; i < pr->t.size(); ++i)
                if (pr->t[i] >= c.start - 1e-9 && pr->t[i] <= c.start + c.dur + 1e-9) { a.t.push_back(pr->t[i]); a.amp.push_back(pr->rms[i]); a.cent.push_back(pr->cent[i]); mx = std::max(mx, pr->rms[i]); }
            if (a.t.size() >= 2 && mx > 1e-7) {
                a.ok = true;
                a.centLo = 1e9; a.centHi = 0;
                for (size_t i = 0; i < a.amp.size(); ++i) {
                    if (a.amp[i] > 0.05 * mx && a.cent[i] > 20) { a.centLo = std::min(a.centLo, a.cent[i]); a.centHi = std::max(a.centHi, a.cent[i]); }
                    a.amp[i] /= mx;
                }
                if (a.centHi <= a.centLo) { a.centLo = std::max(20.0, a.centLo * 0.7); a.centHi = std::max(a.centLo * 1.5, a.centHi * 1.3); }
            }
        }
        return anaCache.emplace(&c, std::move(a)).first->second;
    }
    static double lerpAt(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
        if (xs.empty()) return 0;
        if (x <= xs.front()) return ys.front();
        if (x >= xs.back()) return ys.back();
        const size_t i = (size_t)(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin());
        const double u = (x - xs[i - 1]) / std::max(1e-9, xs[i] - xs[i - 1]);
        return ys[i - 1] + (ys[i] - ys[i - 1]) * u;
    }
    // half height of the shape of a clip at time tt: measured when the clip was rendered, else a drawn envelope
    double shapeHalf(const Clip& cl, double tt, double hh) {
        const ClipAna& an = anaOf(cl);
        if (an.ok) return hh * (0.05 + 0.95 * std::pow(std::max(0.0, std::min(1.0, lerpAt(an.t, an.amp, tt))), 0.6));
        const double u = (tt - cl.start) / cl.dur, ph = cl.id * 1.7;
        return hh * smoothAmp(u) * (0.78 + 0.22 * std::sin(tt * 3.1 + ph)) * (0.9 + 0.1 * std::sin(tt * 0.9 + ph));
    }
    // colour of a clip: its own colour, else the colour of its track (lane colour as the last fallback)
    std::string clipHex(const Clip& cl, const std::string& laneColor) const {
        if (cl.color & 0xFFFFFF) return hex(cl.color);
        const auto it = clipTrack.find(&cl);
        if (it != clipTrack.end()) {
            if (it->second->color & 0xFFFFFF) return hex(it->second->color);
            return hex(paletteRgb((int)(it->second - &p.tracks[0])));
        }
        return laneColor;
    }
    static std::string mixHex(const std::vector<std::string>& cols) {
        if (cols.empty()) return "#000000";
        double r = 0, g = 0, b = 0;
        for (const std::string& c : cols) { unsigned v = 0; if (c.size() == 7) std::sscanf(c.c_str() + 1, "%x", &v); r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; }
        const double n = (double)cols.size();
        return hex(((uint32_t)std::lround(r / n) << 16) | ((uint32_t)std::lround(g / n) << 8) | (uint32_t)std::lround(b / n));
    }

    bool audible(const Track& t) const {
        if (t.muted) return false;
        for (const Track& o : p.tracks) if (o.solo && !o.muted) return t.solo;
        return true;
    }

    void buildLanes() {
        const ScoreInfo& sc = p.score;
        for (const Track& t : p.tracks) for (const Clip& c : t.clips) clipTrack[&c] = &t;
        std::vector<RLane> defs(sc.lanes.size());
        std::map<int, size_t> defIndex;
        for (size_t i = 0; i < sc.lanes.size(); ++i) {
            defIndex[sc.lanes[i].id] = i;
            defs[i].name = sc.lanes[i].name;
            defs[i].sub = sc.lanes[i].sub;
            defs[i].kind = sc.lanes[i].kind;
            defs[i].clef = sc.lanes[i].clef;
            if (sc.lanes[i].color & 0xFFFFFF) defs[i].color = hex(sc.lanes[i].color);
        }
        std::deque<RLane> autoLanes;                  // deque: references stay valid while lanes are added
        std::map<std::string, size_t> autoIndex;
        // A track without a lane of its own choice gets a lane named like the track. Tracks with the same name share it.
        auto laneOf = [&](int laneId, size_t ti, const std::string* name = nullptr) -> RLane& {
            const auto it = defIndex.find(laneId);
            if (laneId > 0 && it != defIndex.end()) return defs[it->second];
            const Track& t = p.tracks[ti];
            const std::string label = name && !name->empty() ? *name : (t.scoreLabel.empty() ? t.name : t.scoreLabel);
            const std::string key = sc.mergeSameName ? "n:" + normName(label) : "t:" + std::to_string(ti);
            auto ai = autoIndex.find(key);
            if (ai == autoIndex.end()) {
                RLane l;
                l.byName = true;
                l.name = label;
                l.sub = t.scoreSub;
                l.kind = t.scoreKind;
                l.clef = t.scoreClef;
                autoLanes.push_back(l);
                ai = autoIndex.emplace(key, autoLanes.size() - 1).first;
            } else {
                RLane& l = autoLanes[ai->second];
                if (l.kind == 0 && t.scoreKind > 0) l.kind = t.scoreKind;
                if (l.clef == 0) l.clef = t.scoreClef;
                if (l.sub.empty()) l.sub = t.scoreSub;
            }
            return autoLanes[ai->second];
        };
        std::vector<RLane*> primary(p.tracks.size(), nullptr);
        for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
            const Track& t = p.tracks[ti];
            if (t.scoreKind == 3 || !audible(t)) continue;   // hidden or muted tracks are not in the score (like a render)
            const uint32_t rgb = (t.color & 0xFFFFFF) ? (t.color & 0xFFFFFF) : paletteRgb((int)ti);
            auto place = [&](RLane& l) -> int {
                if (l.color.empty()) l.color = hex(rgb);
                if (std::find(l.tracks.begin(), l.tracks.end(), &t) == l.tracks.end()) {
                    if (!l.tracks.empty()) l.merged = true;
                    l.tracks.push_back(&t);
                }
                l.used = true;
                if (!primary[ti]) primary[ti] = &l;
                if (t.scoreOverlap == 1) l.separated = true;
                const std::string key = l.byName ? std::string() : normName(t.name);   // tracks of one named lane share ONE row
                for (size_t r = 0; r < l.rowKeys.size(); ++r) if (l.rowKeys[r] == key) return (int)r;
                l.rowKeys.push_back(key);
                l.rowNames.push_back(l.byName ? std::string() : t.name);
                return (int)l.rowKeys.size() - 1;
            };
            auto addItems = [&](RLane& l) {
                for (const TrackMark& mk : t.marks) {
                    if (mk.scorePos <= 0) continue;
                    ScoreItem it;
                    it.t = mk.t; it.dur = std::max(0.2, mk.dur);
                    it.text = mk.text; for (char& ch : it.text) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                    it.color = hex((mk.color & 0xFFFFFF) ? (mk.color & 0xFFFFFF) : rgb);
                    it.kind = t.kind; it.type = mk.type; it.shape = mk.shape; it.lang = mk.lang; it.pos = mk.scorePos; it.size = mk.size; it.font = mk.font; it.ratio = mk.ratio; it.border = mk.border;
                    l.items.push_back(it);
                }
            };
            if (t.kind == 1) {                                                  // marker track: only notes with 'In score' set reach the score (the lane with the same name)
                bool any = false;
                for (const TrackMark& mk : t.marks) if (mk.scorePos > 0) any = true;
                if (any) { RLane& l = laneOf(t.scoreLane, ti); place(l); addItems(l); }
                continue;
            }
            if (t.kind == 2) { RLane& fl = laneOf(t.scoreLane, ti); place(fl); addItems(fl); }   // graph track: its items sit above / below the lane   // free automation: a lane that only holds its automation
            for (const Clip& c : t.clips) {
                if (c.scoreLane < 0 || c.muted) continue;
                RLane& l = laneOf(c.scoreLane > 0 ? c.scoreLane : t.scoreLane, ti);
                const int row = place(l);
                l.clips.push_back({ &t, &c, row });
                if (c.scoreFreqHi > 0) l.axis = true;
                if (!c.scoreDynamic.empty()) l.dyn = true;
            }
            for (const MidiClip& mc : t.midiClips) {
                const Clip* owner = findClip(t, mc.clipId);
                if (owner && (owner->scoreLane < 0 || owner->muted)) continue;
                RLane& l = laneOf(owner && owner->scoreLane > 0 ? owner->scoreLane : t.scoreLane, ti, &t.scoreMidiLabel);
                place(l);
                l.midi.push_back({ &t, &mc, owner });
                for (const MidiNote& n : mc.notes) { l.lo = std::min(l.lo, n.pitch); l.hi = std::max(l.hi, n.pitch); ++l.notes; }
            }
            for (const Clip& c : t.clips) {       // notes written by hand for clips whose notes are made inside the code
                if (c.scoreLane < 0 || c.muted || c.scoreWritten.empty()) continue;
                std::vector<MidiNote> nn = scoreWrittenNotes(c.scoreWritten, c.dur);
                if (nn.empty()) continue;
                synth.emplace_back();
                MidiClip& mc = synth.back();
                mc.id = -c.id;
                mc.clipId = c.id;
                mc.start = c.start;
                mc.dur = c.dur;
                mc.notes = std::move(nn);
                RLane& l = laneOf(c.scoreLane > 0 ? c.scoreLane : t.scoreLane, ti, &t.scoreMidiLabel);
                place(l);
                l.midi.push_back({ &t, &mc, &c });
                for (const MidiNote& n : mc.notes) { l.lo = std::min(l.lo, n.pitch); l.hi = std::max(l.hi, n.pitch); ++l.notes; }
            }
        }
        for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
            const Track& t = p.tracks[ti];
            if (!primary[ti]) continue;
            const ParsedTrack parsed = parseTrack(t);
            int laneIdx = 0;   // same colour order as the lanes in the editor (the editor counts the lanes that are shown)
            for (const auto& a : t.automation) {
                const int myIdx = t.hiddenAutomation.count(a.first) ? -1 : laneIdx++;
                if (a.second.empty() || myIdx < 0) continue;
                if (t.scoreAutoMuted.count(a.first)) continue;                                   // M: hidden in the score
                if (!t.scoreAutoSolo.empty() && !t.scoreAutoSolo.count(a.first)) continue;       // S: only the soloed lines
                const auto m = t.scoreAuto.find(a.first);
                const int mode = m == t.scoreAuto.end() ? 1 : m->second;      // every automation with points is in the score unless it is switched off
                if (mode <= 0) continue;
                Auto au;
                au.label = a.first; au.track = t.name; au.pts = &a.second; au.mode = mode;
                { const auto sh = t.scoreAutoShape.find(a.first); if (sh != t.scoreAutoShape.end()) au.shape = sh->second; }
                { const auto cv = t.scoreAutoCurve.find(a.first); if (cv != t.scoreAutoCurve.end()) au.ratio = cv->second; }
                { const auto sk = t.scoreAutoStroke.find(a.first); if (sk != t.scoreAutoStroke.end()) au.stroke = std::min(8.0, std::max(0.3, sk->second)); }
                {   // the colour of the automation lane (the one you pick in the editor), not the colour of the track
                    static const uint32_t defaults[] = { 0xF2A65Au, 0x5BC0DEu, 0xB18BF0u, 0x77C77Du, 0xE66B87u, 0xD6C855u };
                    const auto ac = t.automationColors.find(a.first);
                    au.color = hex(ac != t.automationColors.end() && ac->second != 0 ? ac->second : defaults[(size_t)myIdx % 6]);
                }
                {
                    const auto yz = t.scoreAutoYSize.find(a.first);
                    au.ysize = yz == t.scoreAutoYSize.end() ? 1.0 : std::min(400, std::max(50, yz->second)) / 100.0;
                }
                if (t.kind == 2) {
                    au.graph = true;
                    if (au.ranged) for (const AutoPoint& pt : a.second) if (pt.v < au.lo - 1e-9 || pt.v > au.hi + 1e-9) { au.ranged = false; break; }   // data outside 0..1 (older projects): fit the line to its own values
                }
                if (t.kind == 2) { const auto mg = t.scoreAutoMerge.find(a.first); au.merge = mg == t.scoreAutoMerge.end() ? 0 : mg->second; }
                for (const CawParam& cp : parsed.params) if (cp.chn == a.first && cp.max > cp.min) { au.lo = cp.min; au.hi = cp.max; au.ranged = true; }
                RLane* home = primary[ti];
                const auto nm = t.scoreAutoNames.find(a.first);
                if (nm != t.scoreAutoNames.end() && !nm->second.empty() && t.scoreLane <= 0) {   // own name: the same name as another lane puts it in that lane
                    home = &laneOf(0, ti, &nm->second);
                    if (home->color.empty()) home->color = hex((t.color & 0xFFFFFF) ? (t.color & 0xFFFFFF) : paletteRgb((int)ti));
                    home->used = true;
                }
                home->autos.push_back(au);
            }
        }
        for (RLane& l : defs) if (l.used) lanes.push_back(l);
        for (RLane& l : autoLanes) if (l.used) lanes.push_back(l);
        for (RLane& l : lanes) {
            l.staff = l.kind == 1 || (l.kind == 0 && l.notes > 0);
            if (l.kind == 2) l.staff = false;
            if (l.staff) {
                l.noClef = l.clef == 3;
                l.grandStaff = l.clef == 6;
                l.octaveShift = l.clef == 4 ? 12 : l.clef == 5 ? -12 : 0;
                if (l.clef == 2 || l.clef == 5) l.bass = true;
                else if (l.clef == 0 || l.clef == 3) {
                    std::vector<int> ps;
                    for (const SrcMidi& m : l.midi) for (const MidiNote& n : m.m->notes) ps.push_back(n.pitch);
                    if (!ps.empty()) {
                        auto ledgerCost = [&](int ref) {
                            int cost = 0;
                            for (int pitch : ps) {
                                const int step = diatonic(pitch) - diatonic(ref);
                                if (step < 0) cost += (-step) / 2;
                                else if (step > 8) cost += (step - 8) / 2;
                            }
                            return cost;
                        };
                        const int trebleCost = ledgerCost(64);  // E4 is the bottom treble line
                        const int bassCost = ledgerCost(43);    // G2 is the bottom bass line
                        if (bassCost != trebleCost) l.bass = bassCost < trebleCost;
                        else {
                            std::nth_element(ps.begin(), ps.begin() + ps.size() / 2, ps.end());
                            l.bass = ps[ps.size() / 2] < 55;
                        }
                    }
                }
            }
        }
    }

    void assignCues() {
        std::vector<std::pair<std::pair<double, size_t>, const Clip*>> list;
        for (size_t li = 0; li < lanes.size(); ++li) {
            const RLane& l = lanes[li];
            if (l.staff) {
                for (const SrcMidi& m : l.midi) if (m.c && midiShow(m.c) != 2 && !m.m->notes.empty()) list.push_back({ { m.c->start, li }, m.c });
            } else {
                for (const SrcClip& c : l.clips) list.push_back({ { c.c->start, li }, c.c });
            }
        }
        std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        int n = 0;
        for (const auto& e : list) if (!cue.count(e.second)) cue[e.second] = ++n;
    }

    int cueOf(const Clip* c) const { const auto it = cue.find(c); return it == cue.end() ? 0 : it->second; }

    // times, copies, caw parameter values and frequency band of a clip
    std::string params(const Clip& c) {
        std::string s;
        const auto owner = clipTrack.find(&c);
        const ParsedTrack pt = owner != clipTrack.end() ? parseClipForTrack(c, *owner->second) : parseClip(c);
        int n = 0;
        for (const CawParam& cp : pt.params) {
            if (++n > 3) break;
            char b[160]; std::snprintf(b, sizeof b, "%s=%g [%g..%g, step %g]", cp.chn.c_str(), cp.value, cp.min, cp.max, cp.step);
            s += (s.empty() ? "" : "  ") + std::string(b);
        }
        return s;
    }
    std::string dataLine(const Clip& c) {
        const auto it = dataCache.find(&c);
        if (it != dataCache.end()) return it->second;
        std::string s;
        if (p.score.detailedDataLabels) {
            auto joinValues = [](const std::vector<std::string>& values, size_t maxValues) {
                if (values.empty()) return std::string();
                std::vector<size_t> picks;
                const size_t count = std::min(maxValues, values.size());
                for (size_t i = 0; i < count; ++i) {
                    const size_t ix = count <= 1 ? 0 : (i * (values.size() - 1) + (count - 1) / 2) / (count - 1);
                    if (picks.empty() || picks.back() != ix) picks.push_back(ix);
                }
                std::string out;
                for (size_t ix : picks) out += (out.empty() ? "" : ", ") + values[ix];
                if (values.size() > picks.size()) out += ", ...";
                return out;
            };
            if (const ScoreClipProbe* pr = probeOf(c)) {
                std::vector<std::string> numeric, text;
                for (const ScoreDataSample& sample : pr->data) {
                    if (sample.t < c.start - 1e-6 || sample.t > c.start + c.dur + 1e-6) continue;
                    if (sample.isText) {
                        if (sample.text.empty() || (!text.empty() && text.back() == sample.text)) continue;
                        text.push_back(sample.text);
                    } else {
                        char value[40];
                        std::snprintf(value, sizeof value, "%.4g", sample.v);
                        numeric.emplace_back(value);
                    }
                }
                if (!numeric.empty()) s += "Out score: " + joinValues(numeric, 6);
                if (!text.empty()) s += (s.empty() ? "" : "  ") + std::string("Out text: ") + joinValues(text, 3);
            }
            const auto owner = clipTrack.find(&c);
            if (owner != clipTrack.end()) {
                const ParsedTrack parsed = parseClipForTrack(c, *owner->second);
                for (const CawParam& param : parsed.params) {
                    std::vector<std::string> values;
                    const auto lane = owner->second->automation.find(param.chn);
                    if (lane != owner->second->automation.end() && autoLaneActive(*owner->second, param.chn)) {
                        const std::vector<AutoPoint>& points = lane->second;
                        char initial[40];
                        std::snprintf(initial, sizeof initial, "%.4g", autoValueAt(points, c.start, param.value));
                        values.emplace_back(initial);
                        for (const AutoPoint& point : points)
                            if (point.t > c.start + 1e-6 && point.t < c.start + c.dur - 1e-6) {
                                char value[40];
                                std::snprintf(value, sizeof value, "%.4g", point.v);
                                values.emplace_back(value);
                            }
                        char value[40];
                        std::snprintf(value, sizeof value, "%.4g", autoValueAt(points, c.start + c.dur, param.value));
                        values.emplace_back(value);
                    } else {
                        char value[40];
                        std::snprintf(value, sizeof value, "%.4g", param.value);
                        values.emplace_back(value);
                    }
                    char range[100];
                    std::snprintf(range, sizeof range, " [%.4g..%.4g, step %.4g]", param.min, param.max, param.step);
                    s += (s.empty() ? "" : "  ") + std::string("In ") + param.chn + range + ": " + joinValues(values, 4);
                }
            }
            if (s.empty()) s = "No captured score or automation data";
        } else {
            s = clockText(c.start) + "-" + clockText(c.start + c.dur) + "  " + secText(c.dur);
            if (c.runs > 1) s += "  x" + std::to_string(c.runs);
            if (c.scoreFreqHi > 0) s += "  " + hzText(std::max(20.0, c.scoreFreqLo)) + "-" + hzText(c.scoreFreqHi);
            const std::string ps = params(c);
            if (!ps.empty()) s += "  " + ps;
        }
        dataCache[&c] = s;
        return s;
    }

    // ---- geometry
    double autoH(const Auto& a) const { return (a.mode == 2 ? 22.0 : (a.graph ? (detailed ? 52.0 : 44.0) : (detailed ? 38.0 : 30.0))) * a.ysize; }
    // a lane that holds only graph lines: the strips ARE the lane (no empty instrument row above them)
    static bool graphOnly(const RLane& l) {
        if (l.autos.empty() || !l.clips.empty() || !l.midi.empty()) return false;
        for (const Track* t : l.tracks) if (t->kind != 2) return false;
        return true;
    }
    double groupH(const std::vector<const Auto*>& g) const { double h = 0; for (const Auto* a : g) h = std::max(h, autoH(*a)); return h; }
    // Graph lines that are 'combine' or 'half combine' share ONE strip (drawn on top of each other); 'separated' lines and the other automations have a strip of their own.
    std::vector<std::vector<const Auto*>> autoStrips(const RLane& l) const {
        std::vector<std::vector<const Auto*>> out;
        for (const Auto& a : l.autos) {
            if (a.merge < 2 && a.mode == 1 && !out.empty() && out.back().front()->merge < 2 && out.back().front()->mode == 1) out.back().push_back(&a);
            else out.push_back({ &a });
        }
        return out;
    }
    double stripsH(const RLane& l) const { double h = 0; for (const auto& g : autoStrips(l)) h += groupH(g) + 4.0; return h; }
    double labelArea() const { return detailed ? 24.0 : 12.0; }
    double contentH(const RLane& l) const { return l.axis ? 62.0 : 44.0; }
    double rowExtra(const RLane& l) const { return (detailed ? 12.0 : 0.0) + (l.dyn ? 11.0 : 0.0); }
    double graphicRowH(const RLane& l) const { return labelArea() + contentH(l) + rowExtra(l); }
    size_t rowCount(const RLane& l) const { return graphOnly(l) ? 0 : std::max<size_t>(1, l.rowKeys.size()); }
    double spectrumH() const { return detailed ? 100.0 : 66.0; }
    static bool hasItems(const RLane& l, int pos) { for (const ScoreItem& it : l.items) if (it.pos == pos) return true; return false; }
    // size of an item: 13 px at 100 %, the font size makes it at least as tall as its text
    static double itemScale(const ScoreItem& it) { return std::max(0.4, std::min(4.0, it.size / 100.0)); }
    static double itemFont(const ScoreItem& it, double def) { return it.font > 0 ? (double)it.font : def; }
    static double itemHeight(const ScoreItem& it) {
        double h = 13.0 * itemScale(it);
        if (it.kind == 1 || !it.text.empty()) h = std::max(h, itemFont(it, 9.0) * 1.45);
        if (it.kind == 1 && it.type == 4) h = std::max(h, itemFont(it, 9.0) * 1.2 + 4.0 + 6.0 * itemScale(it));   // line + ends + the comment
        return h;
    }
    static double bandH(const RLane& l, int pos) {
        double m = 0.0;
        for (const ScoreItem& it : l.items) if (it.pos == pos) m = std::max(m, itemHeight(it));
        return m > 0.0 ? m + 4.0 : 0.0;
    }
    double itemBand(const RLane& l) const { return bandH(l, 1) + bandH(l, 2); }
    double laneHeight(const RLane& l) const {
        double h;
        if (l.staff) h = labelRows * 12.0 + 8 + 4 + 40 + 36 + 8 + (detailed ? 12.0 : 0.0) + (l.grandStaff ? 72.0 : 0.0);
        else h = rowCount(l) * graphicRowH(l) + 8;
        return h + stripsH(l) + itemBand(l);
    }
    double systemHeight() const {
        double h = 62;
        for (const RLane& l : lanes) h += laneHeight(l);
        if (p.score.spectrum) h += spectrumH() + 14;
        return h + 10;
    }
    std::string laneLabel(const RLane& l, int k) const { return (detailed && k > 0) ? abbreviate(l.name) : l.name; }

    // ---- drawing helpers
    void ruler(Canvas& s, int k, double yR) {
        const double t0 = k * sysSec, t1 = std::min(end, t0 + sysSec);
        const double xe = L + (t1 - t0) * pps;
        s.line(L, yR, xe, yR, "#000000", 0.9);
        {
            for (double t = std::ceil(t0 / tickStep - 1e-9) * tickStep; t <= t1 + 1e-6; t += tickStep) {
                const bool big = std::fmod(t, labelStep) < 1e-6;
                const double x = L + (t - t0) * pps;
                s.line(x, yR, x, yR - (big ? 8 : 4), "#000000", 0.8);
                if (big) s.text(x, yR - 12, clockText(t), 9.5, "#111111", false, "middle");
            }
        }
        for (const ScoreMark& sc : sections) {
            if (sc.t < t0 - 1e-9 || sc.t >= t1) continue;
            const double x = L + (sc.t - t0) * pps;
            s.rect(x - 1, yR - 42, 20, 16, "#ffffff", 1.0, "#000000", 1.0);
            s.text(x + 9, yR - 30, sc.label, 11.5, "#111111", true, "middle");
            s.text(x + 25, yR - 30, fit(sc.text, 240, 10), 10, "#555555", false, "start", true);
        }
    }

    // dynamic marking: "mf", "pp<ff" (crescendo), "f>p" (diminuendo)
    void dynamicMark(Canvas& s, double xa, double xb, double y, const std::string& text, double size) {
        const size_t cut = text.find_first_of("<>");
        if (cut == std::string::npos) { s.text(xa, y, trimmed(text), size, "#111111", true, "start", true); return; }
        const std::string a = trimmed(text.substr(0, cut)), b = trimmed(text.substr(cut + 1));
        const bool cresc = text[cut] == '<';
        double xl = xa, xr = xb;
        if (!a.empty()) { s.text(xa, y, a, size, "#111111", true, "start", true); xl = xa + textWidth(a, size) * 1.15 + 4; }
        if (!b.empty()) { s.text(xb, y, b, size, "#111111", true, "end", true); xr = xb - textWidth(b, size) * 1.15 - 4; }
        if (xr - xl < 10) return;
        const double ym = y - size * 0.34, hw = size * 0.36;
        if (cresc) { s.line(xl, ym, xr, ym - hw, "#111111", 0.9); s.line(xl, ym, xr, ym + hw, "#111111", 0.9); }
        else { s.line(xl, ym - hw, xr, ym, "#111111", 0.9); s.line(xl, ym + hw, xr, ym, "#111111", 0.9); }
    }

    static std::string valueText(double v) { char b[32]; std::snprintf(b, sizeof b, "%.4g", v); return b; }

    // An automation lane, drawn like an instrument lane: label + range on the left, grid, filled curve, break points, peak values.
    void drawAutoLane(Canvas& s, const RLane& ln, const Auto& au, double ys, int k, const std::string& laneColor, size_t gi = 0, const std::vector<const Auto*>& grp = {}, double stripH = 0.0) {
        const std::string& c = au.color.empty() ? laneColor : au.color;
        const double fm = au.merge == 1 ? 3.1 : 1.0;   // half combine: the fill is stronger, the lines lie on top of each other at 50%
        const double lwDef = detailed ? 1.6 : 1.3;
        const double lw1 = au.stroke > 0 ? au.stroke : lwDef;   // stroke width of the line
        const double kr = lw1 / lwDef;                            // ratio to the default width: the dots, bars, dashes and break points of the other shapes scale with it
        const double t0 = k * sysSec, t1 = std::min(end, t0 + sysSec);
        auto X = [&](double t) { return L + (t - t0) * pps; };
        const double xe = X(t1);
        const double h = stripH > 0 ? stripH : autoH(au), top = ys + 1;
        const double pTop = top + 4, pBot = top + h - 4;
        std::vector<AutoPoint> v = *au.pts;
        std::stable_sort(v.begin(), v.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
        double mn = au.ranged ? au.lo : 1e300, mx = au.ranged ? au.hi : -1e300;
        for (const AutoPoint& pt : v) { mn = std::min(mn, pt.v); mx = std::max(mx, pt.v); }
        if (!(mx > mn)) { mn -= 0.5; mx += 0.5; }
        auto Y = [&](double val) { return pBot - (val - mn) / (mx - mn) * (pBot - pTop); };
        auto valueAt = [&](double tt) {
            if (tt <= v.front().t) return v.front().v;
            for (size_t i = 1; i < v.size(); ++i) if (tt <= v[i].t) { const double f = (tt - v[i - 1].t) / std::max(1e-9, v[i].t - v[i - 1].t); return v[i - 1].v + f * (v[i].v - v[i - 1].v); }
            return v.back().v;
        };
        // frame
        if (gi == 0 && !au.overlay) s.rect(L, top, xe - L, h, c, 0.05, "#9A9A9A", 0.5);
        const bool graphLane = graphOnly(ln);
        if (!au.overlay) s.rect(L - 28, top + (double)gi * (h / (double)std::max<size_t>(1, grp.size())), 3, h / (double)std::max<size_t>(1, grp.size()), c, 1.0, "");   // one colour bar per line of a shared strip
        // left: name, where it belongs, range
        const bool second = ln.tracks.size() > 1 || au.track != ln.name;
        if (au.overlay) {}
        else if (grp.size() > 1 || graphLane) {   // graph lane / shared strip: the name of the lane, then every line in its own colour
            if (gi == 0 && graphLane) s.text(14, top + 11, fit(ln.name, L - 86, 10.5), 10.5, "#111111", true);
            const double ly = top + (graphLane ? 22.0 : 10.0) + 9.0 * (double)gi;
            if (ly < top + h - 1) s.text(14, ly, fit(au.label, L - 86, 8.5), 8.5, c, true);
        } else {
            s.text(14, top + 13, fit(au.label, L - 86, 10.5), 10.5, "#111111", true);
            s.text(14, top + 25, fit(second ? au.track : std::string("automation"), L - 86, 8.5), 8.5, "#777777", false, "start", true);
        }
        if (gi == 0 && !au.overlay) {
            s.text(L - 33, top + 10, valueText(mx), 7.5, "#555555", false, "end", true);
            s.text(L - 33, top + h - 3, valueText(mn), 7.5, "#555555", false, "end", true);
        }
        if (au.mode == 1 && gi == 0 && !au.overlay) for (double u : { 0.0, 0.5, 1.0 }) s.line(L, pBot - u * (pBot - pTop), xe, pBot - u * (pBot - pTop), "#000000", 0.3, u == 0.5 ? 0.2 : 0.3, "1,3");
        // curve
        Pts curve;
        if (au.mode == 2) {
            const size_t n = v.size(), stepN = std::max<size_t>(1, n / 24);
            for (size_t i = 0; i < n; i += stepN) if (v[i].t >= t0 - 1e-9 && v[i].t <= t1 + 1e-9) curve.push_back({ X(v[i].t), Y(v[i].v) });
            if (curve.size() < 2) { curve.clear(); curve.push_back({ L, Y(valueAt(t0)) }); curve.push_back({ xe, Y(valueAt(t1)) }); }
            s.smoothPath(curve, c, 1.7 * kr);
            return;
        }
        if (au.shape != 0) {
            // the other looks of the line. Mirror shapes: the value is the HEIGHT of a shape mirrored around a centre line
            const double ex = std::max(0.3, std::min(3.0, au.ratio));
            const double midY = (pTop + pBot) / 2, halfMax = (pBot - pTop) / 2;
            auto halfAt = [&](double val) { return std::pow(std::max(0.0, std::min(1.0, (val - mn) / (mx - mn))), ex) * halfMax; };
            Pts pv;   // (x, value) of the points in this system, with the values at both borders
            std::vector<double> pt_t, pt_v;
            pt_t.push_back(t0); pt_v.push_back(valueAt(t0));
            for (const AutoPoint& pt : v) if (pt.t > t0 + 1e-9 && pt.t < t1 - 1e-9) { pt_t.push_back(pt.t); pt_v.push_back(pt.v); }
            pt_t.push_back(t1); pt_v.push_back(valueAt(t1));
            const size_t n = pt_t.size();
            if (au.shape == 1) {                                       // steps
                Pts st;
                for (size_t i = 0; i < n; ++i) { if (i > 0) st.push_back({ X(pt_t[i]), Y(pt_v[i - 1]) }); st.push_back({ X(pt_t[i]), Y(pt_v[i]) }); }
                s.polyline(st, c, lw1);
            } else if (au.shape == 2 || au.shape == 4) {               // straight lines / dashed
                Pts ln2;
                for (size_t i = 0; i < n; ++i) ln2.push_back({ X(pt_t[i]), Y(pt_v[i]) });
                char dashBuf[32];
                std::snprintf(dashBuf, sizeof dashBuf, "%.2f,%.2f", 5.0 * std::max(1.0, kr), 3.0 * std::max(1.0, kr));
                s.polyline(ln2, c, lw1, au.shape == 4 ? dashBuf : nullptr);
            } else if (au.shape == 3) {                                // dots
                Pts ln3;   // "dots" no longer draws dots: a plain line
                for (size_t i = 0; i < n; ++i) ln3.push_back({ X(pt_t[i]), Y(pt_v[i]) });
                s.polyline(ln3, c, lw1);
            } else if (au.shape == 5) {                                // mirror, smooth
                const int N = std::max(8, (int)((xe - L) / 2.0));
                Pts up, dn;
                for (int i = 0; i <= N; ++i) {
                    const double tt = t0 + (t1 - t0) * i / N, hv = halfAt(valueAt(tt)), x = X(tt);
                    up.push_back({ x, midY - hv }); dn.push_back({ x, midY + hv });
                }
                Pts shape = up;
                for (auto it = dn.rbegin(); it != dn.rend(); ++it) shape.push_back(*it);
                s.fillPoly(shape, c, 0.22 * fm);
                s.polyline(up, c, lw1); s.polyline(dn, c, lw1);
                s.line(L, midY, xe, midY, c, 0.4, 0.6, "2,2");
            } else if (au.shape == 6) {                                // mirror, boxes
                const bool many = n > 66;
                const size_t boxes = many ? 64 : n - 1;
                for (size_t b = 0; b < boxes; ++b) {
                    const double ta = many ? t0 + (t1 - t0) * (double)b / boxes : pt_t[b];
                    const double tb = many ? t0 + (t1 - t0) * (double)(b + 1) / boxes : pt_t[b + 1];
                    const double hv = std::max(0.6, halfAt(many ? valueAt((ta + tb) / 2) : pt_v[b]));
                    s.rect(X(ta), midY - hv, std::max(1.0, X(tb) - X(ta)), 2 * hv, c, std::min(0.9, 0.25 * fm), c, 0.8 * kr);
                }
                s.line(L, midY, xe, midY, c, 0.4, 0.6, "2,2");
            } else if (au.shape == 7) {                                // hairpin: straight wedge between the points
                Pts up, dn;
                for (size_t i = 0; i < n; ++i) { up.push_back({ X(pt_t[i]), midY - halfAt(pt_v[i]) }); dn.push_back({ X(pt_t[i]), midY + halfAt(pt_v[i]) }); }
                Pts shape = up;
                for (auto it = dn.rbegin(); it != dn.rend(); ++it) shape.push_back(*it);
                s.fillPoly(shape, c, 0.22 * fm);
                s.polyline(up, c, lw1); s.polyline(dn, c, lw1);
                s.line(L, midY, xe, midY, c, 0.4, 0.6, "2,2");
            } else {                                                   // bars
                const int N = std::max(8, (int)((xe - L) / 4.0));
                for (int i = 0; i <= N; ++i) {
                    const double tt = t0 + (t1 - t0) * i / N;
                    s.line(X(tt), pBot, X(tt), Y(valueAt(tt)), c, 1.0 * kr, 0.85);
                }
            }
        } else {
        curve.push_back({ L, Y(valueAt(t0)) });
        for (const AutoPoint& pt : v) if (pt.t > t0 + 1e-9 && pt.t < t1 - 1e-9) curve.push_back({ X(pt.t), Y(pt.v) });
        curve.push_back({ xe, Y(valueAt(t1)) });
        Pts area = curve;
        area.push_back({ xe, pBot });
        area.push_back({ L, pBot });
        s.fillPoly(area, c, std::min(0.9, 0.16 * fm));
        s.polyline(curve, c, lw1, nullptr, true);
        size_t inside = 0;
        for (const AutoPoint& pt : v) if (pt.t >= t0 && pt.t <= t1) ++inside;
        (void)inside;   // no dots on the break points: the automation is a plain drawn line
        }
        // peak values
        if (detailed && h >= 34 && !au.overlay) {
            const AutoPoint* hi = nullptr; const AutoPoint* lo = nullptr;
            for (const AutoPoint& pt : v) {
                if (pt.t < t0 - 1e-9 || pt.t > t1 + 1e-9) continue;
                if (!hi || pt.v > hi->v) hi = &pt;
                if (!lo || pt.v < lo->v) lo = &pt;
            }
            if (hi && lo && hi->v > lo->v) {
                const double xh = std::min(xe - 14, std::max(L + 14, X(hi->t))), xl = std::min(xe - 14, std::max(L + 14, X(lo->t)));
                s.text(xh, std::max(top + 8.0, Y(hi->v) - 4), valueText(hi->v), 7.5, "#444444", false, "middle", true);
                if (std::fabs(xh - xl) > 26 || std::fabs(Y(hi->v) - Y(lo->v)) > 10) s.text(xl, std::min(top + h - 1.0, Y(lo->v) + 10), valueText(lo->v), 7.5, "#444444", false, "middle", true);
            }
        }
    }

    // The other (non-mirror) shapes of a combined strip are drawn INSIDE the mirror body: mirrored around the centre line and never taller than the body.
    void drawInsideMirror(Canvas& s, const Auto& env, const std::vector<const Auto*>& oth, double ys, int k, double stripH, const std::string& laneColor) {
        if (!env.pts || env.pts->size() < 2) return;
        const double t0 = k * sysSec, t1 = std::min(end, t0 + sysSec);
        auto X = [&](double t) { return L + (t - t0) * pps; };
        const double xe = X(t1), top = ys + 1;
        const double pTop = top + 4, pBot = top + stripH - 4;
        const double midY = (pTop + pBot) / 2, halfMax = (pBot - pTop) / 2;
        const std::vector<AutoPoint>& ev = *env.pts;   // the body: 0..1, sorted by time
        auto bodyAt = [&](double tt) {
            if (tt <= ev.front().t) return ev.front().v;
            if (tt >= ev.back().t) return ev.back().v;
            size_t lo = 0, hi = ev.size() - 1;
            while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (ev[mid].t <= tt) lo = mid; else hi = mid; }
            const double f = (tt - ev[lo].t) / std::max(1e-9, ev[hi].t - ev[lo].t);
            return ev[lo].v + f * (ev[hi].v - ev[lo].v);
        };
        const double lwDef = detailed ? 1.6 : 1.3;
        for (const Auto* a : oth) {
            std::vector<AutoPoint> v = *a->pts;
            if (v.empty()) continue;
            std::stable_sort(v.begin(), v.end(), [](const AutoPoint& x, const AutoPoint& y2) { return x.t < y2.t; });
            double mn = a->ranged ? a->lo : 1e300, mx = a->ranged ? a->hi : -1e300;
            for (const AutoPoint& pt : v) { mn = std::min(mn, pt.v); mx = std::max(mx, pt.v); }
            if (!(mx > mn)) { mn -= 0.5; mx += 0.5; }
            auto normAt = [&](double tt) {
                double val;
                if (tt <= v.front().t) val = v.front().v;
                else if (tt >= v.back().t) val = v.back().v;
                else { size_t i = 1; while (i < v.size() && v[i].t < tt) ++i; const double f = (tt - v[i - 1].t) / std::max(1e-9, v[i].t - v[i - 1].t); val = v[i - 1].v + f * (v[i].v - v[i - 1].v); }
                return std::min(1.0, std::max(0.0, (val - mn) / (mx - mn)));
            };
            const std::string& col = a->color.empty() ? laneColor : a->color;
            const double lw1 = a->stroke > 0 ? a->stroke : lwDef, kr = lw1 / lwDef;
            auto halfOf = [&](double tt) { (void)normAt; return bodyAt(tt) * halfMax; };   // fills the shared body: both shapes use ONE height
            if (a->shape == 8) {                                       // bars, mirrored, inside the body
                const int N = std::max(8, (int)((xe - L) / 4.0));
                for (int i = 0; i <= N; ++i) {
                    const double tt = t0 + (t1 - t0) * i / N, hh = halfOf(tt);
                    if (hh > 0.3) s.line(X(tt), midY - hh, X(tt), midY + hh, col, 1.0 * kr, 0.9);
                }
            } else {                                                   // curve, steps, lines, dashes, dots: an upper and a lower line inside the body
                const int N = std::max(8, (int)((xe - L) / 2.0));
                Pts up, dn;
                for (int i = 0; i <= N; ++i) {
                    const double tt = t0 + (t1 - t0) * i / N, hh = halfOf(tt), x = X(tt);
                    up.push_back({ x, midY - hh }); dn.push_back({ x, midY + hh });
                }
                if (a->shape == 3) {
                    s.polyline(up, col, lw1); s.polyline(dn, col, lw1);
                } else {
                    char dashBuf[32];
                    std::snprintf(dashBuf, sizeof dashBuf, "%.2f,%.2f", 5.0 * std::max(1.0, kr), 3.0 * std::max(1.0, kr));
                    const char* dash = a->shape == 4 ? dashBuf : nullptr;
                    s.polyline(up, col, lw1, dash); s.polyline(dn, col, lw1, dash);
                }
            }
        }
    }

    // marker / graph track items in a band above (pos 1) or below (pos 2) a lane; the band is as tall as the biggest item (Size, Font size).
    // Marker: comment, letter box, cue (Q), code (coding font, colors of the language). Graph: graphic-score shapes (grain cloud, glissando, swell...).
    void drawItems(Canvas& s, const RLane& ln, int k, double yb, int pos) {
        const double t0 = k * sysSec, t1 = std::min(end, t0 + sysSec);
        auto X = [&](double t) { return L + (t - t0) * pps; };
        const double band = bandH(ln, pos);
        auto drawPrims = [&](int shape, double ax, double ay, double bx, double by, const std::string& c, double ratio) {
            for (const markgfx::Prim& pr : markgfx::shapePrims(shape, ax, ay, bx, by, ratio)) {
                Pts pts;
                for (const auto& q : pr.pts) pts.push_back({ q.first, q.second });
                if (pr.fill) s.fillPoly(pts, c, pr.solid ? 1.0 : 0.25);
                if (pr.closed && !pts.empty()) pts.push_back(pts.front());
                s.polyline(pts, c, pr.sw * 0.9);
            }
        };
        for (const ScoreItem& it : ln.items) {
            if (it.pos != pos || it.t + it.dur < t0 - 1e-9 || it.t > t1) continue;
            const double xa = X(std::max(it.t, t0)), hh = itemHeight(it);
            const double xb = std::max(xa + hh, X(std::min(it.t + it.dur, t1)));
            const double y0 = yb + (band - hh) / 2, y1 = y0 + hh, ym = (y0 + y1) / 2;
            const std::string& c = it.color;
            if (it.kind == 2) {   // graph shape, with its text beside it
                if (it.border) s.rect(xa - 1, y0 - 1, xb - xa + 2, hh + 2, "", 0.0, c, 0.9);   // optional thin border
                drawPrims(it.shape, xa, y0, xb, y1, c, it.ratio / 100.0);
                if (!it.text.empty()) { const double gfs = itemFont(it, 9.0); s.text(markgfx::shapeRight(it.shape, xa, y0, xb, y1, it.ratio / 100.0) + 4, y1 - (hh - gfs * 0.7) / 2, it.text, gfs, "#111111", false, "start", true); }
                continue;
            }
            const double fs = itemFont(it, 9.0), base = y1 - (hh - fs * 0.7) / 2;   // text baseline
            if (it.type == 1) {                                   // letter box
                const double w = std::max(hh, textWidth(it.text, fs * 1.05) + 7);
                s.rect(xa, y0, w, hh, c, 0.12, c, 1.2);
                s.text(xa + w / 2, base, it.text, fs * 1.05, "#111111", true, "middle");
            } else if (it.type == 2) {                            // cue Q
                const double cx = xa + hh / 2;
                if (it.shape == 0) s.circle(cx, ym, hh / 2, "", c, 1.2);
                else if (it.shape == 1) s.rect(xa, y0, hh, hh, c, 0.12, c, 1.2);
                else if (it.shape == 2) { Pts d = { { xa, ym }, { cx, y0 }, { xa + hh, ym }, { cx, y1 }, { xa, ym } }; s.polyline(d, c, 1.2); }
                else if (it.shape == 4) {                         // [ Q ]
                    const double tk = std::min(4.0, hh * 0.3), xr = xa + hh;
                    Pts lb = { { xa + 1 + tk, y0 }, { xa + 1, y0 }, { xa + 1, y1 }, { xa + 1 + tk, y1 } };
                    Pts rb = { { xr - 1 - tk, y0 }, { xr - 1, y0 }, { xr - 1, y1 }, { xr - 1 - tk, y1 } };
                    s.polyline(lb, c, 1.2); s.polyline(rb, c, 1.2);
                }
                else s.line(xa + 1, y1, xa + hh - 1, y1, c, 1.1);
                s.text(cx, base, "Q", fs, "#111111", true, "middle");
                if (!it.text.empty()) s.text(xa + hh + 4, base, fit(it.text, xb - xa - hh - 4, fs), fs, "#111111", false, "start", true);
            } else if (it.type == 4) {                            // span bracket: a horizontal line with 90 degree ends, the comment centred above (ends down) or below (ends up)
                const double tk = std::min(hh * 0.45, 4.0 + 2.0 * itemScale(it)), ww = std::max(10.0, xb - xa), bfs = itemFont(it, 9.0);
                const bool up = it.shape == 1;
                const double yl = up ? y0 + tk : y1 - tk, ye = up ? y0 : y1;
                Pts br = { { xa, ye }, { xa, yl }, { xa + ww, yl }, { xa + ww, ye } };
                s.polyline(br, c, 1.3);
                if (!it.text.empty()) s.text(xa + ww / 2, up ? yl + 2.0 + bfs * 0.8 : yl - 2.5, fit(it.text, ww - 4, bfs), bfs, "#111111", false, "middle", true);
            } else if (it.type == 3) {                            // code: coding font, colors of the language
                static const char* const cols[] = { "#222222", "#8A3FA0", "#2E7D32", "#B5541C", "#6A8F55", "#1F5FA8" };
                const double cfs = itemFont(it, 7.8) * (it.font > 0 ? 1.0 : 1.0), cw = cfs * 0.6;
                std::vector<markgfx::Tok> toks;
                std::istringstream lines(it.text);
                std::string ln2;
                while (std::getline(lines, ln2)) {
                    for (const markgfx::Tok& tk : markgfx::highlightLine(ln2, it.lang)) toks.push_back(tk);
                    toks.push_back({ "   ", 0 });
                }
                double len = 0.0;
                for (const markgfx::Tok& tk : toks) len += tk.text.size() * cw;
                const double w = std::max(xb - xa, std::min(len + 8, 400.0));
                s.rect(xa, y0, w, hh, "#F4F4F4", 1.0, "#999999", 0.6);
                double x = xa + 4;
                const double cbase = y1 - (hh - cfs * 0.7) / 2;
                for (const markgfx::Tok& tk : toks) {
                    if (x + tk.text.size() * cw > xa + w - 2) break;
                    s.text(x, cbase, tk.text, cfs, cols[std::min(5, std::max(0, tk.kind))], tk.kind == 1, "start", tk.kind == 4, true);
                    x += tk.text.size() * cw;
                }
            } else {                                              // comment
                s.line(xa, y1, xb, y1, c, 0.9);
                s.line(xa, y0 + 4, xa, y1, c, 0.9);
                if (!it.text.empty()) s.text(xa + 3, y1 - 3.5, fit(it.text, xb - xa - 4, fs * 0.95), fs * 0.95, "#111111", false, "start", true);
            }
        }
    }

    void drawSpectrum(Canvas& s, int k, double y, double h) {
        const double t0 = k * sysSec, t1 = std::min(end, t0 + sysSec);
        auto X = [&](double t) { return L + (t - t0) * pps; };
        const double xe = X(t1);
        s.text(14, y + 14, "Spectrum", 12.5, "#111111", true);
        s.text(14, y + 27, "master mix", 9.5, "#666666", false, "start", true);
        s.rect(L, y, xe - L, h, "#ffffff", 1.0, "#999999", 0.6);
        if (!spec) {
            s.text(L + 10, y + h / 2 + 4, "render the audio (Render audio) and turn on the spectrum to see the spectrogram of the mix here", 10.5, "#888888", false, "start", true);
            return;
        }
        const int B = spec->bands;
        const double rowH = h / B;
        const double colW = 1.0 / spec->colsPerSec;
        const long c0 = std::max(0L, (long)std::floor(t0 * spec->colsPerSec));
        const long c1 = std::min((long)(spec->data.size() / (size_t)B), (long)std::ceil(t1 * spec->colsPerSec));
        for (int b = 0; b < B; ++b) {
            const double by = y + h - (b + 1) * rowH;
            long c = c0;
            while (c < c1) {
                const int q = spec->data[(size_t)c * (size_t)B + (size_t)b] * 8 / 256;
                long run = 1;
                while (c + run < c1 && (int)(spec->data[(size_t)(c + run) * (size_t)B + (size_t)b] * 8 / 256) == q) ++run;
                if (q > 0) {
                    const double xa = std::max(L, X(c * colW)), xb = std::min(xe, X((c + run) * colW));
                    if (xb > xa) s.rect(xa, by, xb - xa + 0.4, rowH + 0.3, "#1F3A66", std::min(0.95, q / 7.0), "");
                }
                c += run;
            }
        }
        // frequency scale
        for (double f : { 100.0, 1000.0, 10000.0 }) {
            if (f < spec->fLo || f > spec->fHi) continue;
            const double fy = y + h - std::log(f / spec->fLo) / std::log(spec->fHi / spec->fLo) * h;
            s.line(L - 4, fy, L, fy, "#444444", 0.7);
            s.text(L - 6, fy + 3, f >= 1000 ? std::to_string((int)(f / 1000)) + "k" : std::to_string((int)f), 8, "#555555", false, "end");
        }
    }

    // One system: returns its bottom y. `extra` = additional height per lane (page stretching).
    double drawSystem(Canvas& s, int k, double y0, double extra) {
        const double t0 = k * sysSec, t1 = std::min(end, t0 + sysSec);
        auto X = [&](double t) { return L + (t - t0) * pps; };
        const double xe = X(t1);
        const double yR = y0 + 46;
        if (detailed) {
            s.text(40, yR - 12, k == 0 ? std::string("start") : clockText(t0), 12, "#111111", true);
            s.text(40, yR + 4, clockText(t0) + " - " + clockText(t1), 10.5, "#555555", false, "start", true);
        }
        ruler(s, k, yR);
        double y = yR + 16;
        const double sysTop = y0 + 4;
        double bodyTotal = 0;
        for (const RLane& l : lanes) bodyTotal += laneHeight(l) + extra;
        if (p.score.spectrum) bodyTotal += spectrumH() + 14;
        struct Span { double top, bot; bool staff; };
        std::vector<Span> spans;

        for (const RLane& ln : lanes) {
            const double lh = laneHeight(ln) + extra;
            const double laneY0 = y;
            if (hasItems(ln, 1)) { drawItems(s, ln, k, y, 1); y += bandH(ln, 1); }
            const std::string c = ln.color;
            double lastEnd[2] = { -1e9, -1e9 };
            auto placeLabel = [&](double x, double xr, const std::string& main, const std::string& info, double yBase, int dir, int cueNo) {
                int row = 0;
                if (lastEnd[0] > x - 2) row = 1;
                if (row == 1 && (labelRows < 2 || lastEnd[1] > x - 2)) return;
                const double ty = yBase + dir * row * 12.0;
                double tx = x + 2;
                if (detailed && cueNo > 0) {
                    s.circle(tx + 6, ty - 3.5, 6.2, "", "#000000", 0.8);
                    s.text(tx + 6, ty - 0.8, std::to_string(cueNo), 7.5, "#111111", false, "middle");
                    tx += 15;
                }
                const double avail = std::max(20.0, xr - tx);
                s.text(tx, ty, fit(main, avail, 9.5), 9.5, "#111111", true);
                const double used = textWidth(main, 9.5) * 1.12 + 6;
                if (detailed && !info.empty() && used < avail) s.text(tx + used, ty, fit(info, avail - used, 9), 9, "#555555", false, "start", true);
                lastEnd[row] = tx + std::min(avail, used + (detailed ? textWidth(info, 9) : 0.0) + 4);
            };
            double yAfterBody;
            double bandTop, bandBot;

            if (ln.staff) {
                const double base = y + labelRows * 12.0 + 12;
                const double lowerBase = base + 58;
                bandTop = base - 6; bandBot = ln.grandStaff ? lowerBase + 46 : base + 46;
                for (int i = 0; i < 5; ++i) s.line(L - 28, base + i * 10, xe, base + i * 10, "#000000", 0.5);
                if (ln.grandStaff) {
                    for (int i = 0; i < 5; ++i) s.line(L - 28, lowerBase + i * 10, xe, lowerBase + i * 10, "#000000", 0.5);
                    s.gClef(L - 24, base - 4, 11);
                    s.fClef(L - 24, lowerBase + 1, 11);
                } else if (!ln.noClef) {
                    if (ln.bass) s.fClef(L - 24, base + 1, 11);
                    else s.gClef(L - 24, base - 4, 11);
                    if (ln.clef == 4) s.text(L - 7, base - 1, "8va", 6.8, "#111111", true, "start");
                    else if (ln.clef == 5) s.text(L - 7, base + 38, "8vb", 6.8, "#111111", true, "start");
                }
                spans.push_back({ base, bandBot - 6, true });
                s.text(14, base + 22, fit(laneLabel(ln, k), L - 56, 12.5), 12.5, "#111111", true);
                if (!ln.sub.empty() && !(detailed && k > 0)) s.text(14, base + 36, fit(ln.sub, L - 56, 9.5), 9.5, "#666666", false, "start", true);
                auto yOf = [&](int midi) {
                    const int written = midi + ln.octaveShift;
                    const bool bassStaff = ln.grandStaff && midi < 60 ? true : (ln.grandStaff ? false : ln.bass);
                    const double staffBase = ln.grandStaff && bassStaff ? lowerBase : base;
                    const int refMidi = bassStaff ? 43 : 64;
                    return staffBase + 40 - (diatonic(written) - diatonic(refMidi)) * 5.0;
                };
                auto yCont = [&](double midi) {
                    const bool bassStaff = ln.grandStaff && midi < 60 ? true : (ln.grandStaff ? false : ln.bass);
                    const double staffBase = ln.grandStaff && bassStaff ? lowerBase : base;
                    const int refMidi = bassStaff ? 43 : 64;
                    return staffBase + 40 - (midi * 7.0 / 12.0 - refMidi * 7.0 / 12.0) * 5.0;
                };
                const double detailY = ln.grandStaff ? lowerBase + 58 : base + 58;
                const double dataY = ln.grandStaff ? lowerBase + 71 : base + 71;
                const double slurY = ln.grandStaff ? lowerBase + 48 : base + 48;
                std::vector<SrcMidi> items = ln.midi;
                std::stable_sort(items.begin(), items.end(), [](const SrcMidi& a, const SrcMidi& b) { return a.m->start < b.m->start; });
                for (const SrcMidi& it : items) {
                    const MidiClip& mc = *it.m;
                    const int disp = midiShow(it.c);
                    const double a = std::max(mc.start, t0), b = std::min(mc.start + mc.dur, t1);
                    if (b <= a || mc.notes.empty() || disp == 2) continue;
                    const bool first = mc.start >= t0 - 1e-9;
                    double vsum = 0; int lo = 127, hi = 0;
                    for (const MidiNote& n : mc.notes) { vsum += n.velocity; lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
                    const double xa = X(a), xb = X(b), by = base - 12;
                    s.line(xa, by, xb, by, c, 1.2);
                    s.line(xa, by, xa, by + 4, c, 1.2);
                    if (b >= mc.start + mc.dur - 1e-9) s.line(xb, by, xb, by + 4, c, 1.2);
                    if (first) {
                        const std::string nm = it.c ? clipTitle(*it.c) : std::string("MIDI");
                        placeLabel(xa, xb, nm, it.c && it.c->scoreDetail ? clipText(*it.c) : "", by - 3, -1, it.c ? cueOf(it.c) : 0);
                        const bool ownDyn = it.c && !it.c->scoreDynamic.empty();
                        if (ownDyn) dynamicMark(s, xa, xb, detailY, it.c->scoreDynamic, detailed ? 11 : 9.5);
                        else s.text(xa, detailY, dynamicFor(vsum / mc.notes.size()), detailed ? 11 : 9.5, "#111111", true, "start", true);
                        if (detailed) {
                            if (!ownDyn) s.text(xa + 22, detailY, pitchName(lo) + " - " + pitchName(hi) + "  " + std::to_string(mc.notes.size()) + " notes", 8.5, "#666666", false, "start", true);
                            if (it.c) s.text(xa, dataY, fit(dataLine(*it.c), xb - xa, 7.5), 7.5, "#777777", false, "start", true);
                        }
                    }
                    if (disp == 3) {   // pitch range band: lowest to highest note of the region
                        const double yh = yOf(hi) - 4, yl = yOf(lo) + 4;
                        s.rect(xa, yh, xb - xa, std::max(8.0, yl - yh), c, 0.20, c, 1.0);
                        if (first) {
                            s.text(xa + 3, yh - 2, pitchName(hi), 8, "#555555", false, "start", true);
                            s.text(xa + 3, yl + 9, pitchName(lo), 8, "#555555", false, "start", true);
                        }
                    } else if (disp == 1) {
                        Pts pts, raw;
                        const size_t n = mc.notes.size();
                        const size_t win = std::max<size_t>(2, n / 10);
                        for (size_t i = 0; i < n; i += win) {
                            double ts = 0, ps = 0; size_t cnt = 0;
                            for (size_t j = i; j < std::min(n, i + win); ++j) { ts += mc.start + mc.notes[j].start; ps += mc.notes[j].pitch; ++cnt; }
                            raw.push_back({ ts / cnt, ps / cnt });
                        }
                        for (const auto& r : raw) if (r.first >= t0 - 1e-9 && r.first < t1) pts.push_back({ X(r.first), yCont(r.second) });
                        s.smoothPath(pts, c, detailed ? 2.6 : 2.0);
                        if (!pts.empty()) s.note(pts.front().first, pts.front().second, c, detailed ? 3.2 : 2.4);
                    } else if (disp == 5) {
                        std::vector<const MidiNote*> visible;
                        for (const MidiNote& note : mc.notes) {
                            const double tt = mc.start + note.start;
                            if (tt >= t0 - 1e-9 && tt < t1) visible.push_back(&note);
                        }
                        std::stable_sort(visible.begin(), visible.end(), [](const MidiNote* a, const MidiNote* b) { return a->start < b->start; });
                        size_t i = 0;
                        while (i < visible.size()) {
                            size_t j = i + 1;
                            const double firstX = X(mc.start + visible[i]->start);
                            while (j < visible.size() && X(mc.start + visible[j]->start) - firstX <= 12.0) ++j;
                            if (j - i < 3) {
                                for (size_t q = i; q < j; ++q) {
                                    const MidiNote& note = *visible[q];
                                    const double x = X(mc.start + note.start), yy = yOf(note.pitch);
                                    s.note(x, yy, c, detailed ? 3.2 : 2.4);
                                }
                            } else {
                                const size_t count = j - i;
                                const size_t heads = std::min<size_t>(7, count);
                                double xsum = 0.0, yTop = 1e9, yBot = -1e9;
                                for (size_t q = i; q < j; ++q) {
                                    const double x = X(mc.start + visible[q]->start);
                                    const double yy = yOf(visible[q]->pitch);
                                    xsum += x; yTop = std::min(yTop, yy); yBot = std::max(yBot, yy);
                                }
                                const double cx = xsum / count;
                                for (size_t h = 0; h < heads; ++h) {
                                    const size_t ix = i + (heads <= 1 ? 0 : h * (count - 1) / (heads - 1));
                                    const double yy = yOf(visible[ix]->pitch);
                                    const double dx = ((double)h - (heads - 1) * 0.5) * 1.7;
                                    s.note(cx + dx, yy, c, detailed ? 2.9 : 2.3);
                                }
                                const double stemX = cx + 5;
                                s.line(stemX, yTop - 7, stemX, yBot + 7, c, 0.8);
                                s.line(stemX, yTop - 7, stemX + 7, yTop - 12, c, 1.2);
                                s.text(cx, yTop - 15, "x" + std::to_string(count), 7.0, "#555555", false, "middle", true);
                            }
                            i = j;
                        }
                    } else {
                        const double r = detailed ? 4.2 : 2.8;
                        bool labelled = false;
                        double lastName = -1e9;
                        int dynLast = -1;
                        size_t idx = 0;
                        for (const MidiNote& n : mc.notes) {
                            ++idx;
                            const double tn = mc.start + n.start;
                            if (tn < t0 - 1e-9 || tn >= t1) continue;
                            const double x = X(tn), yy = yOf(n.pitch);
                            const bool bassStaff = ln.grandStaff && n.pitch < 60;
                            const double noteStaffBase = ln.grandStaff && bassStaff ? lowerBase : base;
                            for (int lg = 1; lg <= 6; ++lg) {
                                const double ly = noteStaffBase - lg * 10.0;
                                if (yy <= ly + 2 && yy < noteStaffBase - 4) s.line(x - 7, ly, x + 7, ly, "#000000", 0.6);
                                const double ly2 = noteStaffBase + 40 + lg * 10.0;
                                if (yy >= ly2 - 2 && yy > noteStaffBase + 44) s.line(x - 7, ly2, x + 7, ly2, "#000000", 0.6);
                            }
                            const double dl = std::min(n.dur * pps, xe - x);
                            if (dl > 8) s.line(x + r, yy, x + dl, yy, c, 0.8, 0.8);
                            s.note(x, yy, c, r);
                            if (n.quarter == 1) s.qsharp(x, yy, r * 2.2, "#111111");
                            else if (n.quarter == 2) s.qflat(x, yy, r * 2.2, "#111111");
                            else if (n.quarter == 3) s.koron(x, yy, r * 2.2, "#111111");
                            else if (isBlackKey(n.pitch)) { if (((n.pitch % 12) + 12) % 12 == 10) s.flat(x, yy, r * 2.2, "#111111"); else s.sharp(x, yy, r * 2.2, "#111111"); }
                            if (disp == 4) {
                                if (x - lastName > 20) { s.text(x, yy - 8, pitchName(n.pitch), 7.5, "#555555", false, "middle", true); lastName = x; }
                            } else if (detailed && !labelled) { labelled = true; s.text(x, yy - 9, pitchName(n.pitch), 8.5, "#666666", false, "middle"); }
                            if (detailed && (idx - 1) % 5 == 0 && !(it.c && !it.c->scoreDynamic.empty())) {   // dynamic change inside a clip
                                const int d = n.velocity / 20;
                                if (dynLast >= 0 && d != dynLast && !first) {}
                                if (dynLast >= 0 && d != dynLast) s.text(x, detailY, dynamicFor(n.velocity), 9, "#111111", true, "middle", true);
                                dynLast = d;
                            }
                        }
                    }
                    if (detailed && (disp == 0 || disp == 4)) s.slur(xa, xb, slurY, c);
                }
                yAfterBody = y + laneHeight(ln) - stripsH(ln) - itemBand(ln);
            } else {
                const size_t rows = rowCount(ln);
                const double rh = graphicRowH(ln), ch = contentH(ln);
                bandTop = y + labelArea(); bandBot = y + rows * rh - (detailed ? 18 : 6);
                for (size_t r = 0; r < rows; ++r) {
                    const std::string rowName = r < ln.rowNames.size() ? ln.rowNames[r] : std::string();
                    const double ry = y + r * rh;
                    const double cTop = ry + labelArea(), cBot = cTop + ch;
                    const double mid = (cTop + cBot) / 2;
                    lastEnd[0] = lastEnd[1] = -1e9;
                    s.line(L - 28, mid, xe, mid, "#000000", 0.5, 0.5, "3,3");
                    if (r == 0) {
                        spans.push_back({ mid, y + rows * rh - (detailed ? 18 : 6), false });
                        s.text(14, mid - 2, fit(laneLabel(ln, k), L - 56, 12.5), 12.5, "#111111", true);
                        if (!ln.sub.empty() && !(detailed && k > 0)) s.text(14, mid + 11, fit(ln.sub, L - 56, 9.5), 9.5, "#666666", false, "start", true);
                        if (rows > 1 && !rowName.empty()) s.text(14, mid + 23, fit(rowName, L - 56, 9), 9, "#888888", false, "start", true);
                    } else if (!rowName.empty()) {
                        s.text(14, mid + 3, fit(rowName, L - 56, 9.5), 9.5, "#555555", false, "start", true);
                    }
                    auto yF = [&](double f) {   // log frequency axis 50 Hz .. 16 kHz
                        const double u = std::min(1.0, std::max(0.0, std::log(std::max(20.0, f) / 50.0) / std::log(16000.0 / 50.0)));
                        return cBot - u * ch;
                    };
                    if (ln.axis) {
                        for (double f : { 100.0, 1000.0, 10000.0 }) {
                            s.line(L - 4, yF(f), L, yF(f), "#444444", 0.6);
                            s.text(L - 6, yF(f) + 3, f >= 1000 ? std::to_string((int)(f / 1000)) + "k" : std::to_string((int)f), 7.5, "#666666", false, "end");
                            s.line(L, yF(f), xe, yF(f), "#000000", 0.3, 0.25, "1,4");
                        }
                    }
                    const bool hasPitch = ln.notes > 0 && ln.hi >= ln.lo;
                    auto yPitch = [&](double midi) {
                        if (ln.axis) return yF(440.0 * std::pow(2.0, (midi - 69.0) / 12.0));
                        if (ln.hi <= ln.lo) return mid;
                        return mid + 17 - (midi - ln.lo) / (double)(ln.hi - ln.lo) * 34.0;
                    };
                    std::vector<const Clip*> cs;
                    for (const SrcClip& sc : ln.clips) if (sc.row == (int)r) cs.push_back(sc.c);
                    std::stable_sort(cs.begin(), cs.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
                    auto isPitched = [&](const Clip* cl) {
                        if (!hasPitch) return false;
                        for (const SrcMidi& sm : ln.midi) if (sm.c == cl && !sm.m->notes.empty()) return true;
                        return false;
                    };
                    auto geomOf = [&](const Clip* cl, double& gcy, double& ghh) {
                        gcy = mid; ghh = 5 + 4.0 * std::min(cl->runs, 4);
                        if (ln.axis) {
                            if (cl->scoreFreqHi > 0) {
                                const double flo = std::max(20.0, cl->scoreFreqLo), fhi = std::max(flo * 1.05, cl->scoreFreqHi);
                                gcy = (yF(flo) + yF(fhi)) / 2;
                                ghh = std::max(3.0, (yF(flo) - yF(fhi)) / 2);
                            } else ghh = std::min(ghh, ch / 2 - 2);
                        }
                    };
                    // frequency position of a measured clip (centroid of its spectrum) on the frequency axis
                    auto cyAt = [&](const Clip* cl, double cy0, double ghh, double tt) {
                        if (ln.axis && cl->scoreFreqHi <= 0) {
                            const ClipAna& an = anaOf(*cl);
                            if (an.ok) { const double cen = lerpAt(an.t, an.cent, tt); if (cen > 20) return std::min(cBot - ghh, std::max(cTop + ghh, yF(cen))); }
                        }
                        return cy0;
                    };
                    // OVERLAP: the shapes of all 'shape' clips of this row are combined into ONE shape (union of their outlines).
                    // Where only one colour is active the shape has that colour, where clips of different colours overlap it gets both (mixed).
                    std::set<const Clip*> inUnion;
                    if (!ln.separated) {
                        std::vector<const Clip*> sh;
                        for (const Clip* cl : cs) if (cl->scoreStyle == 0 && !isPitched(cl) && cl->start + cl->dur > t0 && cl->start < t1) sh.push_back(cl);
                        if (!sh.empty()) {
                            double tA = 1e18, tB = -1e18;
                            for (const Clip* cl : sh) { tA = std::min(tA, cl->start); tB = std::max(tB, cl->start + cl->dur); }
                            tA = std::max(tA, t0); tB = std::min(tB, t1);
                            struct Col { bool on = false; double x = 0, top = 0, bot = 0; std::string col; };
                            std::vector<Col> cols;
                            const int N = std::max(8, (int)((tB - tA) * pps / 2.5));
                            std::vector<double> gcy(sh.size()), ghh(sh.size());
                            std::vector<std::string> gcol(sh.size());
                            for (size_t q = 0; q < sh.size(); ++q) { geomOf(sh[q], gcy[q], ghh[q]); gcol[q] = clipHex(*sh[q], c); }
                            for (int i = 0; i <= N; ++i) {
                                const double tt = tA + (tB - tA) * i / N;
                                Col cc; cc.x = X(tt); cc.top = 1e18; cc.bot = -1e18;
                                std::vector<std::string> act;
                                for (size_t q = 0; q < sh.size(); ++q) {
                                    if (tt < sh[q]->start || tt > sh[q]->start + sh[q]->dur) continue;
                                    const double am = shapeHalf(*sh[q], tt, ghh[q]), cyT = cyAt(sh[q], gcy[q], ghh[q], tt);
                                    cc.top = std::min(cc.top, cyT - am); cc.bot = std::max(cc.bot, cyT + am);
                                    act.push_back(gcol[q]);
                                    inUnion.insert(sh[q]);
                                }
                                if (!act.empty()) { cc.on = true; cc.col = mixHex(act); }
                                cols.push_back(cc);
                            }
                            size_t i = 0;
                            while (i < cols.size()) {
                                if (!cols[i].on) { ++i; continue; }
                                size_t j = i;                                   // run of the same colour
                                while (j + 1 < cols.size() && cols[j + 1].on && cols[j + 1].col == cols[i].col) ++j;
                                const size_t jEnd = (j + 1 < cols.size() && cols[j + 1].on) ? j + 1 : j;   // share the border column with the next colour
                                Pts top, bot, shape;
                                for (size_t q = i; q <= jEnd; ++q) { top.push_back({ cols[q].x, cols[q].top }); bot.push_back({ cols[q].x, cols[q].bot }); }
                                shape = top;
                                for (auto it = bot.rbegin(); it != bot.rend(); ++it) shape.push_back(*it);
                                s.fillPoly(shape, cols[i].col, 0.30);
                                s.polyline(top, cols[i].col, 1.0);
                                s.polyline(bot, cols[i].col, 1.0);
                                if (i == 0 || !cols[i - 1].on) s.line(cols[i].x, cols[i].top, cols[i].x, cols[i].bot, cols[i].col, 1.0);
                                if (j + 1 >= cols.size() || !cols[j + 1].on) s.line(cols[j].x, cols[j].top, cols[j].x, cols[j].bot, cols[j].col, 1.0);
                                for (size_t q = i; q <= j; q += 3) {            // fine hairlines inside, like the single shape
                                    const double h1 = hash01((uint32_t)(q * 977 + k * 31 + 5)), h2 = hash01((uint32_t)(q * 313 + 11));
                                    const double md = (cols[q].top + cols[q].bot) / 2;
                                    s.line(cols[q].x, md - (md - cols[q].top) * h1, cols[q].x, md + (cols[q].bot - md) * h2, cols[i].col, 0.5, 0.75);
                                }
                                i = j + 1;
                            }
                        }
                    }
                    for (const Clip* cl : cs) {
                        const double a = std::max(cl->start, t0), b = std::min(cl->start + cl->dur, t1);
                        if (b <= a) continue;
                        const std::string nm = clipTitle(*cl);
                        const double xa = X(a), xb = X(b);
                        const bool firstSeg = cl->start >= t0 - 1e-9;
                        bool pitched = false;
                        if (hasPitch) for (const SrcMidi& sm : ln.midi) if (sm.c == cl && !sm.m->notes.empty()) { pitched = true; break; }
                        if (pitched) {
                            if (midiShow(cl) == 2) continue;
                            for (const SrcMidi& sm : ln.midi) {
                                if (sm.c != cl || sm.m->notes.empty()) continue;
                                const MidiClip& mc = *sm.m;
                                const double ma = std::max(mc.start, t0), mb = std::min(mc.start + mc.dur, t1);
                                if (mb <= ma) continue;
                                s.line(X(ma), cTop - 2, X(mb), cTop - 2, c, 1.2);
                                s.line(X(ma), cTop - 2, X(ma), cTop + 2, c, 1.2);
                                if (mc.start >= t0 - 1e-9) placeLabel(X(ma), X(mb), nm, cl->scoreDetail ? clipText(*cl) : std::string(), ry + labelArea() - 4, -1, cueOf(cl));
                                if (midiShow(cl) == 3) {
                                    int plo = 127, phi = 0;
                                    for (const MidiNote& n : mc.notes) { plo = std::min(plo, n.pitch); phi = std::max(phi, n.pitch); }
                                    const double yh = yPitch(phi) - 3, yl = yPitch(plo) + 3;
                                    s.rect(X(ma), yh, X(mb) - X(ma), std::max(6.0, yl - yh), c, 0.22, c, 1.0);
                                    if (detailed && mc.start >= t0 - 1e-9) s.text(X(ma) + 3, yh - 2, pitchName(phi) + " - " + pitchName(plo), 7.5, "#555555", false, "start", true);
                                } else if (midiShow(cl) == 1) {
                                    Pts pts;
                                    const size_t n = mc.notes.size(), win = std::max<size_t>(2, n / 10);
                                    for (size_t i = 0; i < n; i += win) {
                                        double ts = 0, ps = 0; size_t cnt = 0;
                                        for (size_t j = i; j < std::min(n, i + win); ++j) { ts += mc.start + mc.notes[j].start; ps += mc.notes[j].pitch; ++cnt; }
                                        const double tt = ts / cnt;
                                        if (tt >= t0 - 1e-9 && tt < t1) pts.push_back({ X(tt), yPitch(ps / cnt) });
                                    }
                                    s.smoothPath(pts, c, detailed ? 2.4 : 1.8);
                                } else {
                                    double lastName = -1e9;
                                    for (const MidiNote& n : mc.notes) {
                                        const double tn = mc.start + n.start;
                                        if (tn < t0 - 1e-9 || tn >= t1) continue;
                                        const double x = X(tn), yy = yPitch(n.pitch);
                                        s.line(x, yy, x + std::max(3.0, std::min(n.dur * pps, xe - x)), yy, c, detailed ? 2.2 : 1.6, 0.9);
                                        if (midiShow(cl) == 4 && x - lastName > 22) { s.text(x, yy - 4, pitchName(n.pitch), 7, "#555555", false, "start", true); lastName = x; }
                                    }
                                }
                            }
                            if (!cl->scoreDynamic.empty() && firstSeg) dynamicMark(s, xa, xb, cBot + 8, cl->scoreDynamic, 9);
                            if (detailed && cl->scoreDetail && firstSeg) s.text(xa + 2, ry + rh - 3, fit(dataLine(*cl), xb - xa, 7.5), 7.5, "#777777", false, "start", true);
                            continue;
                        }
                        // sound material
                        const std::string cc = clipHex(*cl, c);
                        double cy = mid, hh = 5;
                        geomOf(cl, cy, hh);
                        const double fillOp = ln.separated ? 0.50 : 0.30;   // separated: on top of each other at 50%
                        const int N = std::max(8, (int)((b - a) * pps / 3));
                        if (cl->scoreStyle == 1) {
                            s.line(xa, cy, xb, cy, cc, 2.4);
                            s.line(xa, cy - 6, xa, cy + 6, cc, 1.4);
                            s.line(xb, cy - 6, xb, cy + 6, cc, 1.4);
                        } else if (cl->scoreStyle == 2) {
                            const int count = std::max(6, (int)((b - a) * pps / 4.5) * std::min(cl->runs, 4));
                            for (int i = 0; i < count; ++i) {
                                const double gx = xa + hash01((uint32_t)(cl->id * 131 + i * 17 + k)) * (xb - xa);
                                const double gy = cy + (hash01((uint32_t)(cl->id * 71 + i * 29)) - 0.5) * 2 * hh;
                                s.circle(gx, gy, 1.0 + hash01((uint32_t)(i * 7 + cl->id)) * 1.6, cc);
                            }
                        } else if (cl->scoreStyle == 3) {
                            s.rect(xa, cy - hh, xb - xa, 2 * hh, cc, cl->muted ? 0.08 : fillOp, cc, 1.2);
                            for (double hx = xa + 4; hx < xb - 2; hx += 6) s.line(hx, cy + hh, hx + 5, cy - hh, cc, 0.5, 0.6);
                        } else if (cl->scoreStyle == 4) {
                            // SLICED WAVE (granular): the wave of the sound cut into slices, every slice its own little piece of wave
                            const double sliceW = 5.0, gap = 2.5;
                            for (double sx = xa; sx < xb - 1; sx += sliceW + gap) {
                                const double tt = a + (sx + sliceW / 2 - xa) / pps;
                                const double am = shapeHalf(*cl, tt, hh), cyT = cyAt(cl, cy, hh, tt);
                                s.rect(sx, cyT - am, std::min(sliceW, xb - sx), 2 * am, cc, ln.separated ? 0.30 : 0.16, "");
                                Pts w;
                                for (int j = 0; j <= 6; ++j) w.push_back({ sx + sliceW * j / 6.0, cyT + am * 0.9 * std::sin(j * 1.9 + sx * 0.7 + cl->id) });
                                s.polyline(w, cc, 0.8);
                            }
                        } else if (cl->scoreStyle == 5) {
                            // FM GRAPH: a carrier wave bent by a modulator; the depth of the bend follows the loudness
                            Pts w;
                            const int M = std::max(24, (int)((xb - xa) / 1.2));
                            for (int i = 0; i <= M; ++i) {
                                const double xx = xa + (xb - xa) * i / M, tt = a + (xx - xa) / pps;
                                const double am = shapeHalf(*cl, tt, hh), cyT = cyAt(cl, cy, hh, tt);
                                const double idx = 1.0 + 3.5 * am / std::max(1.0, hh);
                                w.push_back({ xx, cyT + am * std::sin(xx * 0.85 + idx * std::sin(xx * 0.21 + cl->id)) });
                            }
                            s.polyline(w, cc, 1.0);
                            Pts hi, lo;
                            for (int i = 0; i <= N; ++i) {
                                const double tt = a + (b - a) * i / N, am = shapeHalf(*cl, tt, hh), cyT = cyAt(cl, cy, hh, tt);
                                hi.push_back({ X(tt), cyT - am }); lo.push_back({ X(tt), cyT + am });
                            }
                            s.polyline(hi, cc, 0.5, "2,2");
                            s.polyline(lo, cc, 0.5, "2,2");
                            if (firstSeg && detailed) s.text(xa + 2, cy - hh - 2, "FM", 7.5, "#777777", false, "start", true);
                        } else if (cl->scoreStyle == 6) {
                            // DOTS AND LINES (keys, leads): one dot per sounded note at its pitch height, a line for how long it rings
                            const ClipAna& an = anaOf(*cl);
                            std::vector<double> ons;
                            if (an.ok) {
                                for (size_t i = 1; i < an.t.size(); ++i)
                                    if (an.t[i] >= a && an.t[i] <= b && ((an.amp[i] > 0.12 && an.amp[i - 1] <= 0.12) || (an.amp[i] > 0.15 && an.amp[i] > 1.6 * an.amp[i - 1] + 0.05))) ons.push_back(an.t[i]);
                            }
                            if (ons.empty()) { const int n = std::max(1, std::min(cl->runs, 64)); for (int r2 = 0; r2 < n; ++r2) ons.push_back(a + (b - a) * (r2 + 0.5) / n); }
                            if (ons.size() > 600) ons.resize(600);
                            for (size_t i = 0; i < ons.size(); ++i) {
                                const double tEnd0 = i + 1 < ons.size() ? ons[i + 1] : b;
                                double tEnd = tEnd0;
                                if (an.ok) for (size_t q = 0; q < an.t.size(); ++q) if (an.t[q] > ons[i] && an.t[q] < tEnd0 && an.amp[q] < 0.05) { tEnd = an.t[q]; break; }
                                double yy = cy;
                                if (an.ok) {
                                    const double cen = std::max(an.centLo, std::min(an.centHi, lerpAt(an.t, an.cent, ons[i])));
                                    if (ln.axis) yy = std::min(cBot - 2, std::max(cTop + 2, yF(cen)));
                                    else yy = cy + hh * 0.8 - (std::log(cen / an.centLo) / std::log(an.centHi / an.centLo)) * 2 * hh * 0.8;
                                } else yy = cy + (hash01((uint32_t)(cl->id * 17 + i * 5)) - 0.5) * hh;
                                s.line(X(ons[i]), yy, std::max(X(ons[i]) + 3.0, X(tEnd)), yy, cc, 1.4);
                                s.circle(X(ons[i]), yy, 2.4, cc);
                            }
                        } else if (!inUnion.count(cl)) {
                            // SHAPE, separated: the shape of this clip alone (amplitude shape, frequency position when measured)
                            Pts up, dn;
                            for (int i = 0; i <= N; ++i) {
                                const double tt = a + (b - a) * i / N;
                                const double am = shapeHalf(*cl, tt, hh), cyT = cyAt(cl, cy, hh, tt);
                                up.push_back({ X(tt), cyT - am });
                                dn.push_back({ X(tt), cyT + am });
                            }
                            Pts shape = up;
                            for (auto it = dn.rbegin(); it != dn.rend(); ++it) shape.push_back(*it);
                            s.polygon(shape, cc, cl->muted ? 0.08 : fillOp, 1.0, cl->muted);
                            const int step = std::max(2, 8 - 2 * std::min(cl->runs, 3));
                            for (int i = 1; i < N; i += step) {
                                const double h1 = hash01((uint32_t)(cl->id * 977 + i + k * 31)), h2 = hash01((uint32_t)(cl->id * 313 + i * 7));
                                const double md = (up[(size_t)i].second + dn[(size_t)i].second) / 2, amp = md - up[(size_t)i].second;
                                s.line(up[(size_t)i].first, md - amp * h1, up[(size_t)i].first, md + amp * h2, cc, 0.5, 0.75);
                            }
                        }
                        // DETAILED: everything the code sent to the channel "score" (numbers, signals as a curve, strings as text)
                        if (cl->scoreDetail) {
                            if (const ScoreClipProbe* pr = probeOf(*cl)) {
                                double lo = 1e18, hi = -1e18;
                                for (const ScoreDataSample& sd : pr->data) if (!sd.isText && sd.t >= cl->start && sd.t <= cl->start + cl->dur) { lo = std::min(lo, sd.v); hi = std::max(hi, sd.v); }
                                if (hi >= lo) {
                                    Pts curve;
                                    for (const ScoreDataSample& sd : pr->data) {
                                        if (sd.isText || sd.t < a || sd.t > b) continue;
                                        curve.push_back({ X(sd.t), hi > lo ? cy + hh - (sd.v - lo) / (hi - lo) * 2 * hh : cy });
                                    }
                                    s.polyline(curve, "#222222", detailed ? 1.3 : 1.0);
                                    if (firstSeg && detailed) {
                                        char bufHi[32], bufLo[32];
                                        std::snprintf(bufHi, sizeof bufHi, "%.4g", hi); std::snprintf(bufLo, sizeof bufLo, "%.4g", lo);
                                        s.text(xa + 2, cy - hh + 6, bufHi, 6.5, "#444444", false, "start", true);
                                        s.text(xa + 2, cy + hh - 1, bufLo, 6.5, "#444444", false, "start", true);
                                    }
                                }
                                std::string prev;
                                double lastEndX = -1e9;
                                int tr = 0;
                                for (const ScoreDataSample& sd : pr->data) {
                                    if (!sd.isText || sd.t < a || sd.t > b || sd.text == prev) continue;
                                    prev = sd.text;
                                    const double tx = X(sd.t);
                                    if (tx < lastEndX) continue;
                                    s.text(tx, cy - hh - 3 - (tr++ % 2) * 8, fit(sd.text, 120, 7.5), 7.5, "#222222", false, "start", true);
                                    lastEndX = tx + std::min(120.0, textWidth(sd.text, 7.5)) + 3;
                                }
                            }
                        }
                        if (firstSeg) {
                            std::string extra = cl->scoreDetail ? clipText(*cl) : std::string();
                            if (cl->scoreDetail && cl->runs > 1) extra = "x" + std::to_string(cl->runs) + (extra.empty() ? "" : "  " + extra);
                            placeLabel(xa, xb, nm, extra, ry + labelArea() - 4, -1, cueOf(cl));
                        }
                        if (!cl->scoreDynamic.empty() && firstSeg) dynamicMark(s, xa, xb, cBot + 8, cl->scoreDynamic, 9);
                        if (detailed && firstSeg) s.text(xa + 2, ry + rh - 3, fit(dataLine(*cl), xb - xa, 7.5), 7.5, "#777777", false, "start", true);
                    }
                    if (hasPitch && r == 0 && detailed && !ln.axis) {
                        s.text(L - 6, mid - 12, pitchName(ln.hi), 8, "#666666", false, "end", true);
                        s.text(L - 6, mid + 22, pitchName(ln.lo), 8, "#666666", false, "end", true);
                    }
                }
                yAfterBody = y + rows * rh + 8;
            }

            // automation lanes of this score lane: drawn like instruments, right under the lane
            double ys = yAfterBody;
            for (const auto& grp : autoStrips(ln)) {
                // 'combine' lines become ONE shape: the mix of all of them (weighted by their Y size), 'half combine' lines lie on top of it at 50%
                std::vector<const Auto*> draw;
                std::vector<AutoPoint> envPts;
                Auto env;
                std::vector<const Auto*> comb;
                std::vector<Auto> extras;   // extra shape layers of the combined shape
                extras.reserve(8);
                for (const Auto* a : grp) if (a->merge == 0 && a->mode == 1 && grp.size() > 1) comb.push_back(a);
                // With a mirror shape in the group, ALL combined lines reshape ONE common height (weighted mix of their values);
                // the mirror shape is drawn on it and the other shapes (bars, lines, dashes...) fill it to the same height.
                std::vector<const Auto*> mirM, insideM;
                for (const Auto* a : comb) (a->shape >= 5 && a->shape <= 7 ? mirM : insideM).push_back(a);
                const bool mirrorMode = comb.size() >= 2 && !mirM.empty();
                if (!mirrorMode) { mirM = comb; insideM.clear(); }
                if (comb.size() >= 2) {
                    std::vector<double> ts;
                    for (int i = 0; i <= 400; ++i) ts.push_back(end * i / 400.0);
                    for (const Auto* a : comb) for (const AutoPoint& pt : *a->pts) ts.push_back(pt.t);
                    std::sort(ts.begin(), ts.end());
                    struct Nr { std::vector<AutoPoint> v; double mn, mx, w; };
                    std::vector<Nr> nr;
                    for (const Auto* a : comb) {
                        Nr n; n.v = *a->pts; n.w = std::max(0.05, a->ysize);   // the Y size of a line = how much it counts in the mix
                        std::stable_sort(n.v.begin(), n.v.end(), [](const AutoPoint& x, const AutoPoint& y2) { return x.t < y2.t; });
                        n.mn = a->ranged ? a->lo : 1e300; n.mx = a->ranged ? a->hi : -1e300;
                        for (const AutoPoint& pt : n.v) { n.mn = std::min(n.mn, pt.v); n.mx = std::max(n.mx, pt.v); }
                        if (!(n.mx > n.mn)) { n.mn -= 0.5; n.mx += 0.5; }
                        nr.push_back(n);
                    }
                    for (double tt : ts) {
                        double sum = 0.0, wsum = 0.0, best = 0.0;
                        size_t ni = 0;
                        for (const Nr& n : nr) {
                            double val;
                            if (tt <= n.v.front().t) val = n.v.front().v;
                            else if (tt >= n.v.back().t) val = n.v.back().v;
                            else { size_t i = 1; while (i < n.v.size() && n.v[i].t < tt) ++i; const double f = (tt - n.v[i - 1].t) / std::max(1e-9, n.v[i].t - n.v[i - 1].t); val = n.v[i - 1].v + f * (n.v[i].v - n.v[i - 1].v); }
                            const double nv = (val - n.mn) / (n.mx - n.mn);
                            sum += n.w * nv; wsum += n.w;
                            best += n.w * std::pow(std::min(1.0, std::max(0.0, nv)), (comb[ni]->shape >= 5 && comb[ni]->shape <= 7) ? std::max(0.3, std::min(3.0, comb[ni]->ratio)) : 1.0);
                            ++ni;
                        }
                        envPts.push_back({ tt, mirrorMode ? (wsum > 0 ? best / wsum : 0.0) : (wsum > 0 ? sum / wsum : 0.0) });
                    }
                    env = *comb.front();
                    env.pts = &envPts; env.lo = 0.0; env.hi = 1.0; env.ranged = true;
                    {   // the combined shape gets the mix of the colours of its lines (weighted like the mix of the values)
                        std::vector<std::pair<std::string, double>> cols;
                        double sw = 0.0, rt = 0.0;
                        for (const Auto* a : comb) {
                            if (!a->color.empty()) cols.push_back({ a->color, std::max(0.05, a->ysize) });
                            sw += std::max(0.05, a->ysize); rt += std::max(0.05, a->ysize) * a->ratio;
                        }
                        const std::string mixed = mixHexWeighted(cols);
                        if (!mixed.empty()) env.color = mixed;
                        if (sw > 0) env.ratio = rt / sw;
                        double st = 0.0; int stn = 0;
                        for (const Auto* a : comb) if (a->stroke > 0) { st += a->stroke; ++stn; }
                        env.stroke = stn ? st / stn : 0.0;
                    }
                    if (mirrorMode) {
                        env.ratio = 1.0;   // the ratios are already inside the united outline
                        env.shape = mirM.front()->shape;
                        for (const Auto* a : mirM) if (a->shape == 5) { env.shape = 5; break; }
                    }
                    env.label = comb.front()->label;
                    for (size_t i = 1; i < comb.size(); ++i) env.label += "+" + comb[i]->label.substr(comb[i]->label.size() > 4 ? 4 : 0);
                    draw.push_back(&env);
                    // COMBINE also merges the SHAPES: every different shape of the combined lines is drawn on the same mixed curve
                    // (e.g. a mirror shape + dashed lines = one filled mirror body with the dashes inside it)
                    std::vector<int> seen{ env.shape };
                    for (size_t i = 1; i < comb.size() && !mirrorMode; ++i) {
                        if (std::find(seen.begin(), seen.end(), comb[i]->shape) != seen.end()) continue;
                        seen.push_back(comb[i]->shape);
                        Auto ex = env; ex.shape = comb[i]->shape; ex.overlay = true;
                        extras.push_back(ex);
                    }
                }
                for (const Auto* a : grp) {
                    const bool inEnv = comb.size() >= 2 && a->merge == 0 && a->mode == 1;
                    if (!inEnv) draw.push_back(a);
                }
                if (draw.empty()) draw = grp;
                const double gh = groupH(grp);
                for (size_t gi = 0; gi < draw.size(); ++gi) drawAutoLane(s, ln, *draw[gi], ys, k, c, gi, draw, gh);
                for (const Auto& ex : extras) drawAutoLane(s, ln, ex, ys, k, c, 0, draw, gh);
                if (mirrorMode) drawInsideMirror(s, env, insideM, ys, k, gh, c);
                ys += gh + 4.0;
            }
            if (hasItems(ln, 2)) drawItems(s, ln, k, laneY0 + laneHeight(ln) - bandH(ln, 2), 2);
            y = laneY0 + lh;
        }

        // system barline and brackets on the left, like a printed score
        if (!spans.empty()) {
            double top = 1e300, bot = -1e300;
            for (const Span& sp : spans) { top = std::min(top, sp.top); bot = std::max(bot, sp.bot); }
            s.line(L - 28, top, L - 28, bot, "#000000", 1.1);
            size_t i = 0;
            while (i < spans.size()) {
                size_t j = i;
                while (j + 1 < spans.size() && spans[j + 1].staff == spans[i].staff) ++j;
                if (j > i) {
                    const double bt = spans[i].top - 1, bb = spans[j].bot + 1;
                    const bool st = spans[i].staff;
                    s.line(L - 34, bt, L - 34, bb, "#000000", st ? 2.6 : 1.2);
                    s.line(L - 34, bt, L - 28, bt, "#000000", st ? 1.4 : 0.8);
                    s.line(L - 34, bb, L - 28, bb, "#000000", st ? 1.4 : 0.8);
                }
                i = j + 1;
            }
        }
        if (t1 >= end - 1e-9) {   // final barline
            s.line(xe, yR, xe, y + 2, "#000000", 0.9);
            s.line(xe + 3.2, yR, xe + 3.2, y + 2, "#000000", 2.8);
        }
        if (p.score.spectrum) {
            drawSpectrum(s, k, y + 4, spectrumH());
            y += spectrumH() + 14;
        }

        const double sysBottom = y + 4;
        for (const ScoreMark& mk : markers) {
            if (mk.t < t0 - 1e-9 || mk.t >= t1) continue;
            s.line(X(mk.t), sysTop + 20, X(mk.t), sysBottom, "#DD3333", 1.3);
            s.text(X(mk.t) + 4, sysBottom + 10, fit(mk.label + "  " + clockText(mk.t), 240, 10), 10, "#DD3333", false, "start", true);
        }
        for (const ScoreMark& sc : sections)
            if (sc.t >= t0 - 1e-9 && sc.t < t1) s.line(X(sc.t), yR - 26, X(sc.t), sysBottom, "#000000", 0.5, 0.45, "1,4");
        return sysBottom + 14;
    }

    void key(Canvas& s, double ky) {
        s.line(40, ky - 14, W - 40, ky - 14, "#999999", 0.5);
        s.text(40, ky + 4, "Key", 12, "#111111", true);
        s.note(98, ky + 1, "#3CAAC8", 3.5);
        s.text(110, ky + 4, "note: position = pitch, line = duration", 10, "#444444");
        s.rect(370, ky - 7, 34, 11, "#888888", 0.4, "#888888");
        s.text(412, ky + 4, "sound material: thickness = density, height = frequency band", 10, "#444444");
        s.slur(790, 815, ky + 3, "#000000");
        s.text(822, ky + 4, "clip bracket", 10, "#444444");
        s.circle(915, ky - 1, 6.2, "", "#000000", 0.8);
        s.text(915, ky + 2, "1", 7.5, "#111111", false, "middle");
        s.text(926, ky + 4, "cue number", 10, "#444444");
        s.line(1010, ky - 2, 1040, ky - 2, "#DD3333", 1.3);
        s.text(1046, ky + 4, "time marker", 10, "#444444");
        s.text(1130, ky + 4, "[A] section", 10, "#444444");
        { Pts cv = { { 1204, ky + 3 }, { 1214, ky + 3 }, { 1224, ky - 6 }, { 1234, ky - 6 }, { 1244, ky + 3 } }; s.fillPoly({ { 1204, ky + 3 }, { 1214, ky + 3 }, { 1224, ky - 6 }, { 1234, ky - 6 }, { 1244, ky + 3 } }, "#3CAAC8", 0.2); s.polyline(cv, "#3CAAC8", 1.3); }
        s.text(1250, ky + 4, "automation lane", 10, "#444444");
    }

    void footer(Canvas& s, double H, int page, int pages) {
        s.text(40, H - 18, title, 10, "#555555", false, "start", true);
        s.text(W / 2, H - 18, pages > 1 ? "- " + std::to_string(page) + " -" : std::string("- 1 -"), 10, "#555555", false, "middle");
        s.text(W - R, H - 18, p.score.composer, 10, "#555555", false, "end", true);
        if (!p.score.copyright.empty()) s.text(W / 2, H - 6, p.score.copyright, 8.5, "#777777", false, "middle", true);
    }

    void bigHeader(Canvas& s) {
        s.text(40, 54, title, 34, "#111111", true);
        const std::string kind = detailed ? "full score" : "overview";
        s.text(40, 80, p.score.subtitle.empty() ? kind : p.score.subtitle, 14, "#555555", false, "start", true);
        s.text(W - R, 54, p.score.composer, 15, "#111111", true, "end");
        s.text(W - R, 76, kind + "  |  duration " + durText(end), 11.5, "#555555", false, "end", true);
    }

    std::vector<std::string> wrap(const std::string& text, size_t cols) const {
        std::vector<std::string> out;
        std::istringstream in(text);
        std::string para;
        while (std::getline(in, para)) {
            if (!para.empty() && para.back() == '\r') para.pop_back();
            if (para.empty()) { out.push_back(""); continue; }
            std::istringstream words(para);
            std::string w, cur;
            while (words >> w) {
                if (!cur.empty() && cur.size() + 1 + w.size() > cols) { out.push_back(cur); cur.clear(); }
                cur += (cur.empty() ? "" : " ") + w;
            }
            if (!cur.empty()) out.push_back(cur);
        }
        return out;
    }

    std::vector<Event> events() {
        std::vector<Event> ev;
        for (const RLane& l : lanes)
            for (const SrcClip& sc : l.clips) {
                const Clip& c = *sc.c;
                const int q = cueOf(&c);
                if (!q) continue;
                ev.push_back({ q, c.start, c.dur, l.name, clipTitle(c), clipText(c),
                               p.score.detailedDataLabels ? dataLine(c) : params(c), false });
            }
        for (const RLane& l : lanes)       // clips of staff lanes (only their MIDI is drawn)
            if (l.staff) for (const SrcClip& sc : l.clips) {
                const Clip& c = *sc.c;
                const int q = cueOf(&c);
                bool dup = false;
                for (const Event& e : ev) if (e.cue == q) dup = true;
                if (q && !dup) ev.push_back({ q, c.start, c.dur, l.name, clipTitle(c), clipText(c),
                                              p.score.detailedDataLabels ? dataLine(c) : params(c), false });
            }
        for (const ScoreMark& m : sections) ev.push_back({ 0, m.t, 0, "section " + m.label, "", m.text, "", true });
        for (const ScoreMark& m : markers) ev.push_back({ 0, m.t, 0, "marker", "", m.label, "", true });
        std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
        return ev;
    }

    ScorePage newPage(double H = 1060) const { ScorePage pg; pg.w = W; pg.h = H; return pg; }

    ScorePage coverPage(int pages) {
        const double H = 1060;
        Canvas s; s.bw = opt.blackWhite; s.lw = p.score.lineWeight;
        bigHeader(s);
        double y = 140;
        bool anyStaff = false;
        for (const RLane& l : lanes) anyStaff = anyStaff || l.staff;
        s.text(40, y, anyStaff ? "for instruments and electronics" : "for electronics", 16, "#111111", false, "start", true);
        y += 34;
        s.text(40, y, "Lanes", 14, "#111111", true);
        y += 8;
        s.line(40, y, W - 40, y, "#000000", 0.8);
        y += 18;
        s.text(40, y, "name", 10, "#666666"); s.text(380, y, "notation", 10, "#666666"); s.text(520, y, "sources (tracks)", 10, "#666666"); s.text(900, y, "note", 10, "#666666");
        y += 6;
        for (const RLane& l : lanes) {
            y += 20;
            s.rect(40, y - 10, 6, 12, l.color, 1.0, "");
            s.text(54, y, fit(l.name, 310, 12), 12, "#111111", true);
            const std::string staffType = l.grandStaff ? "staff, grand staff" :
                l.noClef ? "staff, no clef" :
                l.clef == 4 ? "staff, treble 8va" :
                l.clef == 5 ? "staff, bass 8vb" :
                l.bass ? "staff, bass clef" : "staff, treble clef";
            s.text(380, y, l.staff ? staffType : (l.axis ? "graphic, frequency axis" : "graphic"), 11, "#333333");
            std::string src;
            for (size_t i = 0; i < l.rowNames.size(); ++i) {
                if (l.rowKeys[i].empty()) {   // named lane: one row, every track of it
                    for (size_t j = 0; j < l.tracks.size(); ++j) src += (j ? ", " : "") + l.tracks[j]->name;
                    if (l.tracks.size() > 1) src += l.separated ? "  (separated)" : "  (overlap)";
                    continue;
                }
                int n = 0;
                for (const Track* t : l.tracks) if (normName(t->name) == l.rowKeys[i]) ++n;
                src += (i ? ", " : "") + l.rowNames[i] + (n > 1 ? " (x" + std::to_string(n) + ")" : std::string());
            }
            s.text(520, y, fit(src, 360, 11), 11, "#333333");
            std::string note = l.sub;
            if (!l.autos.empty()) {
                std::string am;
                for (const Auto& a : l.autos) am += (am.empty() ? "" : ", ") + a.label;
                note += (note.empty() ? "" : "  |  ") + std::string("automation: ") + am;
            }
            s.text(900, y, fit(note, 560, 11), 11, "#555555", false, "start", true);
            if (y > H - 420) break;
        }
        y += 34;
        if (!sections.empty()) {
            s.text(40, y, "Sections", 14, "#111111", true);
            y += 8; s.line(40, y, W - 40, y, "#000000", 0.8); y += 18;
            for (const ScoreMark& m : sections) {
                if (y > H - 300) break;
                s.rect(40, y - 11, 18, 14, "#ffffff", 1.0, "#000000", 1.0);
                s.text(49, y, m.label, 10.5, "#111111", true, "middle");
                s.text(70, y, clockText(m.t), 11, "#333333");
                s.text(130, y, fit(m.text, 1000, 11), 11, "#333333", false, "start", true);
                y += 19;
            }
            y += 20;
        }
        if (!p.score.notes.empty()) {
            s.text(40, y, "Notes", 14, "#111111", true);
            y += 8; s.line(40, y, W - 40, y, "#000000", 0.8); y += 20;
            for (const std::string& ln : wrap(p.score.notes, 170)) {
                if (y > H - 190) break;
                s.text(40, y, ln, 12, "#222222"); y += 17;
            }
        }
        key(s, H - 70);
        footer(s, H, 1, pages);
        ScorePage pg = newPage(H); pg.prims = std::move(s.prims);
        return pg;
    }

    std::vector<ScorePage> eventPages(int firstPage, int pages) {
        std::vector<ScorePage> out;
        const std::vector<Event> ev = events();
        if (ev.empty()) return out;
        const double H = 1060;
        const size_t perPage = 46;
        for (size_t start = 0; start < ev.size(); start += perPage) {
            Canvas s; s.bw = opt.blackWhite; s.lw = p.score.lineWeight;
            s.text(40, 54, title, 20, "#111111", true);
            s.text(40, 76, "Cue list", 13, "#555555", false, "start", true);
            double y = 110;
            const char* timingHeads[] = { "cue", "time", "length", "lane", "clip", "text", "parameters" };
            const double timingXs[] = { 40, 90, 150, 215, 400, 560, 1090 };
            const char* flowHeads[] = { "cue", "lane", "clip", "text", "data sent / received" };
            const double flowXs[] = { 40, 90, 330, 520, 850 };
            if (p.score.detailedDataLabels) {
                for (int i = 0; i < 5; ++i) s.text(flowXs[i], y, flowHeads[i], 10, "#666666");
            } else {
                for (int i = 0; i < 7; ++i) s.text(timingXs[i], y, timingHeads[i], 10, "#666666");
            }
            s.line(40, y + 6, W - 40, y + 6, "#000000", 0.8);
            y += 24;
            for (size_t i = start; i < std::min(ev.size(), start + perPage); ++i) {
                const Event& e = ev[i];
                const std::string tone = e.mark ? "#DD3333" : "#111111";
                if (p.score.detailedDataLabels) {
                    if (e.cue) s.text(flowXs[0], y, std::to_string(e.cue), 11, "#111111", true);
                    s.text(flowXs[1], y, fit(e.lane, 225, 11), 11, "#333333", e.mark);
                    s.text(flowXs[2], y, fit(e.clip, 175, 11), 11, "#111111", !e.mark);
                    s.text(flowXs[3], y, fit(e.text, 310, 11), 11, "#333333", false, "start", true);
                    s.text(flowXs[4], y, fit(e.params, 610, 10), 10, "#666666");
                } else {
                    if (e.cue) s.text(timingXs[0], y, std::to_string(e.cue), 11, "#111111", true);
                    s.text(timingXs[1], y, clockText(e.t), 11, tone, e.mark);
                    if (!e.mark) s.text(timingXs[2], y, secText(e.dur), 11, "#333333");
                    s.text(timingXs[3], y, fit(e.lane, 175, 11), 11, "#333333", e.mark);
                    s.text(timingXs[4], y, fit(e.clip, 150, 11), 11, "#111111", !e.mark);
                    s.text(timingXs[5], y, fit(e.text, 520, 11), 11, "#333333", false, "start", true);
                    s.text(timingXs[6], y, fit(e.params, 370, 10), 10, "#666666");
                }
                y += 19;
            }
            footer(s, H, firstPage + (int)(start / perPage), pages);
            ScorePage pg = newPage(H); pg.prims = std::move(s.prims);
            out.push_back(std::move(pg));
        }
        return out;
    }
};

bool saveFile(const std::string& path, const std::string& data, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "could not write " + path; return false; }
    f << data;
    return (bool)f;
}


// Moves / scales primitives (the full score lays a system out wider and scales it to the page).
void transformPrims(std::vector<ScorePrim>& prims, size_t from, double f, double dx, double dy) {
    for (size_t i = from; i < prims.size(); ++i) {
        ScorePrim& q = prims[i];
        auto X = [&](float v) { return (float)(v * f + dx); };
        auto Y = [&](float v) { return (float)(v * f + dy); };
        switch (q.kind) {
            case ScorePrim::Line: q.x = X(q.x); q.y = Y(q.y); q.w = X(q.w); q.h = Y(q.h); break;
            case ScorePrim::Rect: q.x = X(q.x); q.y = Y(q.y); q.w = (float)(q.w * f); q.h = (float)(q.h * f); break;
            case ScorePrim::Polygon:
            case ScorePrim::Polyline:
                for (size_t j = 0; j + 1 < q.pts.size(); j += 2) { q.pts[j] = X(q.pts[j]); q.pts[j + 1] = Y(q.pts[j + 1]); }
                break;
            case ScorePrim::Ellipse:
            case ScorePrim::Circle: q.x = X(q.x); q.y = Y(q.y); q.w = (float)(q.w * f); q.h = (float)(q.h * f); break;
            case ScorePrim::Text: q.x = X(q.x); q.y = Y(q.y); q.size = (float)(q.size * f); break;
        }
        q.sw = (float)(q.sw * f); q.dash0 = (float)(q.dash0 * f); q.dash1 = (float)(q.dash1 * f);
    }
}

} // namespace

// ------------------------------------------------------------------ public
std::string scorePageToSvg(const ScorePage& page) { return primsToSvg(page); }

bool buildScorePages(const Project& p, const std::string& fallbackTitle, const ScoreRenderOptions& opt, std::vector<ScorePage>& pages, std::string& error) {
    pages.clear();
    Renderer r(p, opt, fallbackTitle);
    if (r.lanes.empty()) { error = "nothing to show: no lane has clips (or all lanes are hidden in the score)"; return false; }

    if (!opt.detailed) {
        const double top = 110, bottom = 90;
        const double nat = r.systemHeight();
        double H = 1060;
        if (top + nat + bottom > H) H = top + nat + bottom;
        const double extra = std::max(0.0, std::min(110.0, (H - top - bottom - nat) / (double)r.lanes.size()));
        Canvas s; s.bw = opt.blackWhite; s.lw = p.score.lineWeight;
        r.bigHeader(s);
        r.drawSystem(s, 0, top, extra);
        r.key(s, H - 60);
        r.footer(s, H, 1, 1);
        ScorePage pg = r.newPage(H); pg.prims = std::move(s.prims);
        pages.push_back(std::move(pg));
        return true;
    }

    // ---- full score: fixed A4 landscape pages, systems read from left to right and top to bottom, page after page.
    // A system that is too tall for its page is laid out wider and scaled down, so the page never grows.
    const double PH = 1060, top = 60, bottom = 70, gap = 12;
    const double avail = PH - top - bottom;
    const double nat = r.systemHeight() + gap;
    const int perPage = p.score.systemsPerPage > 0 ? p.score.systemsPerPage : std::max(1, (int)std::floor(avail / nat));
    const double f = std::min(1.0, avail / (perPage * nat));
    const double slot = avail / perPage;                      // page height of one system
    const double extra = f < 1.0 ? 0.0 : std::max(0.0, std::min(130.0, (slot - nat) / (double)r.lanes.size()));
    r.setLayoutWidth(r.W / f);
    const int sysPages = (r.systems + perPage - 1) / perPage;
    const int evCount = (int)r.events().size();
    const int evPages = p.score.eventList && evCount > 0 ? (evCount + 45) / 46 : 0;
    const bool cover = p.score.coverPage;
    const int total = (cover ? 1 : 0) + sysPages + evPages;
    int pageNo = 1;
    if (cover) {
        r.setLayoutWidth(r.W);                 // the cover page does not use the system width
        pages.push_back(r.coverPage(total));
        r.setLayoutWidth(r.W / f);
        ++pageNo;
    }
    for (int pg = 0; pg < sysPages; ++pg) {
        Canvas s; s.bw = opt.blackWhite; s.lw = p.score.lineWeight;
        s.text(40, 30, r.title, 14, "#111111", true);
        s.text(r.W - r.R, 30, pg == 0 && !cover ? p.score.composer : std::string("full score"), 11, "#555555", false, "end", true);
        for (int i = 0; i < perPage; ++i) {
            const int k = pg * perPage + i;
            if (k >= r.systems) break;
            Canvas sys; sys.bw = opt.blackWhite; sys.lw = p.score.lineWeight;
            r.drawSystem(sys, k, 0.0, extra);
            transformPrims(sys.prims, 0, f, 0.0, top + i * slot);
            for (ScorePrim& q : sys.prims) s.prims.push_back(std::move(q));
        }
        r.footer(s, PH, pageNo, total);
        ScorePage page = r.newPage(PH); page.prims = std::move(s.prims);
        pages.push_back(std::move(page));
        ++pageNo;
    }
    if (evPages > 0) {
        std::vector<ScorePage> ep = r.eventPages(pageNo, total);
        for (ScorePage& e : ep) pages.push_back(std::move(e));
    }
    return true;
}

bool writeScoreDocument(const Project& p, const std::string& fallbackTitle, const ScoreRenderOptions& opt, const std::string& basePath,
                        std::vector<std::string>& files, std::string& error) {
    files.clear();
    std::vector<ScorePage> pages;
    if (!buildScorePages(p, fallbackTitle, opt, pages, error)) return false;
    const std::string suffix = opt.blackWhite ? "_bw" : "";
    if (!opt.detailed) {
        const std::string path = basePath + "_score_overview" + suffix + ".svg";
        if (!saveFile(path, primsToSvg(pages[0]), error)) return false;
        files.push_back(path);
        return true;
    }
    std::vector<std::string> paths;
    std::ostringstream html;
    const std::string ttl = p.score.title.empty() ? fallbackTitle : p.score.title;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>" << esc(ttl) << " - full score</title><style>body{margin:0;background:#6d6d72;font-family:Georgia,serif}"
         << ".bar{position:sticky;top:0;background:#222;color:#eee;padding:8px 16px;font:13px sans-serif;z-index:2}.bar b{color:#fff}"
         << ".page{width:min(96vw,1500px);margin:16px auto;background:#fff;box-shadow:0 2px 12px #0008}.page svg{display:block;width:100%;height:auto}"
         << "@media print{body{background:#fff}.bar{display:none}.page{width:100%;margin:0;box-shadow:none;page-break-after:always;break-after:page}@page{size:A4 landscape;margin:0}}</style></head><body>\n"
         << "<div class=\"bar\"><b>" << esc(ttl) << "</b> &nbsp; full score, " << pages.size() << " pages. Print this page (Ctrl+P, A4 landscape, margins none) to get a PDF.</div>\n";
    for (size_t i = 0; i < pages.size(); ++i) {
        char buf[16]; std::snprintf(buf, sizeof buf, "_p%02d", (int)i + 1);
        const std::string path = basePath + "_score_full" + suffix + buf + ".svg";
        const std::string svg = primsToSvg(pages[i]);
        if (!saveFile(path, svg, error)) return false;
        paths.push_back(path);
        const size_t tag = svg.find("<svg");
        html << "<div class=\"page\">" << (tag == std::string::npos ? svg : svg.substr(tag)) << "</div>\n";
    }
    html << "</body></html>\n";
    const std::string htmlPath = basePath + "_score_full" + suffix + ".html";
    if (!saveFile(htmlPath, html.str(), error)) return false;
    files.push_back(htmlPath);
    for (const std::string& pa : paths) files.push_back(pa);
    return true;
}

bool loadScoreProbe(const std::string& path, ScoreProbeData& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 6 || line[1] != ' ') continue;
        const char type = line[0];
        const size_t n1 = line.find(' ', 2);
        if (n1 == std::string::npos) continue;
        const std::string name = line.substr(2, n1 - 2);
        const size_t n2 = line.find(' ', n1 + 1);
        const std::string tt = line.substr(n1 + 1, n2 == std::string::npos ? std::string::npos : n2 - n1 - 1);
        const std::string rest = n2 == std::string::npos ? std::string() : line.substr(n2 + 1);
        double t = 0;
        try { t = std::stod(tt); } catch (...) { continue; }
        ScoreClipProbe& pr = out[name];
        if (type == 'A') {
            double r = 0, c = 0;
            if (std::sscanf(rest.c_str(), "%lf %lf", &r, &c) != 2) continue;
            pr.t.push_back(t); pr.rms.push_back(r); pr.cent.push_back(c);
        } else if (type == 'D') {
            ScoreDataSample sd; sd.t = t;
            if (std::sscanf(rest.c_str(), "%lf", &sd.v) != 1) continue;
            pr.data.push_back(sd);
        } else if (type == 'S') {
            ScoreDataSample sd; sd.t = t; sd.isText = true; sd.text = rest;
            pr.data.push_back(sd);
        }
    }
    return !out.empty();
}

std::vector<ScoreLaneInfo> scoreLaneSummary(const Project& p) {
    std::vector<ScoreLaneInfo> out;
    ScoreRenderOptions o;
    Renderer r(p, o, "");
    for (const RLane& l : r.lanes) {
        ScoreLaneInfo info;
        info.name = l.name;
        info.staff = l.staff;
        info.merged = l.merged;
        info.noClef = l.noClef;
        info.clips = (int)l.clips.size();
        info.notes = (int)l.notes;
        for (const Track* t : l.tracks) info.tracks.push_back(t->name);
        for (const Auto& a : l.autos) info.automation.push_back(a.label);
        out.push_back(std::move(info));
    }
    return out;
}
