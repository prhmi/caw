#pragma once
// CAW AUDIO INPUT (the microphone / line inputs of the soundcard). ONE capture stream for the whole
// app, opened on the project's input device; every engine reads it at its own pace:
//   Csound          ain inch 1  /  aL, aR ins     (real Csound inputs - CAW feeds spin from this)
//   Pure Data       [adc~ 1 2 ...]                (the REAL inputs again - the CAW cables have their own objects)
//   SuperCollider   SoundIn.ar([0, 1])            (the server's first input buses)
//   any engine      the cable "mic" carries inputs 1-2 (caw_audio_bus.h), e.g. cawbrecv "mic"
// Like the cables it is a ring: each reader keeps its own position, a reader that falls behind
// jumps to the newest audio. The capture callback never blocks.
#include <portaudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include "caw_audio_host.h"

namespace cawin {
constexpr int kMaxCh = 8;
constexpr size_t kCap = 1u << 15;   // 32768 frames ring
struct State {
    std::array<float, kCap * kMaxCh> buf{};
    std::atomic<std::uint64_t> head{0};   // frames ever captured
    std::atomic<int> channels{0};          // 0 = no input open
    PaStream* stream = nullptr;
    std::mutex life;
    std::string device;
    int rate = 0;
};
inline State& state() { static State* s = new State; return *s; }
inline int callback(const void* in, void*, unsigned long frames, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    State& s = state();
    const int ch = s.channels.load(std::memory_order_relaxed);
    const float* src = static_cast<const float*>(in);
    std::uint64_t h = s.head.load(std::memory_order_relaxed);
    for (unsigned long f = 0; f < frames; ++f) {
        float* dst = &s.buf[(size_t)((h + f) % kCap) * kMaxCh];
        for (int c = 0; c < kMaxCh; ++c) dst[c] = (src && c < ch) ? src[f * (unsigned long)ch + (unsigned long)c] : 0.f;
    }
    s.head.store(h + frames, std::memory_order_release);
    return paContinue;
}
// deviceIndex: a PortAudio input device index, -1 = the default input. channels 1..8.
inline bool open(int deviceIndex, int sampleRate, int channels, std::string& error) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.life);
    if (s.stream) return true;
    if (!caw_audio_host::begin(&error)) return false;
    const PaDeviceIndex dev = deviceIndex >= 0 ? deviceIndex : Pa_GetDefaultInputDevice();
    const PaDeviceInfo* info = dev != paNoDevice ? Pa_GetDeviceInfo(dev) : nullptr;
    if (!info || info->maxInputChannels < 1) { error = "No audio input device is available."; return false; }
    const int ch = std::max(1, std::min({kMaxCh, channels, (int)info->maxInputChannels}));
    PaStreamParameters p{};
    p.device = dev; p.channelCount = ch; p.sampleFormat = paFloat32;
    p.suggestedLatency = info->defaultLowInputLatency;
    s.channels.store(ch, std::memory_order_release);
    PaError e = Pa_OpenStream(&s.stream, &p, nullptr, sampleRate, 0, paNoFlag, callback, nullptr);
    if (e == paNoError) e = Pa_StartStream(s.stream);
    if (e != paNoError) {
        if (s.stream) Pa_CloseStream(s.stream);
        s.stream = nullptr; s.channels.store(0);
        error = std::string("Open input: ") + Pa_GetErrorText(e);
        return false;
    }
    s.device = info->name ? info->name : "input"; s.rate = sampleRate;
    return true;
}
inline void close() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.life);
    if (!s.stream) return;
    Pa_StopStream(s.stream); Pa_CloseStream(s.stream); s.stream = nullptr;
    s.channels.store(0, std::memory_order_release);
}
inline bool isOpen() { return state().channels.load(std::memory_order_acquire) > 0; }
inline int channels() { return state().channels.load(std::memory_order_acquire); }
// One reader (an engine). It stays `lag` frames behind the capture head so it never reads a
// half-written block; drift beyond that (an engine paused) resyncs to the newest audio.
struct Reader {
    std::uint64_t pos = 0;
    bool started = false;
    // out: frames * outCh interleaved (input channel c -> out channel c, the rest silent)
    void read(float* out, size_t frames, int outCh, size_t lag = 512) {   // lag 0 = read exactly from pos (a pump that tracks the head itself)
        if (!out || frames == 0 || outCh < 1) return;
        std::memset(out, 0, frames * (size_t)outCh * sizeof(float));
        State& s = state();
        const int ch = s.channels.load(std::memory_order_acquire);
        if (ch <= 0) { started = false; return; }
        const std::uint64_t h = s.head.load(std::memory_order_acquire);
        const std::uint64_t want = h > lag + frames ? h - lag - frames : 0;
        if (lag > 0 && (!started || pos + kCap / 4 < want || pos > want + 4 * lag + frames)) { pos = want; started = true; }
        if (lag == 0 && (!started || pos + kCap / 2 < h)) { pos = h > frames ? h - frames : 0; started = true; }
        const int n = std::min(ch, outCh);
        for (size_t f = 0; f < frames; ++f) {
            const std::uint64_t at = pos + f;
            if (at >= h) break;
            const float* src = &s.buf[(size_t)(at % kCap) * kMaxCh];
            for (int c = 0; c < n; ++c) out[f * (size_t)outCh + (size_t)c] = src[c];
        }
        pos += frames;
    }
};
}  // namespace cawin
