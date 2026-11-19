#pragma once
#include "test_tone_samples.h"
#include "audio_device_match.h"
#include "caw_audio_host.h"
#include <portaudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Tests CAW's selected PortAudio device directly. No Csound engine is involved.
// Match the device AND host API name: numeric Csound dac indexes count only
// output devices and can differ from indexes in this PortAudio build.
class TestSound {
public:
    TestSound() = default;
    TestSound(const TestSound&) = delete;
    TestSound& operator=(const TestSound&) = delete;
    ~TestSound() { if (worker_.joinable()) worker_.join(); }
    bool running() const { return running_.load(std::memory_order_acquire); }
    std::string message() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return message_;
    }
    // selected: exact label from Csound's output-device list, or empty for
    // the same PortAudio default used when Csound has no resolved device.
    void start(const std::string& selected, bool none, int rate, int channels, int buffer, int hardwareBuffer) {
        if (running()) return;
        if (worker_.joinable()) worker_.join();
        if (none) { setMessage("No output device is selected. Choose a soundcard first."); return; }
        setMessage("Opening the selected output for a short test tone...");
        running_.store(true, std::memory_order_release);
        try {
            worker_ = std::thread([this, selected, rate, channels, buffer, hardwareBuffer]() {
                try { play(selected, rate, channels, buffer, hardwareBuffer); }
                catch (const std::exception& e) { setMessage(std::string("Test sound failed: ") + e.what()); }
                catch (...) { setMessage("Test sound failed unexpectedly."); }
                running_.store(false, std::memory_order_release);
            });
        } catch (const std::exception& e) {
            setMessage(std::string("Could not start test sound: ") + e.what());
            running_.store(false, std::memory_order_release);
        }
    }
private:
    struct Playback {
        std::vector<int16_t> pcm;
        size_t cursor = 0;
        int channels = 2;
    };
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::string message_;
    std::thread worker_;
    void setMessage(const std::string& s) {
        std::lock_guard<std::mutex> guard(mutex_);
        message_ = s;
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    static int callback(const void*, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* user) {
        auto& data = *static_cast<Playback*>(user);
        auto* dst = static_cast<int16_t*>(output);
        const size_t count = (size_t)frames * (size_t)data.channels;
        const size_t available = data.pcm.size() - data.cursor;
        const size_t n = std::min(count, available);
        std::copy_n(data.pcm.data() + data.cursor, n, dst);
        std::fill(dst + n, dst + count, 0);
        data.cursor += n;
        return data.cursor == data.pcm.size() ? paComplete : paContinue;
    }
    void play(const std::string& selected, int rate, int channels, int buffer, int hardwareBuffer) {
        std::string initError;
        caw_audio_host::Ref paHost(&initError);
        if (!paHost.ok) { setMessage(std::string("PortAudio initialization failed: ") + initError); return; }
        const int count = Pa_GetDeviceCount();
        if (count < 0) { setMessage(std::string("Device enumeration failed: ") + Pa_GetErrorText(count)); return; }
        int chosen = paNoDevice, matches = 0;
        if (selected.empty()) {
            chosen = Pa_GetDefaultOutputDevice();
        } else {
            const std::string wanted = normalizeCawDeviceLabel(selected);
            for (int i = 0; i < count; ++i) {
                const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
                if (!d || d->maxOutputChannels <= 0 || !d->name) continue;
                const PaHostApiInfo* host = Pa_GetHostApiInfo(d->hostApi);
                if (!host || !host->name) continue;
                const std::string candidate = std::string(d->name) + " [" + host->name + ", " +
                    std::to_string(d->maxInputChannels) + " in, " + std::to_string(d->maxOutputChannels) + " out]";
                if (lower(candidate) == wanted) { chosen = i; ++matches; }
            }
            if (matches != 1) {
                setMessage(matches > 1 ? "Test sound: multiple identical devices matched. Cannot safely choose one."
                    : "Test sound: the selected Csound device was not found in CAW's PortAudio build. Refresh devices; ASIO needs a PortAudio build with the ASIO SDK.");
                return;
            }
        }
        const PaDeviceInfo* device = chosen == paNoDevice ? nullptr : Pa_GetDeviceInfo(chosen);
        if (!device || device->maxOutputChannels <= 0) {
            setMessage("Test sound: no default output is available. Select an output device.");
            return;
        }
        rate = std::max(1000, std::min(384000, rate));
        channels = std::max(2, std::min(64, channels));
        if (channels > device->maxOutputChannels) {
            setMessage("Test sound: the selected device has fewer outputs than the project's channel count.");
            return;
        }
        Playback data;
        data.channels = channels;
        data.pcm = makeTestTone(rate, channels);
        PaStreamParameters output = {};
        output.device = chosen;
        output.channelCount = channels;
        output.sampleFormat = paInt16;
        output.suggestedLatency = std::max(0.001, (double)hardwareBuffer / rate);
        PaStream* stream = nullptr;
        PaError error = Pa_OpenStream(&stream, nullptr, &output, rate,
                              (unsigned long)std::max(1, buffer), paNoFlag, callback, &data);
        if (error != paNoError) {
            setMessage(std::string("Could not open selected output: ") + Pa_GetErrorText(error) +
                       ". Stop Csound if the device is busy, then retry.");
            return;
        }
        struct Close { PaStream* s; ~Close() { Pa_CloseStream(s); } } close{stream};
        error = Pa_StartStream(stream);
        if (error != paNoError) { setMessage(std::string("Test sound could not start: ") + Pa_GetErrorText(error)); return; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        int active = 1;
        while ((active = Pa_IsStreamActive(stream)) > 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (active > 0) {
            Pa_AbortStream(stream);
            setMessage("Test sound timed out. Check the selected device or stop Csound and retry.");
        } else if (active < 0) {
            Pa_AbortStream(stream);
            setMessage(std::string("Test sound failed: ") + Pa_GetErrorText(active));
        } else {
            const PaHostApiInfo* host = Pa_GetHostApiInfo(device->hostApi);
            const std::string hostName = host && host->name ? host->name : "unknown host";
            setMessage(std::string("Test sound sent to ") + device->name + " [" + hostName + "]. Did you hear it?");
        }
    }
};
