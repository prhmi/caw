#pragma once
#include <string>

// Quote one argument for ProcessRunner's platform-specific command parser.
inline std::string processArg(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : s) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') { out.append(slashes * 2 + 1, '\\'); out += '"'; }
        else { out.append(slashes, '\\'); out += c; }
        slashes = 0;
    }
    out.append(slashes * 2, '\\');
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
#endif
}
