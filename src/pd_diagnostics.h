#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace pd_diagnostics {

struct SourceLine {
    int line = 0;        // line number in CAW's readable PD editor
    bool approximate = false;
    std::vector<int> candidates; // possible rows when Pd's message cannot identify one object
};

inline bool tokenChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '~' || c == '/' || c == '.' || c == '-';
}

inline std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

inline bool hasToken(const std::string& sourceLine, const std::string& token) {
    if (token.empty()) return false;
    const std::string line = lower(sourceLine);
    const std::string needle = lower(token);
    std::size_t pos = 0;
    while ((pos = line.find(needle, pos)) != std::string::npos) {
        const bool left = pos == 0 || !tokenChar(static_cast<unsigned char>(line[pos - 1]));
        const std::size_t end = pos + needle.size();
        const bool right = end == line.size() || !tokenChar(static_cast<unsigned char>(line[end]));
        if (left && right) return true;
        pos = end;
    }
    return false;
}

inline void addMatches(const std::vector<std::string>& lines, const std::string& token,
                       int weight, std::map<int, int>& scores) {
    if (token.size() < 2) return;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (hasToken(lines[i], token)) scores[static_cast<int>(i + 1)] += weight;
}

inline std::string trimToken(std::string token) {
    while (!token.empty() && (token.front() == '\'' || token.front() == '"' ||
           token.front() == '(' || token.front() == '[' || token.front() == '<'))
        token.erase(token.begin());
    while (!token.empty() && (token.back() == '\'' || token.back() == '"' ||
           token.back() == ')' || token.back() == ']' || token.back() == '>' ||
           token.back() == ':' || token.back() == '.'))
        token.pop_back();
    return token;
}

inline std::string normalizeObjectText(std::string text) {
    if (text.rfind("verbose(", 0) == 0) {
        const std::size_t end = text.find("):");
        if (end != std::string::npos) text.erase(0, end + 2);
    }
    text = lower(std::move(text));
    std::string normalized;
    bool pendingSpace = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace) normalized.push_back(' ');
        pendingSpace = false;
        normalized.push_back(static_cast<char>(c));
    }
    while (!normalized.empty() && (normalized.back() == ';' || std::isspace(static_cast<unsigned char>(normalized.back()))))
        normalized.pop_back();
    return normalized;
}

// Runtime PD errors rarely include source coordinates. Match concrete object,
// selector, or symbol names from the error to the readable lines, and return
// a line only if one row is a clearly better fit than the others.
inline SourceLine inferEditorLine(const std::string& readableText, const std::string& message,
                                  const std::vector<int>& fallbackCandidates = {}) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < readableText.size()) {
        const std::size_t end = readableText.find('\n', begin);
        const std::size_t stop = end == std::string::npos ? readableText.size() : end;
        std::string line = readableText.substr(begin, stop - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        begin = end == std::string::npos ? readableText.size() : end + 1;
    }
    if (lines.empty()) return {};

    std::map<int, int> scores;
    std::smatch match;
    const std::string low = lower(message);
    static const std::regex objectTextPattern(R"pd(PD object text:\s*(.+))pd",
                                               std::regex_constants::icase);
    if (std::regex_search(message, match, objectTextPattern)) {
        const std::string objectText = normalizeObjectText(match[1].str());
        std::vector<int> exactRows;
        if (!objectText.empty()) {
            for (std::size_t i = 0; i < lines.size(); ++i)
                if (normalizeObjectText(lines[i]) == objectText)
                    exactRows.push_back(static_cast<int>(i + 1));
        }
        if (exactRows.size() == 1) return {exactRows.front(), false, {}};
        if (!exactRows.empty()) return {0, true, std::move(exactRows)};
    }
    static const std::regex cannotCreate(
        R"pd((?:couldn't|could not|can't|cannot)\s+create\s+['"]?([A-Za-z0-9_./~\-]+))pd",
        std::regex_constants::icase);
    static const std::regex noMethod(
        R"pd(no\s+method\s+for\s+['"]([^'"]+)['"])pd", std::regex_constants::icase);
    static const std::regex noObject(
        R"pd(no\s+such\s+object\s*:?\s*['"]?([A-Za-z0-9_./~\-]+))pd",
        std::regex_constants::icase);
    static const std::regex unknownObject(
        R"pd(unknown\s+object\s*:?\s*['"]?([A-Za-z0-9_./~\-]+))pd",
        std::regex_constants::icase);
    static const std::regex gotToken(
        R"pd(got\s+['"]([^'"]+)['"])pd", std::regex_constants::icase);
    static const std::regex errorPrefix(
        R"pd(error:\s*([A-Za-z0-9_./~\-]+)\s*:)pd", std::regex_constants::icase);

    if (std::regex_search(message, match, cannotCreate))
        addMatches(lines, trimToken(match[1].str()), 5, scores);
    if (std::regex_search(message, match, noMethod)) addMatches(lines, match[1].str(), 4, scores);
    if (std::regex_search(message, match, noObject)) addMatches(lines, match[1].str(), 5, scores);
    if (std::regex_search(message, match, unknownObject)) addMatches(lines, match[1].str(), 5, scores);
    if (std::regex_search(message, match, gotToken)) addMatches(lines, match[1].str(), 3, scores);
    if (std::regex_search(message, match, errorPrefix)) addMatches(lines, match[1].str(), 2, scores);

    // A quoted selector or symbol may be the only useful clue in other PD messages.
    static const std::regex quoted(R"pd(['"]([^'"]{2,64})['"])pd");
    for (std::sregex_iterator it(message.begin(), message.end(), quoted), end; it != end; ++it) {
        const std::string token = (*it)[1].str();
        const std::string tokenLow = lower(token);
        if (tokenLow == "float" || tokenLow == "symbol" || tokenLow == "list" ||
            tokenLow == "bang" || tokenLow == "error") continue;
        addMatches(lines, token, 1, scores);
    }

    const bool objectCreationError = low.find("couldn't create") != std::string::npos ||
        low.find("could not create") != std::string::npos ||
        low.find("can't create") != std::string::npos ||
        low.find("cannot create") != std::string::npos;
    if (scores.empty()) return objectCreationError ? SourceLine{0, true, fallbackCandidates} : SourceLine{};
    int bestLine = 0, bestScore = 0, runnersUp = 0;
    for (const auto& entry : scores) {
        if (entry.second > bestScore) {
            runnersUp = bestScore;
            bestScore = entry.second;
            bestLine = entry.first;
        } else if (entry.second > runnersUp) {
            runnersUp = entry.second;
        }
    }
    if (bestLine > 0 && bestScore >= 3 && bestScore > runnersUp)
        return {bestLine, true, {}};
    SourceLine uncertain;
    uncertain.approximate = true;
    for (const auto& entry : scores)
        if (entry.second == bestScore) uncertain.candidates.push_back(entry.first);
    if (uncertain.candidates.empty() && objectCreationError)
        uncertain.candidates = fallbackCandidates;
    if (uncertain.candidates.size() == 1) {
        uncertain.line = uncertain.candidates.front();
        uncertain.candidates.clear();
    }
    if (uncertain.candidates.empty() && uncertain.line == 0) return {};
    return uncertain;
}

} // namespace pd_diagnostics
