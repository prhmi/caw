#include <filesystem>
#include "csd_builder.h"
#include "osc_sender.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

std::string channelName(const std::string& instr, const std::string& chn) {
    return instr + "_" + chn;
}

std::string scoreProbePath() {
    std::string s = (workDir() / "caw_score_data.txt").string();
    for (char& ch : s) if (ch == '\\') ch = '/';
    return s;
}

namespace {
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

// "dawrun" / self-scheduling.  In a clip's code you can write
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
    static const std::regex selfRe(R"re(^(\s*)(?:schedule|dawrun|cawRun)\s+"this"\s*,\s*(.*)$)re");
    static const std::regex dawRe(R"re(^(\s*)(?:dawrun|cawRun)\s+(.*)$)re");
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
            CawParam p;
            p.chn  = (*it)[1];
            p.min  = std::stod((*it)[2]);
            p.max  = std::stod((*it)[3]);
            p.step = std::stod((*it)[4]);
            p.value = (*it)[5].matched ? std::stod((*it)[5]) : (p.min + p.max) * 0.5;
            out.params.push_back(p);
        }
    }

    // 2) translate:  kGet caw "input1"   ->   kGet chnget "Track_input1"
    static const std::regex use(R"re((\w+)\s+caw\s+"([^"]+)")re");
    std::string result;
    size_t last = 0;
    for (std::sregex_iterator it(code.begin(), code.end(), use), end; it != end; ++it) {
        result.append(code, last, it->position() - last);
        result += (*it)[1].str() + " chnget \"" + channelName(instrName, (*it)[2]) + "\"";
        last = it->position() + it->length();
    }
    result.append(code, last, std::string::npos);
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
            if (std::regex_match(line, m, recSend)) {
                out.hasRec = true;
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

std::string pdWavPath(const std::string& name) {
    std::string path = (workDir() / "caw_pd" / (name + ".wav")).string();
    std::replace(path.begin(), path.end(), '\\', '/');   // Csound strings: forward slashes
    return path;
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

bool clipHasMidiNotes(const Project& p, int clipId) {
    for (const Track& t : p.tracks)
        for (const MidiClip& mc : t.midiClips)
            if (mc.clipId == clipId) for (const MidiNote& n : mc.notes) if (n.start < mc.dur) return true;
    return false;
}

std::vector<MidiNote> clipMidiNotes(const Project& p, int clipId) {
    std::vector<MidiNote> all;
    for (const Track& t : p.tracks)
        for (const MidiClip& mc : t.midiClips) {
            if (mc.clipId != clipId) continue;
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

// The wav of a Pd / SuperCollider clip with  midi: "on"  starts where the clip starts on the timeline (the patch itself sounds from
// there) or at the first MIDI note if that is earlier. Everything inside the wav is relative to this origin, so the timeline decides the timing.
double midiWavOrigin(const Project& p, int clipId) {
    const Clip* c = clipById(p, clipId);
    double origin = c ? c->start : 0.0;
    for (const MidiNote& n : clipMidiNotes(p, clipId)) origin = std::min(origin, n.start);
    return std::max(0.0, origin);
}

double midiWavLength(const Project& p, int clipId) {
    const Clip* c = clipById(p, clipId);
    double end = c ? c->start + c->dur : 0.0;
    for (const MidiNote& n : clipMidiNotes(p, clipId)) end = std::max(end, n.start + n.dur);
    return std::max(0.1, end - midiWavOrigin(p, clipId)) + kMidiTail;
}


std::map<int, std::vector<double>> cawCopyValues(const std::string& clipCode, int copies) {
    std::map<int, std::vector<double>> out;
    const size_t a = clipCode.find("<caw>"), b = clipCode.find("</caw>");
    if (a == std::string::npos || b == std::string::npos || b < a) return out;
    const std::string block = clipCode.substr(a + 5, b - (a + 5));
    const int n = std::min(256, std::max(1, copies));
    static const std::regex pLine(R"re(\{\s*p\s*:\s*(\d+)\s*,([^}]*)\})re");
    static const std::regex attr(R"re((\w+)\s*:\s*([^,}]+))re");
    static const std::regex opRe(R"re((?:^|,)\s*([-+*/])\s*([^,}]+))re");
    auto num = [](std::string v, double& d) {
        const size_t f = v.find_first_not_of(" \t"), l = v.find_last_not_of(" \t");
        if (f == std::string::npos) return false;
        v = v.substr(f, l - f + 1);
        char* end = nullptr;
        d = std::strtod(v.c_str(), &end);
        return end && *end == '\0';
    };
    for (std::sregex_iterator it(block.begin(), block.end(), pLine), end; it != end; ++it) {
        const int pn = std::atoi((*it)[1].str().c_str());
        if (pn < 4 || pn > 60) continue;
        const std::string rest = (*it)[2].str();
        std::map<std::string, std::string> at;
        for (std::sregex_iterator ai(rest.begin(), rest.end(), attr), aend; ai != aend; ++ai) at[(*ai)[1].str()] = (*ai)[2].str();
        std::smatch om;
        double from = 0.0, to = 0.0, step = 0.0;
        const bool hasFrom = at.count("from") && num(at["from"], from);
        std::vector<double> v((size_t)n, 0.0);
        if (std::regex_search(rest, om, opRe)) {
            const std::string op = om[1].str();
            if (!num(om[2].str(), step)) continue;
            if (!hasFrom) from = (op == "*" || op == "/") ? 1.0 : 0.0;
            for (int i = 0; i < n; ++i)
                v[(size_t)i] = op == "+" ? from + i * step : op == "-" ? from - i * step : op == "*" ? from * std::pow(step, i) : from / std::pow(step, i);
        } else if (hasFrom && at.count("to") && num(at["to"], to)) {
            for (int i = 0; i < n; ++i) v[(size_t)i] = from + (to - from) * i / std::max(n - 1, 1);
        } else if (hasFrom) {
            for (int i = 0; i < n; ++i) v[(size_t)i] = from;
        } else continue;
        out[pn] = std::move(v);
    }
    return out;
}

int pdInletCount(const std::string& patch) {
    int depth = 0, count = 0;
    size_t pos = 0;
    while (pos < patch.size()) {
        size_t e = pos;
        while (e < patch.size() && !(patch[e] == ';' && (e == 0 || patch[e - 1] != '\\'))) ++e;
        std::istringstream words(patch.substr(pos, e - pos));
        std::vector<std::string> w;
        for (std::string t; words >> t;) w.push_back(t);
        pos = e + 1;
        if (w.size() >= 2 && w[0] == "#N" && w[1] == "canvas") ++depth;
        else if (w.size() >= 2 && w[0] == "#X" && w[1] == "restore") --depth;
        else if (depth == 1 && w.size() >= 5 && w[0] == "#X" && w[1] == "obj" && (w[4] == "inlet" || w[4] == "inlet~")) ++count;
    }
    return count;
}

std::string scParamDeclarations(const std::string& code, const std::map<int, std::vector<double>>& copyValues) {
    // comments out, so "// p4 = pitch" does not declare anything
    std::string clean;
    {
        std::istringstream in(code);
        for (std::string l; std::getline(in, l);) { const size_t c = l.find("//"); clean += (c == std::string::npos ? l : l.substr(0, c)) + "\n"; }
    }
    std::set<int> ks;
    for (const auto& kv : copyValues) ks.insert(kv.first);
    static const std::regex pRe(R"re(\bp(\d{1,2})\b)re");
    for (std::sregex_iterator it(clean.begin(), clean.end(), pRe), end; it != end; ++it) {
        const int k = std::atoi((*it)[1].str().c_str());
        if (k >= 4 && k <= 60) ks.insert(k);
    }
    // names the code declares itself:  var a = 1, b;   arg x, y = 2;
    std::set<std::string> own;
    {
        static const std::regex declRe(R"re(\b(?:var|arg)\b([^;]*);)re");
        for (std::sregex_iterator it(clean.begin(), clean.end(), declRe), end; it != end; ++it) {
            const std::string list = (*it)[1].str();
            int depth = 0;
            bool wantName = true;
            for (size_t i = 0; i < list.size(); ++i) {
                const char ch = list[i];
                if (ch == '(' || ch == '[' || ch == '{') ++depth;
                else if (ch == ')' || ch == ']' || ch == '}') --depth;
                else if (ch == ',' && depth == 0) wantName = true;
                else if (wantName && (std::isalpha((unsigned char)ch) || ch == '_')) {
                    size_t j = i;
                    while (j < list.size() && (std::isalnum((unsigned char)list[j]) || list[j] == '_')) ++j;
                    own.insert(list.substr(i, j - i));
                    wantName = false;
                    i = j - 1;
                }
            }
        }
    }
    auto declared = [&](const std::string& name) { return own.count(name) > 0; };
    std::string parts;
    auto addVar = [&](const std::string& name, const std::string& def) {
        if (declared(name)) return;
        parts += (parts.empty() ? "" : ", ") + name + " = " + def;
    };
    for (int k : ks) addVar("p" + std::to_string(k), "\\p" + std::to_string(k) + ".ir(0)");
    if (std::regex_search(clean, std::regex(R"re(\bindx\b)re"))) addVar("indx", "\\indx.ir(0)");
    if (std::regex_search(clean, std::regex(R"re(\biRun\b)re"))) addVar("iRun", "\\iRun.ir(1)");
    return parts.empty() ? std::string() : "var " + parts + ";\n";
}

ParsedTrack parseClip(const Clip& c, bool taps) {
    if (c.lang >= 1) {
        // Pure Data / SuperCollider clip: the app renders the patch to a wav (see the Pd render in the app); this instrument plays it.
        //   <caw> { chn: "cutoff", range: (0,1000,1) }                         automation lane -> [r cutoff] in the patch
        //   <caw> { send: "pitch", to: "Lead1", chn: "input1" }                [s pitch] in the patch -> channel input1 of instrument Lead1
        ParsedTrack out;
        const size_t a = c.code.find("<caw>"), b = c.code.find("</caw>");
        const std::string block = (a != std::string::npos && b != std::string::npos && b > a) ? c.code.substr(a + 5, b - (a + 5)) : std::string();
        static const std::regex chnRe(
            R"re(\{\s*chn:\s*"([^"]+)"\s*,\s*range:\s*\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)\s*(?:,\s*value:\s*([-+0-9.eE]+)\s*)?\})re");
        static const std::regex sendRe(R"re(\{\s*send:\s*"([^"]+)"\s*,\s*to:\s*"([^"]+)"\s*,\s*chn:\s*"([^"]+)"\s*\})re");
        for (std::sregex_iterator it(block.begin(), block.end(), chnRe), end; it != end; ++it) {
            CawParam p;
            p.chn = (*it)[1];
            p.min = std::stod((*it)[2]);
            p.max = std::stod((*it)[3]);
            p.step = std::stod((*it)[4]);
            p.value = (*it)[5].matched ? std::stod((*it)[5]) : (p.min + p.max) * 0.5;
            out.params.push_back(p);
        }
        // midi: "on"  -> the MIDI lane drives this clip (Pd: [r midinote] ..., SuperCollider: note, vel, gate controls)
        static const std::regex midiOnRe(R"re(midi\s*:\s*"on")re", std::regex_constants::icase);
        out.midiEnabled = std::regex_search(block, midiOnRe);
        for (std::sregex_iterator it(block.begin(), block.end(), sendRe), end; it != end; ++it)
            out.pdSends.push_back({(*it)[1].str(), (*it)[2].str(), (*it)[3].str()});
        // score data:  Pd  [s score]  is found by itself;  SuperCollider (and Pd if you like)  <caw> { send: "score" }  sends the control values
        // after the second output to the channel "score" of this clip.
        {
            static const std::regex scoreOnly(R"re(\{\s*send:\s*"score"\s*\})re");
            static const std::regex pdScore(R"re(\bs\s+score\b)re");
            bool want = std::regex_search(block, scoreOnly) || (c.lang == 1 && std::regex_search(c.code, pdScore));
            for (const PdSend& sd : out.pdSends) if (sd.name == "score") want = false;
            if (want) out.pdSends.push_back({"score", c.name, "score"});
            for (const PdSend& sd : out.pdSends) if (sd.name == "score" && sd.to == c.name && sd.chn == "score") out.scoreSig = 1;
        }
        // automation data:  Pd  [s rec]  is found by itself;  Pd and SuperCollider  <caw> { send: "rec" }  sends the control values (after the second
        // output, in the order of the sends) to the channel "rec" of this clip. Capture / Rec write them into an automation lane.
        {
            static const std::regex recOnly(R"re(\{\s*send:\s*"rec"\s*\})re");
            static const std::regex pdRec(R"re(\bs\s+rec\b)re");
            bool want = std::regex_search(block, recOnly) || (c.lang == 1 && std::regex_search(c.code, pdRec));
            for (const PdSend& sd : out.pdSends) if (sd.name == "rec") want = false;
            if (want) out.pdSends.push_back({"rec", c.name, "rec"});
            for (const PdSend& sd : out.pdSends) if (sd.name == "rec" && sd.to == c.name && sd.chn == "rec") out.hasRec = true;
        }
        // the wav holds L, R and one extra channel per send (control values as a signal, 32-bit float)
        std::string outs = "aPdL, aPdR";
        for (size_t k = 0; k < out.pdSends.size(); ++k) outs += ", aPdS" + std::to_string(k + 1);
        out.body = outs + " diskin2 \"" + pdWavPath(c.name) + "\", 1, p4\nouts aPdL, aPdR\n";   // p4 = seconds to skip (timeline started inside the clip)
        if (!out.pdSends.empty()) out.body += "iPdScale = ampdbfs(0)\n";   // Csound scales sound files by 0dbfs: send values are divided back
        for (size_t k = 0; k < out.pdSends.size(); ++k) {
            const std::string n = std::to_string(k + 1);
            out.body += "kPdS" + n + " downsamp aPdS" + n + "\nkPdS" + n + " = kPdS" + n + " / iPdScale\nchnset kPdS" + n + ", \"" + channelName(out.pdSends[k].to, out.pdSends[k].chn) + "\"\n";
        }
        out.lineMap.assign((size_t)std::count(out.body.begin(), out.body.end(), '\n'), 0);
        if (taps) out.outChannels = addMeterTaps(c.name, out.body, &out.lineMap);
        return out;
    }
    return parseCode(c.name, c.code, taps, c.runs);
}

std::vector<LaneMeterInfo> meterLayout(const Project& p) {
    std::vector<LaneMeterInfo> out;
    int next = masterChannels(p.cs);
    for (const Track& t : p.tracks) {
        int ch = 0;
        for (const Clip& c : t.clips) ch = std::max(ch, parseClip(c, true).outChannels);
        if (ch <= 0) continue;
        out.push_back({t.id, next, ch});
        next += ch;
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
    for (const Clip& c : t.clips) merge(parseClip(c));
    if (t.clips.empty()) merge(parseCode(t.name, t.code));
    return out;
}

std::string buildHeader(const CsoundSettings& s) {
    std::ostringstream o;
    o << "sr = " << s.sr << "\n"
      << "ksmps = " << s.ksmps << "\n"
      << "nchnls = " << s.nchnls << "\n";
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

std::string buildOptions(const CsoundSettings& s, const std::string& mode) {
    if (mode != "render") {
        // the typed options win; an empty box falls back to the generated line
        return s.options.empty() ? defaultOptions(s) : s.options;
    }
    static const char* fmt[] = {"-W -s", "-W -3", "-W -f"};
    return std::string(fmt[std::min(2, std::max(0, s.renderFormat))]) + " -d";
}

std::string buildCsd(const Project& p, const std::string& mode, double fromTime, std::vector<OrcSpan>* spans) {
    if (spans) spans->clear();
    std::ostringstream o;
    o << "<CsoundSynthesizer>\n<CsOptions>\n" << buildOptions(p.cs, mode) << "\n</CsOptions>\n"
      << "<CsInstruments>\n" << buildHeader(p.cs) << "\n";

    std::ostringstream score;
    double endT = 0.0;
    bool any = false;
    bool anySolo = false;
    for (const Track& t : p.tracks) if (t.solo && !t.muted) anySolo = true;
    int autoId = 0;
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

    const bool engine = mode == "engine";
    const bool probe = mode == "render" && p.score.analyze;           // Render audio also measures the clips for the score
    const bool tap = (engine && p.cs.monitorFeed) || probe;           // meter buses (gaCawT_*) are needed by the monitor and by the probe
    std::vector<std::string> probeAmp;                                // clips measured (amplitude + frequency)
    std::vector<std::pair<std::string, int>> probeData;              // clips that send to the channel "score" (name, 1 number / 2 signal / 3 string)
    std::set<std::string> busDeclared;
    if (tap) {   // meter buses must exist before the instruments that write to them
        for (const Track& t : p.tracks)
            for (const Clip& c : t.clips) {
                const int ch = parseClip(c, true).outChannels;
                if (!busDeclared.insert(c.name).second) continue;
                for (int k = 1; k <= ch; ++k) o << "gaCawT_" << c.name << "_" << k << " init 0\n";
            }
    }

    std::set<int> pdMidiPlayed;   // Pure Data / SuperCollider clips driven by MIDI: one wav from 0 s, started once
    for (const Track& t : p.tracks) {
        const bool audible = !t.muted && (!anySolo || t.solo);

        // Every clip is its own instrument: own instr name, own code, own channels, own MIDI.
        for (const Clip& clip : t.clips) {
            const ParsedTrack pc = parseClip(clip, tap);
            if (emitted.insert(clip.name).second) {
                if (probe) {
                    if (pc.outChannels >= 1) probeAmp.push_back(clip.name);
                    if (pc.scoreSig) {
                        o << (pc.scoreSig == 2 ? "chn_a" : pc.scoreSig == 3 ? "chn_S" : "chn_k") << " \"" << channelName(clip.name, "score") << "\", 3\n";
                        probeData.push_back({clip.name, pc.scoreSig});
                    }
                }
                for (const CawParam& c : pc.params) {
                    const std::string fullName = channelName(clip.name, c.chn);
                    o << "chn_k \"" << fullName << "\", 3\n";
                    auto it = t.automation.find(c.chn);
                    const double initial = it == t.automation.end() ? c.value : automationValueAt(it->second, fromTime, c.value);
                    o << "chnset " << initial << ", \"" << fullName << "\"\n";
                }
                if (pc.hasRec) o << "chn_k \"" << channelName(clip.name, "rec") << "\", 3\n";
                o << "\ninstr " << clip.name << "\n";
                if (spans) {
                    const std::string so = o.str();
                    OrcSpan sp;
                    sp.name = clip.name;
                    sp.first = (int)std::count(so.begin(), so.end(), '\n') + 1;   // Csound counts the lines of the whole .csd file (options and header lines included)
                    sp.lineMap = pc.lineMap;
                    spans->push_back(sp);
                }
                o << pc.body << "\nendin\n\n";
            }
            if (!audible || clip.muted || mode == "engine") continue;

            // Live mode starts every instrument indefinitely so user code can schedule and communicate across instruments.
            if (mode == "live") {
                score << "i \"" << clip.name << "\" 0 -1\n";
                endT = std::max(endT, 1.0);
                any = true;
                continue;
            }

            if (pc.midiEnabled && clip.lang >= 1) {
                // Pd / SuperCollider with midi: "on": ONE wav (the patch itself + its MIDI notes) from its origin on the timeline
                if (pdMidiPlayed.insert(clip.id).second) {
                    const double org = midiWavOrigin(p, clip.id), endAbs = org + midiWavLength(p, clip.id);
                    const double st = std::max(0.0, org - fromTime), du = endAbs - std::max(org, fromTime);
                    if (du > 0.0) {
                        score << "i \"" << clip.name << "\" " << st << " " << du << " " << std::max(0.0, fromTime - org) << "\n";
                        endT = std::max(endT, st + du);
                        any = true;
                    }
                }
            } else {   // a Csound clip always plays from its box; with midi: "on" its MIDI clips add their notes on top (below)
                // "runs" = how many times the instr is triggered; the clip is split into equal slots
                const int runs = (pc.launcher || clip.lang >= 1) ? 1 : std::min(256, std::max(1, clip.runs));   // a Pd clip is one wav: never split
                for (int r = 0; r < runs; ++r) {
                    const double slotStart = clip.start + clip.dur * (double)r / runs;
                    const double slotEnd = clip.start + clip.dur * (double)(r + 1) / runs;
                    const double en = slotEnd - fromTime;
                    if (en <= 0.0) continue;
                    const double st = std::max(0.0, slotStart - fromTime);
                    const double du = en - st;
                    if (du <= 0.0) continue;
                    score << "i \"" << clip.name << "\" " << st << " " << du;
                    if (clip.lang >= 1) score << " " << std::max(0.0, fromTime - clip.start);   // wav offset when playing from the middle of the clip
                    else if (pc.midiEnabled) score << " 60 100";
                    score << "\n";
                    endT = std::max(endT, st + du);
                    any = true;
                }
            }
        }
        if (!audible || mode == "engine") continue;

        // MIDI clips are free in time: they only borrow the instrument (name) of their Csound clip.
        for (const MidiClip& mc : t.midiClips) {
            const Clip* target = nullptr;
            for (const Track& other : p.tracks)
                for (const Clip& c : other.clips) if (c.id == mc.clipId) target = &c;
            if (!target || target->muted) continue;
            if (target->lang >= 1) continue;   // Pure Data / SuperCollider: the notes are inside the wav that the clip loop above starts
            std::vector<MidiNote> notes = mc.notes;
            std::stable_sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
            const double mcEnd = mc.start + mc.dur;
            for (const MidiNote& note : notes) {
                if (note.start >= mc.dur) continue;
                const double absStart = mc.start + note.start;
                const double absEnd = std::min(mcEnd, absStart + note.dur);
                const double en = absEnd - fromTime;
                if (en <= 0.0) continue;
                const double st = std::max(0.0, absStart - fromTime);
                const double dur = en - st;
                if (dur <= 0.0) continue;
                score << "i \"" << target->name << "\" " << st << " " << dur << " "
                      << std::min(127, std::max(0, note.pitch)) << " "
                      << std::min(127, std::max(1, note.velocity)) << "\n";
                endT = std::max(endT, st + dur);
                any = true;
            }
        }

        // A lane's automation drives the matching channel of every clip on that lane.
        for (const auto& [channel, rawPoints] : t.automation) {
            if (rawPoints.empty()) continue;
            std::vector<std::string> targets;
            for (const Clip& clip : t.clips) {
                const ParsedTrack pc = parseClip(clip);
                for (const CawParam& c : pc.params)
                    if (c.chn == channel) {
                        const std::string target = channelName(clip.name, channel);
                        if (std::find(targets.begin(), targets.end(), target) == targets.end()) targets.push_back(target);
                        break;
                    }
            }
            if (targets.empty()) continue;
            std::vector<AutoPoint> points = rawPoints;
            std::stable_sort(points.begin(), points.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
            const std::string instrument = "CawAutomation" + std::to_string(autoId++);
            o << "instr " << instrument << "\n"
              << "kAuto line p4, p3, p5\n";
            for (const std::string& target : targets) o << "chnset kAuto, \"" << target << "\"\n";
            o << "endin\n\n";

            if (points.size() == 1) {
                const double st = std::max(0.0, points.front().t - fromTime);
                if (points.front().t + 0.01 >= fromTime) {
                    score << "i \"" << instrument << "\" " << st << " 0.01 " << points.front().v << " " << points.front().v << "\n";
                    endT = std::max(endT, st + 0.01);
                    any = true;
                }
            } else {
                for (size_t i = 0; i + 1 < points.size(); ++i) {
                    const AutoPoint& a = points[i];
                    const AutoPoint& b = points[i + 1];
                    if (b.t <= fromTime || b.t <= a.t) continue;
                    const double segmentStart = std::max(a.t, fromTime);
                    const double span = b.t - a.t;
                    const double startValue = segmentStart <= a.t ? a.v : a.v + (b.v - a.v) * ((segmentStart - a.t) / span);
                    const double st = std::max(0.0, segmentStart - fromTime);
                    const double dur = b.t - segmentStart;
                    if (dur <= 0.0) continue;
                    score << "i \"" << instrument << "\" " << st << " " << dur << " " << startValue << " " << b.v << "\n";
                    endT = std::max(endT, st + dur);
                    any = true;
                }
            }
        }
    }
    if (mode == "engine") {
        // generic automation instrument: p4 = start value, p5 = end value, p6 = channel name
        o << "instr CawAuto\nSch strcpy p6\nkA line p4, p3, p5\nchnset kA, Sch\nendin\n\n";
        // The timeline (Play button) sends OSC messages to this always-on listener; each one becomes a score event.
        o << "giCawOsc OSCinit " << kCawOscPort << "\n"
          << "instr CawOsc\n"
          << "Sname init \"\"\n"
          << "Sline init \"\"\n"
          << "kDel init 0\nkDur init 0\nkP4 init 0\nkP5 init 0\nkP6 init 0\nkEvN init 0\n"
          << "nxtI:\n"
          << "kGotI OSClisten giCawOsc, \"/caw/i\", \"sfffff\", Sname, kDel, kDur, kP4, kP5, kP6\n"
          << "if kGotI == 0 kgoto nxtA\n"
          << "Sline sprintfk \"i \\\"%s\\\" %f %f %f %f\", Sname, kDel, kDur, kP4, kP5\n"
          << "scoreline Sline, 1\n"
          << "kEvN += 1\n"
          << "puts Sline, kEvN\n"                // diagnostics: the console shows every timeline event the engine received
          << "kgoto nxtI\n"
          << "nxtA:\n"
          << "kGotA OSClisten giCawOsc, \"/caw/a\", \"sffff\", Sname, kDel, kDur, kP4, kP5\n"
          << "if kGotA == 0 kgoto nxtO\n"
          << "Sline sprintfk \"i \\\"CawAuto\\\" %f %f %f %f \\\"%s\\\"\", kDel, kDur, kP4, kP5, Sname\n"
          << "scoreline Sline, 1\n"
          << "kgoto nxtA\n"
          << "nxtO:\n"
          << "kGotO OSClisten giCawOsc, \"/caw/off\", \"s\", Sname\n"
          << "if kGotO == 0 kgoto done\n"
          << "Sline sprintfk \"i \\\"-%s\\\" 0 0\", Sname\n"
          << "scoreline Sline, 1\n"
          << "turnoff2 Sname, 0, 0\n"            // fixed-length notes (like a Pd clip playing its wav) ignore the negative i-statement: stop them for real
          << "kgoto nxtO\n"
          << "done:\n"
          << "kEnd = 0\n"
          << "endin\n\n";
        score << "i \"CawOsc\" 0 86400\n";
        if (p.cs.monitorFeed) {
            // ---- monitor: runs after every other instrument and prints meter / scope / spectrum frames (20 per second)
            const std::vector<LaneMeterInfo> layout = meterLayout(p);
            std::ostringstream mon;
            const int mch = masterChannels(p.cs);
            // every output channel of the master (nchnls) gets its own peak meter
            if (p.cs.nchnls <= 1) {
                mon << "instr CawMon\naM1 monitor\naL = aM1\naR = aM1\n";
            } else {
                std::string list;
                for (int c = 1; c <= p.cs.nchnls; ++c) list += std::string(c > 1 ? ", " : "") + "aM" + std::to_string(c);
                mon << "instr CawMon\n" << list << " monitor\naL = aM1\naR = aM2\n";
            }
            mon << "kTrig metro 20\nchn_k \"cawScopeDiv\", 3\n";   // the app sets cawScopeDiv: control periods per scope point (= the time window)
            std::vector<std::string> peaks;
            for (int c = 1; c <= mch; ++c) {
                const std::string id = std::to_string(c);
                mon << "aAbsM" << id << " = abs(aM" << id << ")\n"
                    << "kPkM" << id << " max_k aAbsM" << id << ", kTrig, 1\n";
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
                        for (const Clip& clip : t.clips) {
                            if (!seen.insert(clip.name).second) continue;
                            if (parseClip(clip, true).outChannels < c) continue;
                            sum += (sum.empty() ? "" : " + ") + std::string("gaCawT_") + clip.name + "_" + std::to_string(c);
                        }
                        const std::string id = std::to_string(lm.first + c - 1);
                        mon << "aLn" << id << " = abs(" << (sum.empty() ? "0" : sum) << ")\n"
                            << "kLn" << id << " max_k aLn" << id << ", kTrig, 1\n";
                        peaks.push_back("kLn" + id);
                    }
                }
            }
            for (const std::string& name : busDeclared) {
                int ch = 0;
                for (const Track& t : p.tracks) for (const Clip& c : t.clips) if (c.name == name) ch = std::max(ch, parseClip(c, true).outChannels);
                for (int c = 1; c <= ch; ++c) mon << "gaCawT_" << name << "_" << c << " = 0\n";
            }
            // scope ring buffers (one point per control period)
            // scope ring buffers: one point per kDiv control periods, the largest value of those periods (so a long window still shows peaks)
            mon << "kDiv chnget \"cawScopeDiv\"\nif kDiv < 1 then\nkDiv = 1\nendif\n"
                << "kScL[] init " << kScopePoints << "\nkScR[] init " << kScopePoints << "\nkW init 0\nkScN init 0\nkAccL init 0\nkAccR init 0\n"
                << "kSmpL downsamp aL\nkSmpR downsamp aR\n"
                << "if abs(kSmpL) > abs(kAccL) then\nkAccL = kSmpL\nendif\n"
                << "if abs(kSmpR) > abs(kAccR) then\nkAccR = kSmpR\nendif\n"
                << "kScN = kScN + 1\nif kScN >= kDiv then\nkScL[kW] = kAccL\nkScR[kW] = kAccR\nkAccL = 0\nkAccR = 0\nkScN = 0\n"
                << "kW = kW + 1\nif kW >= " << kScopePoints << " then\nkW = 0\nendif\nendif\n";
            // spectrum bands
            mon << "aMono = (aL + aR) * 0.5\n";
            const double ratio = std::pow(18000.0 / 25.0, 1.0 / (kMeterBands - 1));
            for (int b = 0; b < kMeterBands; ++b) {
                const double fc = 25.0 * std::pow(ratio, b);
                if (fc * 1.2 > p.cs.sr * 0.45) { mon << "kBd" << b << " = 0\n"; continue; }   // above what this sample rate can hold
                mon << "aBd" << b << " butterbp aMono, " << fc << ", " << fc * (ratio - 1.0) * 1.5 << "\n"
                    << "kBd" << b << " rms aBd" << b << "\n";
            }
            // ---- send one frame to the app over UDP (OSC); a counter changes on every pulse, so each pulse sends exactly once
            mon << "kCnt init 0\nif kTrig == 1 then\nkCnt = kCnt + 1\nendif\n";
            auto sendList = [&](const char* address, int startIndex, const std::vector<std::string>& names) {
                for (size_t i = 0; i < names.size(); i += 16) {
                    const size_t n = std::min<size_t>(16, names.size() - i);
                    mon << "OSCsend kCnt, \"127.0.0.1\", " << kCawMonPort << ", \"" << address << "\", \"i" << std::string(n, 'f') << "\", "
                        << (startIndex + (int)i);
                    for (size_t k = 0; k < n; ++k) mon << ", " << names[i + k];
                    mon << "\n";
                }
            };
            sendList("/caw/m", 0, peaks);
            std::vector<std::string> bands, scopeL, scopeR;
            for (int b = 0; b < kMeterBands; ++b) bands.push_back("kBd" + std::to_string(b));
            sendList("/caw/s", 0, bands);
            mon << "if kTrig == 1 then\n";   // the scope points are collected only when a frame is sent (20 times a second), not in every control period
            for (int j = 0; j < kScopePoints; ++j) {
                mon << "kQ" << j << " = kW + " << j << "\nif kQ" << j << " >= " << kScopePoints << " then\nkQ" << j << " = kQ" << j << " - " << kScopePoints << "\nendif\n"
                    << "kPL" << j << " = kScL[kQ" << j << "]\nkPR" << j << " = kScR[kQ" << j << "]\n";
                scopeL.push_back("kPL" + std::to_string(j));
                scopeR.push_back("kPR" + std::to_string(j));
            }
            mon << "endif\n";
            sendList("/caw/l", 0, scopeL);
            sendList("/caw/r", 0, scopeR);
            {   // live Rec: the value each clip sends to its channel "rec", 50 times per second ("/caw/c": start index + values)
                const std::vector<std::string> recNames = recClipNames(p);
                if (!recNames.empty()) {
                    mon << "kRecTrig metro 50\nkRecCnt init 0\nif kRecTrig == 1 then\nkRecCnt = kRecCnt + 1\nendif\n";
                    std::vector<std::string> vars;
                    for (size_t k = 0; k < recNames.size(); ++k) {
                        mon << "kRecV" << k << " chnget \"" << channelName(recNames[k], "rec") << "\"\n";
                        vars.push_back("kRecV" + std::to_string(k));
                    }
                    for (size_t i = 0; i < vars.size(); i += 16) {
                        const size_t n = std::min<size_t>(16, vars.size() - i);
                        mon << "OSCsend kRecCnt, \"127.0.0.1\", " << kCawMonPort << ", \"/caw/c\", \"i" << std::string(n, 'f') << "\", " << (int)i;
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
        int n = 0;
        for (const std::string& name : probeAmp) {
            const std::string id = std::to_string(++n);
            o << "aPb" << id << " = gaCawT_" << name << "_1\n"
              << "kPbR" << id << " rms aPb" << id << "\n"
              << "fPb" << id << " pvsanal aPb" << id << ", 1024, 256, 1024, 1\n"
              << "kPbC" << id << " pvscent fPb" << id << "\n"
              << "if kPbM == 1 then\nfprintks \"" << file << "\", \"A " << name << " %f %f %f\\n\", kPbT, kPbR" << id << ", kPbC" << id << "\nendif\n";
        }
        for (const auto& [name, sig] : probeData) {
            const std::string id = std::to_string(++n);
            const std::string chan = channelName(name, "score");
            if (sig == 3) o << "SPb" << id << " chnget \"" << chan << "\"\nif kPbM == 1 then\nfprintks \"" << file << "\", \"S " << name << " %f %s\\n\", kPbT, SPb" << id << "\nendif\n";
            else {
                if (sig == 2) o << "aPd" << id << " chnget \"" << chan << "\"\nkPd" << id << " downsamp aPd" << id << "\n";
                else o << "kPd" << id << " chnget \"" << chan << "\"\n";
                o << "if kPbM == 1 then\nfprintks \"" << file << "\", \"D " << name << " %f %f\\n\", kPbT, kPd" << id << "\nendif\n";
            }
        }
        for (const std::string& name : busDeclared) {   // the buses are summed anew in every control period
            int ch = 0;
            for (const Track& t : p.tracks) for (const Clip& c : t.clips) if (c.name == name) ch = std::max(ch, parseClip(c, true).outChannels);
            for (int c = 1; c <= ch; ++c) o << "gaCawT_" << name << "_" << c << " = 0\n";
        }
        o << "endin\n\n";
        score << "i \"CawScoreProbe\" 0 " << (endT + std::max(0.0, p.cs.tail)) << "\n";
    }
    if (any && mode != "live" && p.cs.tail > 0.0) {
        o << "instr CawTail\nendin\n\n";
        score << "i \"CawTail\" " << endT << " " << p.cs.tail << "\n";
    }
    // "project" = CsoundQt-style run: whole project from 0:00, Csound stays alive (f0) until it is stopped.
    if (mode == "project" || mode == "engine") score << "f 0 86400\n";
    else score << "e\n";
    o << "</CsInstruments>\n<CsScore>\n" << score.str() << "</CsScore>\n</CsoundSynthesizer>\n";
    return o.str();
}


std::vector<std::string> recClipNames(const Project& p) {
    std::vector<std::string> out;
    for (const Track& t : p.tracks)
        for (const Clip& c : t.clips)
            if (parseClip(c).hasRec && std::find(out.begin(), out.end(), c.name) == out.end()) out.push_back(c.name);
    return out;
}

std::string buildClipCaptureCsd(const Project& p, const Clip& clip, const std::string& outFile, double dur) {
    const ParsedTrack pc = parseClip(clip, false);
    std::string file = outFile;
    for (char& ch : file) if (ch == '\\') ch = '/';
    std::ostringstream o;
    o << "<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n<CsInstruments>\n" << buildHeader(p.cs) << "\n";
    for (const CawParam& c : pc.params) {
        const std::string fullName = channelName(clip.name, c.chn);
        o << "chn_k \"" << fullName << "\", 3\n" << "chnset " << c.value << ", \"" << fullName << "\"\n";
    }
    const std::string rec = channelName(clip.name, "rec");
    o << "chn_k \"" << rec << "\", 3\n\ninstr " << clip.name << "\n" << pc.body << "\nendin\n\n"
      << "instr CawRec\nkRT times\nkRM metro 100\nkRV chnget \"" << rec << "\"\n"
      << "if kRM == 1 then\nfprintks \"" << file << "\", \"%f %f\\n\", kRT, kRV\nendif\nendin\n"
      << "</CsInstruments>\n<CsScore>\ni \"" << clip.name << "\" 0 " << dur;
    if (pc.midiEnabled && clip.lang == 0) o << " 60 100";
    o << "\ni \"CawRec\" 0 " << dur << "\ne\n</CsScore>\n</CsoundSynthesizer>\n";
    return o.str();
}

// A CSD with only this clip's instrument, played once, no audio output (-n): a quick syntax / init check.
std::string buildClipTestCsd(const Project& p, const Clip& clip, OrcSpan& span) {
    const ParsedTrack pc = parseClip(clip, false);
    std::ostringstream o;
    o << "<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n<CsInstruments>\n" << buildHeader(p.cs) << "\n";
    for (const CawParam& c : pc.params) {
        const std::string fullName = channelName(clip.name, c.chn);
        o << "chn_k \"" << fullName << "\", 3\n" << "chnset " << c.value << ", \"" << fullName << "\"\n";
    }
    o << "\ninstr " << clip.name << "\n";
    const std::string prefix = o.str();
    span.name = clip.name;
    span.first = (int)std::count(prefix.begin(), prefix.end(), '\n') + 1;   // line of the first body line in the .csd file
    span.lineMap = pc.lineMap;
    const double dur = std::min(5.0, std::max(0.1, clip.dur));
    std::ostringstream tail;
    tail << pc.body << "\nendin\n</CsInstruments>\n<CsScore>\ni \"" << clip.name << "\" 0 " << dur;
    if (pc.midiEnabled && clip.lang == 0) tail << " 60 100";
    else if (clip.lang >= 1) tail << " 0";   // a Pd / SC clip reads p4 (seconds to skip in the wav): without it Csound warns "uses 4 p-fields but is given 3"
    tail << "\ne\n</CsScore>\n</CsoundSynthesizer>\n";
    return prefix + tail.str();
}
