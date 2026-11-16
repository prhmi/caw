#pragma once
#include <algorithm>
#include <array>
#include <cstddef>

// Experimental building block for a future libpd backend. This does not load
// libpd or connect it to CAW. All storage is fixed before processing; the
// callback must itself be suitable for the calling audio thread.
// Exactly one tick of latency is introduced. Interleaved float input/output.
namespace caw {
template<int Tick = 64, int MaxChannels = 8>
class FixedTickAdapter {
    static_assert(Tick > 0 && MaxChannels > 0, "positive limits required");
public:
    using Process = bool (*)(void*, const float*, float*, int, int) noexcept;
    // Configure/reset only while processing is stopped. A null input pointer
    // represents silence. Zero input channels are supported; output >= 1.
    bool configure(int inputs, int outputs, Process process, void* context) noexcept {
        if (inputs < 0 || inputs > MaxChannels || outputs < 1 || outputs > MaxChannels || !process) return false;
        inChannels_ = inputs; outChannels_ = outputs; process_ = process; context_ = context;
        input_.fill(0.f); output_.fill(0.f); position_ = 0; failed_ = false;
        return true;
    }
    // Returns false after a backend failure and writes silence until reset.
    // Caller must pass non-overlapping buffers (or exactly the same buffer
    // when input and output channel counts match).
    bool process(const float* in, float* out, int frames) noexcept {
        if (!out || frames < 0 || !process_) return false;
        if (failed_) { std::fill_n(out, static_cast<std::size_t>(frames) * outChannels_, 0.f); return false; }
        for (int f = 0; f < frames; ++f) {
            for (int c = 0; c < inChannels_; ++c)
                input_[position_ * inChannels_ + c] = in ? in[static_cast<std::size_t>(f) * inChannels_ + c] : 0.f;
            for (int c = 0; c < outChannels_; ++c)
                out[static_cast<std::size_t>(f) * outChannels_ + c] = output_[position_ * outChannels_ + c];
            if (++position_ == Tick) {
                position_ = 0;
                output_.fill(0.f);
                if (!process_(context_, input_.data(), output_.data(), inChannels_, outChannels_)) {
                    failed_ = true;
                    std::fill_n(out + static_cast<std::size_t>(f + 1) * outChannels_,
                                static_cast<std::size_t>(frames - f - 1) * outChannels_, 0.f);
                    return false;
                }
            }
        }
        return true;
    }
    static constexpr int latencyFrames() noexcept { return Tick; }
private:
    std::array<float, Tick * MaxChannels> input_{};
    std::array<float, Tick * MaxChannels> output_{};
    int inChannels_ = 0, outChannels_ = 0, position_ = 0;
    Process process_ = nullptr;
    void* context_ = nullptr;
    bool failed_ = false;
};
}
