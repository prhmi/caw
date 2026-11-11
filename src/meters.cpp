#include "meters.h"
#include <cmath>
#include <complex>
#include <vector>

MasterTap& masterTap() {
    static MasterTap tap;
    return tap;
}

void MasterTap::push(const float* L, const float* R, int n) {
    unsigned p = pos.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        l[(p + i) & (N - 1)] = L[i];
        r[(p + i) & (N - 1)] = R ? R[i] : L[i];
    }
    pos.store(p + (unsigned)n, std::memory_order_release);
}

void MasterTap::snapshot(float* outL, float* outR, int n) const {
    unsigned p = pos.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        unsigned idx = (p + N - (unsigned)n + (unsigned)i) & (N - 1);
        outL[i] = l[idx];
        outR[i] = r[idx];
    }
}

void computeSpectrum(const float* x, int size, float* outDb) {
    static std::vector<std::complex<float>> a;
    a.assign((size_t)size, std::complex<float>(0.f, 0.f));
    const float twoPi = 6.28318530718f;
    for (int i = 0; i < size; ++i) {
        float w = 0.5f - 0.5f * std::cos(twoPi * (float)i / (float)(size - 1));
        a[(size_t)i] = x[i] * w;
    }

    // bit reversal
    for (int i = 1, j = 0; i < size; ++i) {
        int bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[(size_t)i], a[(size_t)j]);
    }
    // butterflies
    for (int len = 2; len <= size; len <<= 1) {
        float ang = -twoPi / (float)len;
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < size; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (int k = 0; k < len / 2; ++k) {
                std::complex<float> u = a[(size_t)(i + k)];
                std::complex<float> v = a[(size_t)(i + k + len / 2)] * w;
                a[(size_t)(i + k)] = u + v;
                a[(size_t)(i + k + len / 2)] = u - v;
                w *= wl;
            }
        }
    }

    // Hann coherent gain is 0.5 -> a full-scale sine peaks at |X| = N/4
    for (int k = 0; k < size / 2; ++k) {
        float mag = std::abs(a[(size_t)k]) * 4.0f / (float)size;
        outDb[k] = 20.0f * std::log10(mag > 1e-6f ? mag : 1e-6f);
    }
}
