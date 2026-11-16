#pragma once
#include "monitor_frame.h"
#include <charconv>
#include <cctype>
#include <string>
#include <system_error>
#include <vector>

// Locale-independent fast parsing for the high-volume raw monitor lines.
// Other Csound messages retain their existing parser.
inline bool parseRawMonitorValues(const char* first, const char* last,
                                  int& start, std::vector<float>& values) {
    auto spaces = [&]() {
        while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    };
    spaces();
    auto index = std::from_chars(first, last, start);
    if (index.ec != std::errc() || index.ptr == first) return false;
    first = index.ptr;
    values.clear();
    values.reserve(MonitorFrameAssembler::chunkSize);
    while (first != last) {
        if (!std::isspace(static_cast<unsigned char>(*first))) return false;
        spaces();
        if (first == last) break;
        float value = 0;
        auto parsed = std::from_chars(first, last, value, std::chars_format::general);
        if (parsed.ec != std::errc() || parsed.ptr == first ||
            values.size() >= (size_t)MonitorFrameAssembler::chunkSize) return false;
        values.push_back(value);
        first = parsed.ptr;
    }
    return true;
}
