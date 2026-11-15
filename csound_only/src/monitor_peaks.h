#pragma once
#include "monitor_bands.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// Optional tonal-peak visualization. This is an estimate, not a replacement
// for the unmodified band-energy view. Window-sidelobe suppression can
// suppress a real quiet tone close to a louder one.
namespace monitor_peaks {
struct Peak { double frequency = 0, db = -160; };
// Visual width only, centred on the interpolated frequency rather than
// snapped to fixed octave bands. This is not a measured bandwidth.
inline std::pair<double, double> visualBounds(const Peak& peak) {
    constexpr double factor = 1.0594630943592953; // 2^(1/12)
    return {peak.frequency / factor, peak.frequency * factor};
}
inline std::vector<Peak> find(const std::vector<float>& fftDb, double sampleRate) {
    std::vector<Peak> candidates, peaks;
    if (fftDb.size() != monitor_fft::bins || !(sampleRate > 0)) return peaks;
    const double spacing = sampleRate / monitor_fft::fftSize;
    const double resolution = sampleRate / monitor_fft::size;
    constexpr double pi = 3.14159265358979323846;
    for (int i = 1; i < monitor_fft::bins - 1; ++i) {
        const double hz = i * spacing;
        if (hz < 25 || hz > std::min(18000.0, sampleRate * 0.5) || fftDb[(size_t)i] < -120) continue;
        const double a = fftDb[(size_t)i - 1], b = fftDb[(size_t)i], c = fftDb[(size_t)i + 1];
        if (!(b > a && b >= c)) continue;
        const double denom = a - 2 * b + c;
        const double offset = std::abs(denom) > 1e-12 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / denom)) : 0;
        candidates.push_back({(i + offset) * spacing, b - 0.25 * (a - c) * offset});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
    // Bounded work in broadband noise. The strongest candidates get priority.
    if (candidates.size() > 256) candidates.resize(256);
    for (const Peak& candidate : candidates) {
        bool likelyLeakage = false;
        for (const Peak& strong : peaks) {
            // For x >= 2, the Hann sidelobe response is at most 1/(6*pi).
            // With the 8 dB tolerance below, leakage is impossible when the
            // candidate is within 17.5 dB of the stronger peak. For x < 2
            // the old algorithm also skips the pair. Avoid sin/log for both.
            if (candidate.db > strong.db - 17.5) continue;
            const double x = std::abs(candidate.frequency - strong.frequency) / resolution;
            if (x < 2.0) continue;
            const double response = std::abs(std::sin(pi * x) / (pi * x * (1.0 - x * x)));
            const double predicted = strong.db + 20.0 * std::log10(std::max(response, 1e-12));
            if (candidate.db <= predicted + 8.0) { likelyLeakage = true; break; }
        }
        if (!likelyLeakage) peaks.push_back(candidate);
    }
    return peaks;
}

// Put the strongest accepted tonal peak in each supplied frequency
// position. Two real tones in the same band share one displayed column.
inline std::vector<monitor_bands::Band> project(const std::vector<monitor_bands::Band>& measured,
                                                const std::vector<Peak>& peaks) {
    auto out = measured;
    for (auto& band : out) band.db = -160;
    for (const Peak& peak : peaks)
        for (auto& band : out)
            if (peak.frequency >= band.lo && peak.frequency < band.hi) {
                band.db = std::max(band.db, peak.db);
                break;
            }
    return out;
}
}
