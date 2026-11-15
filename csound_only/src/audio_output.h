#pragma once
#include "audio_device_match.h"
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

// The soundcard is the clock. Producers submit normalized interleaved audio
// to a bounded SPSC queue; the PortAudio callback only copies samples.
struct CawAudioSettings {
    std::string deviceLabel; // Csound's exact device label, empty = default
    int sampleRate = 48000;
    int channels = 2;
    int softwareBuffer = 512;
    int hardwareBuffer = 2048;
    float fullScale = 1.f;
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
        size_t capacity = 1;
        while (capacity < (size_t)std::max(512, s.softwareBuffer * 4)) capacity <<= 1;
        capacity_ = capacity;
        data_.assign(capacity_ * (size_t)channels_, 0.f);
        head_.store(0); tail_.store(0);
        underflows_.store(0);
        PaStreamParameters output = {};
        output.device = selected;
        output.channelCount = channels_;
        output.sampleFormat = paFloat32;
        output.suggestedLatency = std::max(0.001, (double)s.hardwareBuffer / s.sampleRate);
        e = Pa_OpenStream(&stream_, nullptr, &output, s.sampleRate,
                          (unsigned long)std::max(1, s.softwareBuffer), paNoFlag, callback, this);
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
        if (!input || !stream_) return false;
        size_t submitted = 0;
        while (submitted < frames && !stop.load(std::memory_order_relaxed)) {
            const uint64_t head = head_.load(std::memory_order_relaxed);
            const uint64_t tail = tail_.load(std::memory_order_acquire);
            const size_t space = capacity_ - (size_t)(head - tail);
            if (!space) {
                if (Pa_IsStreamActive(stream_) <= 0) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const size_t n = std::min(space, (size_t)frames - submitted);
            for (size_t i = 0; i < n; ++i) {
                const size_t dst = ((size_t)(head + i) & (capacity_ - 1)) * channels_;
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
    const std::string& description() const { return description_; }
    uint64_t underflows() const { return underflows_.load(std::memory_order_relaxed); }
    void close() {
        if (stream_) { Pa_AbortStream(stream_); Pa_CloseStream(stream_); stream_ = nullptr; }
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
        const uint64_t tail = self.tail_.load(std::memory_order_relaxed);
        const uint64_t head = self.head_.load(std::memory_order_acquire);
        const size_t n = std::min((size_t)frames, (size_t)(head - tail));
        for (size_t i = 0; i < n; ++i) {
            const size_t src = ((size_t)(tail + i) & (self.capacity_ - 1)) * self.channels_;
            for (int c = 0; c < self.channels_; ++c)
                dst[i * self.channels_ + c] = self.data_[src + (size_t)c];
        }
        std::fill(dst + n * self.channels_, dst + (size_t)frames * self.channels_, 0.f);
        if (n < frames && head > 0) self.underflows_.fetch_add(1, std::memory_order_relaxed);
        self.tail_.store(tail + n, std::memory_order_release);
        return paContinue;
    }
    PaStream* stream_ = nullptr;
    bool initialized_ = false;
    int channels_ = 0;
    float scale_ = 1.f;
    size_t capacity_ = 0;
    std::vector<float> data_;
    std::string description_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    std::atomic<uint64_t> underflows_{0};
};
