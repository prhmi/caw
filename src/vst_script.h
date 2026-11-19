#pragma once
// The "CAW code" of VSTi clips, interpreted by CAW's VST engine itself, so Run VST plays the
// clip code (note / cc / msg, every / when, P1..P8 and the little math set) with NO Csound
// engine running. The same language vst_lang.h translates to Csound; this is the runtime for
// the VST-only path. When Csound runs, the translated instrument is authoritative and these
// scripts are not used (no double notes).
//
// Not supported here (they need the Csound graph): audio input level(), raw `csound:` lines and
// plain Csound statements. They are ignored, exactly like unknown lines are skipped.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace vstscript {

struct Expr;
using ExprP = std::shared_ptr<Expr>;

struct Expr {
    enum Kind { Num, Var, Param, Un, Bin, Ter, Call } kind = Num;
    double num = 0.0;
    std::string op;        // Un / Bin operator
    std::string fn;        // Call
    int index = -1;        // Var index (>= 0), Param slot (0..7), reserved var (< 0), state slot (Call: smooth)
    ExprP a, b, c;
};

struct Stmt {
    enum Kind { Every, When, Assign, Note, Cc, Msg, Skip } kind = Skip;
    ExprP a, b, c;         // Every: interval | When: condition | Assign: value | Note: pitch/vel/len | Cc: num/val | Msg: status/d1/d2
    int var = -1;          // Assign target: variable index
    int slot = -1;         // Assign target: P slot (0..7)
    char op = 0;           // Assign: 0 set, else + - * /
    int id = -1;           // Every: runtime state slot
    std::vector<Stmt> body, otherwise;
};

struct Program {
    std::vector<Stmt> body;
    std::vector<std::string> varNames;
    bool writesP[8] = {false, false, false, false, false, false, false, false};
    int states = 0;        // runtime state doubles (Every timers, smooth values)
    bool ok = true;
};

// One tick of the clip: t = seconds since the clip started (Csound's timeinsts()), dur = the
// clip length (p3), pitch / vel = p4 / p5, p = the current P1..P8 values (0..1).
struct Env {
    double t = 0.0, dur = 1.0, dt = 0.0, pitch = 0.0, vel = 0.0;
    double p[8] = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
};

struct Events {
    struct Note { int pitch = 60, vel = 100; double len = 0.5; };
    struct Midi { int status = 0, d1 = 0, d2 = 0; };
    std::vector<Note> notes;
    std::vector<Midi> midi;
};

struct Runtime {
    std::vector<double> vars;
    std::vector<double> state;
    uint64_t rng = 0x9E3779B97F4A7C15ull;

    void init(const Program& prog, const double* initP) {
        vars.assign(prog.varNames.size(), 0.0);
        state.assign((size_t)prog.states, std::nan(""));
        rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(prog.states * 2654435761u + 1u);
        for (int k = 0; k < 8; ++k) {
            double v = initP ? initP[k] : 0.5;
            if (!std::isfinite(v)) v = 0.5;
            p[k] = std::min(1.0, std::max(0.0, v));
        }
    }
    double p[8] = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    double nextRand() {   // xorshift64*: deterministic per clip, no allocation
        rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
        return (double)((rng * 2685821657736338717ull) >> 11) / 9007199254740992.0;
    }
};

namespace detail {

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
inline bool startsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}
inline std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out; int depth = 0; std::string cur;
    for (char c : s) {
        if (c == '(') ++depth; else if (c == ')') --depth;
        if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); } else cur += c;
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

struct Token { enum T { End, Num, Id, Sym } t = End; double num = 0.0; std::string text; };

inline std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    for (size_t i = 0; i < s.size();) {
        const char ch = s[i];
        if (std::isspace((unsigned char)ch)) { ++i; continue; }
        if (std::isdigit((unsigned char)ch) || (ch == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) ++j;
            Token t; t.t = Token::Num; t.num = std::atof(s.substr(i, j - i).c_str()); t.text = s.substr(i, j - i);
            out.push_back(t); i = j; continue;
        }
        if (std::isalpha((unsigned char)ch) || ch == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
            const std::string id = s.substr(i, j - i); i = j;
            Token t;
            if (id == "and") { t.t = Token::Sym; t.text = "&&"; }
            else if (id == "or") { t.t = Token::Sym; t.text = "||"; }
            else if (id == "not") { t.t = Token::Sym; t.text = "!"; }
            else { t.t = Token::Id; t.text = id; }
            out.push_back(t); continue;
        }
        Token t; t.t = Token::Sym;
        const size_t rest = s.size() - i;
        if (rest >= 2 && ((ch == '=' && s[i + 1] == '=') || (ch == '!' && s[i + 1] == '=') ||
                          (ch == '<' && s[i + 1] == '=') || (ch == '>' && s[i + 1] == '=') ||
                          (ch == '&' && s[i + 1] == '&') || (ch == '|' && s[i + 1] == '|'))) {
            t.text.assign(s, i, 2); i += 2; out.push_back(t); continue;
        }
        if (ch == '=') { t.text = "=="; ++i; out.push_back(t); continue; }   // lone = compares inside the language
        t.text.assign(1, ch); ++i; out.push_back(t);
    }
    out.push_back(Token());
    return out;
}

inline bool isParamName(const std::string& id, int& slot) {
    if (id.size() == 2 && (id[0] == 'P' || id[0] == 'p') && id[1] >= '1' && id[1] <= '8') {
        slot = id[1] - '1';
        return true;
    }
    return false;
}

class ExprParser {
public:
    ExprParser(std::vector<Token> toks, Program& prog, std::map<std::string, int>& names, int& nextState)
        : t_(std::move(toks)), prog_(prog), names_(names), nextState_(nextState) {}

    ExprP parseAll() {
        ExprP e = parseTernary();
        if (!e || cur().t != Token::End) return nullptr;
        return e;
    }

private:
    const Token& cur() const { return t_[i_ < t_.size() ? i_ : t_.size() - 1]; }
    bool sym(const char* s) { if (cur().t == Token::Sym && cur().text == s) { ++i_; return true; } return false; }

    int varIndex(const std::string& name) {
        auto it = names_.find(name);
        if (it != names_.end()) return it->second;
        const int id = (int)prog_.varNames.size();
        prog_.varNames.push_back(name);
        names_[name] = id;
        return id;
    }
    static ExprP node(Expr::Kind k) { auto e = std::make_shared<Expr>(); e->kind = k; return e; }

    ExprP parseTernary() {
        ExprP c = parseOr();
        if (!c) return nullptr;
        if (sym("?")) {
            ExprP a = parseTernary();
            if (!a || !sym(":")) return nullptr;
            ExprP b = parseTernary();
            if (!b) return nullptr;
            ExprP e = node(Expr::Ter); e->a = c; e->b = a; e->c = b; return e;
        }
        return c;
    }
    ExprP bin(ExprP a, const char* op, ExprP b) {
        ExprP e = node(Expr::Bin); e->op = op; e->a = std::move(a); e->b = std::move(b); return e;
    }
    ExprP parseOr() {
        ExprP a = parseAnd(); if (!a) return nullptr;
        while (cur().t == Token::Sym && cur().text == "||") { ++i_; ExprP b = parseAnd(); if (!b) return nullptr; a = bin(a, "||", b); }
        return a;
    }
    ExprP parseAnd() {
        ExprP a = parseEq(); if (!a) return nullptr;
        while (cur().t == Token::Sym && cur().text == "&&") { ++i_; ExprP b = parseEq(); if (!b) return nullptr; a = bin(a, "&&", b); }
        return a;
    }
    ExprP parseEq() {
        ExprP a = parseRel(); if (!a) return nullptr;
        for (;;) {
            if (cur().t != Token::Sym) break;
            const std::string op = cur().text;
            if (op != "==" && op != "!=") break;
            ++i_; ExprP b = parseRel(); if (!b) return nullptr; a = bin(a, op.c_str(), b);
        }
        return a;
    }
    ExprP parseRel() {
        ExprP a = parseAdd(); if (!a) return nullptr;
        for (;;) {
            if (cur().t != Token::Sym) break;
            const std::string op = cur().text;
            if (op != "<" && op != ">" && op != "<=" && op != ">=") break;
            ++i_; ExprP b = parseAdd(); if (!b) return nullptr; a = bin(a, op.c_str(), b);
        }
        return a;
    }
    ExprP parseAdd() {
        ExprP a = parseMul(); if (!a) return nullptr;
        for (;;) {
            if (!(sym("+") || sym("-"))) return a;
            const std::string op = t_[i_ - 1].text;
            ExprP b = parseMul(); if (!b) return nullptr; a = bin(a, op.c_str(), b);
        }
    }
    ExprP parseMul() {
        ExprP a = parseUnary(); if (!a) return nullptr;
        for (;;) {
            if (!(sym("*") || sym("/") || sym("%"))) return a;
            const std::string op = t_[i_ - 1].text;
            ExprP b = parseUnary(); if (!b) return nullptr; a = bin(a, op.c_str(), b);
        }
    }
    ExprP parseUnary() {
        if (sym("-")) { ExprP e = node(Expr::Un); e->op = "-"; e->a = parseUnary(); return e->a ? e : nullptr; }
        if (sym("+")) return parseUnary();
        if (sym("!")) { ExprP e = node(Expr::Un); e->op = "!"; e->a = parseUnary(); return e->a ? e : nullptr; }
        return parsePrimary();
    }
    ExprP parsePrimary() {
        const Token& tok = cur();
        if (tok.t == Token::Num) { ExprP e = node(Expr::Num); e->num = tok.num; ++i_; return e; }
        if (sym("(")) {
            ExprP e = parseTernary();
            if (!e || !sym(")")) return nullptr;
            return e;
        }
        if (tok.t == Token::Id) {
            const std::string id = tok.text;
            ++i_;
            if (cur().t == Token::Sym && cur().text == "(") {
                ++i_;
                auto call = node(Expr::Call);
                call->fn = id == "cpsmidinn" ? "mtof" : id;
                if (!(cur().t == Token::Sym && cur().text == ")")) {
                    int nargs = 0;
                    for (;;) {
                        ExprP arg = parseTernary();
                        if (!arg) return nullptr;
                        if (nargs == 0) call->a = arg;
                        else if (nargs == 1) call->b = arg;
                        else if (nargs == 2) call->c = arg;
                        ++nargs;
                        if (!sym(",")) break;
                    }
                }
                if (!sym(")")) return nullptr;
                if (call->fn == "smooth") call->index = nextState_++;   // one-pole state per call site
                return call;
            }
            int slot = -1;
            if (isParamName(id, slot)) { ExprP e = node(Expr::Param); e->index = slot; return e; }
            if (id == "pitch") { ExprP e = node(Expr::Var); e->index = -2; return e; }
            if (id == "vel") { ExprP e = node(Expr::Var); e->index = -3; return e; }
            if (id == "dur") { ExprP e = node(Expr::Var); e->index = -4; return e; }
            if (id == "time") { ExprP e = node(Expr::Var); e->index = -5; return e; }
            if (id == "pi") { ExprP e = node(Expr::Var); e->index = -6; return e; }
            if (id == "in") { ExprP e = node(Expr::Num); e->num = 0.0; return e; }   // no audio input in the VST engine
            ExprP e = node(Expr::Var); e->index = varIndex(id); return e;
        }
        return nullptr;
    }

    std::vector<Token> t_;
    size_t i_ = 0;
    Program& prog_;
    std::map<std::string, int>& names_;
    int& nextState_;
};

} // namespace detail

// ---- expression evaluation ----------------------------------------------------------------
namespace detail {

inline double shape(double u, int type) {   // 0 sine, 1 triangle, 2 square, 3 saw: 0..1
    u -= std::floor(u);
    if (type == 1) return u < 0.5 ? u * 2.0 : 2.0 - u * 2.0;
    if (type == 2) return u < 0.5 ? 1.0 : 0.0;
    if (type == 3) return u;
    return 0.5 - 0.5 * std::cos(u * 6.283185307179586);
}

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline double eval(const ExprP& e, const Env& env, Runtime& rt) {
    if (!e) return 0.0;
    switch (e->kind) {
    case Expr::Num: return e->num;
    case Expr::Var:
        if (e->index == -2) return env.pitch;
        if (e->index == -3) return env.vel;
        if (e->index == -4) return env.dur;
        if (e->index == -5) return env.t;
        if (e->index == -6) return 3.141592653589793;
        return e->index >= 0 && (size_t)e->index < rt.vars.size() ? rt.vars[(size_t)e->index] : 0.0;
    case Expr::Param: return e->index >= 0 && e->index < 8 ? env.p[e->index] : 0.0;
    case Expr::Un: {
        const double v = eval(e->a, env, rt);
        return e->op == "!" ? (v == 0.0 ? 1.0 : 0.0) : -v;
    }
    case Expr::Ter: return eval(e->a, env, rt) != 0.0 ? eval(e->b, env, rt) : eval(e->c, env, rt);
    case Expr::Bin: {
        if (e->op == "&&") return (eval(e->a, env, rt) != 0.0 && eval(e->b, env, rt) != 0.0) ? 1.0 : 0.0;
        if (e->op == "||") return (eval(e->a, env, rt) != 0.0 || eval(e->b, env, rt) != 0.0) ? 1.0 : 0.0;
        const double a = eval(e->a, env, rt), b = eval(e->b, env, rt);
        if (e->op == "+") return a + b;
        if (e->op == "-") return a - b;
        if (e->op == "*") return a * b;
        if (e->op == "/") return b == 0.0 ? 0.0 : a / b;
        if (e->op == "%") return b == 0.0 ? 0.0 : std::fmod(a, b);
        if (e->op == "==") return a == b ? 1.0 : 0.0;
        if (e->op == "!=") return a != b ? 1.0 : 0.0;
        if (e->op == "<") return a < b ? 1.0 : 0.0;
        if (e->op == ">") return a > b ? 1.0 : 0.0;
        if (e->op == "<=") return a <= b ? 1.0 : 0.0;
        if (e->op == ">=") return a >= b ? 1.0 : 0.0;
        return 0.0;
    }
    case Expr::Call: {
        const std::string& f = e->fn;
        if (f == "lfo" || f == "tri" || f == "sqr" || f == "saw") {
            const double freq = e->a ? eval(e->a, env, rt) : 1.0;
            const int type = f == "tri" ? 1 : (f == "sqr" ? 2 : (f == "saw" ? 3 : 0));
            return shape(freq * env.t, type);
        }
        if (f == "rnd") {
            if (!e->a) return rt.nextRand();
            const double x = eval(e->a, env, rt);
            if (!e->b) return rt.nextRand() * x;   // rnd(n): 0..n
            const double lo = x, hi = eval(e->b, env, rt);
            return lo + rt.nextRand() * (hi - lo);
        }
        const bool hasB = e->b != nullptr, hasC = e->c != nullptr;
        const double x = e->a ? eval(e->a, env, rt) : 0.0;
        if (f == "uni") return 0.5 + 0.5 * x;
        if (f == "smooth") {
            const double tc = hasB ? eval(e->b, env, rt) : 0.1;
            double& y = rt.state[(size_t)std::max(0, e->index)];
            if (std::isnan(y) || tc <= 0.0 || env.dt <= 0.0) y = x;
            else y += (x - y) * std::min(1.0, env.dt / tc);
            return y;
        }
        if (f == "line") {
            const double b = hasB ? eval(e->b, env, rt) : 1.0;
            const double span = env.dur > 1e-6 ? env.dur : 1.0;
            return x + (b - x) * clamp01(env.t / span);
        }
        if (f == "clip") {
            const double lo = hasB ? eval(e->b, env, rt) : 0.0, hi = hasC ? eval(e->c, env, rt) : 1.0;
            return std::min(hi, std::max(lo, x));
        }
        if (f == "min") return hasB ? std::min(x, eval(e->b, env, rt)) : x;
        if (f == "max") return hasB ? std::max(x, eval(e->b, env, rt)) : x;
        if (f == "abs") return std::fabs(x);
        if (f == "sin") return std::sin(x);
        if (f == "cos") return std::cos(x);
        if (f == "int") return std::trunc(x);
        if (f == "sqrt") return x <= 0.0 ? 0.0 : std::sqrt(x);
        if (f == "mtof") return 440.0 * std::pow(2.0, (x - 69.0) / 12.0);
        if (f == "level") return 0.0;
        return 0.0;
    }
    }
    return 0.0;
}

inline void run(const std::vector<Stmt>& list, Env& env, Runtime& rt, Events& out);

inline void runOne(const Stmt& s, Env& env, Runtime& rt, Events& out) {
    switch (s.kind) {
    case Stmt::Every: {
        const double iv = eval(s.a, env, rt);
        if (iv <= 1e-4) return;
        double& last = rt.state[(size_t)std::max(0, s.id)];
        const double now = env.t;
        const bool first = std::isnan(last) || now < last - 1e-9;
        const bool crossed = !first && std::floor(now / iv) > std::floor(last / iv);
        last = now;
        if (first || crossed) run(s.body, env, rt, out);
        return;
    }
    case Stmt::When:
        if (eval(s.a, env, rt) != 0.0) run(s.body, env, rt, out);
        else run(s.otherwise, env, rt, out);
        return;
    case Stmt::Assign: {
        double v = eval(s.a, env, rt);
        if (s.slot >= 0) {
            if (s.op) {
                const double cur = env.p[s.slot];
                if (s.op == '+') v = cur + v;
                else if (s.op == '-') v = cur - v;
                else if (s.op == '*') v = cur * v;
                else if (s.op == '/') v = v == 0.0 ? cur : cur / v;
            }
            env.p[s.slot] = clamp01(v);
        } else if (s.var >= 0 && (size_t)s.var < rt.vars.size()) {
            double& dst = rt.vars[(size_t)s.var];
            if (s.op == '+') dst += v;
            else if (s.op == '-') dst -= v;
            else if (s.op == '*') dst *= v;
            else if (s.op == '/') dst = v == 0.0 ? dst : dst / v;
            else dst = v;
        }
        return;
    }
    case Stmt::Note: {
        Events::Note n;
        n.pitch = std::min(127, std::max(0, (int)std::lround(eval(s.a, env, rt))));
        n.vel = std::min(127, std::max(0, (int)std::lround(eval(s.b, env, rt))));
        n.len = std::max(0.005, eval(s.c, env, rt));
        out.notes.push_back(n);
        return;
    }
    case Stmt::Cc:
        out.midi.push_back({0xB0, std::min(127, std::max(0, (int)std::lround(eval(s.a, env, rt)))),
                                  std::min(127, std::max(0, (int)std::lround(eval(s.b, env, rt))))});
        return;
    case Stmt::Msg:
        out.midi.push_back({std::min(255, std::max(0, (int)std::lround(eval(s.a, env, rt)))),
                            std::min(127, std::max(0, (int)std::lround(eval(s.b, env, rt)))),
                            std::min(127, std::max(0, (int)std::lround(eval(s.c, env, rt))))});
        return;
    case Stmt::Skip: return;
    }
}

inline void run(const std::vector<Stmt>& list, Env& env, Runtime& rt, Events& out) {
    for (const Stmt& s : list) runOne(s, env, rt, out);
}

} // namespace detail

// One control tick: statements in order, P1..P8 read and written through env.p.
inline void tick(const Program& prog, Env& env, Runtime& rt, Events& out) {
    detail::run(prog.body, env, rt, out);
}

class Compiler {
public:
    Program compile(const std::string& code);
private:
    std::vector<std::string> lines_;
    Program prog_;
    std::map<std::string, int> names_;
    int nextState_ = 0;
    size_t pos_ = 0;
    std::vector<Stmt> parseBlock();
    void skipBlock();
    ExprP expr(const std::string& text);
    void statement(const std::string& line, std::vector<Stmt>& out);
};

inline ExprP Compiler::expr(const std::string& text) {
    detail::ExprParser p(detail::lex(text), prog_, names_, nextState_);
    return p.parseAll();
}

inline void Compiler::skipBlock() {
    if (pos_ < lines_.size() && lines_[pos_] == "{") {
        int depth = 0;
        do {
            const std::string& l = lines_[pos_];
            if (l == "{") ++depth;
            else if (l == "}") --depth;
            ++pos_;
        } while (pos_ < lines_.size() && depth > 0);
    }
}

inline void Compiler::statement(const std::string& line, std::vector<Stmt>& out) {
    auto has = [&](const char* p) { return detail::startsWith(line, p); };
    if (has("note ") || line == "note") {
        const auto a = detail::splitArgs(line.size() > 5 ? line.substr(5) : "");
        Stmt s; s.kind = Stmt::Note;
        s.a = expr(a.size() > 0 && !a[0].empty() ? a[0] : "60");
        s.b = expr(a.size() > 1 && !a[1].empty() ? a[1] : "100");
        s.c = expr(a.size() > 2 && !a[2].empty() ? a[2] : "0.5");
        if (!s.a || !s.b || !s.c) return;
        out.push_back(std::move(s)); return;
    }
    if (has("cc ")) {
        const auto a = detail::splitArgs(line.substr(3));
        Stmt s; s.kind = Stmt::Cc;
        s.a = expr(a.size() > 0 && !a[0].empty() ? a[0] : "1");
        s.b = expr(a.size() > 1 && !a[1].empty() ? a[1] : "0");
        if (!s.a || !s.b) return;
        out.push_back(std::move(s)); return;
    }
    if (has("msg ")) {
        const auto a = detail::splitArgs(line.substr(4));
        Stmt s; s.kind = Stmt::Msg;
        s.a = expr(a.size() > 0 && !a[0].empty() ? a[0] : "144");
        s.b = expr(a.size() > 1 && !a[1].empty() ? a[1] : "0");
        s.c = expr(a.size() > 2 && !a[2].empty() ? a[2] : "0");
        if (!s.a || !s.b || !s.c) return;
        out.push_back(std::move(s)); return;
    }
    if (has("csound:")) return;   // raw Csound needs the Csound engine
    // assignment:  name op= value
    size_t eq = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '=') continue;
        if (i + 1 < line.size() && line[i + 1] == '=') { ++i; continue; }
        if (i > 0 && (line[i - 1] == '=' || line[i - 1] == '<' || line[i - 1] == '>' || line[i - 1] == '!')) continue;
        eq = i; break;
    }
    if (eq == std::string::npos) return;   // plain Csound line: ignored
    std::string lhs = detail::trim(line.substr(0, eq));
    char op = 0;
    if (!lhs.empty() && (lhs.back() == '+' || lhs.back() == '-' || lhs.back() == '*' || lhs.back() == '/')) {
        op = lhs.back(); lhs.pop_back(); lhs = detail::trim(lhs);
    }
    if (lhs.empty() || !(std::isalpha((unsigned char)lhs[0]) || lhs[0] == '_')) return;
    for (char ch : lhs) if (!(std::isalnum((unsigned char)ch) || ch == '_')) return;
    const std::string rhs = detail::trim(line.substr(eq + 1));
    int slot = -1;
    if (detail::isParamName(lhs, slot) && detail::startsWith(rhs, "cawget")) return;   // lane binding: the picked lane already supplies P
    Stmt s; s.kind = Stmt::Assign; s.op = op;
    s.a = expr(rhs);
    if (!s.a) return;
    if (slot >= 0) { s.slot = slot; prog_.writesP[slot] = true; }
    else {
        auto it = names_.find(lhs);
        if (it == names_.end()) { s.var = (int)prog_.varNames.size(); prog_.varNames.push_back(lhs); names_[lhs] = s.var; }
        else s.var = it->second;
    }
    out.push_back(std::move(s));
}

inline std::vector<Stmt> Compiler::parseBlock() {
    std::vector<Stmt> out;
    while (pos_ < lines_.size()) {
        const std::string line = lines_[pos_];
        if (line == "}") { ++pos_; return out; }
        if (line == "@ELSE") return out;   // the caller consumes it
        ++pos_;
        if (line == "{") continue;
        if (detail::startsWith(line, "every ") || detail::startsWith(line, "when ")) {
            const bool every = detail::startsWith(line, "every ");
            ExprP head = expr(line.substr(every ? 6 : 5));
            if (!head) { skipBlock(); continue; }
            if (pos_ < lines_.size() && lines_[pos_] == "{") ++pos_;
            Stmt s; s.kind = every ? Stmt::Every : Stmt::When; s.a = head;
            if (every) s.id = nextState_++;
            s.body = parseBlock();
            if (pos_ < lines_.size() && lines_[pos_] == "@ELSE") {
                ++pos_;
                if (pos_ < lines_.size() && lines_[pos_] == "{") ++pos_;
                s.otherwise = parseBlock();
            }
            out.push_back(std::move(s));
            continue;
        }
        statement(line, out);
    }
    return out;
}

inline Program Compiler::compile(const std::string& code) {
    prog_ = Program();
    names_.clear();
    nextState_ = 0;
    pos_ = 0;
    lines_.clear();
    std::string text;
    {
        std::string all;
        size_t a = 0;
        while (a <= code.size()) {
            const size_t b = code.find('\n', a);
            std::string line = code.substr(a, b == std::string::npos ? std::string::npos : b - a);
            size_t p = line.find('#'); size_t q = line.find("//");
            if (q != std::string::npos && (p == std::string::npos || q < p)) p = q;
            if (p != std::string::npos) line = line.substr(0, p);
            all += line + "\n";
            if (b == std::string::npos) break;
            a = b + 1;
        }
        try { all = std::regex_replace(all, std::regex(R"(\}\s*else\s*\{)"), "\n@ELSE\n"); } catch (...) {}
        for (char ch : all) {
            if (ch == '{' || ch == '}') { text += '\n'; text += ch; text += '\n'; }
            else if (ch == ';') text += '\n';
            else text += ch;
        }
    }
    {
        size_t a = 0;
        while (a <= text.size()) {
            const size_t b = text.find('\n', a);
            std::string line = detail::trim(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (!line.empty()) lines_.push_back(std::move(line));
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    prog_.body = parseBlock();
    prog_.states = nextState_;
    prog_.ok = true;
    return prog_;
}

} // namespace vstscript
