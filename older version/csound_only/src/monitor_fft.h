#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

// Single-sided peak-amplitude spectrum of a normalized, uniformly sampled
// audio frame. Periodic Hann window, coherent-gain correction. Values are
// amplitudes relative to 0 dBFS, NOT a power spectral density estimate.
namespace monitor_fft {
constexpr int size = 2048;
// A longer DFT samples the same window transform more densely. Zero padding
// does NOT improve the physical resolution of the 2048-sample observation.
constexpr int fftSize = size * 4;
constexpr int bins = fftSize / 2 + 1;
// These tables depend only on the fixed transform size, not on the signal.
// Initialize once rather than calculating thousands of cosines per frame.
struct Tables {
    std::array<double, size> window{};
    std::array<unsigned, fftSize> reversed{};
    std::array<std::complex<double>, fftSize / 2> twiddle{};
    Tables() {
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < size; ++i)
            window[i] = 0.5 - 0.5 * std::cos(2.0 * pi * i / size);
        for (unsigned i = 1, j = 0; i < fftSize; ++i) {
            unsigned bit = fftSize >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            reversed[i] = j;
        }
        for (int i = 0; i < fftSize / 2; ++i) {
            const double angle = -2.0 * pi * i / fftSize;
            twiddle[i] = {std::cos(angle), std::sin(angle)};
        }
    }
};
inline std::vector<float> spectrum(const float* samples) {
    static const Tables tables;
    std::vector<std::complex<double>> x(fftSize);
    for (int i = 0; i < size; ++i) {
        x[i] = std::isfinite(samples[i]) ? samples[i] * tables.window[i] : 0.0;
    }
    for (unsigned i = 1; i < fftSize; ++i) {
        const unsigned j = tables.reversed[i];
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= fftSize; len <<= 1) {
        for (int i = 0; i < fftSize; i += len) {
            for (int j = 0; j < len / 2; ++j) {
                const auto a = x[i + j], b = tables.twiddle[j * (fftSize / len)] * x[i + j + len / 2];
                x[i + j] = a + b; x[i + j + len / 2] = a - b;
            }
        }
    }
    std::vector<float> db(bins);
    for (int i = 0; i < bins; ++i) {
        const double amp = std::abs(x[i]) * ((i == 0 || i == fftSize / 2) ? 1.0 : 2.0) / (size * 0.5);
        db[i] = (float)(20.0 * std::log10(std::max(amp, 1e-8)));
    }
    return db;
}
}
