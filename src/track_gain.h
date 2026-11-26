#pragma once
#include <array>
#include <atomic>
#include <cmath>

// The track fader x volume lane for the engines whose audio CAW does not mix as samples
// (live Csound, PD, SuperCollider). Written by the UI thread every frame (pushEngineTrackGains),
// read lock-free on the audio / worker threads. A track never pushed reads unity, so it always sounds.
class TrackGains {
public:
    static constexpr int capacity = 256;
    void set(int trackId, float gain) noexcept {
        if (trackId <= 0) return;
        if (!std::isfinite(gain)) gain = 1.f;
        gain = gain < 0.f ? 0.f : (gain > 1e10f ? 1e10f : gain);
        for (int i = 0; i < capacity; ++i) {
            const int id = ids_[i].load(std::memory_order_acquire);
            if (id == trackId) { gains_[i].store(gain, std::memory_order_release); return; }
            if (id == 0) {
                gains_[i].store(gain, std::memory_order_release);
                int expected = 0;
                if (ids_[i].compare_exchange_strong(expected, trackId, std::memory_order_acq_rel)) return;
                if (expected == trackId) { gains_[i].store(gain, std::memory_order_release); return; }
            }
        }
    }
    float get(int trackId) const noexcept {
        if (trackId <= 0) return 1.f;
        for (int i = 0; i < capacity; ++i) {
            const int id = ids_[i].load(std::memory_order_acquire);
            if (id == trackId) return gains_[i].load(std::memory_order_acquire);
            if (id == 0) break;
        }
        return 1.f;
    }
private:
    std::array<std::atomic<int>, capacity> ids_{};
    std::array<std::atomic<float>, capacity> gains_{};
};
inline TrackGains& trackGains() { static TrackGains g; return g; }
