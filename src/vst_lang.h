#pragma once
// "CAW code" of the clips on VSTi / VSTe tracks: a small language that is translated to Csound before the clip is built.
//   P1 .. P8            the 8 picked plugin parameters (0..1). Read and write. Their automation lanes drive them.
//   x = expr, x += 1    your own variables (start at 0)
//   every 0.5 { ... }   run the block every 0.5 seconds           when P3 > 0.8 { ... }   run it while the condition is true
//   note 60, 100, 0.4   play a MIDI note (pitch, velocity, length in seconds)             cc 1, 127   MIDI controller
//   pitch vel dur time  pitch / velocity of the MIDI clip note that started this clip, clip length, seconds since the clip started
//   lfo(f) tri(f) saw(f) sqr(f) rnd(n) rnd(a,b) smooth(x,t) line(a,b) clip(x,lo,hi) min max abs sin cos int sqrt level(in) mtof(n) uni(x)
//   p2 = cawget("test1")  bind parameter 2 to the automation lane "test1" (wins over the slot's own lane)
//   seed 0              new random numbers every run (like Csound's seed 0); seed 5 = the same sequence every run
//   csound: <any Csound line>
#include <algorithm>
#include <cctype>
#include <map>
#include <map>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "model.h"

namespace vstlang {
inline std::string cleanBus(const std::string& name) {   // letters, digits, _ and - only ("" stays "")
    std::string s;
    for (char ch : name) if (std::isalnum((unsigned char)ch) || ch == '_' || ch == '-') s += ch;
    return s;
}
inline std::string busName(const Track& t) {   // audio bus of a VSTe track: cawsa "<name>", aL, aR
    const std::string s = cleanBus(t.name);
    return s.empty() ? "vst" + std::to_string(t.id) : s;
}
struct Ctx { int id = 0; bool fx = false; std::vector<std::string> pre; int tmp = 0; int ev = 0; std::set<std::string> vars; };

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
inline std::vector<std::string> splitTop(const std::string& s) {
    std::vector<std::string> out; int depth = 0; std::string cur;
    for (char c : s) {
        if (c == '(') ++depth; else if (c == ')') --depth;
        if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); } else cur += c;
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}
inline std::string temp(Ctx& c) { return "kCawT" + std::to_string(++c.tmp); }
inline std::string varName(Ctx& c, const std::string& n, bool assign) {
    if (n.size() == 2 && (n[0] == 'P' || n[0] == 'p') && n[1] >= '1' && n[1] <= '8') return std::string("kP") + n[1];
    if (!assign) {
        if (n == "pitch") return "p4";
        if (n == "vel") return "p5";
        if (n == "dur") return "p3";
        if (n == "time") return "timeinsts()";
        if (n == "pi") return "3.14159265";
    }
    if (n == "and") return "&&";
    if (n == "or") return "||";
    if (n == "not") return "!";
    std::string v = "k" + n;
    if (assign) c.vars.insert(v);
    return v;
}
std::string expr(Ctx& c, const std::string& s, bool cond = false);

inline std::string call(Ctx& c, const std::string& fn, const std::vector<std::string>& rawArgs) {
    std::vector<std::string> a;
    for (const std::string& r : rawArgs) a.push_back(expr(c, r));
    auto arg = [&](size_t i, const char* def) { return i < a.size() && !a[i].empty() ? a[i] : std::string(def); };
    auto lfoType = [&](int type) { const std::string t = temp(c); c.pre.push_back(t + " lfo " + arg(1, "1") + ", " + arg(0, "1") + ", " + std::to_string(type)); return t; };
    if (fn == "lfo") return lfoType(0);
    if (fn == "tri") return lfoType(1);
    if (fn == "sqr") return lfoType(2);
    if (fn == "saw") return lfoType(4);
    if (fn == "uni") return "(0.5 + 0.5 * (" + arg(0, "0") + "))";
    if (fn == "rnd") {
        const std::string t = temp(c);
        c.pre.push_back(a.size() >= 2 ? t + " random " + a[0] + ", " + a[1] : t + " random 0, " + arg(0, "1"));
        return t;
    }
    if (fn == "smooth") { const std::string t = temp(c); c.pre.push_back(t + " port " + arg(0, "0") + ", " + arg(1, "0.1")); return t; }
    if (fn == "line") { const std::string t = temp(c); c.pre.push_back(t + " line " + arg(0, "0") + ", p3, " + arg(1, "1")); return t; }
    if (fn == "clip") { const std::string t = temp(c); c.pre.push_back(t + " limit " + arg(0, "0") + ", " + arg(1, "0") + ", " + arg(2, "1")); return t; }
    if (fn == "level") { const std::string raw = temp(c); c.pre.push_back(raw + " rms aInL"); const std::string t = temp(c); c.pre.push_back(t + " port " + raw + ", 0.05"); return t; }   // one ksmps of rms is a spike: follow the loudness smoothly
    if (fn == "min") return "((" + arg(0, "0") + ") < (" + arg(1, "0") + ") ? (" + arg(0, "0") + ") : (" + arg(1, "0") + "))";
    if (fn == "max") return "((" + arg(0, "0") + ") > (" + arg(1, "0") + ") ? (" + arg(0, "0") + ") : (" + arg(1, "0") + "))";
    if (fn == "mtof") return "cpsmidinn(" + arg(0, "60") + ")";
    std::string out = fn + "(";
    for (size_t i = 0; i < a.size(); ++i) out += (i ? ", " : "") + a[i];
    return out + ")";
}

inline std::string expr(Ctx& c, const std::string& s, bool cond) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const char ch = s[i];
        if (std::isspace((unsigned char)ch)) { out += ' '; ++i; continue; }
        if (std::isdigit((unsigned char)ch) || (ch == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) out += s[i++];
            continue;
        }
        if (std::isalpha((unsigned char)ch) || ch == '_') {
            size_t j = i; while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
            const std::string id = s.substr(i, j - i);
            size_t k = j; while (k < s.size() && s[k] == ' ') ++k;
            if (k < s.size() && s[k] == '(') {
                int depth = 0; size_t e = k;
                for (; e < s.size(); ++e) { if (s[e] == '(') ++depth; else if (s[e] == ')' && --depth == 0) break; }
                if (e >= s.size()) { out += id; i = j; continue; }
                out += call(c, id, splitTop(s.substr(k + 1, e - k - 1)));
                i = e + 1; continue;
            }
            if (id == "in") { out += "aInL"; i = j; continue; }
            out += varName(c, id, false); i = j; continue;
        }
        if (cond && ch == '=' && !(i + 1 < s.size() && s[i + 1] == '=') && !(i > 0 && (s[i - 1] == '=' || s[i - 1] == '<' || s[i - 1] == '>' || s[i - 1] == '!'))) { out += "=="; ++i; continue; }
        out += ch; ++i;
    }
    return out;
}

inline std::string translate(const Track& t, const std::string& code) {
    Ctx c; c.id = t.id; c.fx = t.kind == 4;
    const std::string idStr = std::to_string(t.id);
    const size_t n = std::min<size_t>(8, t.vstPick.size());
    std::map<int, std::string> pBind;   // parameter slot (0..7) -> automation lane, from p2 = cawget("test1") lines
    // 1. normalise: comments, braces, else
    std::string text;
    {
        std::istringstream in(code); std::string line;
        while (std::getline(in, line)) {
            size_t p = line.find('#'); size_t q = line.find("//");
            if (q != std::string::npos && (p == std::string::npos || q < p)) p = q;
            if (p != std::string::npos) line = line.substr(0, p);
            text += line + "\n";
        }
        text = std::regex_replace(text, std::regex(R"(\}\s*else\s*\{)"), "\n@ELSE\n");
        std::string t2;
        for (char ch : text) { if (ch == '{') t2 += "\n{\n"; else if (ch == '}') t2 += "\n}\n"; else if (ch == ';') t2 += "\n"; else t2 += ch; }
        text = t2;
    }
    // 2. statements
    std::ostringstream body;
    auto flush = [&]() { for (const std::string& p : c.pre) body << p << "\n"; c.pre.clear(); };
    std::string pending;
    std::istringstream in(text); std::string seg;
    static const std::regex assignRe(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*([+\-*/]?)=(?!=)\s*(.+)$)");
    while (std::getline(in, seg)) {
        seg = trim(seg);
        if (seg.empty()) continue;
        std::smatch m;
        if (seg.rfind("csound:", 0) == 0) { body << trim(seg.substr(7)) << "\n"; continue; }
        if (seg == "}") { body << "endif\n"; continue; }
        if (seg == "@ELSE") { body << "else\n"; continue; }
        if (seg == "{") {
            if (pending.rfind("every ", 0) == 0) {
                const std::string interval = expr(c, pending.substr(6)); flush();
                const std::string v = "kCawEv" + std::to_string(++c.ev);
                body << v << " metro 1 / max_cawsafe(" << interval << ")\nif " << v << " == 1 then\n";
            } else if (pending.rfind("when ", 0) == 0) {
                const std::string cond = expr(c, pending.substr(5), true); flush();
                body << "if (" << cond << ") then\n";
            } else body << "if 1 == 1 then\n";
            pending.clear(); continue;
        }
        if (seg.rfind("every ", 0) == 0 || seg.rfind("when ", 0) == 0) { pending = seg; continue; }
        if (seg.rfind("note ", 0) == 0 || seg == "note") {
            const auto a = splitTop(seg.size() > 5 ? seg.substr(5) : "");
            const std::string pitch = expr(c, a.size() > 0 ? a[0] : "60"), vel = expr(c, a.size() > 1 ? a[1] : "100"), len = expr(c, a.size() > 2 ? a[2] : "0.5");
            flush();
            body << "event \"i\", \"CawVstNote_" << idStr << "\", 0, " << len << ", " << pitch << ", " << vel << "\n";
            continue;
        }
        if (seg.rfind("cc ", 0) == 0) {
            const auto a = splitTop(seg.substr(3));
            const std::string num = expr(c, a.size() > 0 ? a[0] : "1"), val = expr(c, a.size() > 1 ? a[1] : "0");
            flush(); body << "CawVstEv_" << idStr << " 176, " << num << ", " << val << "\n"; continue;
        }
        if (seg.rfind("msg ", 0) == 0) {
            const auto a = splitTop(seg.substr(4));
            const std::string s0 = expr(c, a.size() > 0 ? a[0] : "144"), d1 = expr(c, a.size() > 1 ? a[1] : "0"), d2 = expr(c, a.size() > 2 ? a[2] : "0");
            flush(); body << "CawVstEv_" << idStr << " " << s0 << ", " << d1 << ", " << d2 << "\n"; continue;
        }
        static const std::regex bindRe(R"re(^([pP][1-8])\s*=\s*cawget\s*(?:\(\s*)?"([^"]*)"(?:\s*\))?\s*$)re");
        std::smatch bm;
        if (std::regex_match(seg, bm, bindRe)) { pBind[(int)(bm[1].str()[1] - '1')] = bm[2].str(); continue; }   // p2 = cawget("test1"): the code binds the parameter to an automation lane
        if (std::regex_match(seg, m, assignRe)) {
            const std::string name = varName(c, m[1].str(), true), op = m[2].str();
            const std::string rhs = expr(c, m[3].str()); flush();
            body << name << " = " << (op.empty() ? "" : name + " " + op + " ") << "(" << rhs << ")\n";
            continue;
        }
        body << seg << "\n";   // anything else is plain Csound
    }
    // 3. the instrument: lanes in, code, parameters out
    std::ostringstream o;
    o << "; ---- generated from the CAW code of this VST track ----\n";
    for (size_t k = 0; k < 8; ++k) {
        const auto b = pBind.find((int)k);
        if (b != pBind.end()) o << "kP" << (k + 1) << " cawget \"" << b->second << "\"\n";   // the code binding wins over the slot's own lane
        else if (k < n && k < t.vstPickName.size()) o << "kP" << (k + 1) << " cawget \"" << t.vstPickName[k] << "\"\n";
        else o << "kP" << (k + 1) << " init 0.5\n";
    }
    if (c.fx) {
        bool anyIn = false; for (const auto& in : t.vstInputs) if (in.on) { anyIn = true; break; }
        if (anyIn)   // audio input routing: line 1 / 2 of the routed lines (see the AUDIO INPUT section of the VST window)
            o << "aInL chnget \"cawvstin_" << idStr << "_1\"\naInR chnget \"cawvstin_" << idStr << "_2\"\n";
        else o << "aInL = 0\naInR = 0\n";
    }
    else o << "aInL = 0\naInR = 0\n";
    for (const std::string& v : c.vars) if (!(v.size() == 3 && v[1] == 'P')) o << v << " init 0\n";
    o << "if p4 > 0 then\neventi \"i\", \"CawVstNote_" << idStr << "\", 0, p3, p4, p5\nendif\n";
    o << body.str();
    // Send every slot that has a picked parameter AND every code-bound slot (p2 = cawget "lane"),
    // even when nothing is picked: the channel exists (vstOrchestra declares bound slots too) and
    // the rack reads it once the slot gets a parameter. A bound slot without a pick is inert but
    // never breaks the orchestra.
    int boundMax = -1;
    for (const auto& b : pBind) boundMax = std::max(boundMax, b.first);
    const size_t nSend = std::min<size_t>(8, std::max<size_t>(n, (size_t)std::max(0, boundMax + 1)));
    for (size_t k = 0; k < nSend; ++k) o << "chnset kP" << (k + 1) << ", \"cawvst_" << idStr << "_p" << (k + 1) << "\"\n";
    std::string r = o.str();
    // max_cawsafe(x) = never divide by zero
    r = std::regex_replace(r, std::regex(R"(max_cawsafe\(([^\n]*)\)\n)"), "(($1) < 0.001 ? 0.001 : ($1))\n");
    return r;
}
}
