#pragma once
#include <cctype>
#include <string>

// Csound 6/7 may append a channel-count annotation outside the device
// name: "dac1 (Device [ASIO, 2 in, 2 out]) [ch:2]".
// Strip only that exact trailing annotation and the dac-number wrapper.
inline std::string normalizeCawDeviceLabel(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    const auto mark = s.rfind(" [ch:");
    if (mark != std::string::npos && s.back() == ']') {
        bool digits = mark + 5 < s.size() - 1;
        for (size_t i = mark + 5; i + 1 < s.size(); ++i)
            if (!std::isdigit((unsigned char)s[i])) digits = false;
        if (digits) s.erase(mark);
    }
    if (s.rfind("dac", 0) == 0) {
        size_t i = 3;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i > 3 && i + 2 < s.size() && s.compare(i, 2, " (") == 0 && s.back() == ')')
            s = s.substr(i + 2, s.size() - i - 3);
    }
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Inspect the host API field, not the device's name: e.g. "ASIO4ALL
// Speakers [Windows WASAPI, ...]" is WASAPI, not an ASIO device.
inline bool cawDeviceHasHostApi(const std::string& label, const std::string& tag) {
    if (tag.empty()) return true;
    std::string s = normalizeCawDeviceLabel(label), key = tag;
    for (char& c : key) c = (char)std::tolower((unsigned char)c);
    const auto left = s.rfind('[');
    if (left == std::string::npos) return false;
    const auto comma = s.find(',', left);
    if (comma == std::string::npos) return false;
    return s.substr(left + 1, comma - left - 1).find(key) != std::string::npos;
}
