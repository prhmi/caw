#include "pd_text.h"

#include <cctype>
#include <map>
#include <sstream>
#include <vector>

namespace {

struct Statement {
    std::string text;       // without the closing ';'
    bool ended = true;
};

// A Pd file is a list of statements, each ends with ';' (an escaped "\;" inside a message does not end it).
std::vector<std::string> splitStatements(const std::string& patch) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < patch.size(); ++i) {
        const char c = patch[i];
        if (c == ';' && (i == 0 || patch[i - 1] != '\\')) {
            out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    return out;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Pd breaks long lines with a newline; in the readable text one element is one line.
std::string oneLine(const std::string& s) {
    std::string r;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') { space = true; continue; }
        if (space && !r.empty()) r += ' ';
        space = false;
        r += c;
    }
    return r;
}

// "#X obj 30 20 osc~ 440" -> kind "obj", rest "osc~ 440", and the part in front of the text ("#X obj 30 20 ")
struct Element {
    std::string kind;        // obj, msg, text, atom, restore
    std::string head;        // everything before the editable text
    std::string body;        // editable text
    int depth = 1;           // 1 = top level of the patch
    bool editable = false;
};

bool parseElement(const std::string& stmt, Element& e) {
    std::istringstream in(stmt);
    std::string hash, kind;
    if (!(in >> hash >> kind) || hash != "#X") return false;
    auto skipWord = [&](size_t& pos) {
        while (pos < stmt.size() && std::isspace((unsigned char)stmt[pos])) ++pos;
        while (pos < stmt.size() && !std::isspace((unsigned char)stmt[pos])) ++pos;
    };
    auto skipSpace = [&](size_t& pos) { while (pos < stmt.size() && std::isspace((unsigned char)stmt[pos])) ++pos; };
    size_t pos = 0;
    skipWord(pos); skipWord(pos);                     // "#X" "obj"
    if (kind == "obj" || kind == "msg" || kind == "text") {
        skipWord(pos); skipWord(pos);                 // x y
        skipSpace(pos);
        e.kind = kind;
        e.editable = true;
    } else if (kind == "restore") {
        skipWord(pos); skipWord(pos);                 // x y
        skipSpace(pos);
        e.kind = "obj";                               // "pd name" is the subpatch box
        e.editable = true;
    } else if (kind == "floatatom" || kind == "symbolatom" || kind == "array") {
        e.kind = kind == "array" ? "array" : "atom";
        e.head = stmt;
        return true;
    } else return false;
    e.head = stmt.substr(0, pos);
    e.body = stmt.substr(pos);
    return true;
}

}  // namespace

std::string pdToReadable(const std::string& patch) {
    std::ostringstream out;
    std::ostringstream links;
    const std::vector<std::string> statements = splitStatements(patch);
    std::vector<int> counter(1, 0);              // elements counted per open canvas (the connect numbers)
    int depth = 0;
    for (const std::string& raw : statements) {
        const std::string stmt = trimmed(raw);
        if (stmt.rfind("#N canvas", 0) == 0) { ++depth; counter.push_back(0); continue; }
        if (stmt.rfind("#X connect", 0) == 0) {
            std::istringstream in(stmt);
            std::string a, b;
            int from, outlet, to, inlet;
            if ((in >> a >> b >> from >> outlet >> to >> inlet) && depth == 1)
                links << (from + 1) << (outlet ? "." + std::to_string(outlet) : std::string()) << " -> " << (to + 1) << (inlet ? "." + std::to_string(inlet) : std::string()) << "\n";
            continue;
        }
        Element e;
        if (!parseElement(stmt, e)) continue;
        const bool isRestore = stmt.rfind("#X restore", 0) == 0;
        if (isRestore) {
            --depth;
            counter.pop_back();
        }
        const int n = ++counter.back();
        if (depth == 1) {
            if (e.editable) out << n << ": " << e.kind << (e.kind.size() < 4 ? "  " : " ") << oneLine(e.body) << "\n";
            else out << n << ": " << e.kind << "\n";
        } else if (e.editable) {
            out << "    (inside a subpatch) " << e.kind << " " << oneLine(e.body) << "\n";
        }
    }
    std::string result = "# Objects and messages of the patch. Change the text after the kind; build and connect in Pure Data (Edit in Pd).\n";
    result += out.str();
    const std::string l = links.str();
    if (!l.empty()) result += "# connections (from[.outlet] -> to[.inlet])\n" + l;
    return result;
}

std::string pdApplyReadable(const std::string& patch, const std::string& readable) {
    // the new texts by element number
    std::map<int, std::pair<std::string, std::string>> edits;    // number -> (kind, text)
    std::istringstream in(readable);
    std::string line;
    while (std::getline(in, line)) {
        size_t i = 0;
        while (i < line.size() && std::isdigit((unsigned char)line[i])) ++i;
        if (i == 0 || i >= line.size() || line[i] != ':') continue;
        const int number = std::atoi(line.substr(0, i).c_str());
        std::istringstream rest(line.substr(i + 1));
        std::string kind;
        if (!(rest >> kind)) continue;
        std::string text;
        std::getline(rest, text);
        edits[number] = {kind, trimmed(text)};
    }

    std::vector<std::string> statements = splitStatements(patch);
    std::vector<int> counter(1, 0);
    int depth = 0;
    std::string out;
    for (size_t s = 0; s < statements.size(); ++s) {
        std::string stmt = statements[s];
        const std::string t = trimmed(stmt);
        if (t.rfind("#N canvas", 0) == 0) { ++depth; counter.push_back(0); out += stmt + ";"; continue; }
        Element e;
        if (t.rfind("#X connect", 0) != 0 && parseElement(t, e)) {
            const bool isRestore = t.rfind("#X restore", 0) == 0;
            if (isRestore) { --depth; counter.pop_back(); }
            const int n = ++counter.back();
            const auto it = depth == 1 ? edits.find(n) : edits.end();
            if (it != edits.end() && e.editable && it->second.first == e.kind) {
                std::string text = it->second.second;
                if (oneLine(e.body) != text) {
                    const size_t lead = stmt.find_first_not_of("\n\r \t");
                    stmt = (lead == std::string::npos ? std::string() : stmt.substr(0, lead)) + e.head + text;
                }
            }
        }
        out += stmt + ";";
    }
    // anything after the last ';' (a trailing newline)
    const size_t lastSemi = patch.rfind(';');
    if (lastSemi != std::string::npos) out += patch.substr(lastSemi + 1);
    return out;
}

std::string pdEmptyPatch() { return "#N canvas 0 50 450 300 12;\n"; }
