#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

// Scratch owned by one VST instance, allocated during prepare rather than on
// every block. An unexpectedly oversized block grows safely rather than being
// truncated; normal blocks up to the prepared maximum do not allocate.
class VstAudioBuffers {
public:
    void prepare(int maxBlock) {
        const size_t n = (size_t)std::max(1, maxBlock);
        left_.resize(n); right_.resize(n); outLeft_.resize(n); outRight_.resize(n);
    }
    void begin(const float* input, int frames, int channels) {
        if (frames <= 0 || channels <= 0) return;
        if ((size_t)frames > left_.size()) prepare(frames);
        for (int i = 0; i < frames; ++i) {
            left_[i] = input ? input[(size_t)i * channels] : 0.f;
            right_[i] = input ? input[(size_t)i * channels + (channels > 1)] : 0.f;
        }
        std::fill_n(outLeft_.data(), frames, 0.f);
        std::fill_n(outRight_.data(), frames, 0.f);
    }
    void finish(float* output, int frames, int channels) const {
        if (!output || frames <= 0 || channels <= 0) return;
        for (int i = 0; i < frames; ++i) {
            output[(size_t)i * channels] = outLeft_[i];
            if (channels > 1) output[(size_t)i * channels + 1] = outRight_[i];
        }
    }
    float* left() { return left_.data(); }
    float* right() { return right_.data(); }
    float* outLeft() { return outLeft_.data(); }
    float* outRight() { return outRight_.data(); }
private:
    std::vector<float> left_, right_, outLeft_, outRight_;
};
