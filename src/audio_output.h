#pragma once
#include "audio_device_match.h"
#include "caw_audio_host.h"
#include "monitor_view.h"   // (was monitor_capture.h, phase-1 merge)
#include <portaudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// The soundcard is the clock. Producers submit normalized interleaved audio
// to a bounded SPSC queue; the PortAudio callback copies it and mixes any
// host-pumped real-time source (SuperCollider) into the same device stream.
using CawRealtimeSourceFn = void (*)(void*, float*, unsigned long, int) noexcept;

// MASTER CHAIN: applied to the FINAL device mix of every CAW audio stream (the app installs it:
// master FX chain, then master gain). Called on the audio thread inside the PortAudio callback,
// before the monitor capture. Null / disabled = the mix goes out untouched.
using CawMasterChainFn = void (*)(float*, unsigned long, int) noexcept;
inline CawMasterChainFn g_masterChainFn = nullptr;
inline std::atomic<bool> g_masterChainEnabled{false};
// A freeze render runs without audible playback: the device callback still drives the real-time
// capture engines, but the mix the speaker gets is zeroed right after the freeze capture copied it.
inline std::atomic<bool> g_freezeSilent{false};

// TRACK FREEZE (Cubase-style lock): while the freeze render runs, the audio callback copies the
// final master mix (BEFORE the master FX chain - the frozen WAV must not bake the master) into
// this sink. A writer thread outside the callback turns the copied audio into the track's WAV.
// The callback only touches the atomic flag and the preallocated ring buffer: it never locks
// and never allocates. The sink is owned by the freeze module (track_freeze.h) on the UI side.
struct FreezeCaptureSink {
    static constexpr size_t CAP = 1u << 17;            // frames in the ring (~2.8 s at 48 kHz)
    static constexpr int MAXCH = 8;                    // ring channels (the project mix can use up to 8)
    std::atomic<bool> active{false};
    std::atomic<int> users{0};                        // callbacks currently inside copy()
    std::atomic<int> inChannels{2};                    // channels the callback delivers (1..MAXCH)
    std::array<float, CAP * MAXCH> buf{};              // interleaved ring (inChannels stride)
    std::atomic<size_t> r{0}, w{0};                    // frame positions, wrapping
    unsigned long dropped = 0;                         // frames lost to a full ring (writer too slow)
    void copy(const float* interleaved, unsigned long frames, int deviceChannels) noexcept {
        users.fetch_add(1, std::memory_order_acq_rel);
        if (interleaved && frames && active.load(std::memory_order_acquire)) {
            const int ch = std::max(1, std::min(MAXCH, deviceChannels));
            inChannels.store(ch, std::memory_order_relaxed);
            for (unsigned long i = 0; i < frames; ++i) {
                const size_t wpos = w.load(std::memory_order_relaxed);
                if ((wpos + 1) % CAP == r.load(std::memory_order_acquire)) {
                    r.store((r.load(std::memory_order_relaxed) + 1) % CAP, std::memory_order_release);   // full: drop the oldest
                    ++dropped;
                }
                for (int c = 0; c < ch; ++c)
                    buf[(size_t)wpos * (size_t)ch + (size_t)c] = interleaved[(size_t)i * (size_t)deviceChannels + (size_t)c];
                w.store((wpos + 1) % CAP, std::memory_order_release);
            }
        }
        users.fetch_sub(1, std::memory_order_release);
    }
    void quiesce() noexcept {
        active.store(false, std::memory_order_release);
        while (users.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    }
    bool pop(float* out, size_t frames, int channels) noexcept {   // writer thread: false when the ring has no full block
        if (channels < 1 || channels > MAXCH) return false;
        const size_t wv = w.load(std::memory_order_acquire);
        const size_t rv = r.load(std::memory_order_relaxed);
        const size_t queued = (wv + CAP - rv) % CAP;
        if (queued < frames) return false;
        const int src = std::max(1, std::min(MAXCH, inChannels.load(std::memory_order_relaxed)));
        for (size_t i = 0; i < frames; ++i) {
            const size_t rpos = r.load(std::memory_order_relaxed);
            for (int c = 0; c < channels; ++c)
                out[(size_t)i * (size_t)channels + (size_t)c] =
                    c < src ? buf[(size_t)rpos * (size_t)src + (size_t)c] : 0.f;
            r.store((rpos + 1) % CAP, std::memory_order_release);
        }
        return true;
    }
    void reset() { r.store(0, std::memory_order_release); w.store(0, std::memory_order_release); dropped = 0; }
    size_t queued() const {
        const size_t wv = w.load(std::memory_order_acquire), rv = r.load(std::memory_order_acquire);
        return (wv + CAP - rv) % CAP;
    }
};
inline std::atomic<FreezeCaptureSink*> g_freezeSink{nullptr};   // set/reset by the freeze module, read by the audio callback
inline std::atomic<FreezeCaptureSink*> g_exportSink{nullptr};   // the real-time whole-project export: the FINAL master mix (after the master chain)

struct CawAudioSettings {
    std::string deviceLabel; // Csound's exact device label, empty = default
    int sampleRate = 48000;
    int channels = 2;
    int softwareBuffer = 512;
    int hardwareBuffer = 2048;
    float fullScale = 1.f;
    // Zero keeps the original 4-buffer queue. A host may request a smaller
    // power-of-two queue when low-latency live control matters more.
    int queueFrames = 0;
    CawRealtimeSourceFn realtimeSource = nullptr;
    void* realtimeSourceContext = nullptr;
};

class CawAudioOutput {
public:
    CawAudioOutput() = default;
    CawAudioOutput(const CawAudioOutput&) = delete;
    CawAudioOutput& operator=(const CawAudioOutput&) = delete;
    ~CawAudioOutput() { close(); }
    bool open(const CawAudioSettings& s, std::string& error) {
        close();
        if (s.channels < 1 || s.channels > 64 || s.sampleRate < 1000 || s.sampleRate > 384000 ||
            !std::isfinite(s.fullScale) || s.fullScale <= 0.f) {
            error = "Unsupported audio format."; return false;
        }
        if (!caw_audio_host::begin(&error)) return false;
        hostRef_ = true;
        const int count = Pa_GetDeviceCount();
        if (count < 0) { error = Pa_GetErrorText(count); close(); return false; }
        int selected = paNoDevice, matches = 0;
        if (s.deviceLabel.empty()) selected = Pa_GetDefaultOutputDevice();
        else {
            const auto wanted = normalizeCawDeviceLabel(s.deviceLabel);
            for (int i = 0; i < count; ++i) {
                const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
                if (!d || !d->name || d->maxOutputChannels <= 0) continue;
                const PaHostApiInfo* host = Pa_GetHostApiInfo(d->hostApi);
                if (!host || !host->name) continue;
                const std::string name = std::string(d->name) + " [" + host->name + ", " +
                    std::to_string(d->maxInputChannels) + " in, " + std::to_string(d->maxOutputChannels) + " out]";
                if (lower(name) == wanted) { selected = i; ++matches; }
            }
            if (matches != 1) {
                error = matches > 1 ? "Output device name is ambiguous." :
                    "Selected device/driver is unavailable to CAW's PortAudio. ASIO needs an ASIO-enabled build.";
                close(); return false;
            }
        }
        const PaDeviceInfo* dev = selected == paNoDevice ? nullptr : Pa_GetDeviceInfo(selected);
        if (!dev || dev->maxOutputChannels < 1) {
            error = "Selected output is unavailable."; close(); return false;
        }
        // CAW is a virtual soundcard: the audio setting's channel count IS the mix width every
        // engine writes. It requires a real soundcard with at least that many outputs - a smaller
        // device is an error (user rule: setting 2 folds wide clips 1+3 / 2+4 inside the mix;
        // setting 4 needs 4 real outputs). The engines fold their wider clips into the mix
        // themselves (channel k plays on mix channel k mod N), so no device-side wrap here.
        if ((int)dev->maxOutputChannels < std::max(1, s.channels)) {
            error = "The audio setting needs " + std::to_string(std::max(1, s.channels)) +
                    " output channels, but \"" + (dev->name ? std::string(dev->name) : std::string("?")) +
                    "\" has only " + std::to_string(dev->maxOutputChannels) +
                    ". Set the audio setting to " + std::to_string(dev->maxOutputChannels) +
                    " channels or pick a soundcard with more outputs.";
            close(); return false;
        }
        channels_ = std::max(1, s.channels);
        projectChannels_ = channels_;
        scale_ = s.fullScale;
        realtimeSource_ = s.realtimeSource;
        realtimeSourceContext_ = s.realtimeSourceContext;
        if (realtimeSource_) {
            const size_t scratchFrames = std::max<size_t>(65536, static_cast<size_t>(std::max(s.softwareBuffer, s.hardwareBuffer)));
            realtimeScratch_.assign(scratchFrames * static_cast<size_t>(projectChannels_), 0.f);
        } else realtimeScratch_.clear();
        size_t capacity = 1;
        const int requested = s.queueFrames > 0 ? std::max(64, s.queueFrames) : std::max(512, s.softwareBuffer * 4);
        while (capacity < (size_t)requested) capacity <<= 1;
        capacity_ = capacity;
        data_.assign(capacity_ * (size_t)channels_, 0.f);
        positions_.assign(capacity_, -1);
        head_.store(0); tail_.store(0);
        flushRequest_.store(0); flushDone_.store(0); playedPosition_.store(-1);
        underflows_.store(0);
        expectingAudio_.store(true);
#ifdef _WIN32
        spaceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!spaceEvent_) { error = "Could not create the audio wake event."; close(); return false; }
#endif
        PaStreamParameters output = {};
        output.device = selected;
        output.channelCount = channels_;
        output.sampleFormat = paFloat32;
        output.suggestedLatency = std::max(0.001, (double)s.hardwareBuffer / s.sampleRate);
        unsigned long callbackFrames = static_cast<unsigned long>(std::max(1, s.softwareBuffer));
        if (realtimeSource_) callbackFrames = ((callbackFrames + 63UL) / 64UL) * 64UL;
        PaError e = Pa_OpenStream(&stream_, nullptr, &output, s.sampleRate,
                          callbackFrames, paNoFlag, callback, this);
        if (e != paNoError) { error = std::string("Open output: ") + Pa_GetErrorText(e); close(); return false; }
        e = Pa_StartStream(stream_);
        if (e != paNoError) { error = std::string("Start output: ") + Pa_GetErrorText(e); close(); return false; }
        const PaHostApiInfo* host = Pa_GetHostApiInfo(dev->hostApi);
        description_ = std::string(dev->name ? dev->name : "Output") + " [" +
                       (host && host->name ? host->name : "unknown driver") + "]";
        // The device may run at its own mix-format rate even when another rate was requested
        // (shared-mode drivers): every voice then plays at fileRate/streamRate speed. Name the
        // actual rate in the description so the "playing through" log line exposes a mismatch.
        const PaStreamInfo* si = Pa_GetStreamInfo(stream_);
        actualRate_ = si ? (int)std::lround(si->sampleRate) : s.sampleRate;
        description_ += " @ " + std::to_string(actualRate_) + " Hz";
        if (actualRate_ != s.sampleRate)
            description_ += " (CAW asked for " + std::to_string(s.sampleRate) + " Hz - sounds are pitched/speed-shifted until the project rate matches)";
        return true;
    }
    // Runs on the producer thread, never in the soundcard callback.
    template<class Sample>
    bool push(const Sample* input, unsigned frames, const std::atomic<bool>& stop) {
        return pushTagged(input, frames, stop, -1, nullptr, 0);
    }
    // timelineFrame is the first source frame; -1 marks untimed silence.
    // A changed command generation interrupts a blocked producer promptly.
    template<class Sample>
    bool pushTagged(const Sample* input, unsigned frames, const std::atomic<bool>& stop,
                    std::int64_t timelineFrame, const std::atomic<std::uint64_t>* generation,
                    std::uint64_t expectedGeneration) {
        if (!input || !stream_) return false;
        size_t submitted = 0;
        while (submitted < frames && !stop.load(std::memory_order_relaxed)) {
            if (generation && generation->load(std::memory_order_acquire) != expectedGeneration) return false;
            const uint64_t head = head_.load(std::memory_order_relaxed);
            const uint64_t tail = tail_.load(std::memory_order_acquire);
            const size_t space = capacity_ - (size_t)(head - tail);
            if (!space) {
                if (Pa_IsStreamActive(stream_) <= 0) return false;
#ifdef _WIN32
                // Windows may round sleep_for(1ms) up to ~15ms. Wake as soon
                // as the audio callback consumes data instead.
                WaitForSingleObject(spaceEvent_, 20);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
                continue;
            }
            const size_t n = std::min(space, (size_t)frames - submitted);
            for (size_t i = 0; i < n; ++i) {
                const size_t dst = ((size_t)(head + i) & (capacity_ - 1)) * channels_;
                positions_[(size_t)(head + i) & (capacity_ - 1)] = timelineFrame < 0 ? -1 : timelineFrame + static_cast<std::int64_t>(submitted + i);
                const size_t src = (submitted + i) * channels_;
                for (int c = 0; c < channels_; ++c) {
                    const float value = (float)(input[src + (size_t)c] / scale_);
                    data_[dst + (size_t)c] = std::isfinite(value) ? value : 0.f;
                }
            }
            head_.store(head + n, std::memory_order_release);
            submitted += n;
        }
        return submitted == frames;
    }
    // Called only by the producer. The callback owns the read cursor and
    // acknowledges the discard before the producer enqueues new audio.
    bool discardPending(const std::atomic<bool>& stop) {
        if (!stream_) return false;
        const auto request = flushRequest_.fetch_add(1, std::memory_order_acq_rel) + 1;
        while (flushDone_.load(std::memory_order_acquire) < request && !stop.load(std::memory_order_relaxed)) {
            if (Pa_IsStreamActive(stream_) <= 0) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !stop.load(std::memory_order_relaxed);
    }
    std::int64_t playedPosition() const { return playedPosition_.load(std::memory_order_acquire); }
    void clearPlayedPosition() { playedPosition_.store(-1, std::memory_order_release); }
    const std::string& description() const { return description_; }
    int actualRate() const { return actualRate_; }   // the rate the device really opened at (may differ from the request)
    int projectChannels() const { return projectChannels_ > 0 ? projectChannels_ : channels_; }   // the project mix width this stream renders
    int deviceChannels() const { return channels_; }   // what the soundcard really opened with
    uint64_t underflows() const { return underflows_.load(std::memory_order_relaxed); }
    void setExpectingAudio(bool value) { expectingAudio_.store(value,std::memory_order_relaxed); }
    // Set only before open or after close. The capture must outlive the stream.
    void setMonitorCapture(MonitorCapture* capture) { capture_ = capture; }
    void close() {
        if (stream_) { Pa_AbortStream(stream_); Pa_CloseStream(stream_); stream_ = nullptr; }
#ifdef _WIN32
        if (spaceEvent_) { CloseHandle(spaceEvent_); spaceEvent_ = nullptr; }
#endif
        if (hostRef_) { caw_audio_host::end(); hostRef_ = false; }
    }
private:
    static std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    static int callback(const void*, void* out, unsigned long frames,
                        const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* user) {
        auto& self = *static_cast<CawAudioOutput*>(user);
        float* dst = static_cast<float*>(out);
        const auto request = self.flushRequest_.load(std::memory_order_acquire);
        if (request != self.flushDone_.load(std::memory_order_relaxed)) {
            self.tail_.store(self.head_.load(std::memory_order_acquire), std::memory_order_release);
            std::fill(dst, dst + (size_t)frames * self.channels_, 0.f);
            self.mixRealtime(dst, frames);
            self.playedPosition_.store(-1, std::memory_order_release);
            self.flushDone_.store(request, std::memory_order_release);
#ifdef _WIN32
            if (self.spaceEvent_) SetEvent(self.spaceEvent_);
#endif
            return paContinue;
        }
        const uint64_t tail = self.tail_.load(std::memory_order_relaxed);
        const uint64_t head = self.head_.load(std::memory_order_acquire);
        const size_t n = std::min((size_t)frames, (size_t)(head - tail));
        for (size_t i = 0; i < n; ++i) {
            const size_t src = ((size_t)(tail + i) & (self.capacity_ - 1)) * self.channels_;
            for (int c = 0; c < self.channels_; ++c)
                dst[i * self.channels_ + c] = self.data_[src + (size_t)c];
        }
        if (n) {
            const auto stamp = self.positions_[(size_t)(tail + n - 1) & (self.capacity_ - 1)];
            if (stamp >= 0) self.playedPosition_.store(stamp + 1, std::memory_order_release);
        }
        std::fill(dst + n * self.channels_, dst + (size_t)frames * self.channels_, 0.f);
        self.mixRealtime(dst, frames);
        if (n < frames && head > 0 && self.expectingAudio_.load(std::memory_order_relaxed)) self.underflows_.fetch_add(1, std::memory_order_relaxed);
        self.tail_.store(tail + n, std::memory_order_release);
#ifdef _WIN32
        if (self.spaceEvent_) SetEvent(self.spaceEvent_);
#endif
        return paContinue;
    }
    void mixRealtime(float* output, unsigned long frames) noexcept {
        // The hook renders the PROJECT's channel count (out1..outN). Everything downstream (the
        // freeze capture, the silence gate, the master chain, the export capture, the meters)
        // rides that full-width mix. The device was opened with exactly that width (open() errors
        // on a smaller card), so the copy below is 1:1 - the engines fold their wider clips into
        // the mix (channel k plays on mix channel k mod N), the virtual soundcard's one rule.
        if (!realtimeSource_ || !output) return;
        const int projCh = projectChannels_ > 0 ? projectChannels_ : channels_;
        const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(projCh);
        if (count > realtimeScratch_.size()) return;   // callback must never allocate
        float* proj = realtimeScratch_.data();
        std::fill(proj, proj + count, 0.f);
        realtimeSource_(realtimeSourceContext_, proj, frames, projCh);
        const int devCh = channels_;
        // the device content already in output (e.g. Csound's own pushed blocks) joins the project mix
        for (unsigned long i = 0; i < frames; ++i)
            for (int c = 0; c < devCh; ++c)
                proj[(size_t)i * (size_t)projCh + (size_t)(c % projCh)] += output[(size_t)i * (size_t)devCh + (size_t)c];
        if (FreezeCaptureSink* sink = g_freezeSink.load(std::memory_order_acquire))
            sink->copy(proj, frames, projCh);   // freeze render: the raw master mix (pre master FX)
        if (g_freezeSilent.load(std::memory_order_acquire)) std::fill(proj, proj + count, 0.f);   // the render must be silent
        if (g_masterChainEnabled.load(std::memory_order_relaxed) && g_masterChainFn)
            g_masterChainFn(proj, frames, projCh);
        if (FreezeCaptureSink* sink = g_exportSink.load(std::memory_order_acquire))
            sink->copy(proj, frames, projCh);   // real-time export: the FINAL project mix (post master FX)
        captureOutput(proj, frames);
        for (unsigned long i = 0; i < frames; ++i)
            for (int c = 0; c < devCh; ++c)
                output[(size_t)i * (size_t)devCh + (size_t)c] = proj[(size_t)i * (size_t)projCh + (size_t)(c % projCh)];
    }
    void captureOutput(const float* samples, unsigned long frames) noexcept {
        if (!capture_) return;
        for (int c = 0; c < channels_; ++c) {
            float peak = 0.f;
            for (unsigned long i = 0; i < frames; ++i)
                peak = std::max(peak, std::fabs(samples[(size_t)i * channels_ + c]));
            capture_->addMeter(c, peak);
        }
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        capture_->pushInterleaved(samples, static_cast<int>(frames), channels_, now);
    }
    PaStream* stream_ = nullptr;
    MonitorCapture* capture_ = nullptr;
#ifdef _WIN32
    HANDLE spaceEvent_ = nullptr;
#endif
    bool hostRef_ = false;
    int channels_ = 0;
    float scale_ = 1.f;
    size_t capacity_ = 0;
    std::vector<float> data_;
    std::vector<std::int64_t> positions_;
    std::string description_;
    int actualRate_ = 0;
    int projectChannels_ = 0;   // the project mix width the hook renders (out1..outN)
    CawRealtimeSourceFn realtimeSource_ = nullptr;
    void* realtimeSourceContext_ = nullptr;
    std::vector<float> realtimeScratch_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    std::atomic<uint64_t> underflows_{0};
    std::atomic<bool> expectingAudio_{true};
    std::atomic<std::uint64_t> flushRequest_{0}, flushDone_{0};
    std::atomic<std::int64_t> playedPosition_{-1};
};
