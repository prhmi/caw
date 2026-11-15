#pragma once
#include "monitor_fft.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Logarithmic energy bands on an exact base-2 grid referenced to 1 kHz.
// Each FFT sample belongs to at most one band. Levels use a peak-sine
// dBFS reference: a 0.5-peak sine fully contained in one band is -6.02 dBFS.
// This is not IEC 61260 filter-bank certification or a PSD measurement.
namespace monitor_bands {
constexpr int kColumnBandsPerOctave = 6; // narrower 1/6-octave columns, edge to edge
struct Band { double lo, center, hi, db; };
inline std::vector<Band> analyze(const std::vector<float>& fftDb, double sampleRate, int bandsPerOctave = 3) {
    std::vector<Band> out;
    if (fftDb.size() != monitor_fft::bins || !(sampleRate > 0) || bandsPerOctave < 1 || bandsPerOctave > 48) return out;
    const double edge = std::pow(2.0, 1.0 / (2.0 * bandsPerOctave));
    const double hzPerBin = sampleRate / monitor_fft::fftSize;
    // One-sided peak amplitudes, periodic Hann power correction, 4x padded
    // FFT integration. 2/3 is Hann coherent gain^2 / mean-square gain;
    // divide by the zero-padding factor to avoid counting interpolated bins.
    const double correction = (2.0 / 3.0) * monitor_fft::size / monitor_fft::fftSize;
    for (int k = -8 * bandsPerOctave; k <= 6 * bandsPerOctave; ++k) {
        const double center = 1000.0 * std::pow(2.0, (double)k / bandsPerOctave);
        const double lo = center / edge, hi = center * edge;
        if (center < 20.0 || lo >= sampleRate * 0.5) continue;
        int begin = std::max(1, (int)std::ceil(lo / hzPerBin));
        int end = std::min(monitor_fft::bins - 1, (int)std::ceil(hi / hzPerBin));
        double energy = 0.0;
        for (int j = begin; j < end; ++j) {
            const double amplitude = std::pow(10.0, fftDb[(size_t)j] / 20.0);
            energy += amplitude * amplitude;
        }
        const double amp = std::sqrt(energy * correction);
        out.push_back({lo, center, hi, 20.0 * std::log10(std::max(amp, 1e-8))});
    }
    return out;
}

}
