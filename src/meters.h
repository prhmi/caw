#pragma once
#include <atomic>

// Ring buffer holding the most recent master-output samples.
// The audio engine will call push() from its thread; the UI reads with snapshot().
struct MasterTap {
    static constexpr unsigned N = 8192;  // power of two
    float l[N] = {};
    float r[N] = {};
    std::atomic<unsigned> pos{0};

    void push(const float* L, const float* R, int n);
    void snapshot(float* outL, float* outR, int n) const;  // newest n samples, oldest first
};

MasterTap& masterTap();

// Hann-windowed FFT. size must be a power of two.
// outDb[k] for k < size/2, in dB where 0 dB = a full-scale sine.
void computeSpectrum(const float* mono, int size, float* outDb);
