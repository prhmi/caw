#pragma once
#include "audio_device_match.h"
#include "monitor_capture.h"
#include <portaudio.h>
#include <algorithm>
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
        PaError e = Pa_Initialize();
        if (e != paNoError) { error = std::string("PortAudio initialization: ") + Pa_GetErrorText(e); return false; }
        initialized_ = true;
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
        if (!dev || dev->maxOutputChannels < s.channels) {
            error = "Selected output is unavailable or has too few channels."; close(); return false;
        }
        channels_ = s.channels;
        scale_ = s.fullScale;
        realtimeSource_ = s.realtimeSource;
        realtimeSourceContext_ = s.realtimeSourceContext;
        if (realtimeSource_) {
            const size_t scratchFrames = std::max<size_t>(65536, static_cast<size_t>(std::max(s.softwareBuffer, s.hardwareBuffer)));
            realtimeScratch_.assign(scratchFrames * static_cast<size_t>(channels_), 0.f);
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
        e = Pa_OpenStream(&stream_, nullptr, &output, s.sampleRate,
                          callbackFrames, paNoFlag, callback, this);
        if (e != paNoError) { error = std::string("Open output: ") + Pa_GetErrorText(e); close(); return false; }
        e = Pa_StartStream(stream_);
        if (e != paNoError) { error = std::string("Start output: ") + Pa_GetErrorText(e); close(); return false; }
        const PaHostApiInfo* host = Pa_GetHostApiInfo(dev->hostApi);
        description_ = std::string(dev->name ? dev->name : "Output") + " [" +
                       (host && host->name ? host->name : "unknown driver") + "]";
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
    uint64_t underflows() const { return underflows_.load(std::memory_order_relaxed); }
    void setExpectingAudio(bool value) { expectingAudio_.store(value,std::memory_order_relaxed); }
    // Set only before open or after close. The capture must outlive the stream.
    void setMonitorCapture(MonitorCapture* capture) { capture_ = capture; }
    void close() {
        if (stream_) { Pa_AbortStream(stream_); Pa_CloseStream(stream_); stream_ = nullptr; }
#ifdef _WIN32
        if (spaceEvent_) { CloseHandle(spaceEvent_); spaceEvent_ = nullptr; }
#endif
        if (initialized_) { Pa_Terminate(); initialized_ = false; }
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
            self.captureOutput(dst, frames);
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
        self.captureOutput(dst, frames);
        if (n < frames && head > 0 && self.expectingAudio_.load(std::memory_order_relaxed)) self.underflows_.fetch_add(1, std::memory_order_relaxed);
        self.tail_.store(tail + n, std::memory_order_release);
#ifdef _WIN32
        if (self.spaceEvent_) SetEvent(self.spaceEvent_);
#endif
        return paContinue;
    }
    void mixRealtime(float* output, unsigned long frames) noexcept {
        if (!realtimeSource_ || !output) return;
        const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels_);
        if (count > realtimeScratch_.size()) return; // callback must never allocate
        std::fill(realtimeScratch_.begin(), realtimeScratch_.begin() + count, 0.f);
        realtimeSource_(realtimeSourceContext_, realtimeScratch_.data(), frames, channels_);
        for (size_t i = 0; i < count; ++i) output[i] += realtimeScratch_[i];
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
    bool initialized_ = false;
    int channels_ = 0;
    float scale_ = 1.f;
    size_t capacity_ = 0;
    std::vector<float> data_;
    std::vector<std::int64_t> positions_;
    std::string description_;
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
