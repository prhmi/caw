#pragma once
#include "monitor_fft.h"
#include <algorithm>
#include <cmath>
#include <vector>

// The engine writes up to 30 samples per chunk in U then V order. A warning or a
// truncated pipe line must never silently splice old samples into a new FFT.
struct MonitorFrameAssembler {
    static constexpr int chunkSize = 30; // Csound printf supports at most 30 value arguments
    float left[monitor_fft::size] = {}, right[monitor_fft::size] = {};
    int leftCount = -1, rightCount = -1;
    void reset() { leftCount = rightCount = -1; }
    bool push(char tag, int start, const std::vector<float>& samples, float invFullScale) {
        if (tag != 'U' && tag != 'V') return false;
        if (tag == 'U' && start == 0) { leftCount = 0; rightCount = -1; }
        int& count = tag == 'U' ? leftCount : rightCount;
        if (tag == 'V' && start == 0 && leftCount == monitor_fft::size) rightCount = 0;
        const bool valid = count >= 0 && start == count &&
            start >= 0 && start < monitor_fft::size &&
            samples.size() == (size_t)std::min(chunkSize, monitor_fft::size - start) &&
            std::all_of(samples.begin(), samples.end(), [](float x) { return std::isfinite(x); });
        if (!valid) { reset(); return false; }
        float* target = tag == 'U' ? left : right;
        for (size_t i = 0; i < samples.size(); ++i) target[start + i] = samples[i] * invFullScale;
        count += (int)samples.size();
        const bool complete = tag == 'V' && count == monitor_fft::size && leftCount == monitor_fft::size;
        if (complete) reset(); // an entire missing U half must not reuse the previous left channel
        return complete;
    }
};
