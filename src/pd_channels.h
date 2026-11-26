#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include "pd_timeline.h"

// Pure Data patch channel width: a .pd patch file scanned for [dac~ N ...] objects. The highest
// channel any dac~ writes is the patch's output width. CAW is the virtual soundcard: PD renders
// at max(soundcard width, patch needs) and folds the extra channels into the master (k mod N) -
// the same rule every engine follows. A bare [dac~] without numbers plays the configured width,
// so it never widens anything.
inline int pdPatchMaxChannelText(const std::string& text) {
    int maxCh = 0;
    size_t at = text.find("dac~");
    while (at != std::string::npos) {
        size_t i = at + 4;
        int num = 0;
        while (i < text.size()) {
            const char q = text[i];
            if (q >= '0' && q <= '9') { num = num * 10 + (q - '0'); if (num > 64) num = 64; ++i; }
            else if (q == ' ' || q == '\t') {
                if (num) { maxCh = std::max(maxCh, num); num = 0; }
                ++i;
            } else break;
        }
        if (num) maxCh = std::max(maxCh, num);
        at = text.find("dac~", at + 4);
    }
    return std::min(maxCh, 8);   // dac~ 9..24 are the CAW cable sends (cawr~ / caws~), not track channels
}

inline int pdPatchMaxChannel(const std::string& path) {
    if (path.empty()) return 0;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::string text;
    char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    std::fclose(f);
    return pdPatchMaxChannelText(text);
}

inline int pdPlanMaxChannel(const std::vector<pd_timeline::Event>& plan) {
    int maxCh = 0;
    for (const auto& e : plan) maxCh = std::max(maxCh, pdPatchMaxChannel(e.patch));
    return maxCh;
}
