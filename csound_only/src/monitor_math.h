#pragma once

#include <algorithm>
#include <cmath>

// Display-only monitor math. The monitor data are decimated k-rate samples
// and an interpolated filter bank, not a full-resolution oscilloscope or FFT.
namespace monitor_math {
inline float clamp01(float x) { return std::max(0.f, std::min(1.f, x)); }

inline float sample(const float* samples, int n, float position, bool cubic) {
    if (!samples || n <= 0) return 0.f;
    position = std::max(0.f, std::min((float)(n - 1), position));
    const int i = (int)position;
    const float t = position - (float)i;
    auto at = [&](int j) { return samples[std::min(n - 1, std::max(0, j))]; };
    if (!cubic || i + 1 >= n) return at(i) + (at(i + 1) - at(i)) * t;
    const float a = at(i - 1), b = at(i), c = at(i + 1), d = at(i + 2);
    const float v = 0.5f * (2.f * b + (-a + c) * t +
        (2.f * a - 5.f * b + 4.f * c - d) * t * t +
        (-a + 3.f * b - 3.f * c + d) * t * t * t);
    // Suppress cubic overshoot. This is a display interpolation, not new data.
    return std::max(std::min(b, c), std::min(std::max(b, c), v));
}

inline float triggerStart(const float* left, const float* right, int n) {
    if (!left || !right || n < 4) return 0.f;
    // Search only the first quarter. The displayed frame never wraps the buffer
    // or joins samples from different times.
    const int end = std::max(1, n / 4);
    for (int source = 0; source < 3; ++source) {
        for (int i = 0; i < end && i + 1 < n; ++i) {
            const float a = source == 0 ? 0.5f * (left[i] + right[i]) : source == 1 ? left[i] : right[i];
            const float b = source == 0 ? 0.5f * (left[i + 1] + right[i + 1]) : source == 1 ? left[i + 1] : right[i + 1];
            if (a <= 0.f && b > 0.f && b - a > 1e-6f)
                return (float)i + (-a / (b - a));
        }
    }
    return 0.f;
}

inline float frequencyAt(float u, float lo, float hi, bool logarithmic) {
    u = clamp01(u);
    lo = std::max(1.f, lo);
    hi = std::max(lo + 1.f, hi);
    return logarithmic ? lo * std::pow(hi / lo, u) : lo + (hi - lo) * u;
}

inline float frequencyPosition(float f, float lo, float hi, bool logarithmic) {
    lo = std::max(1.f, lo);
    hi = std::max(lo + 1.f, hi);
    f = std::max(lo, std::min(hi, f));
    return logarithmic ? std::log(f / lo) / std::log(hi / lo) : (f - lo) / (hi - lo);
}
}
