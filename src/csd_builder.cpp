#include "volume_limits.h"
#include <filesystem>
#include "csd_builder.h"
#include "pd_channels.h"
// g_projectPath (app_state.h): relative PD patch paths resolve for the meter lane allocation.
// A forward declaration instead of the header: app_state.h needs the app's include context.
extern std::string g_projectPath;
#include "caw_toggles.h"
#include "vst_lang.h"
#include "monitor_view.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <locale>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

// Which basic math opcodes the user's Csound really has (setCsoundMathOps, see csd_builder.h).
// The flag source is a REAL compile probe (csound_api_runner.cpp probeMath: tiny abs / sqrt / pow
// orchestras compiled through the same API library the engine uses) - NOT the 'csound -z' text list,
// whose human column format cannot be parsed reliably and once made CAW falsely refuse an installed
// Csound 7 that compiles abs and sqrt fine. abs is the direct |x| form and the first choice;
// sqrt(x*x) is the fallback; genuine square roots (spectrum band magnitudes) use sqrt, pow(x, 0.5)
// works too. Defaults assume a complete Csound until a probe says otherwise.
static bool g_hasAbs = true, g_hasSqrt = true, g_hasPow = true;
void setCsoundMathOps(bool hasAbs, bool hasSqrt, bool hasPow) {
    g_hasAbs = hasAbs; g_hasSqrt = hasSqrt; g_hasPow = hasPow;
}
// |x| for the meter/scope code: abs when this Csound has it, otherwise |x| via sqrt(x*x).
static std::string cawAbs(const std::string& x) {
    return g_hasAbs ? "abs(" + x + ")" : "sqrt(" + x + " * " + x + ")";
}

std::string channelName(const std::string& instr, const std::string& chn) {
    return instr + "_" + chn;
}

std::string scoreProbePath() {
    std::string s = (workDir() / "caw_score_data.txt").string();
    for (char& ch : s) if (ch == '\\') ch = '/';
    return s;
}

namespace {
// Hex encoding keeps arbitrary literal stream names safe in Csound channel
// names and in the whitespace-delimited probe file.
std::string scoreNameHex(const std::string& name) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : name) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}

std::vector<std::string> splitArgs(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char ch : text) {
        if (ch == '(' || ch == '[') ++depth;
        if (ch == ')' || ch == ']') --depth;
        if (ch == ',' && depth == 0) { out.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    out.push_back(cur);
    for (std::string& a : out) {
        const size_t b = a.find_first_not_of(" \t\r");
        a = b == std::string::npos ? std::string() : a.substr(b, a.find_last_not_of(" \t\r") - b + 1);
    }
    return out;
}

// The track's volume lane at a time (0..2, 1 = no lane); same rule as trackVolumeAutomation in app.cpp.
double trackVolumeAutomationAt(const Track& t, double time) {
    const auto lane = t.automation.find("volume");
    if (lane == t.automation.end() || lane->second.empty()) return 1.0;
    double lo = 0.0, hi = 2.0, initial = 1.0;
    const auto range = t.automationRanges.find("volume");
    if (range != t.automationRanges.end()) {
        lo = range->second.min;
        hi = range->second.max > lo ? range->second.max : lo + 1.0;
        initial = range->second.value;
    }
    return std::max(lo, std::min(hi, autoValueAt(lane->second, time, initial)));
}

// Live track volume: every out / outs / outq / outch line of a clip scales its SIGNAL arguments by
// kCawVol (declared at the top of the instrument). Applied on the out line itself, so it needs no
// mixer instrument and no ordering. The meter / VSTe taps are separate lines, so they stay pre-fader.
// Array outputs (aOut[]) are left as they are: they cannot be multiplied in place.
std::string applyTrackVolume(const std::string& body) {
    static const std::regex outRe(R"re(^(\s*)(outch|outs|outq|outh|outo|outx|outc|out)\b\s*(.*)$)re");
    std::istringstream in(body);
    std::string line, result; std::smatch m;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!std::regex_match(line, m, outRe)) { result += line + "\n"; continue; }
        std::string args = m[3].str(), comment;
        const size_t semi = args.find(';');
        if (semi != std::string::npos) { comment = " " + args.substr(semi); args = args.substr(0, semi); }
        const std::string op = m[2].str();
        std::vector<std::string> list = splitArgs(args);
        bool ok = !list.empty();
        for (size_t i = 0; i < list.size() && ok; ++i) {
            if (op == "outch" && i % 2 == 0) continue;   // channel numbers
            const std::string& e = list[i];
            if (e.empty()) { ok = false; break; }
            if (std::all_of(e.begin(), e.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_'; }) &&
                std::regex_search(body, std::regex("\\b" + e + "\\s*\\["))) { ok = false; break; }   // an array
        }
        if (!ok) { result += line + "\n"; continue; }
        std::string rebuilt = m[1].str() + op + " ";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i) rebuilt += ", ";
            rebuilt += (op == "outch" && i % 2 == 0) ? list[i] : "(" + list[i] + ")*kCawVol";
        }
        result += rebuilt + comment + "\n";
    }
    return result;
}

// After every out / outs / outq / outch line add "gaCawT_<instr>_<ch> = gaCawT_<instr>_<ch> + (expr)" so the
// monitor instrument can meter each lane per output channel. Returns the number of channels found.
// map (optional) is kept in step with the body: tap lines get the clip line of the out line they follow.
int addMeterTaps(const std::string& instrName, std::string& body, std::vector<int>* map = nullptr) {
    static const std::regex outRe(R"re(^(\s*)(outch|outs|outq|outh|outo|outx|outc|out)\b\s*(.*)$)re");
    std::istringstream in(body);
    std::string line, result;
    int maxCh = 0;
    std::vector<int> newMap;
    size_t lineNo = 0;
    while (std::getline(in, line)) {
        const int src = map && lineNo < map->size() ? (*map)[lineNo] : 0;
        ++lineNo;
        result += line + "\n";
        newMap.push_back(src);
        std::smatch m;
        if (!std::regex_match(line, m, outRe)) continue;
        std::string args = m[3].str();
        const size_t semi = args.find(';');
        if (semi != std::string::npos) args = args.substr(0, semi);
        const std::string op = m[2].str();
        const std::vector<std::string> list = splitArgs(args);
        for (size_t i = 0; i < list.size(); ++i) {
            std::string expr = list[i];
            int ch = (int)i + 1;
            if (op == "outch") {
                if (i % 2 == 0) continue;                       // channel numbers
                const std::string chn = list[i - 1];
                ch = (!chn.empty() && std::all_of(chn.begin(), chn.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }))
                         ? std::atoi(chn.c_str()) : (int)(i / 2) + 1;
            }
            if (expr.empty() || ch < 1 || ch > 8) continue;
            // arrays cannot be summed with +, skip them
            if (std::all_of(expr.begin(), expr.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_'; }) &&
                std::regex_search(body, std::regex("\\b" + expr + "\\s*\\["))) continue;
            const std::string bus = "gaCawT_" + instrName + "_" + std::to_string(ch);
            result += m[1].str() + bus + " = " + bus + " + (" + expr + ")\n";
            newMap.push_back(src);
            maxCh = std::max(maxCh, ch);
        }
    }
    body = result;
    if (map) *map = newMap;
    return maxCh;
}

// The clip's meter tap as a named audio channel too: the VST rack reads these by name every
// block, so a VSTe input can be re-wired while the engine runs - no CSD rebuild, no stale lines.
// One channel per output channel, added right after the gaCawT_* tap line (same scope, and the
// VSTe output gates wrap only the `out` line, so the tap keeps running in replace and mix mode).
std::string addVstTapChannels(const std::string& body) {
    static const std::regex tapRe(R"re(^\s*gaCawT_([A-Za-z_]\w*)_([1-8])\s*=\s*gaCawT_[A-Za-z_]\w*_[1-8]\s*\+\s*\((.*)\)\s*$)re");
    std::istringstream in(body);
    std::string line, result; std::smatch m;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        result += line + "\n";
        if (std::regex_match(line, m, tapRe))
            result += "chnmix (" + m[3].str() + "), \"cawtap_" + m[1].str() + "_" + m[2].str() + "\"\n";
    }
    return result;
}

// "cawMath" / self-scheduling. Legacy cawRun and dawrun remain supported.
// In a clip's code you can write
//     (cawRun is the short form: cawRun <p4 maths>, <p5 maths>, <p6 start step inc>, ...  with iRun = copies, indx = copy number)
//     indx = 0
//     while indx < 5 do
//       schedule "this", indx * 0.5, 4, p4 + indx * 2, p5 * 0.8      ; or:  dawrun "this", indx * 0.5, 4, p4 + indx * 2, p5 * 0.8
//       indx += 1
//     od
//     ...the sound of the instrument...
// Every p-field is plain Csound maths. The clip's start runs everything down to the end of the block that holds the last
// "this" line (the launcher) and then switches itself off; each scheduled copy (p1 has a fraction) skips the launcher
// and runs the rest of the code (the voice).
// map (optional): for every line of the returned code, the 1-based line of the clip it came from (0 = generated line).
// srcBase: clip line of the first input line (1 = the code is the clip as typed, 0 = one generated line was put on top).
std::string applyDawRun(const std::string& instrName, const std::string& code, int runs, bool& launcher,
                        std::vector<int>* map = nullptr, int srcBase = 1) {
    static const std::regex selfRe(R"re(^(\s*)(?:schedule|dawrun|cawRun|cawMath)\s+"this"\s*,\s*(.*)$)re");
    static const std::regex dawRe(R"re(^(\s*)(?:dawrun|cawRun|cawMath)\s+(.*)$)re");
    static const std::regex openRe(R"re(^\s*(?:while\b.*\bdo|until\b.*\bdo|if\b.*\bthen)\s*(?:;.*)?$)re");
    static const std::regex closeRe(R"re(^\s*(?:od|endif|fi)\s*(?:;.*)?$)re");
    std::vector<std::string> lines;
    {
        std::istringstream in(code);
        std::string l;
        while (std::getline(in, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }
    }
    std::vector<int> srcs;
    for (size_t k = 0; k < lines.size(); ++k) srcs.push_back((int)k + srcBase);
    const std::string child = "nstrnum(\"" + instrName + "\") + 0.001";
    // one-line form:  cawRun <p4 maths>, <p5 maths>, <p6 maths>, ...
    //   starts N copies at the clip start (N = the clip's x field), each lasting the clip length (p3).
    //   indx = 0 .. N-1 can be used inside the maths. The lines below are the sound of every copy.
    {
        std::vector<std::string> expanded;
        std::vector<int> expandedSrc;
        auto emit = [&](const std::string& text, int src) { expanded.push_back(text); expandedSrc.push_back(src); };
        for (size_t k = 0; k < lines.size(); ++k) {
            const std::string& line = lines[k];
            const int src = srcs[k];
            std::smatch m;
            if (std::regex_match(line, m, dawRe) && (m[2].str().empty() || m[2].str()[0] != '"')) {
                std::string args = m[2].str();
                const size_t semi = args.find(';');
                if (semi != std::string::npos) args = args.substr(0, semi);
                const std::string ind = m[1].str();
                std::string call = ind + "  schedule " + child + ", 0, p3";
                for (const std::string& arg : splitArgs(args))
                    if (!arg.empty()) call += ", " + arg;
                const std::string n = std::to_string(std::min(256, std::max(1, runs)));
                emit(ind + "iRun = " + n, src);
                emit(ind + "indx = 0", src);
                emit(ind + "while indx < " + n + " do", src);
                emit(call, src);
                emit(ind + "  indx += 1", src);
                emit(ind + "od", src);
                continue;
            }
            emit(line, src);
        }
        lines.swap(expanded);
        srcs.swap(expandedSrc);
    }
    int lastSelf = -1, firstSelf = -1;
    for (size_t k = 0; k < lines.size(); ++k) {
        std::smatch m;
        if (std::regex_match(lines[k], m, selfRe)) lines[k] = m[1].str() + "schedule " + child + ", " + m[2].str();
        else if (std::regex_match(lines[k], m, dawRe) && !m[2].str().empty() && m[2].str()[0] == '"')
            lines[k] = m[1].str() + "schedule " + m[2].str();                          // cawRun "Other", ... = plain schedule
        if (lines[k].find(child) != std::string::npos && lines[k].find("schedule") != std::string::npos) {
            lastSelf = (int)k;
            if (firstSelf < 0) firstSelf = (int)k;
        }
    }
    launcher = lastSelf >= 0;
    std::string out;
    std::vector<int> outSrc;
    auto put = [&](const std::string& text, int src) { out += text + "\n"; outSrc.push_back(src); };
    if (lastSelf < 0) {
        for (size_t k = 0; k < lines.size(); ++k) put(lines[k], srcs[k]);
        if (map) *map = outSrc;
        return out;
    }
    // the launcher = the top-level block(s) holding the "this" lines (while ... od / if ... endif, or the single line);
    // lines above it run for every copy, lines below it are the voice
    int depth = 0, end = lastSelf, start = 0;
    for (int k = 0; k <= lastSelf; ++k) {
        if (depth == 0 && k <= firstSelf) start = k;
        if (std::regex_match(lines[(size_t)k], openRe)) ++depth;
        else if (std::regex_match(lines[(size_t)k], closeRe)) --depth;
    }
    for (int k = lastSelf + 1; depth > 0 && k < (int)lines.size(); ++k) {
        end = k;
        if (std::regex_match(lines[(size_t)k], openRe)) ++depth;
        else if (std::regex_match(lines[(size_t)k], closeRe)) --depth;
    }
    const int startSrc = srcs[(size_t)start], endSrc = srcs[(size_t)std::min(end, (int)lines.size() - 1)];
    for (int k = 0; k < start; ++k) put(lines[(size_t)k], srcs[(size_t)k]);
    put("if p1 == int(p1) then", startSrc);                              // started by the timeline: the launcher
    for (int k = start; k <= end && k < (int)lines.size(); ++k) put(lines[(size_t)k], srcs[(size_t)k]);
    put("turnoff", endSrc);
    put("else", endSrc);                                                 // a scheduled copy: the voice
    for (int k = end + 1; k < (int)lines.size(); ++k) put(lines[(size_t)k], srcs[(size_t)k]);
    put("endif", endSrc);
    if (map) *map = outSrc;
    return out;
}

ParsedTrack parseCodeUncached(const std::string& instrName, const std::string& sourceCode, bool taps, int runs) {
    ParsedTrack out;
    std::string code = sourceCode;
    {   // Csound 7: "outs" is deprecated, "out" does the same (same line, so error lines still match the editor)
        static const std::regex outsRe(R"re((^|\n)([ \t]*)outs\b)re");
        code = std::regex_replace(code, outsRe, "$1$2out");
    }

    // 1) extract <caw> ... </caw>
    std::string recVar;                 // <caw> { rec: kLfo }
    std::map<int, std::string> pMath;   // copies: p-field number -> Csound expression built from  {p: 6, from: 400, +100}  lines
    size_t a = code.find("<caw>"), b = code.find("</caw>");
    if (a != std::string::npos && b != std::string::npos && b > a) {
        std::string block = code.substr(a + 5, b - (a + 5));
        static const std::regex midiOn(R"re(midi\s*:\s*"on")re", std::regex_constants::icase);
        out.midiEnabled = std::regex_search(block, midiOn);
        {   // { rec: kLfo }  = send this variable to the automation (Capture / Rec); same as  chnset kLfo, "rec"  at the end of the instrument
            static const std::regex recVarRe(R"re(\{\s*rec\s*:\s*([A-Za-z_]\w*)\s*\})re");
            std::smatch rm;
            if (std::regex_search(block, rm, recVarRe)) recVar = rm[1].str();
        }
        code.replace(a, (b + 6) - a, std::string((size_t)std::count(code.begin() + (long)a, code.begin() + (long)(b + 6), '\n'), '\n'));   // keep the line count: error lines match the editor

        // { chn: "input1", range: (0,127,0.1) }   or   { chn: "input1", range: (0,127,0.1), value: 60 }
        static const std::regex re(
            R"re(\{\s*chn:\s*"([^"]+)"\s*,\s*range:\s*\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)\s*(?:,\s*value:\s*([-+0-9.eE]+)\s*)?\})re");
        // Copies (the clip's x box): one line per p-field, no Csound maths needed.
        //   {p: 5, from: 0, to: 1}     spread from the first to the last copy
        //   {p: 6, from: 100, +100}    per copy + - * /  (x3: 100, 200, 300 | from 400, -100: 400, 300, 200 | from 100, *2: 100, 200, 400 | from 800, /2: 800, 400, 200)
        // A p-field without a line is passed on as it is (for MIDI clips: pitch and velocity).
        {
            static const std::regex pLine(R"re(\{\s*p\s*:\s*(\d+)\s*,([^}]*)\})re");
            static const std::regex attr(R"re((\w+)\s*:\s*([^,}]+))re");
            static const std::regex opRe(R"re((?:^|,)\s*([-+*/])\s*([^,}]+))re");
            auto trim = [](const std::string& v) {
                const size_t f = v.find_first_not_of(" \t"), l = v.find_last_not_of(" \t");
                return f == std::string::npos ? std::string("0") : v.substr(f, l - f + 1);
            };
            for (std::sregex_iterator it(block.begin(), block.end(), pLine), end; it != end; ++it) {
                const int pn = std::atoi((*it)[1].str().c_str());
                if (pn < 4 || pn > 60) continue;
                const std::string rest = (*it)[2].str();
                std::map<std::string, std::string> at;
                for (std::sregex_iterator ai(rest.begin(), rest.end(), attr), aend; ai != aend; ++ai)
                    at[(*ai)[1].str()] = trim((*ai)[2].str());
                std::smatch om;
                std::string e;
                if (std::regex_search(rest, om, opRe)) {
                    const std::string op = om[1].str(), val = trim(om[2].str());
                    const std::string f = at.count("from") ? at["from"] : (op == "*" || op == "/" ? "1" : "0");
                    if (op == "+") e = "(" + f + ") + indx * (" + val + ")";
                    else if (op == "-") e = "(" + f + ") - indx * (" + val + ")";
                    else if (op == "*") e = "(" + f + ") * (" + val + ") ^ indx";
                    else e = "(" + f + ") / ((" + val + ") ^ indx)";
                }
                else if (at.count("from") && at.count("to"))
                    e = "(" + at["from"] + ") + ((" + at["to"] + ") - (" + at["from"] + ")) * indx / max(iRun - 1, 1)";
                else if (at.count("from")) e = "(" + at["from"] + ")";
                if (!e.empty()) pMath[pn] = e;
            }
        }
        for (std::sregex_iterator it(block.begin(), block.end(), re), end; it != end; ++it) {
            try {
                CawParam p;
                p.chn  = (*it)[1];
                p.min  = std::stod((*it)[2]);
                p.max  = std::stod((*it)[3]);
                p.step = std::stod((*it)[4]);
                p.value = (*it)[5].matched ? std::stod((*it)[5]) : (p.min + p.max) * 0.5;
                if (!std::isfinite(p.min) || !std::isfinite(p.max) || !std::isfinite(p.step) ||
                    !std::isfinite(p.value) || p.max <= p.min || p.step <= 0.0) continue;
                p.value = quantizeAutomation(p.value, p);
                out.params.push_back(p);
            } catch (const std::exception&) {
                // An incomplete or invalid declaration must not crash the editor.
            }
        }
    }

    // 2) Translate clip-local cawget. Legacy "caw" remains valid.
    // Match whole code lines only, so comments, strings and unrelated chnget calls stay untouched.
    static const std::regex use(R"re(^([ \t]*)([ki][A-Za-z0-9_]+)[ \t]+(?:cawget|caw)[ \t]+"([^"]+)"([ \t]*(?:;.*)?)$)re");
    static const std::regex useHub(R"re(^([ \t]*)([ki][A-Za-z0-9_]+)[ \t]+cawr[ \t]+"([A-Za-z0-9_/-]+)"([ \t]*(?:;.*)?)$)re");
    std::istringstream input(code);
    std::string result, line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch match;
        if (std::regex_match(line, match, use)) {
            const std::string variable = match[2].str();
            const std::string channel = match[3].str();
            const auto param = std::find_if(out.params.begin(), out.params.end(),
                [&](const CawParam& p) { return p.chn == channel; });
            if (param != out.params.end()) {
                std::ostringstream numbers;
                numbers.imbue(std::locale::classic());
                numbers << std::setprecision(17) << param->min << ", " << param->max << ", " << param->step;
                line = match[1].str() + variable + (variable[0] == 'i' ? " CawInputI \"" : " CawInputK \"")
                    + channelName(instrName, channel) + "\", " + numbers.str() + match[4].str();
            } else {
                line = match[1].str() + variable + " chnget \"" + channelName(instrName, channel) + "\"" + match[4].str();
            }
        } else if (std::regex_match(line, match, useHub)) {
            // 2c) kVar cawr "name" = read CAW's shared hub channel caws/<name> (all clips of one Run
            //     and all engines share it; PD sends into it with [s $0-caws-<name>]).
            line = match[1].str() + match[2].str() + " chnget \"caws/" + match[3].str() + "\"" + match[4].str();
        }
        result += line + '\n';
    }
    // 2b) score data:  chnset kVal, "score"  /  chnset aSig, "score"  /  chnset Stext, "score"
    //     goes to the clip's own channel (<instr>_score); the score reads it during Render audio. Line count stays the same.
    {
        static const std::regex scoreSend(R"re(^(\s*chnset\s+)([^,;]+?)(\s*,\s*)"score"(.*)$)re");
        std::istringstream in(result);
        std::string line, rebuilt;
        bool changed = false;
        while (std::getline(in, line)) {
            std::smatch m;
            static const std::regex recSend(R"re(^(\s*chnset\s+)([^,;]+?)(\s*,\s*)"rec"(.*)$)re");
            static const std::regex writeSend(R"re(^(\s*)cawset\s+([^,;]+?)\s*,\s*"([^"]+)"(\s*(?:;.*)?)$)re");
            static const std::regex hubSend(R"re(^(\s*)caws\s+([^,;]+?)\s*,\s*"([A-Za-z0-9_/-]+)"(\s*(?:;.*)?)$)re");
            static const std::regex namedScore(R"re(^([ \t]*)cawscore[ \t]+"([^"\r\n]+)"[ \t]*,[ \t]*([^;]+?)([ \t]*(?:;.*)?)$)re");
            if (std::regex_match(line, m, namedScore)) {
                const std::string name = m[2].str(), expr = m[3].str();
                size_t f = expr.find_first_not_of(" \t");
                if (f != std::string::npos && (expr[f] == '-' || expr[f] == '+'))
                    f = expr.find_first_not_of(" \t", f + 1);
                const size_t ratePos = f != std::string::npos && expr[f] == 'g' && f + 1 < expr.size() ? f + 1 : f;
                const char ch = ratePos == std::string::npos ? 'k' : expr[ratePos];
                const int sig = (ch == '"' || ch == 'S') ? 3 : ch == 'a' ? 2 :
                    (ch == 'i' || ch == 'p' || ch == '-' || ch == '+' || ch == '.' || (ch >= '0' && ch <= '9')) ? 4 : 1;
                out.scoreStreams.emplace(name, sig);
                line = m[1].str() + "chnset " + expr + ", \"" +
                    channelName(instrName, "cawscore_" + scoreNameHex(name)) + "\"" + m[4].str();
                changed = true;
            } else if (std::regex_match(line, m, writeSend)) {
                const std::string channel = m[3].str();
                if (std::find(out.writeChannels.begin(), out.writeChannels.end(), channel) == out.writeChannels.end())
                    out.writeChannels.push_back(channel);
                out.hasRec = true;
                line = m[1].str() + "chnset " + m[2].str() + ", \"" + channelName(instrName, "write_" + channel) + "\"" + m[4].str();
                changed = true;
            } else if (std::regex_match(line, m, hubSend)) {
                // 2d) caws <expr>, "name" = write CAW's shared hub channel caws/<name>. Every clip of
                //     one Run reads it with cawr; CAW mirrors it to PD ([r $0-cawr-<name>]) and SC (~cawr).
                //     It is NOT an automation recording channel (no writeChannels / rec state).
                line = m[1].str() + "chnset " + m[2].str() + ", \"caws/" + m[3].str() + "\"" + m[4].str();
                changed = true;
            } else if (std::regex_match(line, m, recSend)) {
                out.hasRec = true;
                if (std::find(out.writeChannels.begin(), out.writeChannels.end(), std::string()) == out.writeChannels.end())
                    out.writeChannels.push_back(std::string()); // legacy unnamed rec
                line = m[1].str() + m[2].str() + m[3].str() + "\"" + channelName(instrName, "rec") + "\"" + m[4].str();
                changed = true;
            } else if (std::regex_match(line, m, scoreSend)) {
                const std::string expr = m[2].str();
                const size_t f = expr.find_first_not_of(" \t");
                const char ch = f == std::string::npos ? 'k' : expr[f];
                const int sig = (ch == '"' || ch == 'S') ? 3 : (ch == 'a' ? 2 : 1);
                if (out.scoreSig == 0) out.scoreSig = sig;
                line = m[1].str() + expr + m[3].str() + "\"" + channelName(instrName, "score") + "\"" + m[4].str();
                changed = true;
            }
            rebuilt += line + "\n";
        }
        if (changed) result = rebuilt;
    }
    if (!recVar.empty()) {
        if (!result.empty() && result.back() != '\n') result += '\n';
        result += "chnset " + recVar + ", \"" + channelName(instrName, "rec") + "\"\n";
        out.hasRec = true;
        out.writeChannels.push_back(std::string());
    }
    if (!pMath.empty()) {   // the <caw> copies lines become one cawRun line on top of the code
        std::string line = "cawRun ";
        for (int pn = 4; pn <= pMath.rbegin()->first; ++pn) {
            line += pn > 4 ? ", " : "";
            line += pMath.count(pn) ? pMath[pn] : "p" + std::to_string(pn);
        }
        result = line + "\n" + result;
    }
    out.body = applyDawRun(instrName, result, runs, out.launcher, &out.lineMap, pMath.empty() ? 1 : 0);
    if (taps) out.outChannels = addMeterTaps(instrName, out.body, &out.lineMap);
    return out;
}
}  // namespace

// The UI asks for parsed clips many times per frame; std::regex is slow, so memoize by (name, code).
ParsedTrack parseCode(const std::string& instrName, const std::string& sourceCode, bool taps, int runs) {
    static std::unordered_map<std::string, ParsedTrack> cache;
    std::string key;
    key.reserve(instrName.size() + sourceCode.size() + 2);
    key += taps ? 'T' : 'N';
    key += std::to_string(runs);
    key += instrName;
    key.push_back('\x1f');
    key += sourceCode;
    auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    ParsedTrack out = parseCodeUncached(instrName, sourceCode, taps, runs);
    if (cache.size() > 2048) cache.clear();
    cache.emplace(std::move(key), out);
    return out;
}

std::filesystem::path workDir() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "CAW";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

double quantizeAutomation(double value, const CawParam& param) {
    if (!std::isfinite(value)) return param.value;
    if (!(param.max > param.min)) return value;
    value = std::min(param.max, std::max(param.min, value));
    if (std::isfinite(param.step) && param.step > 0.0) {
        const double units = (value - param.min) / param.step;
        if (std::isfinite(units)) value = param.min + std::round(units) * param.step;
    }
    return std::min(param.max, std::max(param.min, value));
}

double autoValueAt(const std::vector<AutoPoint>& points, double time, double fallback) {
    if (points.empty() || time < points.front().t) return fallback;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (time <= points[i + 1].t) {
            const double span = points[i + 1].t - points[i].t;
            if (span <= 0.0) return points[i + 1].v;
            return points[i].v + (points[i + 1].v - points[i].v) * ((time - points[i].t) / span);
        }
    }
    return points.back().v;
}

bool autoLaneActive(const Track& t, const std::string& channel) {
    if (t.scoreAutoMuted.count(channel)) return false;
    if (!t.scoreAutoSolo.empty() && !t.scoreAutoSolo.count(channel)) return false;
    return true;
}

bool clipHasMidiNotes(const Project& p, int clipId) {
    for (const Track& t : p.tracks)
        for (const MidiClip& mc : t.midiClips)
            if (!mc.muted && mc.clipId == clipId) for (const MidiNote& n : mc.notes) if (n.start < mc.dur) return true;
    return false;
}

std::vector<MidiNote> clipMidiNotes(const Project& p, int clipId) {
    std::vector<MidiNote> all;
    for (const Track& t : p.tracks)
        for (const MidiClip& mc : t.midiClips) {
            if (mc.muted || mc.clipId != clipId) continue;
            const double mcEnd = mc.start + mc.dur;
            for (const MidiNote& n : mc.notes) {
                if (n.start >= mc.dur) continue;
                MidiNote a;
                a.start = mc.start + n.start;
                a.dur = std::min(mcEnd, a.start + n.dur) - a.start;
                if (a.dur <= 0.0) continue;
                a.pitch = std::min(127, std::max(0, n.pitch));
                a.velocity = std::min(127, std::max(1, n.velocity));
                all.push_back(a);
            }
        }
    std::stable_sort(all.begin(), all.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    return all;
}

namespace {
const Clip* clipById(const Project& p, int id) {
    for (const Track& t : p.tracks) for (const Clip& c : t.clips) if (c.id == id) return &c;
    return nullptr;
}
}  // namespace

namespace {
ParsedTrack parseClipWithRangesUncached(const Clip& c0, const Track* t, bool taps) {
    Clip c = c0;
    if (t && (t->kind == 3 || t->kind == 4)) c.code = vstlang::translate(*t, c0.code);   // CAW code -> Csound
    // Metadata is assembled privately for the existing preprocessor, never shown
    // or stored as part of the user's Csound text.
    std::string metadata;
    static const std::regex inputRe(R"re((?:^|\n)[ \t]*[ki][A-Za-z0-9_]+[ \t]+(?:cawget|caw)[ \t]+"([^"]+)")re");
    static const std::regex scInputRe(R"re(~caw[Gg]et[ \t]*\.[ \t]*(?:\(|value[ \t]*\()[ \t]*"([^"]+)")re");
    static const std::regex writeRe(R"re((?:^|\n)[ \t]*cawset[ \t]+[^,;\r\n]+,[ \t]*"([^"]+)")re");
    std::set<std::string> names;
    for (std::sregex_iterator it(c.code.begin(), c.code.end(), inputRe), end; it != end; ++it)
        names.insert((*it)[1].str());
    for (std::sregex_iterator it(c.code.begin(), c.code.end(), scInputRe), end; it != end; ++it)
        names.insert((*it)[1].str());
    for (std::sregex_iterator it(c.code.begin(), c.code.end(), writeRe), end; it != end; ++it)
        names.insert((*it)[1].str());
    for (const std::string& name : names) {
        CawParam p{name, 0.0, 1.0, 0.001, 0.5};
        if (t) { auto found = t->automationRanges.find(name); if (found != t->automationRanges.end()) p = found->second; }
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << std::setprecision(17) << "{chn: \"" << name << "\", range: (" << p.min << "," << p.max << "," << p.step << "), value: " << p.value << "}";
        metadata += line.str();
    }
    metadata += c.pFieldRules;
    if (c.midiEnabled) metadata += " midi: \"on\" ";
    if (c.code.find("<caw>") != std::string::npos) {
        ParsedTrack parsed = parseCode(c.name, c.code, taps, c.runs); // older projects during import
        parsed.midiEnabled = parsed.midiEnabled || c.midiEnabled;
        return parsed;
    }
    return parseCode(c.name, "<caw>" + metadata + "</caw>" + c.code, taps, c.runs);
}

// Cache the entire clip parse, including metadata scans and VST translation.
// The exact key contains every input used by this parser; editing code, MIDI,
// parameter ranges or VST routing/parameter names causes a fresh parse.
ParsedTrack parseClipWithRanges(const Clip& c, const Track* t, bool taps) {
    static std::unordered_map<std::string, ParsedTrack> cache;
    static size_t bytes = 0;
    std::string key;
    auto addRaw = [&key](const auto& value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    auto addText = [&key, &addRaw](const std::string& value) {
        const size_t n = value.size(); addRaw(n); key.append(value);
    };
    addText(c.name); addText(c.code); addText(c.pFieldRules);
    addRaw(c.runs); addRaw(c.midiEnabled); addRaw(taps);
    const bool hasTrack = t != nullptr; addRaw(hasTrack);
    if (t) {
        addRaw(t->kind);
        const size_t count = t->automationRanges.size(); addRaw(count);
        for (const auto& item : t->automationRanges) {
            addText(item.first);
            addRaw(item.second.min); addRaw(item.second.max);
            addRaw(item.second.step); addRaw(item.second.value);
        }
        if (t->kind == 3 || t->kind == 4) {
            addRaw(t->id); addText(t->name);
            const size_t picks = t->vstPick.size(); addRaw(picks);
            const size_t names = t->vstPickName.size(); addRaw(names);
            for (const std::string& name : t->vstPickName) addText(name);
        }
    }
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    ParsedTrack parsed = parseClipWithRangesUncached(c, t, taps);
    // Bound retained source/output memory. A single huge clip is not cached.
    size_t estimate = sizeof(ParsedTrack) + key.size() + parsed.body.size() + parsed.lineMap.size() * sizeof(int);
    for (const auto& p : parsed.params) estimate += p.chn.size() + sizeof(p);
    for (const auto& s : parsed.writeChannels) estimate += s.size();
    for (const auto& s : parsed.scoreStreams) estimate += s.first.size() + sizeof(s) + 3 * sizeof(void*);
    constexpr size_t maxBytes = 16u * 1024u * 1024u;
    if (estimate <= maxBytes) {
        if (bytes + estimate > maxBytes || cache.size() >= 4096) { cache.clear(); bytes = 0; }
        cache.emplace(std::move(key), parsed);
        bytes += estimate;
    }
    return parsed;
}
}

ParsedTrack parseClip(const Clip& c, bool taps) { return parseClipWithRanges(c, nullptr, taps); }
ParsedTrack parseClipForTrack(const Clip& c, const Track& t, bool taps) { return parseClipWithRanges(c, &t, taps); }

std::vector<LaneMeterInfo> meterLayout(const Project& p, const std::string& projectPath) {
    std::vector<LaneMeterInfo> out;
    int next = masterChannels(p.cs);
    for (const Track& t : p.tracks) {
        const bool vst = t.kind == 3 || t.kind == 4;
        int ch = vst ? 2 : 0;
        if (vst) ch = vstTrackOutputChannels ? std::max(1, std::min(8, vstTrackOutputChannels(t.id))) : std::max(1, std::min(8, t.vstTrackChannels()));   // the track's channel format
        // A VST track always RESERVES 8 meter slots in the engine's monitor list: changing its
        // channel count while running can then never shift another track's slots (a Csound
        // source's meters went dark when a VSTe went from 2 to 4 / 6 channels). The track header
        // draws `channels` bars and reads VST peaks by track id anyway.
        const int reserve = vst ? 8 : 0;
        if (t.kind == 6) {   // a PD patch's [dac~ N] says how wide it plays (at least stereo)
            for (const Clip& c : t.clips) {
                if (c.code.empty()) continue;
                std::filesystem::path file = std::filesystem::u8path(c.code);
                if (file.is_relative() && !projectPath.empty()) file = std::filesystem::u8path(projectPath).parent_path() / file;
                ch = std::max(ch, std::max(2, pdPatchMaxChannel(file.u8string())));
            }
        }
        if (!vst && t.kind != 6) for (const Clip& c : t.clips) ch = std::max(ch, parseClip(c, true).outChannels);
        if (ch <= 0) continue;
        // A VST track always reaches the master now (master replace or mix), so it keeps its
        // default meter color. Source tracks whose own output a master-mode VSTe replaces are
        // grayed in the draw call via trackConsumedByMasterVste, not here.
        const bool csoundOnly = false;
        out.push_back({t.id, next, ch, csoundOnly});
        next += std::max(ch, reserve);
    }
    return out;
}

ParsedTrack parseTrack(const Track& t) {
    ParsedTrack out;
    if (t.kind == 1) return out;   // marker track: nothing to automate
    if (t.kind == 2) {             // graph track: up to 8 lines (line1 .. line8), only drawn in the score, never sent to the sound
        auto add = [&out](const std::string& name) {
            for (const CawParam& q : out.params) if (q.chn == name) return;
            CawParam p; p.chn = name; p.min = 0.0; p.max = 1.0; p.step = 0.001; p.value = 0.0;
            out.params.push_back(p);
        };
        for (int k = 1; k <= 8; ++k) add("line" + std::to_string(k));
        for (const auto& kv : t.automation) add(kv.first);   // lines of older projects
        return out;
    }
    auto merge = [&out](const ParsedTrack& p) {
        out.midiEnabled = out.midiEnabled || p.midiEnabled;
        out.hasRec = out.hasRec || p.hasRec;
        for (const CawParam& param : p.params) {
            const bool known = std::any_of(out.params.begin(), out.params.end(),
                [&param](const CawParam& q) { return q.chn == param.chn; });
            if (!known) out.params.push_back(param);
        }
    };
    for (const Clip& c : t.clips) merge(parseClipForTrack(c, t));
    if (t.clips.empty()) {
        Clip prototype;
        prototype.name = t.name;
        prototype.code = t.code;
        prototype.midiEnabled = t.templateMidiEnabled;
        prototype.pFieldRules = t.templatePFieldRules;
        merge(parseClipForTrack(prototype, t));
    }
    return out;
}

std::string buildHeader(const CsoundSettings& s, int renderNchnls) {
    std::ostringstream o;
    const int n = renderNchnls > 0 ? renderNchnls : s.nchnls;
    o << "sr = " << s.sr << "\n"
      << "ksmps = " << s.ksmps << "\n"
      << "nchnls = " << n << "\n";
    if (s.nchnlsIn > 0) o << "nchnls_i = " << s.nchnlsIn << "\n";
    o << "0dbfs = " << s.zdbfs << "\n"
      << "A4 = " << s.a4 << "\n";
    return o.str();
}

namespace {
std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}
std::string joinWords(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}
bool allDigits(const std::string& v) {
    return !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; });
}
}  // namespace

std::string defaultOptions(const CsoundSettings& s) {
    std::ostringstream o;
    o << "-odac";
    if (s.outDev >= 0) o << s.outDev;
    if (s.inDev >= 0) o << " -iadc" << s.inDev;
    std::string backend = s.rtaudio;
    if (s.outDev == -2) backend = "dummy";   // output "None": Csound's dummy audio module keeps real-time timing but opens no soundcard
    // ASIO, WASAPI and DirectSound are PortAudio host APIs on Windows,
    // not Csound real-time audio modules. Preserve older project settings.
    if (backend == "asio" || backend == "wasapi" || backend == "dsound" || backend == "directsound") backend = "portaudio";
    if (backend == "winmm") backend = "winmme";
    o << " -+rtaudio=" << backend << " -b" << s.bufSW << " -B" << s.bufHW << " -d";  // -d = no graphic displays
    return o.str();
}

void syncSettingsFromOptions(CsoundSettings& s) {
    const std::vector<std::string> w = splitWords(s.options);
    bool dummyBackend = false;
    for (size_t i = 0; i < w.size(); ++i) {
        const std::string& t = w[i];
        if (t.rfind("-odac", 0) == 0) {
            const std::string rest = t.substr(5);
            if (rest.empty()) s.outDev = -1;
            else if (allDigits(rest)) s.outDev = std::atoi(rest.c_str());
        } else if (t.rfind("-iadc", 0) == 0) {
            const std::string rest = t.substr(5);
            if (rest.empty()) s.inDev = -1;
            else if (allDigits(rest)) s.inDev = std::atoi(rest.c_str());
        } else if (t.rfind("-+rtaudio=", 0) == 0) {
            const std::string v = t.substr(10);
            if (v == "dummy" || v == "null") dummyBackend = true;
            else if (!v.empty()) s.rtaudio = v;
        } else if (t.size() >= 2 && (t[1] == 'b' || t[1] == 'B') && t[0] == '-' && (t.size() == 2 || allDigits(t.substr(2)))) {
            std::string num = t.substr(2);
            if (num.empty() && i + 1 < w.size() && allDigits(w[i + 1])) num = w[++i];
            if (!num.empty()) {
                const int v = std::min(262144, std::max(1, std::atoi(num.c_str())));
                if (t[1] == 'b') s.bufSW = v; else s.bufHW = v;
            }
        }
    }
    if (dummyBackend) s.outDev = -2;
    else if (s.outDev == -2) s.outDev = -1;
}

void syncOptionsFromSettings(CsoundSettings& s) {
    if (s.options.empty()) s.options = defaultOptions(s);
    std::vector<std::string> w = splitWords(s.options);
    const std::vector<std::string> d = splitWords(defaultOptions(s));   // d[0] = -odac<N>; the rest in a known order
    auto isBuf = [](const std::string& t, char kind) {
        return t.size() >= 2 && t[0] == '-' && t[1] == kind && (t.size() == 2 || allDigits(t.substr(2)));
    };
    auto replaceOrAdd = [&](const std::function<bool(const std::string&)>& match, const std::string& value, bool front) {
        for (size_t i = 0; i < w.size(); ++i)
            if (match(w[i])) {
                const bool split = (isBuf(w[i], 'b') || isBuf(w[i], 'B')) && w[i].size() == 2 && i + 1 < w.size() && allDigits(w[i + 1]);
                w[i] = value;
                if (split) w.erase(w.begin() + (long)i + 1);
                return;
            }
        if (value.empty()) return;
        if (front) w.insert(w.begin(), value); else w.push_back(value);
    };
    replaceOrAdd([](const std::string& t) { return t.rfind("-odac", 0) == 0; }, d[0], true);
    // input device: add only when set, drop the token when it is back to the default
    {
        bool found = false;
        for (size_t i = 0; i < w.size(); ++i)
            if (w[i].rfind("-iadc", 0) == 0) {
                found = true;
                if (s.inDev >= 0) w[i] = "-iadc" + std::to_string(s.inDev);
                else { w.erase(w.begin() + (long)i); }
                break;
            }
        if (!found && s.inDev >= 0) w.push_back("-iadc" + std::to_string(s.inDev));
    }
    std::string backend = s.rtaudio;
    if (backend == "asio" || backend == "wasapi" || backend == "dsound" || backend == "directsound") backend = "portaudio";
    if (backend == "winmm") backend = "winmme";
    if (s.outDev == -2) backend = "dummy";
    replaceOrAdd([](const std::string& t) { return t.rfind("-+rtaudio=", 0) == 0; }, "-+rtaudio=" + backend, false);
    replaceOrAdd([&](const std::string& t) { return isBuf(t, 'b'); }, "-b" + std::to_string(s.bufSW), false);
    replaceOrAdd([&](const std::string& t) { return isBuf(t, 'B'); }, "-B" + std::to_string(s.bufHW), false);
    s.options = joinWords(w);
}

int (*g_cawDefaultDevice)(bool capture) = nullptr;
int g_cawScopeDiv = 1;
int g_cawScopePeak = 0;
int g_cawMonitorPort = 0;
bool g_cawMonitorApi = false;

std::string buildOptions(const CsoundSettings& s, const std::string& mode) {
    if (mode != "render") {
        // the typed options win; an empty box falls back to the generated line
        std::string opts = s.options.empty() ? defaultOptions(s) : s.options;
        // "System default" = the Windows default sound card: looked up at every Run, so it follows the Windows control panel
        if (g_cawDefaultDevice && s.outDev == -1 && opts.find("-+rtaudio=portaudio") != std::string::npos) {
            std::vector<std::string> w = splitWords(opts);
            for (std::string& t : w)
                if (t == "-odac") {
                    const int id = g_cawDefaultDevice(false);
                    if (id >= 0) t = "-odac" + std::to_string(id);
                }
            opts = joinWords(w);
        }
        return opts;
    }
    // CAW owns offline audio output and writes the chosen file itself.
    return "-n -d";
}

std::vector<std::string> engineClipNames(const Project& p) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const Track& t : p.tracks) if (t.kind != 5 && t.kind != 6 && t.kind != 7)
        for (const Clip& c : t.clips)
            if (seen.insert(c.name).second) out.push_back(c.name);
    return out;
}

// The VSTi note helper instruments (one per VSTi track), project order. The live
// transport addresses instruments by number; MIDI clips on a VSTi track send their
// notes straight to "CawVstNote_<id>", so that name must be resolvable too
// (type 14 rows resolve against gSCawVstNotes, the note-helper twin of gSCawClips).
std::vector<std::string> engineVstNoteNames(const Project& p) {
    std::vector<std::string> out;
    for (const Track& t : p.tracks) if (t.kind == 3)
        out.push_back("CawVstNote_" + std::to_string(t.id));
    return out;
}

std::string engineFilePrefix() {   // the command files: <prefix>serial.txt and <prefix><n>.txt, forward slashes
    std::string s = (workDir() / "caw_cmd_").string();
    for (char& ch : s) if (ch == '\\') ch = '/';
    return s;
}

std::vector<std::string> engineChannelNames(const Project& p) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const Track& t : p.tracks) if (t.kind != 5 && t.kind != 6 && t.kind != 7)
        for (const Clip& c : t.clips)
            for (const CawParam& prm : parseClip(c).params) {
                const std::string n = channelName(c.name, prm.chn);
                if (seen.insert(n).second) out.push_back(n);
            }
    return out;
}

// ---- THE timeline: every clip is an instrument with a start time on the timeline; this list says what starts when.
// The render (score lines) and the live Play (rows for the running Csound) both come from this one function.
std::vector<TimelineEvent> timelineEvents(const Project& p, double from, bool includeSounding) {
    std::vector<TimelineEvent> out;
    auto add = [&](bool automation, const std::string& name, double s, double e, double a, double b) {
        double st = s - from, du = e - s;
        if (s < from) {
            if (!includeSounding) return;      // already started: it keeps sounding, it is not started again
            st = 0.0;
            du = e - from;
        }
        if (du <= 0.0) return;
        TimelineEvent ev;
        ev.automation = automation; ev.name = name; ev.start = st; ev.dur = du; ev.a = a; ev.b = b;
        out.push_back(std::move(ev));
    };
    std::unordered_map<int, const Clip*> clipsById;
    for (const Track& t : p.tracks)
        for (const Clip& c : t.clips) clipsById.emplace(c.id, &c);  // first duplicate matches old lookup
    auto findClip = [&](int id) -> const Clip* {
        const auto it = clipsById.find(id);
        return it == clipsById.end() ? nullptr : it->second;
    };
    bool anySolo = false;
    for (const Track& t : p.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : p.tracks) {
        if (t.kind == 5 || t.kind == 6 || t.kind == 7) continue; // hosted elsewhere (PD / SC / sample engine), never a Csound source
        const bool audible = !t.muted && (!anySolo || t.solo) && !trackFrozen(t);   // frozen: no Csound events run for this track (the WAV carries the sound)
        std::vector<ParsedTrack> parsed;
        for (const Clip& c : t.clips) parsed.push_back(parseClipForTrack(c, t));

        if (audible) {
            // 1. a clip plays from its box ("runs" = the box is split into equal slots; a launcher clip is one slot). With midi: "on" the box plays like the editor Run (p4 60, p5 100).
            for (size_t i = 0; i < t.clips.size(); ++i) {
                const Clip& c = t.clips[i];
                if (c.muted) continue;
                const ParsedTrack& pc = parsed[i];
                const int runs = pc.launcher ? 1 : std::min(256, std::max(1, c.runs));
                for (int r = 0; r < runs; ++r)
                    add(false, c.name, c.start + c.dur * (double)r / runs, c.start + c.dur * (double)(r + 1) / runs,
                        (t.kind == 0 && pc.midiEnabled) ? 60.0 : 0.0, (t.kind == 0 && pc.midiEnabled) ? 100.0 : 0.0);
            }
            // 2. MIDI clips are free in time. On a VSTi track the notes go STRAIGHT to the plugin's
            //    note helper (CawVstNote_<id>): no code clip is needed, a muted or deleted clip
            //    never silences them - the DAW-like path (the instrument is its own 4th engine).
            //    On a Csound track they borrow the instrument (name) of their clip.
            //    Every note is one start (p4 pitch, p5 velocity).
            for (const MidiClip& mc : t.midiClips) {
                if (mc.muted) continue;
                if (t.kind == 3 || t.kind == 4) {
                    for (const MidiNote& n : mc.notes) {
                        if (n.start >= mc.dur) continue;
                        const double s = mc.start + n.start;
                        add(false, "CawVstNote_" + std::to_string(t.id), s, std::min(mc.start + mc.dur, s + n.dur),
                            (double)std::min(127, std::max(0, n.pitch)), (double)std::min(127, std::max(1, n.velocity)));
                    }
                    continue;
                }
                const Clip* target = findClip(mc.clipId);
                if (!target || target->muted) continue;
                for (const MidiNote& n : mc.notes) {
                    if (n.start >= mc.dur) continue;
                    const double s = mc.start + n.start;
                    add(false, target->name, s, std::min(mc.start + mc.dur, s + n.dur),
                        (double)std::min(127, std::max(0, n.pitch)), (double)std::min(127, std::max(1, n.velocity)));
                }
            }
        }
        // 3. automation: a lane drives the channel of the same name in every clip of the lane (one ramp per segment between two points)
        if (trackFrozen(t)) continue;   // frozen: the automation is baked into the WAV, no ramps are scheduled
        for (const auto& [channel, rawPoints] : t.automation) {
            std::vector<std::pair<std::string, double>> targets;   // channel of the clip, its default value
            for (size_t i = 0; i < t.clips.size(); ++i)
                for (const CawParam& c : parsed[i].params)
                    if (c.chn == channel) {
                        const std::string target = channelName(t.clips[i].name, channel);
                        bool have = false;
                        for (const auto& tg : targets) if (tg.first == target) have = true;
                        if (!have) targets.push_back({target, c.value});
                        break;
                    }
            if (targets.empty()) continue;
            if (rawPoints.empty() || !autoLaneActive(t, channel)) {   // no points / M / S: use the default
                for (const auto& tg : targets) add(true, tg.first, from, from + 0.01, tg.second, tg.second);
                continue;
            }
            std::vector<AutoPoint> pts = rawPoints;
            std::stable_sort(pts.begin(), pts.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
            if (pts.front().t > from)           // before the first point the channel has its default value
                for (const auto& tg : targets) add(true, tg.first, from, from + 0.01, tg.second, tg.second);
            if (pts.size() == 1) {
                const double at = std::max(from, pts[0].t);
                for (const auto& tg : targets) add(true, tg.first, at, at + 0.01, pts[0].v, pts[0].v);
                continue;
            }
            if (from >= pts.back().t) {   // seek / Play / live replan after the final point: hold the last value
                for (const auto& tg : targets) add(true, tg.first, from, from + 0.01, pts.back().v, pts.back().v);
                continue;
            }
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                const AutoPoint& a = pts[i];
                const AutoPoint& b = pts[i + 1];
                if (b.t <= a.t || b.t <= from) continue;
                const double va = a.t >= from ? a.v : a.v + (b.v - a.v) * ((from - a.t) / (b.t - a.t));   // a ramp that is already running starts where it would be now
                for (const auto& tg : targets) add(true, tg.first, std::max(a.t, from), b.t, va, b.v);
            }
        }
    }
    // in time order; at the same time the ramps go first, so a clip that starts there reads the new value
    std::stable_sort(out.begin(), out.end(), [](const TimelineEvent& a, const TimelineEvent& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.automation && !b.automation;
    });
    return out;
}

// A number that changes whenever anything Play would start changes (clips, notes, automation, mute / solo): the live timeline re-plans when it changes.
unsigned long long timelineSignature(const Project& p) {
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&h](unsigned long long v) { h ^= v; h *= 1099511628211ULL; };
    auto num = [&](double v) { mix((unsigned long long)(long long)std::llround(v * 1000.0)); };
    for (const TimelineEvent& ev : timelineEvents(p, 0.0, true)) {
        mix(ev.automation ? 11 : 10);
        for (char ch : ev.name) mix((unsigned char)ch);
        num(ev.start); num(ev.dur); num(ev.a); num(ev.b);
    }
    return h;
}

namespace {
std::string automationOpcodes() {
    // k / i UDOs keep the namespaced Csound channels private to each clip.
    // Rounding is applied at read time, so even interpolated automation respects the step.
    return
        "opcode CawInputK, k, Siii\n"
        "SCawName, iCawMin, iCawMax, iCawStep xin\n"
        "kCawValue chnget SCawName\n"
        "kCawValue = limit(kCawValue, iCawMin, iCawMax)\n"
        "kCawValue = iCawMin + round((kCawValue - iCawMin) / iCawStep) * iCawStep\n"
        "kCawValue = limit(kCawValue, iCawMin, iCawMax)\n"
        "xout kCawValue\nendop\n\n"
        "opcode CawInputI, i, Siii\n"
        "SCawName, iCawMin, iCawMax, iCawStep xin\n"
        "iCawValue chnget SCawName\n"
        "iCawValue = limit(iCawValue, iCawMin, iCawMax)\n"
        "iCawValue = iCawMin + round((iCawValue - iCawMin) / iCawStep) * iCawStep\n"
        "iCawValue = limit(iCawValue, iCawMin, iCawMax)\n"
        "xout iCawValue\nendop\n\n";
}

std::string includeDirectives(const Project& p, bool scoreSection) {
    std::string out;
    for (const IncludeAsset& asset : p.includes) {
        if (asset.score != scoreSection || asset.path.empty()) continue;
        std::string path = std::filesystem::path(asset.path).generic_string();
        std::string escaped;
        escaped.reserve(path.size() + 8);
        for (char c : path) {
            if (c == '"') escaped += "\\\"";
            else escaped.push_back(c);
        }
        out += "#include \"" + escaped + "\"\n";
    }
    return out;
}
}

std::string resolveCawFiles(const Project& p, const std::string& code) {
    static const std::regex re(R"re(^([ \t]*)(g?S[A-Za-z0-9_]+)[ \t]+cawfile[ \t]+"([^"]+)"([ \t]*(?:;.*)?)$)re");
    std::istringstream in(code);
    std::string line, result;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, re)) {
            const AudioAsset* match = nullptr;
            bool ambiguous = false;
            for (const AudioAsset& file : p.audioPool) {
                if (file.name != m[3].str() && std::filesystem::path(file.path).filename().string() != m[3].str()) continue;
                if (match && match->path != file.path) ambiguous = true;
                else match = &file;
            }
            if (match && !ambiguous) {
                std::string path = std::filesystem::path(match->path).generic_string(), escaped;
                for (char c : path) { if (c == '"') escaped += "\\\""; else escaped += c; }
                line = m[1].str() + m[2].str() + " strcpy \"" + escaped + "\"" + m[4].str();
            } // Missing or ambiguous aliases remain as cawfile, producing a Csound error.
        }
        result += line + '\n';
    }
    return result;
}

// ---- VSTi / VSTe tracks: one always-running instrument holds the track code; the engine thread (vst_rack.h) reads the channels it writes.
std::string vstBusName(const Track& t) { return vstlang::busName(t); }

// Resolve the 8 VSTe input lines into concrete line ranges (see resolveVstInputRoutes in the header).
// Every CONFIGURED input gets a range (in slot order) whether it is on or not: the Csound routing is
// fixed for the run, so a later live on/off toggle just tells the rack to pack or skip that slot's
// already-routed lines. A track input takes one line per output channel of every clip of that track
// (in project order, no mixdown, matching the legacy single-input behaviour); a clips input takes the
// channels of the clip with that name; a master input always takes two (L, R). Lines past 8 are dropped.
std::vector<VstInputRoute> resolveVstInputRoutes(const Project& p, const Track& vt) {
    std::vector<VstInputRoute> out;
    int cursor = 0;
    for (int s = 0; s < 8; ++s) {
        const auto& in = vt.vstInputs[s];
        const bool configured = in.mode == 2 || ((in.mode == 0 || in.mode == 1) && !in.name.empty());
        if (!configured) continue;
        if (cursor >= 8) break;
        VstInputRoute r; r.slot = s; r.mode = in.mode; r.name = in.name; r.lineStart = cursor;
        // A Pure Data (kind 6) / SuperCollider (kind 5) source renders outside Csound: its whole
        // stereo mix feeds the slot (engine 1/2, resolved in collectVstJobs), two lines reserved.
        const bool pdOrScSource = in.mode != 2 && [&]() {
            for (const Track& o : p.tracks) {
                if (o.kind != 5 && o.kind != 6) continue;
                if (in.mode == 0 ? o.name == in.name : std::any_of(o.clips.begin(), o.clips.end(),
                        [&](const Clip& c) { return c.name == in.name; })) return true;
            }
            return false;
        }();
        // A VSTi / VSTe source is also rendered outside Csound (the rack adds its audio straight
        // to the master): the slot gets the source's stereo output, the SAME channels the source
        // sends to the master (engine 3, srcId = the source track, wired by the rack).
        const Track* vstSource = nullptr;
        if (!pdOrScSource && in.mode != 2) {
            for (const Track& o : p.tracks) {
                if (o.kind != 3 && o.kind != 4) continue;
                const bool match = in.mode == 0 ? o.name == in.name : std::any_of(o.clips.begin(), o.clips.end(),
                        [&](const Clip& c) { return c.name == in.name; });
                if (match) { vstSource = &o; break; }
            }
        }
        // A sample track or any frozen source renders through CAW's sample engine: its stereo mix
        // reaches the rack through a per-track ring (engine 4, srcId = the track; a clip input hears
        // the whole track, the same rule as a VST source).
        const bool sampleSource = !pdOrScSource && !vstSource && in.mode != 2 && [&]() {
            for (const Track& o : p.tracks) {
                if (o.kind != 7 && !trackFrozen(o)) continue;
                const bool match = in.mode == 0 ? o.name == in.name : std::any_of(o.clips.begin(), o.clips.end(),
                        [&](const Clip& c) { return c.name == in.name; });
                if (match) return true;
            }
            return false;
        }();
        if (pdOrScSource) {
            r.lineCount = std::min(2, 8 - cursor);
        } else if (vstSource || sampleSource) {
            r.lineCount = std::min(2, 8 - cursor);
        } else if (in.mode == 2) {                            // master: always stereo
            r.lineCount = std::min(2, 8 - cursor);
        } else {                                              // Csound track / clips: one tap channel per output channel
            const int room = 8 - cursor;
            if (in.mode == 0) {                               // track: every clip of that track, in project order
                for (const Track& o : p.tracks)
                    if (o.name == in.name && o.kind != 5 && o.kind != 6 && o.kind != 3 && o.kind != 4 && o.kind != 7)
                        for (const Clip& c : o.clips) {
                            const int ch = std::max(1, parseClip(c, true).outChannels);
                            for (int k = 1; k <= ch && (int)r.chans.size() < room; ++k)
                                r.chans.push_back("cawtap_" + c.name + "_" + std::to_string(k));
                        }
            } else {                                          // clips: the channels of the clip with that name
                int ch = 0;
                for (const Track& o : p.tracks)
                    if (o.kind != 5 && o.kind != 6 && o.kind != 3 && o.kind != 4 && o.kind != 7)
                        for (const Clip& c : o.clips)
                            if (c.name == in.name) ch = std::max(ch, parseClip(c, true).outChannels);
                for (int k = 1; k <= ch && (int)r.chans.size() < room; ++k)
                    r.chans.push_back("cawtap_" + in.name + "_" + std::to_string(k));
            }
            r.lineCount = (int)r.chans.size();
        }
        cursor += r.lineCount;
        out.push_back(r);
    }
    return out;
}

// A track feeds a master-mode (replace) VSTe when any enabled input of that VSTe names this track
// (mode 0) or one of its clips (mode 1). Used by the UI to gray the source lane meter.
bool trackConsumedByMasterVste(const Project& p, const Track& sourceTrack) {
    for (const Track& vt : p.tracks) {
        if (vt.kind != 4 || vt.vstOutMode != 0) continue;
        for (const auto& in : vt.vstInputs) {
            if (!in.on) continue;
            if (in.mode == 0 && in.name == sourceTrack.name) return true;
            if (in.mode == 1)
                for (const Clip& c : sourceTrack.clips)
                    if (c.name == in.name) return true;
        }
    }
    return false;
}

// Declarations of a VST track (once): the parameter channels, the MIDI event ring (a table the engine reads), the note helper instrument.
std::string vstOrchestra(const Project& p, const Track& t) {
    const std::string id = std::to_string(t.id);
    const size_t n = std::min<size_t>(8, t.vstPick.size());
    // Slots bound in the code (p2 = cawget "lane") may exceed the picked parameters: their value
    // channels must exist too, or the generated chnset would write an undeclared channel.
    int boundMax = -1;
    {
        static const std::regex bindRe(R"re(^[ \t]*[pP]([1-8])[ \t]*=[ \t]*cawget)re");
        for (const Clip& c : t.clips)
            for (std::sregex_iterator it = std::sregex_iterator(c.code.begin(), c.code.end(), bindRe), end; it != end; ++it)
                boundMax = std::max(boundMax, (*it)[1].str()[0] - '1');
    }
    const size_t nCh = std::min<size_t>(8, std::max<size_t>(n, (size_t)std::max(0, boundMax + 1)));
    std::ostringstream o;
    o << "gkCawVstW_" << id << " init 0\n"
      << "giCawVstTab_" << id << " ftgen 0, 0, 4096, 2, 0\n"
      << "chnk \"cawvst_" << id << "_wr\", 3\nchnk \"cawvst_" << id << "_tab\", 3\n"
      << "chnset giCawVstTab_" << id << ", \"cawvst_" << id << "_tab\"\n";
    // The VSTe's input audio (`in` / `level(in)` in clip code, cawvstin_<id>_1/2) is NOT declared
    // here: the engine creates those two audio channels from C before this CSD compiles
    // (csoundGetChannelPtr), so the generated CSD carries no VST channels.
    for (size_t k = 1; k <= nCh; ++k) {
        const double saved = k <= t.vstInit.size() && std::isfinite(t.vstInit[k - 1]) ? t.vstInit[k - 1] : 0.5;
        const double initial = std::min(1.0, std::max(0.0, saved));
        o << "chnk \"cawvst_" << id << "_p" << k << "\", 3\n"
          << "chnset " << std::setprecision(10) << initial << ", \"cawvst_" << id << "_p" << k << "\"\n";
    }
    o << "opcode CawVstEv_" << id << ", 0, kkk\n"                      // one MIDI message into the ring: status, data1, data2
      << "kS, kD1, kD2 xin\n"
      << "gkCawVstW_" << id << " = gkCawVstW_" << id << " + 1\n"
      << "kBase = (gkCawVstW_" << id << " % 1024) * 4\n"
      << "tablew kS, kBase, giCawVstTab_" << id << "\ntablew kD1, kBase + 1, giCawVstTab_" << id << "\ntablew kD2, kBase + 2, giCawVstTab_" << id << "\n"
      << "chnset gkCawVstW_" << id << ", \"cawvst_" << id << "_wr\"\nendop\n\n"
      << "instr CawVstNote_" << id << "\n"                              // one note: on at the start, off when it ends
      << "kF init 1\nkOff init 0\nif kF == 1 then\nCawVstEv_" << id << " 144, p4, p5\nkF = 0\nendif\n"
      << "xtratim 0.02\nkR release\nif kR == 1 && kOff == 0 then\nCawVstEv_" << id << " 128, p4, 0\nkOff = 1\nendif\nendin\n\n";
    return o.str();
}
// CAW AUDIO BUSES between the engines (caw_audio_bus.h). In Csound a bus is a pair of audio channels:
//   cawbsend "name", aL, aR   adds into cawbo_<name>_L / _R  (CAW moves them into the bus after every ksmps, then zeroes them)
//   aL, aR cawbrecv "name"    reads    cawbi_<name>_L / _R  (CAW fills them from the bus before every ksmps)
// Every clip of the Run shares the channels, so several senders add up and any clip can listen.
const char* kCawBusUdos =
    "opcode cawbsend, 0, Saa\n"
    "SN, aL, aR xin\n"
    "SL sprintf \"cawbo_%s_L\", SN\nSR sprintf \"cawbo_%s_R\", SN\n"
    "chnmix aL, SL\nchnmix aR, SR\n"
    "endop\n\n"
    "opcode cawbsend, 0, Sa\n"                     // mono: the same signal on both sides
    "SN, aM xin\n"
    "cawbsend SN, aM, aM\n"
    "endop\n\n"
    "opcode cawbrecv, aa, S\n"
    "SN xin\n"
    "SL sprintf \"cawbi_%s_L\", SN\nSR sprintf \"cawbi_%s_R\", SN\n"
    "aL chnget SL\naR chnget SR\n"
    "xout aL, aR\n"
    "endop\n\n";

const char* kVstUdos =
    "opcode cawsa, 0, Saa\n"                       // cawsa "TrackName", aLeft, aRight: send audio into a VSTe track (any number of senders add up)
    "SN, aL, aR xin\n"
    "SA strcat \"cawbus_\", SN\nSL strcat SA, \"_L\"\nSR strcat SA, \"_R\"\n"
    "chnmix aL, SL\nchnmix aR, SR\n"
    "endop\n\n"
    "opcode cawra, aa, S\n"                       // aL, aR cawra "BusName": read the sound a VSTi / VSTe sent to the bus "BusName"
    "SN xin\n"
    "SA strcat \"cawbus_\", SN\nSL strcat SA, \"_L\"\nSR strcat SA, \"_R\"\n"
    "aL chnget SL\naR chnget SR\n"
    "xout aL, aR\n"
    "endop\n\n";

// Append to one buffer while counting emitted lines. This avoids copying and
// rescanning the complete orchestra prefix for every source-map entry.
class CountingStringBuffer : public std::streambuf {
public:
    std::string str() const { return buffer_.str(); }
    size_t lines() const { return lines_; }
protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        const std::streamsize written = buffer_.sputn(data, count);
        lines_ += (size_t)std::count(data, data + written, '\n');
        return written;
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        const int_type result = buffer_.sputc(traits_type::to_char_type(ch));
        if (!traits_type::eq_int_type(result, traits_type::eof()) && traits_type::to_char_type(ch) == '\n') ++lines_;
        return result;
    }
private:
    std::stringbuf buffer_;
    size_t lines_ = 0;
};

// The channel count the Csound engine really renders: the soundcard setup's width, widened to
// the widest out / outs / outch / outq ... channel any clip code (or the orchestra code) writes.
// Wider than the setup is fine: the engine folds the extra channels into the master (k mod N),
// the same virtual-soundcard rule every engine follows. Meter taps already know the widest
// channel (addMeterTaps parses every out opcode; outch with a literal channel number counts).
int csoundRenderChannels(const Project& p) {
    int n = masterChannels(p.cs);
    for (const Track& t : p.tracks) {
        if (t.kind != 0) continue;
        for (const Clip& c : t.clips) n = std::max(n, parseClip(c, true).outChannels);
    }
    if (!p.orchestraCode.empty()) {
        static const std::regex outchRe(R"re(\boutch\s+(\d+))re");
        std::smatch m;
        std::string::const_iterator at = p.orchestraCode.cbegin();
        while (std::regex_search(at, p.orchestraCode.cend(), m, outchRe)) {
            n = std::max(n, std::atoi(m[1].str().c_str()));
            at = m.suffix().first;
        }
    }
    return std::max(1, std::min(64, n));
}

std::string buildCsd(const Project& p, const std::string& mode, double fromTime, std::vector<OrcSpan>* spans) {
    if (spans) spans->clear();
    CountingStringBuffer orchestraBuffer;
    std::ostream o(&orchestraBuffer);
    const int renderNchnls = csoundRenderChannels(p);
    o << "<CsoundSynthesizer>\n<CsOptions>\n" << buildOptions(p.cs, mode) << "\n</CsOptions>\n"
      << "<CsInstruments>\n" << buildHeader(p.cs, renderNchnls) << "\n";
    o << automationOpcodes();
    for (const Track& t : p.tracks) if (t.kind == 3 || t.kind == 4) { o << kVstUdos; break; }
    o << kCawBusUdos;   // audio buses between the engines: cawbsend / cawbrecv (always available)
    if (!p.orchestraCode.empty()) o << resolveCawFiles(p, p.orchestraCode) << "\n\n";
    o << includeDirectives(p, false);
    if (!p.includes.empty()) o << "\n";
    if (!p.udoCode.empty()) o << resolveCawFiles(p, p.udoCode) << "\n\n";
    for (const std::string& instr : p.instrPool) o << resolveCawFiles(p, instr) << "\n\n";
    const bool engine = mode == "engine";
    const std::vector<std::string> chans = engineChannelNames(p);
    {   // names the timeline refers to by number (see engineChannelNames)
        auto arr = [&o](const char* var, const std::vector<std::string>& v) {
            o << var << "[] fillarray ";
            if (v.empty()) o << "\"\"";
            for (size_t i = 0; i < v.size(); ++i) o << (i ? ", " : "") << "\"" << v[i] << "\"";
            o << "\n";
        };
        arr("gSCawChans", chans);                           // automation ramps (CawAuto) name their channel by number
        if (engine) {
            arr("gSCawClips", engineClipNames(p));            // the commands name clips by number (fixed when Run is pressed)
            arr("gSCawVstNotes", engineVstNoteNames(p));      // type 14 rows: MIDI notes straight to a VSTi's note helper
            o << "giCawSerial init 0\ngiCawTabA init 0\ngiCawTabB init 0\n";
        }
    }

    std::ostringstream score;
    double endT = 0.0;
    bool any = false;
    std::set<std::string> emitted;  // clips that share an instr name define it only once

    auto automationValueAt = [](const std::vector<AutoPoint>& points, double time, double fallback) {
        if (points.empty() || time < points.front().t) return fallback;
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            if (time <= points[i + 1].t) {
                const double span = points[i + 1].t - points[i].t;
                if (span <= 0.0) return points[i + 1].v;
                const double x = (time - points[i].t) / span;
                return points[i].v + (points[i + 1].v - points[i].v) * x;
            }
        }
        return points.back().v;
    };

    const bool probe = mode == "render" && p.score.analyze;           // Render audio also measures the clips for the score
    bool vstRouting = false;                                          // VSTe audio input routing reads the clip taps
    bool anyVste = false;
    for (const Track& vt : p.tracks) {
        if (vt.kind == 4) anyVste = true;
        for (const auto& in : vt.vstInputs)
            if ((in.mode == 0 || in.mode == 1) && !in.name.empty()) vstRouting = true;   // configured inputs are routed even when off, so a live on/off toggle has audio to unmute
    }
    // Meter / route buses (gaCawT_* and their named cawtap_* mirrors) are needed by the monitor, by
    // the probe and by the VSTe input routing. In the engine they exist whenever a VSTe track does,
    // so an effect can be wired to any clip while the engine runs.
    const bool tap = (engine && p.cs.monitorFeed) || probe || vstRouting || (engine && anyVste);
    std::vector<std::string> probeAmp;                                // clips measured (amplitude + frequency)
    struct ProbeStream { std::string clip, stream; int sig; };
    std::vector<ProbeStream> probeData;
    std::set<std::string> busDeclared;
    if (tap) {   // meter buses must exist before the instruments that write to them
        for (const Track& t : p.tracks) if (t.kind != 5 && t.kind != 6 && t.kind != 7)
            for (const Clip& c : t.clips) {
                if (!busDeclared.insert(c.name).second) continue;
                // ALL EIGHT channels of every clip, whatever the code writes today: a LIVE code
                // edit can widen the output (outch 3 added later) and its tap line must never
                // reference an undeclared bus ("Variable 'gaCawT_x_3' used before defined").
                for (int k = 1; k <= 8; ++k) {
                    o << "gaCawT_" << c.name << "_" << k << " init 0\n";
                    o << "chna \"cawtap_" << c.name << "_" << k << "\", 3\n";   // the rack reads these by name (live re-wiring)
                }
            }
    }
    if (tap) {   // the taps accumulate (+=); clear them once per pass so the routed VSTe lines carry exactly the current pass (the monitor clears them again at the end when it runs)
        o << "instr CawVstTapClear\n";
        o << "aCawTapZero init 0\n";
        std::set<std::string> tcDone;
        for (const Track& tc : p.tracks) if (tc.kind != 5 && tc.kind != 6 && tc.kind != 7) for (const Clip& tcc : tc.clips)
            if (tcDone.insert(tcc.name).second) {
                const int tch = parseClip(tcc, true).outChannels;
                for (int k = 1; k <= tch; ++k) {
                    o << "gaCawT_" << tcc.name << "_" << k << " = 0\n";
                    o << "chnset aCawTapZero, \"cawtap_" << tcc.name << "_" << k << "\"\n";
                }
            }
        o << "endin\n\n";
        score << "i \"CawVstTapClear\" 0 86400\n";
    }

    // Every VSTe track gets one live gate per clip: cawvstmix_<clip>_<vstId>. 0 = the source
    // reaches the master, 1 = this effect REPLACES it. The app owns the values (wiring, AUDIO
    // OUTPUT, M / S, input on/off) and writes them while the engine plays, so a re-wire works
    // with no Run: a pair nobody replaces stays 0, and a gate channel that does not exist yet
    // (a clip added after Run) reads 0 as well - the safe state is "the source sounds".
    std::vector<int> vstGateIds;
    for (const Track& vt : p.tracks)
        if (vt.kind == 4 && !vt.vstPath.empty()) vstGateIds.push_back(vt.id);
    if (!vstGateIds.empty()) {   // declare the gate channels up front (chnk default = 0 = source audible)
        std::set<std::string> gateDecl;
        for (const Track& t : p.tracks) if (t.kind != 5 && t.kind != 6 && t.kind != 7)
            for (const Clip& c : t.clips) {
                if (!gateDecl.insert(c.name).second) continue;
                for (int id : vstGateIds)
                    o << "chnk \"cawvstmix_" << c.name << "_" << id << "\", 3\n";
            }
    }

    if (engine)   // the track fader x volume lane of every live Csound track (CAW writes it while the engine runs)
        for (const Track& t : p.tracks) if (t.kind == 0) {
            const double g = std::max(0.0, std::min((double)kMaxVolumeGain, (double)t.volume)) * trackVolumeAutomationAt(t, fromTime);
            o << "chnk \"cawvol_" << t.id << "\", 3\nchnset " << g << ", \"cawvol_" << t.id << "\"\n";
        }
    for (const Track& t : p.tracks) {
        if (t.kind == 5 || t.kind == 6 || t.kind == 7) continue; // .pd path / sample engine audio is not orchestra code
        if (t.kind == 3 || t.kind == 4) o << vstOrchestra(p, t);   // then its clips are built like any other clip (CAW code translated first)

        // Every clip is its own instrument: own instr name, own code, own channels, own MIDI.
        for (const Clip& clip : t.clips) {
            const ParsedTrack pc = parseClipForTrack(clip, t, tap);
            if (emitted.insert(clip.name).second) {
                if (probe) {
                    if (pc.outChannels >= 1) probeAmp.push_back(clip.name);
                    if (pc.scoreSig) {
                        o << (pc.scoreSig == 2 ? "chna" : pc.scoreSig == 3 ? "chnS" : "chnk") << " \"" << channelName(clip.name, "score") << "\", 3\n";
                        probeData.push_back({clip.name, "", pc.scoreSig});
                    }
                    for (const auto& stream : pc.scoreStreams) {
                        const std::string chan = channelName(clip.name, "cawscore_" + scoreNameHex(stream.first));
                        o << (stream.second == 2 ? "chna" : stream.second == 3 ? "chnS" : "chnk") << " \"" << chan << "\", 3\n";
                        probeData.push_back({clip.name, stream.first, stream.second});
                    }
                }
                for (const CawParam& c : pc.params) {
                    const std::string fullName = channelName(clip.name, c.chn);
                    o << "chnk \"" << fullName << "\", 3\n";
                    auto it = t.automation.find(c.chn);
                    const double initial = (it == t.automation.end() || !autoLaneActive(t, c.chn)) ? c.value : automationValueAt(it->second, fromTime, c.value);   // muted lane = default value
                    o << "chnset " << initial << ", \"" << fullName << "\"\n";
                }
                if (pc.hasRec) o << "chnk \"" << channelName(clip.name, "rec") << "\", 3\n";
                for (const std::string& channel : pc.writeChannels)
                    if (!channel.empty()) o << "chnk \"" << channelName(clip.name, "write_" + channel) << "\", 3\n";
                o << "\ninstr " << clip.name << "\n";
                o << "iCawP6 = p6\n";
                const bool cawVol = engine && t.kind == 0;   // live Csound track: the fader x volume lane rides every out line
                if (cawVol) o << "kCawVol chnget \"cawvol_" << t.id << "\"\nkCawVol portk kCawVol, 0.02\n";   // every transport row fires 6 p-fields: consume the 6th, or Csound 7 warns "uses N p-fields but is given 6"
                if (spans) {
                    OrcSpan sp;
                    sp.name = clip.name;
                    sp.first = (int)orchestraBuffer.lines() + 1;  // lines of the whole .csd, including header
                    sp.lineMap = pc.lineMap;
                    spans->push_back(sp);
                }
                // VSTe source clips: wrap the clip's own `out` lines in one live gate per effect
                // (cawvstmix_<clip>_<id>, 0 = source audible, 1 = replaced by that effect). The
                // source plays only when EVERY effect lets it through, exactly like a DAW bus.
                // The values are written by the app while the engine runs, and the meter taps
                // after each `out` stay outside the gates, so a VSTe input keeps receiving the
                // clip signal in both modes.
                std::string body = resolveCawFiles(p, pc.body);
                if (tap) body = addVstTapChannels(body);   // the rack reads the clip taps by name (live wiring)
                if (!vstGateIds.empty()) {
                    static const std::regex outLineRe(R"re(^(\s*)(outch|outs|outq|outh|outo|outx|outc|out)\b\s*(.*)$)re");
                    std::string gates, endifs;
                    for (int gateId : vstGateIds) {
                        const std::string gateVar = "kCawGate" + std::to_string(gateId);
                        gates += gateVar + " chnget \"cawvstmix_" + clip.name + "_" + std::to_string(gateId) + "\"\n";
                        gates += "if " + gateVar + " == 0 then\n";
                        endifs += "endif\n";
                    }
                    std::istringstream in(body); std::string line, rebuilt; std::smatch m;
                    while (std::getline(in, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (std::regex_match(line, m, outLineRe)) rebuilt += gates + line + "\n" + endifs;
                        else rebuilt += line + "\n";
                    }
                    body = rebuilt;
                }
                if (cawVol) body = applyTrackVolume(body);
                o << body << "\nendin\n\n";
            }
        }
    }

    // ---- the score = the timeline events: every clip is an instrument, its start time on the timeline is p2 (a render and a live Play use the same score)
    {
        o << "instr CawAuto\nSCawC = gSCawChans[p6 - 1]\nkCawA line p4, p3, p5\nchnset kCawA, SCawC\nendin\n\n";   // a ramp of one automation channel: p4 -> p5 over p3, p6 = channel number
        if (!engine) {   // Run only defines the instruments; Play sends the same events to the running Csound (see CawTransport)
            for (const TimelineEvent& ev : timelineEvents(p, fromTime)) {
                if (ev.automation) {
                    const int ix = (int)(std::find(chans.begin(), chans.end(), ev.name) - chans.begin()) + 1;
                    score << "i \"CawAuto\" " << ev.start << " " << ev.dur << " " << ev.a << " " << ev.b << " " << ix << "\n";
                } else {
                    score << "i \"" << ev.name << "\" " << ev.start << " " << ev.dur;
                    if (ev.a != 0.0 || ev.b != 0.0) score << " " << ev.a << " " << ev.b;
                    score << "\n";
                }
                endT = std::max(endT, ev.start + ev.dur);
                any = true;
            }
        }
    }
    if (engine) {
        // Run: Csound stays alive and reads small numbered command files.
        // The API host publishes the last completed serial via a control channel;
        // the legacy process path polls caw_cmd_serial.txt instead.
        //   caw_cmd_<n>.txt     = command n:  mode  rows  then <rows> rows of 6 numbers:  type  number  delay  dur  a  b
        //       mode 1 = replaces the running timeline, 0 = plays next to it
        //       type 10 start clip <number> (delay s from now, dur, p4 = a, p5 = b)   type 11 ramp of channel <number> from a to b
        //       type 12 silence everything                                              type 13 stop clip <number>
        // The loader turns the rows into a private table (numbers only) and starts ONE CawTransport per command: it fires every row at its delay.
        // Stopping the transport leaves nothing pending in Csound.
        const std::string pre = engineFilePrefix();
        o << "instr CawStop\n"                                   // silence: every clip and every ramp
          << "iCawP6 = p6\n"                                     // consume the transport row's 6th field: Csound 7 warns about unused p-fields
          << "iCawK = 0\niCawLen lenarray gSCawClips\n"
          << "while iCawK < iCawLen do\nSCawN = gSCawClips[iCawK]\niCawL strlen SCawN\nif iCawL > 0 then\nturnoff2i nstrnum(SCawN), 0, 0\nendif\niCawK += 1\nod\n"
          << "turnoff2i nstrnum(\"CawAuto\"), 0, 0\n";
        // VST note events live in separate child instruments. Release those
        // too, so a loop wrap or transport stop sends their note-offs.
        for (const Track& track : p.tracks)
            if (track.kind == 3 || track.kind == 4)
                o << "turnoff2i nstrnum(\"CawVstNote_" << track.id << "\"), 0, 1\n";
        o << "endin\n\n"
          << "instr CawStopOne\niCawP6 = p6\nSCawN = gSCawClips[p4 - 1]\nturnoff2i nstrnum(SCawN), 0, 0\nendin\n\n"
          << "opcode CawNum, i, S\nSIn xin\niLen strlen SIn\niJ = 1\niN = 0\n"          // the digits of a text as a number (a CR / LF or an empty text does no harm)
          << "while iJ <= iLen do\niC strchar SIn, iJ\nif iC >= 48 && iC <= 57 then\niN = iN * 10 + iC - 48\nendif\niJ += 1\nod\nxout iN\nendop\n\n"
          ;
        if (g_cawMonitorApi) {
            // The API host publishes completed command files through a channel.
            o << "instr CawPoll\nkCawT metro 100\nkCawS chnget \"__caw_cmd_serial\"\n"
              << "if kCawT == 1 && kCawS > giCawSerial then\n"
              << "event \"i\", \"CawLoad\", 0, 0, kCawS\nendif\nendin\n\n"
              << "instr CawLoad\niCawS = p4\nif iCawS > 0 then\n";
        } else {
            o << "instr CawPoll\nkCawT metro 100\nif kCawT == 1 then\nevent \"i\", \"CawLoad\", 0, 0\nendif\nendin\n\n"
              << "instr CawLoad\n"
              << "SCawL, iCawOk readfi \"" << pre << "serial.txt\"\n"
              << "iCawS CawNum SCawL\nif iCawOk > 0 then\n";
        }
        o << "while giCawSerial < iCawS do\n"                  // every command that is new, in order
          << "giCawSerial += 1\n"
          << "SCawF sprintf \"" << pre << "%d.txt\", giCawSerial\n"
          << "iCawF ftgen 0, 0, 0, -23, SCawF\n"
          << "iCawMode table 0, iCawF\niCawRows table 1, iCawF\n"
          << "if iCawMode == 1 then\nturnoff2i nstrnum(\"CawTransport\"), 0, 0\nturnoff2i nstrnum(\"CawAuto\"), 0, 0\nendif\n"
          << "if iCawRows > 0 then\n"
          << "iCawSz = iCawRows * -6\niCawTn ftgen 0, 0, iCawSz, -2, 0\n"
          << "if iCawMode == 1 then\nif giCawTabB > 0 then\nftfree giCawTabB, 0\nendif\ngiCawTabB = giCawTabA\ngiCawTabA = iCawTn\nendif\n"
          << "iCawK = 0\n"
          << "while iCawK < iCawRows do\n"
          << "iCawO = 2 + iCawK * 6\n"
          << "iCawTy table iCawO, iCawF\niCawIx table iCawO + 1, iCawF\niCawDl table iCawO + 2, iCawF\niCawDu table iCawO + 3, iCawF\niCawA table iCawO + 4, iCawF\niCawB table iCawO + 5, iCawF\n"
          << "iCawP1 = 0\niCawP6 = 0\n"
          << "if iCawTy == 10 then\nSCawN = gSCawClips[iCawIx - 1]\niCawP1 = nstrnum(SCawN)\n"
          << "elseif iCawTy == 14 then\nSCawN = gSCawVstNotes[iCawIx - 1]\niCawP1 = nstrnum(SCawN)\n"
          << "elseif iCawTy == 11 then\niCawP1 = nstrnum(\"CawAuto\")\niCawP6 = iCawIx\n"
          << "elseif iCawTy == 12 then\niCawP1 = nstrnum(\"CawStop\")\n"
          << "else\niCawP1 = nstrnum(\"CawStopOne\")\niCawA = iCawIx\nendif\n"
          << "iCawQ = iCawK * 6\n"
          << "tablew iCawP1, iCawQ, iCawTn\ntablew iCawDl, iCawQ + 1, iCawTn\ntablew iCawDu, iCawQ + 2, iCawTn\n"
          << "tablew iCawA, iCawQ + 3, iCawTn\ntablew iCawB, iCawQ + 4, iCawTn\ntablew iCawP6, iCawQ + 5, iCawTn\n"
          << "iCawK += 1\nod\n"
          << "eventi \"i\", \"CawTransport\", 0, 86400, iCawTn, iCawRows\n"
          << "endif\n"
          << "ftfree iCawF, 0\n"
          << "od\n";
        if (g_cawMonitorApi) o << "chnset iCawS, \"__caw_loaded_serial\"\n";
        o << "endif\nendin\n\n"
          << "instr CawTransport\n"                              // p4 = table of rows  p1 delay dur a b p6  (sorted by delay), p5 = number of rows
          << "kCawRow init 0\nkCawNow timeinsts\nkCawMore = 1\n"
          << "while kCawMore == 1 do\n"
          << "if kCawRow >= p5 then\nkCawMore = 0\nelse\n"
          << "kCawB = kCawRow * 6\nkCawI1 = kCawB + 1\n"
          << "kCawDly table kCawI1, p4\n"
          << "if kCawDly > kCawNow then\nkCawMore = 0\nelse\n"
          << "kCawI2 = kCawB + 2\nkCawI3 = kCawB + 3\nkCawI4 = kCawB + 4\nkCawI5 = kCawB + 5\n"
          << "kCawP1 table kCawB, p4\nkCawDu table kCawI2, p4\nkCawA table kCawI3, p4\nkCawV table kCawI4, p4\nkCawP6 table kCawI5, p4\n"
          << "event \"i\", kCawP1, 0, kCawDu, kCawA, kCawV, kCawP6\n"
          << "kCawRow += 1\nendif\nendif\nod\n"
          << "if kCawRow >= p5 then\nturnoff\nendif\nendin\n\n";
        score << "i \"CawPoll\" 0 86400\n";
        if (p.cs.monitorFeed && !cawEnv("CAW_NO_MON")) {
            // ---- monitor: runs after every other instrument; OSC when enabled, otherwise legacy text
            const std::vector<LaneMeterInfo> layout = meterLayout(p, g_projectPath);
            std::ostringstream mon;
            // The engine renders renderNchnls channels; the master meter folds them down to the
            // soundcard setup's width (channel k plays on meter k mod N) - it shows what the
            // soundcard really gets, exactly like every other engine's fold.
            const int rch = renderNchnls;
            const int mch = masterChannels(p.cs);
            auto foldExpr = [&](int c) {
                if (rch <= 1) return std::string("aM1");
                std::string fold;
                for (int k = c; k <= rch; k += mch) fold += (fold.empty() ? "" : " + ") + ("aM" + std::to_string(k));
                return fold;
            };
            // 'monitor' returns every rendered channel; the metered pair folds to the setup width
            if (rch <= 1) {
                mon << "instr CawMon\naM1 monitor\naL = aM1\naR = aM1\n";
            } else {
                std::string list;
                for (int c = 1; c <= rch; ++c) list += std::string(c > 1 ? ", " : "") + "aM" + std::to_string(c);
                mon << "instr CawMon\n" << list << " monitor\naL = " << foldExpr(1) << "\naR = " << (mch >= 2 ? foldExpr(2) : foldExpr(1)) << "\n";
            }
            mon << "kTrig metro 30\n";
            std::vector<std::string> peaks;
            for (int c = 1; c <= mch; ++c) {
                const std::string id = std::to_string(c);
                mon << "aAbsM" << id << " = " << cawAbs("(" + foldExpr(c) + ")") << "\n"   // the folded channel: |x| via abs or sqrt(x*x)
                    << "kPkM" << id << " maxk aAbsM" << id << ", kTrig, 1\n";
                peaks.push_back("kPkM" + id);
            }
            // lane buses
            std::set<std::string> cleared;
            for (const LaneMeterInfo& lm : layout) {
                for (const Track& t : p.tracks) {
                    if (t.id != lm.trackId) continue;
                    for (int c = 1; c <= lm.channels; ++c) {
                        std::string sum;
                        std::set<std::string> seen;
                        if (t.kind != 3 && t.kind != 4 && t.kind != 5 && t.kind != 6 && t.kind != 7) for (const Clip& clip : t.clips) {
                            if (!seen.insert(clip.name).second) continue;
                            if (parseClip(clip, true).outChannels < c) continue;
                            sum += (sum.empty() ? "" : " + ") + std::string("gaCawT_") + clip.name + "_" + std::to_string(c);
                        }
                        // VST peaks arrive from the API rack, not from Csound's clip tap buses.
                        const std::string id = std::to_string(lm.first + c - 1);
                        // Empty sum (PD / VST lanes: their audio never runs through a Csound tap bus)
                        // means plain 0 - NOT abs((0)): abs with an i-rate constant is a signature the
                        // fresh Csound 7 builds reject ("Unable to find opcode entry for 'abs'"), and
                        // |0| is 0 anyway.
                        mon << "aLn" << id << " = " << (sum.empty() ? std::string("0") : cawAbs("(" + sum + ")")) << "\n"
                            << "kLn" << id << " maxk aLn" << id << ", kTrig, 1\n";
                        // the peak list IS the slot list: a VST lane reserves 8 slots (see meterLayout),
                        // so pad up to this lane's slot before adding it
                        while ((int)peaks.size() < lm.first + c - 1) peaks.push_back("0");
                        peaks.push_back("kLn" + id);
                    }
                }
            }
            for (const std::string& name : busDeclared) {
                int ch = 0;
                for (const Track& t : p.tracks) if(t.kind != 5 && t.kind != 6 && t.kind != 7) for (const Clip& c : t.clips) if (c.name == name) ch = std::max(ch, parseClip(c, true).outChannels);
                for (int c = 1; c <= ch; ++c) mon << "gaCawT_" << name << "_" << c << " = 0\n";
            }
            // Legacy scope data is only needed by the text fallback.
            if (!g_cawMonitorApi && g_cawMonitorPort == 0) {
            mon << "kDiv init " << std::max(1, g_cawScopeDiv) << "\n"   // control periods per scope point (= the scope time window, set by the app before each Play)
                << "kScL[] init " << kScopePoints << "\nkScR[] init " << kScopePoints << "\nkW init 0\nkScN init 0\nkAccL init 0\nkAccR init 0\n"
                << "kSmpL downsamp aL\nkSmpR downsamp aR\n";
            if (g_cawScopePeak)
                mon << "if " << cawAbs("kSmpL") << " > " << cawAbs("kAccL") << " then\nkAccL = kSmpL\nendif\n"
                    << "if " << cawAbs("kSmpR") << " > " << cawAbs("kAccR") << " then\nkAccR = kSmpR\nendif\n";
            else
                mon << "kAccL = kSmpL\nkAccR = kSmpR\n";
            mon
                << "kScN = kScN + 1\nif kScN >= kDiv then\nkScL[kW] = kAccL\nkScR[kW] = kAccR\nkAccL = 0\nkAccR = 0\nkScN = 0\n"
                << "kW = kW + 1\nif kW >= " << kScopePoints << " then\nkW = 0\nendif\nendif\n";
            }
            // Legacy text output (also retained for the explicit fallback).
            auto sendList = [&](char tag, int startIndex, const std::vector<std::string>& names, const char* trigger = "kTrig", size_t chunkSize = 16) {
                for (size_t i = 0; i < names.size(); i += chunkSize) {
                    const size_t n = std::min(chunkSize, names.size() - i);
                    mon << "printf \"CAW" << tag << " " << (startIndex + (int)i);
                    for (size_t k = 0; k < n; ++k) mon << " %.9g";
                    mon << "\\n\", " << trigger;
                    for (size_t k = 0; k < n; ++k) mon << ", " << names[i + k];
                    mon << "\n";
                }
            };
            if (!g_cawMonitorApi && g_cawMonitorPort == 0) {
            mon << "kOutL[] init " << kScopePoints << "\nkOutR[] init " << kScopePoints << "\n"
                << "if kTrig == 1 then\n"
                << "kJ = 0\nwhile kJ < " << kScopePoints << " do\n"
                << "kQ = kW + kJ\nif kQ >= " << kScopePoints << " then\nkQ = kQ - " << kScopePoints << "\nendif\n"
                << "kOutL[kJ] = kScL[kQ]\nkOutR[kJ] = kScR[kQ]\nkJ += 1\nod\n"
                << "endif\n";
            }
            if (g_cawMonitorApi) {
                mon << "if kTrig == 1 then\n";
                for (size_t i = 0; i < peaks.size(); ++i)
                    mon << "chnset " << peaks[i] << ", \"__caw_meter_" << i << "\"\n";
                mon << "endif\n";
            } else if (g_cawMonitorPort > 0) {
                mon << "kMeterSeq init 0\nif kTrig == 1 then\nkMeterSeq = (kMeterSeq + 1) % 1000000\nendif\n";
                for (size_t i = 0; i < peaks.size(); i += 16) {
                    const size_t n = std::min<size_t>(16, peaks.size() - i);
                    mon << "OSCsend kMeterSeq, \"127.0.0.1\", " << g_cawMonitorPort
                        << ", \"/caw/meter\", \"ii" << std::string(n, 'f')
                        << "\", kMeterSeq, " << i;
                    for (size_t k = 0; k < n; ++k) mon << ", " << peaks[i + k];
                    mon << "\n";
                }
            } else sendList('M', 0, peaks);
            std::vector<std::string> scopeL, scopeR;
            for (int j = 0; j < kScopePoints; ++j) {
                scopeL.push_back("kOutL[" + std::to_string(j) + "]");
                scopeR.push_back("kOutR[" + std::to_string(j) + "]");
            }
            if (!g_cawMonitorApi && g_cawMonitorPort == 0) {
                sendList('L', 0, scopeL);
                sendList('R', 0, scopeR);
            }
            // Complete uniform 2048-sample stereo frames. Binary OSC can
            // refresh at 60 Hz; keep the expensive text fallback at 30 Hz.
            if (!g_cawMonitorApi) {
            mon << "iRawL ftgen 0, 0, -" << monitor_fft::size << ", 2, 0\n"
                << "iRawR ftgen 0, 0, -" << monitor_fft::size << ", 2, 0\n"
                << "aRawPhase phasor sr / " << monitor_fft::size << "\n"
                << "aRawIndex = aRawPhase * " << monitor_fft::size << "\n"
                << "tablew aL, aRawIndex, iRawL, 0, 0, 1\n"
                << "tablew aR, aRawIndex, iRawR, 0, 0, 1\n"
                << "kRawPhase downsamp aRawPhase\n"
                << "kRawTrig metro " << (g_cawMonitorPort > 0 ? 60 : 30) << "\n"
                << "kRawAge timeinsts\nif kRawAge < " << monitor_fft::size << " / sr then\nkRawTrig = 0\nendif\n";
            if (g_cawMonitorPort > 0) {
                mon << "kRawSeq init 0\nif kRawTrig == 1 then\n"
                    << "kRawHead = (int(kRawPhase * " << monitor_fft::size << " + 0.5) + ksmps) % " << monitor_fft::size << "\n"
                    << "kRawSeq = (kRawSeq + 1) % 1000000\n"
                    << "OSCsend kRawSeq, \"127.0.0.1\", " << g_cawMonitorPort
                    << ", \"/caw/raw\", \"iiGG\", kRawSeq, kRawHead, iRawL, iRawR\nendif\n";
            } else {
                mon << "kRawL[] init " << monitor_fft::size << "\nkRawR[] init " << monitor_fft::size << "\n"
                    << "if kRawTrig == 1 then\n"
                    << "kRawHead = (int(kRawPhase * " << monitor_fft::size << " + 0.5) + ksmps) % " << monitor_fft::size << "\n"
                    << "kRawJ = 0\nwhile kRawJ < " << monitor_fft::size << " do\n"
                    << "kRawQ = (kRawHead + kRawJ) % " << monitor_fft::size << "\n"
                    << "kRawL[kRawJ] table kRawQ, iRawL\nkRawR[kRawJ] table kRawQ, iRawR\n"
                    << "kRawJ += 1\nod\nendif\n";
                std::vector<std::string> rawL, rawR;
                for (int j = 0; j < monitor_fft::size; ++j) {
                    rawL.push_back("kRawL[" + std::to_string(j) + "]");
                    rawR.push_back("kRawR[" + std::to_string(j) + "]");
                }
                sendList('U', 0, rawL, "kRawTrig", MonitorFrameAssembler::chunkSize);
                sendList('V', 0, rawR, "kRawTrig", MonitorFrameAssembler::chunkSize);
            }
            }
            {   // live Rec: the value each clip sends to its channel "rec", 50 times per second (CAWC <first index> <values>)
                const std::vector<CawWriteTarget> recNames = cawWriteTargets(p);
                if (!recNames.empty()) {
                    mon << "kRecTrig metro 50\n";
                    std::vector<std::string> vars;
                    for (size_t k = 0; k < recNames.size(); ++k) {
                        const std::string local = recNames[k].channel.empty() ? "rec" : "write_" + recNames[k].channel;
                        mon << "kRecV" << k << " chnget \"" << channelName(recNames[k].clip, local) << "\"\n";
                        vars.push_back("kRecV" + std::to_string(k));
                    }
                    for (size_t i = 0; i < vars.size(); i += 16) {
                        const size_t n = std::min<size_t>(16, vars.size() - i);
                        mon << "printf \"CAWC " << (int)i;
                        for (size_t k = 0; k < n; ++k) mon << " %f";
                        mon << "\\n\", kRecTrig";
                        for (size_t k = 0; k < n; ++k) mon << ", " << vars[i + k];
                        mon << "\n";
                    }
                }
            }
            mon << "endin\n\n";
            o << mon.str();
            score << "i \"CawMon\" 0 86400\n";   // the monitor runs for the whole engine session (it is the last instrument defined)
        }
    }
    if (probe && any && (!probeAmp.empty() || !probeData.empty())) {
        // ---- score probe: runs after every clip instrument (it is defined last) and writes 20 lines per second to a text file:
        //   A <clip> <time> <rms> <centroid Hz>     measured amplitude and frequency position of the clip
        //   D <clip> <time> <value>                 number / signal sent to the channel "score"
        //   S <clip> <time> <text>                  string sent to the channel "score"
        const std::string file = scoreProbePath();
        o << "instr CawScoreProbe\nkPbT times\nkPbM metro 20\n";
        int probeFft = 2048;
        while (probeFft < 2 * std::max(1, p.cs.ksmps)) probeFft *= 2;
        int n = 0;
        for (const std::string& name : probeAmp) {
            const std::string id = std::to_string(++n);
            o << "aPb" << id << " = gaCawT_" << name << "_1\n"
              << "kPbR" << id << " rms aPb" << id << "\n"
              << "fPb" << id << " pvsanal aPb" << id << ", " << probeFft << ", " << probeFft / 2 << ", " << probeFft << ", 1\n"
              << "kPbC" << id << " pvscent fPb" << id << "\n"
              << "if kPbM == 1 then\nfprintks \"" << file << "\", \"A " << name << " %f %f %f\\n\", kPbT, kPbR" << id << ", kPbC" << id << "\nendif\n";
        }
        for (const auto& stream : probeData) {
            const std::string& name = stream.clip;
            const int sig = stream.sig;
            const std::string id = std::to_string(++n);
            const std::string encoded = scoreNameHex(stream.stream);
            const std::string chan = channelName(name, stream.stream.empty() ? "score" : "cawscore_" + encoded);
            const std::string prefix = stream.stream.empty() ? "" : encoded + " ";
            if (sig == 3) o << "SPb" << id << " chnget \"" << chan << "\"\nif kPbM == 1 then\nfprintks \"" << file << "\", \"" << (stream.stream.empty() ? "S " : "T ") << name << " " << prefix << "%f %s\\n\", kPbT, SPb" << id << "\nendif\n";
            else if (sig == 2 && !stream.stream.empty()) {
                // Capture the full audio-rate extrema in each 50 ms interval.
                // Plot as a min/max envelope, never as a 20 Hz audio waveform.
                o << "aPd" << id << " chnget \"" << chan << "\"\n"
                  << "kPdLo" << id << " maxk aPd" << id << ", kPbM, 3\n"
                  << "kPdHi" << id << " maxk aPd" << id << ", kPbM, 2\n"
                  << "if kPbM == 1 && kPbT >= 0.05 then\nfprintks \"" << file << "\", \"W " << name << " " << prefix
                  << "%f %.9g %.9g\\n\", kPbT, kPdLo" << id << ", kPdHi" << id << "\nendif\n";
                // Capture actual FFT data from this specific audio channel.
                // pvs magnitudes are combined into logarithmic bands and
                // stored for relative, time-averaged score visualization.
                const int fftSize = probeFft;
                const double fHi = std::min(18000.0, 0.5 * std::max(1, p.cs.sr));
                o << "fSp" << id << " pvsanal aPd" << id << ", " << fftSize << ", " << fftSize / 2 << ", " << fftSize << ", 1\n"
                  << "iSpTab" << id << " ftgen 0, 0, -" << (fftSize / 2 + 1) << ", 2, 0\n"
                  << "kSpNew" << id << " pvsftw fSp" << id << ", iSpTab" << id << "\n"
                  << "kSpBands" << id << "[] init " << kScoreSpectrumBins << "\n"
                  << "if kPbM == 1 && kPbT >= " << fftSize << " / sr then\n"
                  << "kSpJ" << id << " = 0\nwhile kSpJ" << id << " < " << kScoreSpectrumBins << " do\n"
                  << "kSpF0" << id << " = 50 * exp(log(" << (fHi / 50.0) << ") * kSpJ" << id << " / " << kScoreSpectrumBins << ")\n"
                  << "kSpF1" << id << " = 50 * exp(log(" << (fHi / 50.0) << ") * (kSpJ" << id << " + 1) / " << kScoreSpectrumBins << ")\n"
                  << "kSpLo" << id << " = min(" << fftSize / 2 << ", max(1, int(kSpF0" << id << " * " << fftSize << " / sr)))\n"
                  << "kSpHi" << id << " = min(" << fftSize / 2 << ", max(kSpLo" << id << ", int(kSpF1" << id << " * " << fftSize << " / sr)))\n"
                  << "kSpSum" << id << " = 0\nkSpK" << id << " = kSpLo" << id << "\n"
                  << "while kSpK" << id << " <= kSpHi" << id << " do\n"
                  << "kSpV" << id << " table kSpK" << id << ", iSpTab" << id << "\n"
                  << "kSpSum" << id << " += kSpV" << id << " * kSpV" << id << "\n"
                  << "kSpK" << id << " += 1\nod\n"
                  << "kSpBands" << id << "[kSpJ" << id << "] = " << (g_hasSqrt ? "sqrt(kSpSum" + id + ")"
                                    : g_hasPow ? "pow(kSpSum" + id + ", 0.5)" : "kSpSum" + id) << "\n"   // genuine square root (band magnitude): sqrt, else pow(x, 0.5), else the raw sum
                  << "kSpJ" << id << " += 1\nod\n";
                for (int start = 0; start < kScoreSpectrumBins; start += 16) {
                    o << "fprintks \"" << file << "\", \"F " << name << " " << prefix << "%f " << start;
                    for (int k = 0; k < 16; ++k) o << " %.9g";
                    o << "\\n\", kPbT";
                    for (int k = 0; k < 16; ++k) o << ", kSpBands" << id << "[" << (start + k) << "]";
                    o << "\n";
                }
                o << "endif\n";
            }
            else {
                if (sig == 2) o << "aPd" << id << " chnget \"" << chan << "\"\nkPd" << id << " downsamp aPd" << id << "\n";
                else o << "kPd" << id << " chnget \"" << chan << "\"\n";
                o << "if kPbM == 1 then\nfprintks \"" << file << "\", \"" << (stream.stream.empty() ? "D " : sig == 4 ? "I " : "N ") << name << " " << prefix
                  << (stream.stream.empty() ? "%f %f" : "%f %.9g") << "\\n\", kPbT, kPd" << id << "\nendif\n";
            }
        }
        for (const std::string& name : busDeclared) {   // the buses are summed anew in every control period
            int ch = 0;
            for (const Track& t : p.tracks) if(t.kind != 5 && t.kind != 6 && t.kind != 7) for (const Clip& c : t.clips) if (c.name == name) ch = std::max(ch, parseClip(c, true).outChannels);
            for (int c = 1; c <= ch; ++c) o << "gaCawT_" << name << "_" << c << " = 0\n";
        }
        o << "endin\n\n";
        score << "i \"CawScoreProbe\" 0 " << (endT + std::max(0.0, p.cs.tail)) << "\n";
    }
    if (any && p.cs.tail > 0.0) {
        o << "instr CawTail\nendin\n\n";
        score << "i \"CawTail\" " << endT << " " << p.cs.tail << "\n";
    }
    // Run = CsoundQt-style: Csound stays alive (f0) until it is stopped
    if (engine) score << "f 0 86400\n";
    else score << "e\n";
    o << "</CsInstruments>\n<CsScore>\n" << includeDirectives(p, true) << score.str() << "</CsScore>\n</CsoundSynthesizer>\n";
    return orchestraBuffer.str();
}


std::vector<std::string> recClipNames(const Project& p) {
    std::vector<std::string> out;
    for (const Track& t : p.tracks) if (t.kind != 5 && t.kind != 6 && t.kind != 7)
        for (const Clip& c : t.clips)
            if (parseClip(c).hasRec && std::find(out.begin(), out.end(), c.name) == out.end()) out.push_back(c.name);
    return out;
}

std::vector<CawWriteTarget> cawWriteTargets(const Project& p) {
    std::vector<CawWriteTarget> out;
    std::set<std::pair<std::string, std::string>> seen;
    for (const Track& t : p.tracks) if (t.kind != 5 && t.kind != 6 && t.kind != 7) for (const Clip& c : t.clips)
        for (const std::string& channel : parseClip(c).writeChannels)
            if (seen.insert({c.name, channel}).second) out.push_back({c.name, channel});
    return out;
}

// A CSD with only this clip's instrument, played once, no audio output (-n): a quick syntax / init check.
std::string buildClipTestCsd(const Project& p, const Clip& clip, OrcSpan& span) {
    const Track* owner = nullptr;
    for (const Track& t : p.tracks) for (const Clip& c : t.clips) if (c.id == clip.id) owner = &t;
    const ParsedTrack pc = owner ? parseClipForTrack(clip, *owner, false) : parseClip(clip, false);
    std::ostringstream o;
    o << "<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n<CsInstruments>\n" << buildHeader(p.cs, std::max(masterChannels(p.cs), pc.outChannels)) << "\n";
    o << automationOpcodes();
    if (!p.orchestraCode.empty()) o << resolveCawFiles(p, p.orchestraCode) << "\n\n";
    o << includeDirectives(p, false);
    if (!p.includes.empty()) o << "\n";
    if (!p.udoCode.empty()) o << resolveCawFiles(p, p.udoCode) << "\n\n";
    for (const std::string& instr : p.instrPool) o << resolveCawFiles(p, instr) << "\n\n";
    for (const CawParam& c : pc.params) {
        const std::string fullName = channelName(clip.name, c.chn);
        o << "chnk \"" << fullName << "\", 3\n" << "chnset " << c.value << ", \"" << fullName << "\"\n";
    }
    o << "\ninstr " << clip.name << "\n";
    o << "iCawP6 = p6\n";   // consume a possible extra transport p-field (same warning source)
    const std::string prefix = o.str();
    span.name = clip.name;
    span.first = (int)std::count(prefix.begin(), prefix.end(), '\n') + 1;   // line of the first body line in the .csd file
    span.lineMap = pc.lineMap;
    const double dur = std::min(5.0, std::max(0.1, clip.dur));
    std::ostringstream tail;
    tail << resolveCawFiles(p, pc.body) << "\nendin\n</CsInstruments>\n<CsScore>\n" << includeDirectives(p, true) << "i \"" << clip.name << "\" 0 " << dur;
    if (pc.midiEnabled) tail << " 60 100";
    tail << "\ne\n</CsScore>\n</CsoundSynthesizer>\n";
    return prefix + tail.str();
}

// Live code editing: the same generator that writes the Run CSD produces the full text, and the
// clip's own `instr <name> ... endin` block is cut out of it. Going through buildCsd keeps the live
// text identical to what Run would compile: VST code translation, VSTe input sends and the live
// output gates all come from the same place. The block is compiled into the RUNNING engine with
// csoundCompileOrc; channels, taps, UDOs and helpers already exist there, so they are not repeated.
std::string buildClipInstrumentCode(const Project& p, const Clip& clip) {
    if (clip.name.empty()) return std::string();
    std::string csd;
    try { csd = buildCsd(p, "engine"); } catch (...) { return std::string(); }
    const std::string head = "instr " + clip.name;
    std::istringstream in(csd);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    // The clip instruments are emitted after the pool code and before the engine helpers
    // (CawAuto / CawStop / CawPoll): the LAST match is the clip's own definition.
    int begin = -1;
    for (size_t i = 0; i < lines.size(); ++i) if (lines[i] == head) begin = (int)i;
    if (begin < 0) return std::string();
    int end = -1;
    for (size_t i = (size_t)begin + 1; i < lines.size(); ++i)
        if (lines[i] == "endin") { end = (int)i; break; }
    if (end < 0) return std::string();
    std::string out;
    for (int i = begin; i <= end; ++i) { out += lines[(size_t)i]; out += '\n'; }
    return out;
}
