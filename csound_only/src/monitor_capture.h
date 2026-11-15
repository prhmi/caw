#pragma once
#include "monitor_fft.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

// Source-independent SPSC snapshot bridge. The audio producer never waits,
// locks or allocates while submitting a block. Three exclusive-ownership
// slots prevent the UI from reading data while it is being overwritten.
class MonitorCapture {
public:
    static constexpr int samples = monitor_fft::size;
    struct Snapshot {
        std::array<float, samples> left{}, right{};
        std::vector<float> meters;
        uint64_t sequence = 0;
        int64_t timestampNs = 0;
    };
    explicit MonitorCapture(int sampleRate, int meterCount)
        : meterCount_(std::max(0, meterCount)), interval_(std::max(1, sampleRate / 30)),
          peaks_((size_t)meterCount_, 0.f) {
        for (auto& slot : slots_) slot.frame.meters.resize((size_t)meterCount_);
    }
    MonitorCapture(const MonitorCapture&) = delete;
    MonitorCapture& operator=(const MonitorCapture&) = delete;

    void addMeter(int index, float peak) noexcept {
        if (index >= 0 && index < meterCount_ && std::isfinite(peak))
            peaks_[(size_t)index] = std::max(peaks_[(size_t)index], std::max(0.f, peak));
    }
    template<typename Sample>
    void pushInterleaved(const Sample* block, int frames, int channels, int64_t timestampNs) noexcept {
        if (!block || frames <= 0 || channels <= 0) return;
        for (int i = 0; i < frames; ++i) {
            float l = static_cast<float>(block[(size_t)i * channels]);
            float r = channels > 1 ? static_cast<float>(block[(size_t)i * channels + 1]) : l;
            pushSample(l, r);
        }
        finishBlock(frames, timestampNs);
    }
    // Planar format is common in plugin and audio-server callbacks.
    template<typename Sample>
    void pushPlanar(const Sample* const* channels, int frames, int channelCount, int64_t timestampNs) noexcept {
        if (!channels || frames <= 0 || channelCount <= 0 || !channels[0]) return;
        const Sample* left = channels[0];
        const Sample* right = channelCount > 1 && channels[1] ? channels[1] : left;
        for (int i = 0; i < frames; ++i) {
            pushSample(static_cast<float>(left[i]), static_cast<float>(right[i]));
        }
        finishBlock(frames, timestampNs);
    }
    // Claim complete slots, copy only new data, release ownership. Old ready
    // slots are discarded and the newest snapshot obtained is returned.
    bool take(Snapshot& out, uint64_t lastSequence) {
        bool found = false;
        uint64_t newest = lastSequence;
        out.meters.assign((size_t)meterCount_, 0.f);
        for (Slot& slot : slots_) {
            int expected = ready;
            if (!slot.state.compare_exchange_strong(expected, reading, std::memory_order_acquire)) continue;
            if (slot.frame.sequence > lastSequence)
                for (int i = 0; i < meterCount_; ++i)
                    out.meters[(size_t)i] = std::max(out.meters[(size_t)i], slot.frame.meters[(size_t)i]);
            if (slot.frame.sequence > newest) {
                out.left = slot.frame.left;
                out.right = slot.frame.right;
                out.sequence = slot.frame.sequence;
                out.timestampNs = slot.frame.timestampNs;
                newest = slot.frame.sequence;
                found = true;
            }
            slot.state.store(free, std::memory_order_release);
        }
        return found;
    }
private:
    enum { free = 0, writing = 1, ready = 2, reading = 3 };
    struct Slot {
        std::atomic<int> state{free};
        Snapshot frame;
    };
    void pushSample(float l, float r) noexcept {
        left_[(size_t)head_] = std::isfinite(l) ? l : 0.f;
        right_[(size_t)head_] = std::isfinite(r) ? r : 0.f;
        head_ = (head_ + 1) % samples;
        if (filled_ < samples) ++filled_;
    }
    void finishBlock(int frames, int64_t timestampNs) noexcept {
        pending_ += frames;
        if (filled_ == samples && pending_ >= interval_) {
            pending_ %= interval_;
            publish(timestampNs);
        }
    }
    void publish(int64_t timestampNs) noexcept {
        Slot* chosen = nullptr;
        bool reclaimed = false;
        // Prefer free space; if the UI is behind, reclaim a ready frame
        // rather than blocking the audio thread.
        for (int pass = 0; pass < 2 && !chosen; ++pass) {
            for (int i = 0; i < 3; ++i) {
                Slot& s = slots_[(size_t)((nextSlot_ + i) % 3)];
                int expected = pass == 0 ? free : ready;
                if (s.state.compare_exchange_strong(expected, writing, std::memory_order_acquire)) {
                    chosen = &s; reclaimed = pass == 1; nextSlot_ = (nextSlot_ + i + 1) % 3; break;
                }
            }
        }
        if (!chosen) return;
        if (reclaimed)
            for (int i = 0; i < meterCount_; ++i)
                peaks_[(size_t)i] = std::max(peaks_[(size_t)i], chosen->frame.meters[(size_t)i]);
        for (int i = 0; i < samples; ++i) {
            const size_t at = (size_t)((head_ + i) % samples);
            chosen->frame.left[(size_t)i] = left_[at];
            chosen->frame.right[(size_t)i] = right_[at];
        }
        std::copy(peaks_.begin(), peaks_.end(), chosen->frame.meters.begin());
        std::fill(peaks_.begin(), peaks_.end(), 0.f);
        chosen->frame.sequence = ++sequence_;
        chosen->frame.timestampNs = timestampNs;
        chosen->state.store(ready, std::memory_order_release);
    }
    int meterCount_, interval_, head_ = 0, filled_ = 0, pending_ = 0, nextSlot_ = 0;
    uint64_t sequence_ = 0;
    std::array<float, samples> left_{}, right_{};
    std::vector<float> peaks_;
    std::array<Slot, 3> slots_;
};
