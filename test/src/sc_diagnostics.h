#pragma once

#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace sc_diagnostics {

struct SourceLine {
    int line = 0;
    bool approximate = false;
};

inline int reportedLine(const std::string& text) {
    static const std::regex linePattern(
        R"re(\bline\s+([0-9]+)(?:\s+char\b|:))re",
        std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(text, match, linePattern)) return 0;
    try { return std::stoi(match[1].str()); }
    catch (...) { return 0; }
}

inline bool isIdentifierChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_';
}

inline bool isIdentifier(const std::string& text) {
    if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_')) return false;
    for (unsigned char c : text) if (!isIdentifierChar(c)) return false;
    return true;
}

inline std::vector<std::string> visibleSourceLines(const std::string& source) {
    std::vector<std::string> lines;
    std::string line;
    bool inBlockComment = false;
    bool inString = false;
    bool escaped = false;
    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
            escaped = false;
            continue;
        }
        if (inBlockComment) {
            if (c == '*' && i + 1 < source.size() && source[i + 1] == '/') {
                line += "  ";
                ++i;
                inBlockComment = false;
            } else line += ' ';
            continue;
        }
        if (inString) {
            line += ' ';
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') break;
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            line += "  ";
            ++i;
            inBlockComment = true;
            continue;
        }
        if (c == '"') {
            line += ' ';
            inString = true;
            continue;
        }
        line += c;
    }
    if (!line.empty() || (!source.empty() && source.back() == '\n')) lines.push_back(line);
    return lines;
}

inline bool hasIdentifier(const std::string& line, const std::string& name) {
    if (!isIdentifier(name)) return false;
    size_t pos = 0;
    while ((pos = line.find(name, pos)) != std::string::npos) {
        const bool left = pos == 0 || !isIdentifierChar(static_cast<unsigned char>(line[pos - 1]));
        const size_t end = pos + name.size();
        const bool right = end == line.size() || !isIdentifierChar(static_cast<unsigned char>(line[end]));
        size_t before = pos;
        while (before > 0 && std::isspace(static_cast<unsigned char>(line[before - 1]))) --before;
        size_t after = end;
        while (after < line.size() && std::isspace(static_cast<unsigned char>(line[after]))) ++after;
        const bool sigilOrMethod = before > 0 &&
            (line[before - 1] == '.' || line[before - 1] == '~' || line[before - 1] == '\\');
        const bool keyword = after < line.size() && line[after] == ':';
        if (left && right && !sigilOrMethod && !keyword) return true;
        pos = end;
    }
    return false;
}

inline bool hasMethodSend(const std::string& line, const std::string& selector) {
    if (!isIdentifier(selector)) return false;
    size_t pos = 0;
    while ((pos = line.find(selector, pos)) != std::string::npos) {
        const size_t end = pos + selector.size();
        if (end < line.size() && isIdentifierChar(static_cast<unsigned char>(line[end]))) {
            pos = end;
            continue;
        }
        size_t dot = pos;
        while (dot > 0 && std::isspace(static_cast<unsigned char>(line[dot - 1]))) --dot;
        if (dot > 0 && line[dot - 1] == '.') return true;
        pos = end;
    }
    return false;
}

// SuperCollider does not retain source-line metadata for most runtime errors.
// Return a location only when the error's missing name has one unambiguous source line.
inline SourceLine inferRuntimeLine(const std::string& source, const std::string& message) {
    static const std::regex missingMethod(
        R"re(Message\s+'([^']+)'\s+not understood)re", std::regex_constants::icase);
    static const std::regex missingVariable(
        R"re(Variable\s+'([A-Za-z_][A-Za-z0-9_]*)'\s+not defined)re", std::regex_constants::icase);
    std::smatch match;
    const bool methodError = std::regex_search(message, match, missingMethod);
    if (!methodError && !std::regex_search(message, match, missingVariable)) return {};
    const std::string name = match[1].str();
    const std::vector<std::string> lines = visibleSourceLines(source);
    int candidate = 0;
    int matches = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const bool found = methodError ? hasMethodSend(lines[i], name) : hasIdentifier(lines[i], name);
        if (found) {
            candidate = static_cast<int>(i + 1);
            ++matches;
            if (matches > 1) return {};
        }
    }
    return matches == 1 ? SourceLine{candidate, true} : SourceLine{};
}

} // namespace sc_diagnostics
