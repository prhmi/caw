#pragma once

// Pure C++ display-work selection, independent of the audio engine and UI.
// Audio capture always continues; only compute the analysis being displayed.
namespace monitor_work {
struct Plan {
    bool fft = false;
    bool thirdBands = false;
    bool columnBands = false;
    bool tonalPeaks = false;
    bool history = false;
};
inline Plan select(bool visible, bool frozen, int mode, int style) {
    Plan p;
    if (!visible || frozen || mode == 1) return p;
    p.fft = true;
    if (mode == 2) p.history = true;
    else if (mode == 0) {
        p.thirdBands = style == 1;
        p.columnBands = style == 3;
        p.tonalPeaks = style == 0;
    }
    return p;
}
}
