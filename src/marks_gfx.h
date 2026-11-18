#pragma once
// Shared by the timeline (ImGui) and the score (SVG): the shapes of marker / graph track items and a tiny code colorizer.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "csound_opcodes.h"

namespace markgfx {

// Graphic-score vocabulary: every shape is drawn to fit its box, so the length of the item on the timeline is the duration of the gesture
// (left = start, right = end, bottom = low, top = high / loud).
static const char* const kShapeNames[] = {"Grain cloud", "Glissando up", "Glissando down", "Swell (crescendo)", "Fade (decrescendo)", "Swell and fade",
                                          "Sustained tone", "Noise band", "Pulses, accelerating", "Pulses, slowing", "Tremolo wave", "Attack burst",
                                          "Tone cluster", "Glissando fan", "Free scribble", "Spiral", "Silence (fermata)", "Rising points",
                                          "Note head", "Sharp", "Flat", "Quarter sharp", "Quarter flat", "Koron"};   // the last six: drawn like the python score library (def.py)
static const int kShapeCount = 24;
static const char* const kCueNames[] = {"Q in a circle", "Q in a square", "Q in a diamond", "Q underlined", "Q in brackets [ Q ]"};
static const int kCueCount = 5;
static const char* const kMarkTypeNames[] = {"Comment", "Letter box (A, B, C...)", "Cue (Q)", "Code", "Span bracket"};
static const int kMarkTypeCount = 5;
static const char* const kBracketNames[] = {"Ticks down, text above", "Ticks up, text below"};
static const char* const kLangNames[] = {"Csound"};

typedef std::pair<double, double> P;
// solid: the shape is filled with the full line colour (grains, thick bars); fill: soft fill of a closed outline
struct Prim { bool fill = false, closed = false, solid = false; double sw = 1.0; std::vector<P> pts; };

inline Prim ellipsePrim(double cx, double cy, double rx, double ry, bool fill, double rotDeg = 0.0, double sw = 1.0) {
    Prim p; p.fill = fill; p.closed = true; p.sw = sw;
    const double r = rotDeg * 3.14159265 / 180.0, cr = std::cos(r), sr = std::sin(r);
    for (int i = 0; i < 20; ++i) {
        const double a = 6.2831853 * i / 20.0, x = std::cos(a) * rx, y = std::sin(a) * ry;
        p.pts.push_back({ cx + x * cr - y * sr, cy + x * sr + y * cr });
    }
    return p;
}
inline Prim polyPrim(std::vector<P> pts, bool fill, bool closed, double sw = 1.0) { Prim p; p.fill = fill; p.closed = closed; p.sw = sw; p.pts = std::move(pts); return p; }
inline Prim solidPrim(std::vector<P> pts, double sw = 1.0) { Prim p = polyPrim(std::move(pts), true, true, sw); p.solid = true; return p; }
inline Prim dotPrim(double cx, double cy, double r) { Prim p = ellipsePrim(cx, cy, r, r, true, 0.0, 0.6); p.solid = true; return p; }

// Primitives of a shape inside the box (x0,y0)-(x1,y1). All shapes stretch to the box.
// ratio (swell, fade, swell and fade): 1 = straight edges, above 1 = slow start (curved in), below 1 = fast start (curved out)
inline std::vector<Prim> shapePrims(int shape, double x0, double y0, double x1, double y1, double ratio = 1.0) {
    std::vector<Prim> out;
    const double pad = 1.5;
    x0 += pad; x1 -= pad; y0 += pad; y1 -= pad;
    const double w = std::max(2.0, x1 - x0), h = std::max(2.0, y1 - y0), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const double PI = 3.14159265358979;
    unsigned seed = 2166136261u + (unsigned)shape * 977u;   // same random picture every time
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xFFFF) / 65535.0; };
    auto smooth = [](double u) { return u * u * (3 - 2 * u); };
    switch (shape) {
    case 0: {   // grain cloud: dense in the middle, thin at the ends
        for (int i = 0; i < 44; ++i) {
            const double u = rnd(), v = rnd(), r = rnd();
            const double spread = 0.25 + 0.75 * std::sin(PI * u);
            out.push_back(dotPrim(x0 + u * w, cy + (v - 0.5) * h * spread, std::max(0.8, (0.04 + 0.07 * r) * h)));
        }
        break;
    }
    case 1: case 2: {   // glissando: smooth rise / fall
        std::vector<P> v;
        for (int i = 0; i <= 32; ++i) { const double u = i / 32.0, f = smooth(u); v.push_back({ x0 + u * w, shape == 1 ? y1 - f * h : y0 + f * h }); }
        out.push_back(polyPrim(v, false, false, 2.0));
        out.push_back(dotPrim(x0, shape == 1 ? y1 : y0, 2.0));
        break;
    }
    case 3: case 4: case 5: {   // swell, fade, swell and fade: the half height follows u^ratio
        const double rr = std::min(4.0, std::max(0.2, ratio));
        auto half = [&](double u) {
            const double s = shape == 3 ? u : shape == 4 ? 1.0 - u : 1.0 - std::fabs(2.0 * u - 1.0);
            return 0.5 * h * std::pow(std::max(0.0, s), rr);
        };
        std::vector<P> v;
        const int N = 32;
        for (int i = 0; i <= N; ++i) { const double u = (double)i / N; v.push_back({ x0 + u * w, cy - half(u) }); }
        for (int i = N; i >= 0; --i) { const double u = (double)i / N; v.push_back({ x0 + u * w, cy + half(u) }); }
        out.push_back(polyPrim(v, true, true, 1.3));
        break;
    }
    case 6: {   // sustained tone: a thick bar with end ticks
        out.push_back(solidPrim({ {x0, cy - h * 0.12}, {x1, cy - h * 0.12}, {x1, cy + h * 0.12}, {x0, cy + h * 0.12} }));
        out.push_back(polyPrim({ {x0, y0 + h * 0.2}, {x0, y1 - h * 0.2} }, false, false, 1.4));
        out.push_back(polyPrim({ {x1, y0 + h * 0.2}, {x1, y1 - h * 0.2} }, false, false, 1.4));
        break;
    }
    case 7: {   // noise band: many short vertical hairs of random length
        const int n = std::max(10, std::min(120, (int)(w / 2.5)));
        for (int i = 0; i < n; ++i) {
            const double x = x0 + w * (i + 0.5) / n, hh = h * (0.18 + 0.32 * rnd());
            out.push_back(polyPrim({ {x, cy - hh}, {x, cy + hh} }, false, false, 0.8));
        }
        break;
    }
    case 8: case 9: {   // pulse trains: spacing shrinks (accelerando) or grows (rallentando)
        const int n = 14;
        for (int i = 0; i < n; ++i) {
            const double u = i / (n - 1.0), p = shape == 8 ? 1.0 - std::pow(1.0 - u, 1.7) : std::pow(u, 1.7), x = x0 + p * w;
            out.push_back(polyPrim({ {x, y0 + h * 0.12}, {x, y1 - h * 0.12} }, false, false, 1.6));
        }
        break;
    }
    case 10: {  // tremolo wave
        std::vector<P> v;
        const double cycles = std::max(2.0, std::min(16.0, w / 14.0));
        for (int i = 0; i <= 80; ++i) { const double u = i / 80.0; v.push_back({ x0 + u * w, cy - std::sin(u * cycles * 2 * PI) * h * 0.42 }); }
        out.push_back(polyPrim(v, false, false, 1.4));
        break;
    }
    case 11: {  // attack burst: rays leaving a point
        for (int i = 0; i <= 10; ++i) {
            const double a = -1.0 + 0.2 * i;
            out.push_back(polyPrim({ {x0 + w * 0.06, cy}, {x1 - w * 0.3 * std::fabs(a), cy + a * h * 0.5} }, false, false, 1.1));
        }
        out.push_back(dotPrim(x0 + w * 0.06, cy, 2.4));
        break;
    }
    case 12: {  // tone cluster: stacked lines inside brackets
        for (int i = 0; i < 5; ++i) { const double y = y0 + h * (0.15 + 0.175 * i); out.push_back(polyPrim({ {x0, y}, {x1, y} }, false, false, 1.4)); }
        out.push_back(polyPrim({ {x0 + 3, y0}, {x0, y0}, {x0, y1}, {x0 + 3, y1} }, false, false, 1.3));
        out.push_back(polyPrim({ {x1 - 3, y0}, {x1, y0}, {x1, y1}, {x1 - 3, y1} }, false, false, 1.3));
        break;
    }
    case 13: {  // glissando fan (Xenakis): lines leave one point and spread
        for (int i = 0; i < 7; ++i) out.push_back(polyPrim({ {x0, cy}, {x1, y0 + h * i / 6.0} }, false, false, 1.1));
        break;
    }
    case 14: {  // free scribble: random walk
        std::vector<P> v;
        for (int i = 0; i < 26; ++i) v.push_back({ x0 + w * i / 25.0, y0 + h * (0.08 + 0.84 * rnd()) });
        out.push_back(polyPrim(v, false, false, 1.4));
        break;
    }
    case 15: {  // spiral, closing in
        std::vector<P> v;
        for (int i = 0; i <= 90; ++i) {
            const double u = i / 90.0, a = u * 4.5 * PI, r = 1.0 - u * 0.95;
            v.push_back({ cx + std::cos(a) * r * w * 0.5, cy + std::sin(a) * r * h * 0.5 });
        }
        out.push_back(polyPrim(v, false, false, 1.3));
        break;
    }
    case 16: {  // silence: fermata arc with a dot
        std::vector<P> v;
        for (int i = 0; i <= 24; ++i) { const double a = PI * i / 24.0; v.push_back({ cx - std::cos(a) * w * 0.5, y1 - h * 0.18 - std::sin(a) * h * 0.78 }); }
        out.push_back(polyPrim(v, false, false, 1.6));
        out.push_back(dotPrim(cx, y1 - h * 0.28, std::max(1.5, h * 0.09)));
        break;
    }
    case 17: {  // rising points: dots climb and spread out
        for (int i = 0; i < 16; ++i) {
            const double u = i / 15.0, jitter = (rnd() - 0.5) * h * 0.55 * u;
            out.push_back(dotPrim(x0 + u * w, std::min(y1 - 1, std::max(y0 + 1, y1 - u * h * 0.8 - h * 0.1 + jitter)), std::max(0.9, h * (0.04 + 0.06 * u))));
        }
        break;
    }
    case 18: case 19: case 20: case 21: case 22: case 23: {   // note head and accidentals of def.py, centred in the box (size = height of the box)
        const double ys = std::max(2.0, std::min(h / 2.2, w * 0.9)), sx = std::min(cx, x0 + ys * 1.6), y = cy;
        auto quad = [&](double ax, double ay, double qx, double qy, double bx, double by) {
            std::vector<P> v;
            for (int i = 0; i <= 12; ++i) { const double t = i / 12.0, u = 1 - t; v.push_back({ u * u * ax + 2 * u * t * qx + t * t * bx, u * u * ay + 2 * u * t * qy + t * t * by }); }
            return v;
        };
        auto cubic = [&](std::vector<P>& v, double ax, double ay, double b1x, double b1y, double b2x, double b2y, double bx, double by) {
            for (int i = 1; i <= 14; ++i) { const double t = i / 14.0, u = 1 - t;
                v.push_back({ u * u * u * ax + 3 * u * u * t * b1x + 3 * u * t * t * b2x + t * t * t * bx, u * u * u * ay + 3 * u * u * t * b1y + 3 * u * t * t * b2y + t * t * t * by }); }
        };
        if (shape == 18) {   // python note(): two cubic curves
            const double r = ys * 0.5 * 1.25, X = sx, Y = y;
            std::vector<P> v; v.push_back({ X - r * 1.2, Y + r * .6 });
            cubic(v, X - r * 1.2, Y + r * .6, X - r * 1.7, Y - r * .3, X + r * .3, Y - r * 1.4, X + r * 1.2, Y - r * .6);
            cubic(v, X + r * 1.2, Y - r * .6, X + r * 1.7, Y + r * .3, X - r * .3, Y + r * 1.4, X - r * 1.2, Y + r * .6);
            out.push_back(solidPrim(v, 1.0));
        } else if (shape == 19) {   // sharp()
            const double X = sx + 1.417 * ys, off = ys * 2 / 3, width = ys / 2;
            const double ybot = y + ys, ybot1 = y + ys / 2, ybot2 = y + ys / 9, ytop = y - ys, ytop2 = y - ys / 2, ytop1 = y - ys / 9;
            const double x1 = X - off - width - ys / 2, x2 = X - off - ys / 2, xmin = x1 - ys / 5, xmax = x2 + ys / 5;
            out.push_back(polyPrim({ {x1, ybot}, {x1, ytop} }, false, false, 1.1));
            out.push_back(polyPrim({ {x2, ybot}, {x2, ytop} }, false, false, 1.1));
            out.push_back(polyPrim({ {xmin, ybot1}, {xmax, ybot2} }, false, false, 2.2));
            out.push_back(polyPrim({ {xmin, ytop1}, {xmax, ytop2} }, false, false, 2.2));
        } else if (shape == 20) {   // bflat()
            const double y = cy + 0.5 * ys, X = sx + 1.1 * ys, off = ys, xl = X - off - ys / 2;
            std::vector<P> v = { {xl, y - ys * 1.5}, {xl, y + ys * 0.5} };
            for (const P& q : quad(xl, y + ys * 0.5, X - off / 4, y - ys * 0.75, xl, y - ys * 0.25)) v.push_back(q);
            out.push_back(polyPrim(v, false, false, 1.3));
        } else if (shape == 21) {   // Qsharp()
            const double X = sx + 1.3 * ys, off = ys * 0.8;
            const double ybot = y + ys, ybot1 = y + ys / 2, ybot2 = y + ys / 9, ytop = y - ys, ytop2 = y - ys / 2, ytop1 = y - ys / 9;
            const double x1 = X - off - ys * 0.5, xmin = x1 - ys / 5, xmax = x1 + ys / 5;
            out.push_back(polyPrim({ {x1, ybot}, {x1, ytop} }, false, false, 1.1));
            out.push_back(polyPrim({ {xmin - 1, ybot1}, {xmax + 1, ybot2} }, false, false, 2.2));
            out.push_back(polyPrim({ {xmin - 1, ytop1}, {xmax + 1, ytop2} }, false, false, 2.2));
        } else if (shape == 22) {   // Qflat()
            const double y = cy + 0.5 * ys, X = sx + 1.4 * ys, off = ys, xl = X + ys * 0.5 - off - ys * 0.5;
            std::vector<P> v = { {xl, y - ys * 1.5}, {xl, y + ys * 0.5} };
            for (const P& q : quad(xl, y + ys * 0.5, X - off * 2.15, y - ys * 0.6, xl, y - ys * 0.25)) v.push_back(q);
            out.push_back(polyPrim(v, false, false, 1.3));
        } else {                    // koron()
            const double k = ys * 0.7 / 10.0, xs = sx, yb = cy + 1.5 * ys * 0.7;
            out.push_back(polyPrim({ {xs, yb}, {xs, yb - 30 * k}, {xs + 7 * k, yb - 25 * k}, {xs, yb - 20 * k} }, false, false, 1.4));
        }
        break;
    }
    default: break;
    }
    return out;
}

// Right edge of what a shape really draws (the note signs sit at the left of their box): the text of a graph item starts right there.
inline double shapeRight(int shape, double x0, double y0, double x1, double y1, double ratio = 1.0) {
    double r = x0;
    for (const Prim& p : shapePrims(shape, x0, y0, x1, y1, ratio)) for (const P& q : p.pts) r = std::max(r, q.first);
    return r;
}

// ---- code colorizer: one line -> pieces with a kind (0 plain, 1 keyword, 2 number, 3 string, 4 comment, 5 opcode)
struct Tok { std::string text; int kind; };

inline const std::set<std::string>& csoundOpcodeSet() {
    static std::set<std::string> s;
    if (s.empty()) for (const char* op : kCsoundOpcodes) s.insert(op);
    return s;
}

inline std::vector<Tok> highlightLine(const std::string& line, int /*lang*/) {
    static const std::set<std::string> csKw = { "instr", "endin", "opcode", "endop", "if", "then", "else", "elseif", "endif", "while", "do", "od", "until",
        "goto", "igoto", "kgoto", "tigoto", "return", "turnoff", "sr", "kr", "ksmps", "nchnls", "nchnls_i", "0dbfs", "A4", "giSine", "gi", "ga", "gk" };
    std::vector<Tok> out;
    size_t i = 0;
    const size_t n = line.size();
    auto push = [&](const std::string& t, int k) { if (!t.empty()) out.push_back({ t, k }); };
    while (i < n) {
        const char c = line[i];
        if (c == ';' || (c == '/' && i + 1 < n && line[i + 1] == '/')) { push(line.substr(i), 4); break; }
        if (c == '"') { size_t j = i + 1; while (j < n && line[j] != '"') { if (line[j] == '\\' && j + 1 < n) ++j; ++j; } push(line.substr(i, std::min(n, j + 1) - i), 3); i = std::min(n, j + 1); continue; }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)line[i + 1]))) {
            size_t j = i; while (j < n && (std::isdigit((unsigned char)line[j]) || line[j] == '.')) ++j;
            push(line.substr(i, j - i), 2); i = j; continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_' || c == '#' || c == '~') {
            size_t j = i + 1; while (j < n && (std::isalnum((unsigned char)line[j]) || line[j] == '_' || line[j] == '~' || line[j] == '#')) ++j;
            const std::string w = line.substr(i, j - i);
            int k = 0;
            k = csKw.count(w) ? 1 : csoundOpcodeSet().count(w) ? 5 : 0;
            push(w, k); i = j; continue;
        }
        size_t j = i + 1;
        while (j < n && !std::isalnum((unsigned char)line[j]) && line[j] != '"' && line[j] != ';' && line[j] != '/' && line[j] != '_') ++j;
        push(line.substr(i, j - i), 0); i = j;
    }
    return out;
}

}  // namespace markgfx
