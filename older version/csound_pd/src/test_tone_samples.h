#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Independent of Csound and Windows, so the generated signal is testable.
inline std::vector<int16_t> makeTestTone(int rate = 48000, int channels = 2) {
    rate = std::max(1000, std::min(384000, rate));
    channels = std::max(1, std::min(64, channels));
    const int frames = rate * 3 / 4;
    const int fade = rate / 50;
    constexpr double pi = 3.14159265358979323846;
    std::vector<int16_t> pcm((size_t)frames * channels, 0);
    for (int i = 0; i < frames; ++i) {
        const double envelope = std::min(1.0, std::min((double)i / fade, (double)(frames - 1 - i) / fade));
        const auto sample = (int16_t)std::lround(32767.0 * 0.18 * envelope * std::sin(2.0 * pi * 440.0 * i / rate));
        pcm[(size_t)i * channels] = sample;
        if (channels > 1) pcm[(size_t)i * channels + 1] = sample;
    }
    return pcm;
}
