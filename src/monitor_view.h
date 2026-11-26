#pragma once

// monitor_view.h - the spectrum / oscilloscope / meter-view stack in one header.
// Merged (phase-1 cleanup) from: monitor_math, monitor_fft, monitor_capture,
// monitor_frame, monitor_parse, monitor_bands, monitor_peaks, monitor_osc, monitor_work.
// Std-only (no ImGui / no engine deps), so engine headers may include it.

// ---- (was monitor_math.h) --------------------------------------------------------------

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

struct ScopeWindow { float start = 0.f; int samples = 2; bool triggered = false; };
// Find the first positive zero crossing inside a recent search region, leaving
// a complete, contiguous window after it. Stereo/mono triggers on mid first,
// with left and right fallbacks for phase-cancelled or one-sided signals.
inline ScopeWindow scopeWindow(const float* left, const float* right, int n, int sr,
                               float ms, bool trigger, int channel) {
    ScopeWindow out;
    if (!left || !right || n < 2 || sr <= 0) return out;
    out.samples = std::max(2, std::min(n, (int)std::lround(ms * sr / 1000.f)));
    const int latest = n - out.samples;
    out.start = (float)latest;
    if (!trigger || latest <= 0) return out;
    const int first = std::max(0, latest - out.samples);
    for (int pass = 0; pass < (channel == 0 ? 3 : 1); ++pass) {
        for (int i = first; i < latest; ++i) {
            auto val = [&](int j) {
                if (channel == 1 || (channel == 0 && pass == 1)) return left[j];
                if (channel == 2 || (channel == 0 && pass == 2)) return right[j];
                return (left[j] + right[j]) * 0.5f;
            };
            const float a = val(i), b = val(i + 1);
            if (a <= 0.f && b > 0.f && b - a > 1e-6f) {
                out.start = i - a / (b - a);
                out.triggered = true;
                return out;
            }
        }
    }
    return out;
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

// ---- (was monitor_fft.h) --------------------------------------------------------------
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
// "Fast" analysis: only the newest fastSize samples are windowed. It reacts
// about 4x sooner to a new sound but its frequency resolution is 4x coarser.
constexpr int fastSize = 512;
// These tables depend only on the fixed transform size, not on the signal.
// Initialize once rather than calculating thousands of cosines per frame.
struct Tables {
    std::array<double, size> window{};
    std::array<double, fastSize> fastWindow{};
    std::array<unsigned, fftSize> reversed{};
    std::array<std::complex<double>, fftSize / 2> twiddle{};
    Tables() {
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i < size; ++i)
            window[i] = 0.5 - 0.5 * std::cos(2.0 * pi * i / size);
        for (int i = 0; i < fastSize; ++i)
            fastWindow[i] = 0.5 - 0.5 * std::cos(2.0 * pi * i / fastSize);
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
// len = size (detailed) or fastSize (newest samples only). Amplitudes are
// normalised to the window actually used, so levels agree between the two.
inline std::vector<float> spectrum(const float* samples, int len = size) {
    static const Tables tables;
    if (len != fastSize) len = size;
    samples += size - len;
    const double* win = len == size ? tables.window.data() : tables.fastWindow.data();
    std::vector<std::complex<double>> x(fftSize);
    for (int i = 0; i < len; ++i) {
        x[i] = std::isfinite(samples[i]) ? samples[i] * win[i] : 0.0;
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
        const double amp = std::abs(x[i]) * ((i == 0 || i == fftSize / 2) ? 1.0 : 2.0) / (len * 0.5);
        db[i] = (float)(20.0 * std::log10(std::max(amp, 1e-8)));
    }
    return db;
}
}

// ---- (was monitor_capture.h) --------------------------------------------------------------
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
        : meterCount_(std::max(0, meterCount)), interval_(std::max(1, sampleRate / 60)),
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

// ---- (was monitor_frame.h) --------------------------------------------------------------
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

// ---- (was monitor_parse.h) --------------------------------------------------------------
#include <charconv>
#include <cctype>
#include <string>
#include <system_error>
#include <vector>

// Locale-independent fast parsing for the high-volume raw monitor lines.
// Other Csound messages retain their existing parser.
inline bool parseRawMonitorValues(const char* first, const char* last,
                                  int& start, std::vector<float>& values) {
    auto spaces = [&]() {
        while (first != last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    };
    spaces();
    auto index = std::from_chars(first, last, start);
    if (index.ec != std::errc() || index.ptr == first) return false;
    first = index.ptr;
    values.clear();
    values.reserve(MonitorFrameAssembler::chunkSize);
    while (first != last) {
        if (!std::isspace(static_cast<unsigned char>(*first))) return false;
        spaces();
        if (first == last) break;
        float value = 0;
        auto parsed = std::from_chars(first, last, value, std::chars_format::general);
        if (parsed.ec != std::errc() || parsed.ptr == first ||
            values.size() >= (size_t)MonitorFrameAssembler::chunkSize) return false;
        values.push_back(value);
        first = parsed.ptr;
    }
    return true;
}

// ---- (was monitor_bands.h) --------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <vector>

// Logarithmic energy bands on an exact base-2 grid referenced to 1 kHz.
// Each FFT sample belongs to at most one band. Levels use a peak-sine
// dBFS reference: a 0.5-peak sine fully contained in one band is -6.02 dBFS.
// This is not IEC 61260 filter-bank certification or a PSD measurement.
namespace monitor_bands {
constexpr int kColumnBandsPerOctave = 6; // narrower 1/6-octave columns, edge to edge
// resolved = false when the band is narrower than the analysis window can
// resolve (main lobe of the Hann window is 4 * sampleRate / length wide). Such
// a band integrates the whole lobe around its centre so a tone is not
// under-read by several dB; it is then a lobe level, not a true band level.
struct Band { double lo, center, hi, db; bool resolved = true; };
inline std::vector<Band> analyze(const std::vector<float>& fftDb, double sampleRate, int bandsPerOctave = 3, int len = monitor_fft::size) {
    std::vector<Band> out;
    if (fftDb.size() != monitor_fft::bins || !(sampleRate > 0) || bandsPerOctave < 1 || bandsPerOctave > 48) return out;
    const double edge = std::pow(2.0, 1.0 / (2.0 * bandsPerOctave));
    const double hzPerBin = sampleRate / monitor_fft::fftSize;
    // One-sided peak amplitudes, periodic Hann power correction, 4x padded
    // FFT integration. 2/3 is Hann coherent gain^2 / mean-square gain;
    // divide by the zero-padding factor to avoid counting interpolated bins.
    const double correction = (2.0 / 3.0) * len / monitor_fft::fftSize;
    for (int k = -8 * bandsPerOctave; k <= 6 * bandsPerOctave; ++k) {
        const double center = 1000.0 * std::pow(2.0, (double)k / bandsPerOctave);
        const double lo = center / edge, hi = center * edge;
        if (center < 20.0 || lo >= sampleRate * 0.5) continue;
        const double lobe = 2.0 * sampleRate / len;   // half main-lobe width in Hz (report only)
        const bool resolved = (hi - lo) >= 2.0 * lobe;
        // Measure the band's OWN frequency range only. Widening unresolved low bands to the whole
        // window lobe made every low band integrate the same wide slice (including DC rumble), so
        // all of them read high on any content - wrong data. A standard RTA reports what falls in
        // the band's edges; resolution limits are stated, not compensated by overlap.
        int begin = std::max(2, (int)std::ceil(lo / hzPerBin));   // bin 2 on: DC / infrasonic leakage stays out
        int end = std::min(monitor_fft::bins - 1, (int)std::ceil(hi / hzPerBin));
        if (end <= begin) {   // narrower than one bin: the nearest bin stands in
            begin = std::max(2, std::min(begin, monitor_fft::bins - 2));
            end = begin + 1;
        }
        double energy = 0.0;
        for (int j = begin; j < end; ++j) {
            const double amplitude = std::pow(10.0, fftDb[(size_t)j] / 20.0);
            energy += amplitude * amplitude;
        }
        const double amp = std::sqrt(energy * correction);
        out.push_back({lo, center, hi, 20.0 * std::log10(std::max(amp, 1e-8)), resolved});
    }
    return out;
}

}

// ---- (was monitor_peaks.h) --------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

// Optional tonal-peak visualization. This is an estimate, not a replacement
// for the unmodified band-energy view. Window-sidelobe suppression can
// suppress a real quiet tone close to a louder one.
namespace monitor_peaks {
struct Peak { double frequency = 0, db = -160; };
// Visual width only, centred on the interpolated frequency rather than
// snapped to fixed octave bands. This is not a measured bandwidth.
inline std::pair<double, double> visualBounds(const Peak& peak) {
    constexpr double factor = 1.0293151018611496; // 2^(1/24): half a semitone each way - narrow markers,
                                                  // so neighbours stay distinguishable (designer pass)
    return {peak.frequency / factor, peak.frequency * factor};
}
inline std::vector<Peak> find(const std::vector<float>& fftDb, double sampleRate, int len = monitor_fft::size) {    std::vector<Peak> candidates, peaks;
    if (fftDb.size() != monitor_fft::bins || !(sampleRate > 0)) return peaks;
    const double spacing = sampleRate / monitor_fft::fftSize;
    const double resolution = sampleRate / len;
    constexpr double pi = 3.14159265358979323846;
    for (int i = 1; i < monitor_fft::bins - 1; ++i) {
        const double hz = i * spacing;
        if (hz < 25 || hz > std::min(18000.0, sampleRate * 0.5) || fftDb[(size_t)i] < -120) continue;
        const double a = fftDb[(size_t)i - 1], b = fftDb[(size_t)i], c = fftDb[(size_t)i + 1];
        if (!(b > a && b >= c)) continue;
        const double denom = a - 2 * b + c;
        const double offset = std::abs(denom) > 1e-12 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / denom)) : 0;
        candidates.push_back({(i + offset) * spacing, b - 0.25 * (a - c) * offset});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
    // Bounded work in broadband noise. The strongest candidates get priority.
    if (candidates.size() > 256) candidates.resize(256);
    for (const Peak& candidate : candidates) {
        bool likelyLeakage = false;
        for (const Peak& strong : peaks) {
            // For x >= 2, the Hann sidelobe response is at most 1/(6*pi).
            // With the 8 dB tolerance below, leakage is impossible when the
            // candidate is within 17.5 dB of the stronger peak. For x < 2
            // the old algorithm also skips the pair. Avoid sin/log for both.
            if (candidate.db > strong.db - 17.5) continue;
            const double x = std::abs(candidate.frequency - strong.frequency) / resolution;
            if (x < 2.0) continue;
            const double response = std::abs(std::sin(pi * x) / (pi * x * (1.0 - x * x)));
            const double predicted = strong.db + 20.0 * std::log10(std::max(response, 1e-12));
            if (candidate.db <= predicted + 8.0) { likelyLeakage = true; break; }
        }
        if (!likelyLeakage) peaks.push_back(candidate);
    }
    return peaks;
}

// ---- temporal tracking (user request: the tonal peak's x-position is too jittery) ----
// The short fast window (512 samples) has a coarse bin spacing, so the interpolated peak swings
// from bin to bin frame by frame. A tracker keeps each tone's DISPLAYED position as an exponentially
// smoothed estimate: real glides still follow (snap on a big jump), noise jitter averages out.
struct Tracked { double frequency; double db; int unseen; };
inline std::vector<Tracked> g_tracker;

inline void resetTracking() { g_tracker.clear(); }

inline std::vector<Peak> track(const std::vector<Peak>& fresh, double sampleRate, int len, double maxGlideHzPerSec = 150.0, double dt = 0.033, int maxPeaks = 16) {
    const double resolution = sampleRate / std::max(64, len);   // one bin's width at this window length
    std::vector<int> taken(fresh.size(), -1);
    std::vector<bool> matched(g_tracker.size(), false);
    // Pass 1: match each fresh peak to the closest tracker within about half a bin (a real tone barely moves).
    for (size_t f = 0; f < fresh.size(); ++f) {
        int best = -1; double bestD = 1e30;
        for (size_t t = 0; t < g_tracker.size(); ++t) {
            if (matched[t]) continue;
            const double d = std::abs(std::log(fresh[f].frequency / g_tracker[t].frequency));
            if (d < bestD) { bestD = d; best = (int)t; }
        }
        if (best >= 0) {
            const double tol = std::log(1.0 + std::max(0.5 * resolution, fresh[f].frequency * 0.004) / fresh[f].frequency);
            if (bestD < tol) taken[f] = best;
        }
    }
    // Pass 2: a fresh peak with no near tracker may belong to a tone that moved a few bins since
    // the last analyzer frame (the short window jitters). Glide the SEEN-LATELY tracker toward it
    // instead of adopting a new marker - a jump between two markers is what reads as "too fast".
    // Tight (user bug): only two bins, and only when the level is compatible. A wider radius let
    // a marker hop from peak to peak in dense material, and its slow speed-limited glide across
    // the display read as a spurious portamento sweep.
    const double glideRadius = std::log(1.0 + 2.0 * resolution);
    for (size_t f = 0; f < fresh.size(); ++f) {
        if (taken[f] >= 0) continue;
        int best = -1; double bestD = 1e30;
        for (size_t t = 0; t < g_tracker.size(); ++t) {
            if (matched[t] || g_tracker[t].unseen > 4) continue;
            if (std::abs(g_tracker[t].db - fresh[f].db) > 8.0) continue;   // a different tone's level: never steal it
            const double d = std::abs(std::log(fresh[f].frequency / g_tracker[t].frequency));
            if (d < bestD) { bestD = d; best = (int)t; }
        }
        if (best >= 0 && bestD < glideRadius) taken[f] = best;
    }
    // The glide step for this update: speed-limited, so the marker NEVER jumps frame to frame.
    const double step = std::max(0.0, maxGlideHzPerSec) * std::max(0.001, dt);
    std::vector<Peak> out;
    const size_t known = matched.size();   // trackers that existed before this frame (adopting grows g_tracker)
    out.reserve(fresh.size());
    for (size_t f = 0; f < fresh.size(); ++f) {
        const int t = taken[f];
        if (t < 0) {   // no tracker: adopt immediately (new tones appear, they do not fade in)
            if ((int)g_tracker.size() < 32) g_tracker.push_back({fresh[f].frequency, fresh[f].db, 0});
            out.push_back(fresh[f]);
            continue;
        }
        Tracked& tr = g_tracker[(size_t)t];
        matched[(size_t)t] = true;
        tr.unseen = 0;
        // Position: glide toward the measurement at the speed limit. A genuinely different tone
        // (more than three octaves away) snaps instead of crawling across the display.
        const double d = fresh[f].frequency - tr.frequency;
        if (std::abs(std::log(fresh[f].frequency / std::max(1.0, std::abs(tr.frequency)))) > std::log(8.0))
            tr.frequency = fresh[f].frequency;
        else if (std::abs(d) <= step)
            tr.frequency = fresh[f].frequency;
        else
            tr.frequency += (d > 0 ? step : -step);
        tr.db = tr.db + 0.35 * (fresh[f].db - tr.db);
        out.push_back({tr.frequency, tr.db});
    }
    // Unmatched trackers survive a few frames with a decaying level, so a tone does not blink out
    // on one noisy frame; then they are forgotten. Only the PRE-EXISTING trackers participate:
    // adoptees were just matched and output (iterating the grown vector here would read past 'matched').
    for (size_t t = 0; t < known; ++t) {
        if (!matched[t]) {
            Tracked& tr = g_tracker[t];
            ++tr.unseen;
            if (tr.unseen <= 6 && tr.db - 1.0 * tr.unseen > -120.0) out.push_back({tr.frequency, tr.db - 1.0 * tr.unseen});
        }
    }
    // Drop stale trackers the moment they stop being displayed (the fade window is 6 frames).
    // A marker that lingers past the window can silently re-capture a nearby peak with its old
    // position and then glide slowly toward the real tone - on screen that reads as a spurious
    // portamento sweep (the "messy after switching modes" bug).
    g_tracker.erase(std::remove_if(g_tracker.begin(), g_tracker.end(),
                                   [](const Tracked& tr) { return tr.unseen > 6; }),
                    g_tracker.end());
    // The displayed set: the strongest maxPeaks tones (Better view reads calmly; Realistic passes
    // a huge cap so every detected peak is shown).
    if ((int)out.size() > maxPeaks) {
        std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
        out.resize(maxPeaks);
    }
    return out;
}

// Tracking variant of find(): same pure detection, then speed-limited glide (see track()).
// maxGlideHzPerSec: how fast a marker may move - small = stable, larger = realistic.
inline std::vector<Peak> findTracked(const std::vector<float>& fftDb, double sampleRate, int len, double maxGlideHzPerSec = 150.0, double dt = 0.033, int maxPeaks = 16) {
    return track(find(fftDb, sampleRate, len), sampleRate, len, maxGlideHzPerSec, dt, maxPeaks);
}

// Put the strongest accepted tonal peak in each supplied frequency
// position. Two real tones in the same band share one displayed column.
inline std::vector<monitor_bands::Band> project(const std::vector<monitor_bands::Band>& measured,
                                                const std::vector<Peak>& peaks) {
    auto out = measured;
    for (auto& band : out) band.db = -160;
    for (const Peak& peak : peaks)
        for (auto& band : out)
            if (peak.frequency >= band.lo && peak.frequency < band.hi) {
                band.db = std::max(band.db, peak.db);
                break;
            }
    return out;
}
}

// ---- (was monitor_osc.h) --------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Decoder for the two local OSC messages generated by CawMon.
// OSC integers and blob lengths are network byte order; Csound's "G" table
// blobs contain native MYFLT values (32-bit float or 64-bit double).
namespace monitor_osc {
struct Packet {
    char kind = 0; // 'B' = complete raw stereo frame; 'M' = meter chunk
    uint32_t sequence = 0;
    int start = 0;
    std::vector<float> values;
};
inline uint32_t read32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline bool oscString(const unsigned char* data, size_t length, size_t& pos, std::string& out) {
    if (pos >= length) return false;
    size_t end = pos;
    while (end < length && data[end] != 0) ++end;
    if (end == length) return false;
    out.assign(reinterpret_cast<const char*>(data + pos), end - pos);
    const size_t padded = (end + 4) & ~size_t(3);
    if (padded > length) return false;
    for (size_t i = end; i < padded; ++i) if (data[i] != 0) return false;
    pos = padded;
    return true;
}
inline bool decode(const unsigned char* data, size_t length, Packet& out) {
    size_t pos = 0;
    std::string path, types;
    if (!oscString(data, length, pos, path) || !oscString(data, length, pos, types) ||
        pos + 8 > length) return false;
    const uint32_t seq = read32(data + pos), index = read32(data + pos + 4);
    pos += 8;
    if (seq >= 1000000) return false;
    Packet result;
    result.sequence = seq;
    if (path == "/caw/raw" && types == ",iibb") {
        if (index >= monitor_fft::size) return false;
        result.kind = 'B';
        result.values.resize(monitor_fft::size * 2);
        size_t width = 0;
        for (int ch = 0; ch < 2; ++ch) {
            if (pos + 4 > length) return false;
            const size_t bytes = read32(data + pos);
            pos += 4;
            if (bytes != monitor_fft::size * sizeof(float) &&
                bytes != monitor_fft::size * sizeof(double)) return false;
            if (ch == 0) width = bytes / monitor_fft::size;
            if (bytes / monitor_fft::size != width || bytes > length - pos) return false;
            for (int i = 0; i < monitor_fft::size; ++i) {
                const size_t src = (index + i) % monitor_fft::size;
                double value = 0;
                if (width == sizeof(float)) {
                    float v;
                    std::memcpy(&v, data + pos + src * width, width);
                    value = v;
                } else {
                    std::memcpy(&value, data + pos + src * width, width);
                }
                const float v = static_cast<float>(value);
                if (!std::isfinite(value) || !std::isfinite(v)) return false;
                result.values[(size_t)ch * monitor_fft::size + i] = v;
            }
            pos += bytes;
        }
    } else if (path == "/caw/meter" && types.size() >= 4 && types.size() <= 19 &&
               types.compare(0, 3, ",ii") == 0 && index <= 65535) {
        const size_t count = types.size() - 3;
        if (!std::all_of(types.begin() + 3, types.end(), [](char c) { return c == 'f'; }) ||
            length - pos != count * 4) return false;
        result.kind = 'M';
        result.start = static_cast<int>(index);
        result.values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t bits = read32(data + pos);
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            if (!std::isfinite(v)) return false;
            result.values.push_back(v);
            pos += 4;
        }
    } else return false;
    if (pos != length) return false;
    out = std::move(result);
    return true;
}
// Sequence wraps every 1,000,000 frames. Equal meter sequences can contain
// different channels; raw snapshots use strict ordering.
inline bool newer(uint32_t a, uint32_t b) {
    const uint32_t delta = (a + 1000000u - b) % 1000000u;
    return delta != 0 && delta < 500000u;
}
}

// ---- (was monitor_work.h) --------------------------------------------------------------

// Pure C++ display-work selection, independent of the audio engine and UI.
// Audio capture always continues; only compute the analysis being displayed.
namespace monitor_work {
struct Plan {
    bool fft = false;
    bool thirdBands = false;
    bool columnBands = false;
    bool tonalPeaks = false;
    bool history = false;
};
inline Plan select(bool visible, bool frozen, int mode, int style) {
    Plan p;
    if (!visible || frozen || mode == 1) return p;
    p.fft = true;
    if (mode == 2) p.history = true;
    else if (mode == 0) {
        p.thirdBands = style == 1;
        p.columnBands = style == 3;
        p.tonalPeaks = style == 0;
    }
    return p;
}
}

