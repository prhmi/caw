#pragma once

#include "process_quote.h"
#include "process_runner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// sclang is the language client; scsynth is embedded in CAW and pumped by the
// existing PortAudio callback. OSC is used only for language/server commands.
class ScEngine {
public:
    static constexpr int languagePort = 58120;
    static constexpr int serverPort = 58110;
    // 64-sample SC blocks reduce per-callback scheduling overhead while
    // keeping the usual low-latency SC block size.
    static constexpr int controlBlockSize = 64;
    // Reserve the maximum supported hardware bus range so SC can coexist
    // with Csound output layouts that change after SC has started.
    static constexpr int serverOutputChannels = 64;
    static constexpr int maxTrackSlots = 32;

    ScEngine() {
        for (auto& track : trackPeaks_)
            for (auto& peak : track) peak.store(0.f, std::memory_order_relaxed);
    }
    ~ScEngine() { stop(); }
    ScEngine(const ScEngine&) = delete;
    ScEngine& operator=(const ScEngine&) = delete;

    bool startEmbedded(const std::string& libraryPath, int sampleRate, int channels, std::string& error) {
        if (serverActive_.load(std::memory_order_acquire) && world_.load(std::memory_order_acquire) &&
            libraryPath_ == libraryPath && sampleRate_ == std::max(8000, sampleRate) &&
            outputChannels_ == std::max(1, channels)) {
            renderEnabled_.store(true, std::memory_order_release);
            return true;
        }
        stopEmbedded();
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "Could not initialize Windows networking for the embedded SuperCollider server.";
            return false;
        }
        winsock_ = true;
        const std::wstring libraryWide = std::filesystem::u8path(libraryPath).wstring();
        library_ = LoadLibraryExW(libraryWide.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library_) library_ = LoadLibraryW(libraryWide.c_str());
        if (!library_) {
            error = "Could not load \"" + libraryPath + "\" (Windows error " +
                std::to_string(GetLastError()) + "). Check that its dependent DLLs are beside it.";
            closeWinsock();
            return false;
        }
        auto symbol = [&](const char* name) -> void* { return reinterpret_cast<void*>(GetProcAddress(library_, name)); };
#else
        library_ = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library_) { const char* why = dlerror(); error = "Could not load \"" + libraryPath + "\": " + (why ? why : "unknown error"); return false; }
        auto symbol = [&](const char* name) -> void* { return dlsym(library_, name); };
#endif
        const auto abiVersion = reinterpret_cast<AbiVersionFn>(symbol("caw_sc_abi_version"));
        if (!abiVersion || abiVersion() < 4) {
            error = "This libscsynth has the old CAW audio ABI. Rebuild it with the latest supplied script.";
            stopEmbedded();
            return false;
        }
        create_ = reinterpret_cast<CreateFn>(symbol("caw_sc_world_new"));
        lastError_ = reinterpret_cast<LastErrorFn>(symbol("caw_sc_last_error"));
        pump_ = reinterpret_cast<PumpFn>(symbol("caw_sc_world_process"));
        destroy_ = reinterpret_cast<DestroyFn>(symbol("caw_sc_world_free"));
        if (!create_ || !pump_ || !destroy_) {
            error = "The selected libscsynth is missing CAW's host-pump entry points. Rebuild it with the supplied script.";
            stopEmbedded();
            return false;
        }
        serverPort_ = serverPort;
        void* world = create_(std::max(8000, sampleRate), controlBlockSize, std::max(1, channels),
                              serverPort, &serverPort_);
        if (!world) {
            const int reason = lastError_ ? lastError_() : 0;
            error = reason == 1
                ? "libscsynth could not create the embedded synthesis world."
                : reason == 2
                    ? "No free local SuperCollider OSC port was found in the 58110-58174 range."
                    : "libscsynth could not create its host-driven server or bind its local OSC port.";
            stopEmbedded();
            return false;
        }
        world_.store(world, std::memory_order_release);
        libraryPath_ = libraryPath;
        sampleRate_ = std::max(8000, sampleRate);
        outputChannels_ = std::max(1, channels);
        renderEnabled_.store(true, std::memory_order_release);
        serverActive_.store(true, std::memory_order_release);
        return true;
    }

    bool canReuseWorld(const std::string& libraryPath, int sampleRate, int channels) const {
        return serverActive_.load(std::memory_order_acquire) &&
               world_.load(std::memory_order_acquire) != nullptr &&
               libraryPath_ == libraryPath &&
               sampleRate_ == std::max(8000, sampleRate) &&
               outputChannels_ == std::max(1, channels);
    }

    bool start(const std::string& executable, const std::string& bootstrap, std::string& error) {
        stopLanguage();
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalidSocket()) {
            error = "Could not create the local OSC socket for SuperCollider.";
            closeSocket();
            return false;
        }
        process_.setHighPriority(true);
        const std::string command = processArg(executable) + " -D -u " +
            std::to_string(languagePort) + " " + processArg(bootstrap);
        if (!process_.start(command, false)) {
            error = process_.lastError();
            closeSocket();
            return false;
        }
        return true;
    }

    void stopLanguage() {
        if (process_.running()) {
            (void)sendMessage("/caw/quit", ",", {});
            for (int i = 0; i < 30 && process_.running(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        process_.stop();
        closeSocket();
    }

    void stopEmbedded() {
        renderEnabled_.store(false, std::memory_order_release);
        serverActive_.store(false, std::memory_order_release);
        while (activeCallbacks_.load(std::memory_order_acquire) != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        void* world = world_.exchange(nullptr, std::memory_order_acq_rel);
        if (world && destroy_) destroy_(world);
        create_ = nullptr; lastError_ = nullptr; pump_ = nullptr; destroy_ = nullptr;
        serverPort_ = serverPort;
        libraryPath_.clear();
        sampleRate_ = 0;
        outputChannels_ = 0;
        clearPeaks();
#ifdef _WIN32
        if (library_) { FreeLibrary(library_); library_ = nullptr; }
        closeWinsock();
#else
        if (library_) { dlclose(library_); library_ = nullptr; }
#endif
    }

    void stop() { stopLanguage(); stopEmbedded(); }
    bool embedded() const { return serverActive_.load(std::memory_order_acquire); }
    int boundServerPort() const { return serverPort_; }
    void setRendering(bool enabled) { renderEnabled_.store(enabled, std::memory_order_release); }
    float takeTrackPeak(int slot, int channel) noexcept {
        if (slot < 0 || slot >= maxTrackSlots || channel < 0 || channel > 1) return 0.f;
        return trackPeaks_[static_cast<size_t>(slot)][static_cast<size_t>(channel)].exchange(0.f, std::memory_order_acq_rel);
    }
    bool running() const { return process_.running(); }
    bool active() const { return process_.active(); }
    bool finished() const { return process_.finished(); }
    int exitCode() const { return process_.exitCode(); }
    std::vector<std::string> takeOutput() { return process_.takeOutput(); }
    void setGain(float gain) { gain_.store(std::isfinite(gain) ? gain : 1.f, std::memory_order_relaxed); }

    // Called by CAW's PortAudio callback. The driver processes one aligned
    // block and writes interleaved float samples into the shared output stream.
    void process(float* output, unsigned long frames, int channels) noexcept {
        if (!output || frames == 0 || channels <= 0) return;
        std::fill(output, output + static_cast<size_t>(frames) * channels, 0.f);
        if (!serverActive_.load(std::memory_order_acquire) || !renderEnabled_.load(std::memory_order_acquire)) return;
        activeCallbacks_.fetch_add(1, std::memory_order_acq_rel);
        if (!serverActive_.load(std::memory_order_acquire) || !renderEnabled_.load(std::memory_order_acquire)) {
            activeCallbacks_.fetch_sub(1, std::memory_order_release);
            return;
        }
        const auto pump = pump_;
        void* world = world_.load(std::memory_order_acquire);
        std::array<float, maxTrackSlots * 2> trackMeters{};
        if (pump && world) pump(world, nullptr, 0, output, channels, static_cast<int>(frames),
                                trackMeters.data(), maxTrackSlots);
        const float gain = gain_.load(std::memory_order_relaxed);
        if (gain != 1.f) for (size_t i = 0; i < static_cast<size_t>(frames) * channels; ++i) output[i] *= gain;
        for (int slot = 0; slot < maxTrackSlots; ++slot)
            for (int channel = 0; channel < 2; ++channel) {
                const float peak = trackMeters[static_cast<size_t>(slot) * 2 + channel] * gain;
                auto& value = trackPeaks_[static_cast<size_t>(slot)][static_cast<size_t>(channel)];
                float seen = value.load(std::memory_order_relaxed);
                while (seen < peak && !value.compare_exchange_weak(seen, peak, std::memory_order_release,
                                                                    std::memory_order_relaxed)) {}
            }
        activeCallbacks_.fetch_sub(1, std::memory_order_release);
    }

    bool startClip(int instance, const std::string& path, float delay, float duration,
                   float p4, float p5, int trackSlot, int clipId) {
        std::vector<unsigned char> args;
        appendInt(args, instance); appendString(args, path);
        appendFloat(args, delay); appendFloat(args, duration); appendFloat(args, p4); appendFloat(args, p5);
        appendInt(args, trackSlot); appendInt(args, clipId);
        return sendMessage("/caw/start", ",isffffii", args);
    }
    bool stopClip(int instance) {
        std::vector<unsigned char> args; appendInt(args, instance);
        return sendMessage("/caw/stop", ",i", args);
    }
    bool stopAll() { return sendMessage("/caw/stopAll", ",", {}); }
    bool resumeLanguage() { return sendMessage("/caw/resume", ",", {}); }

private:
    using CreateFn = void* (*)(int, int, int, int, int*);
    using AbiVersionFn = int (*)();
    using LastErrorFn = int (*)();
    using PumpFn = void (*)(void*, const float*, int, float*, int, int, float*, int);
    using DestroyFn = void (*)(void*);
#ifdef _WIN32
    using Socket = SOCKET;
    HMODULE library_ = nullptr;
#else
    using Socket = int;
    void* library_ = nullptr;
#endif
    ProcessRunner process_;
    Socket socket_ = invalidSocket();
    CreateFn create_ = nullptr;
    LastErrorFn lastError_ = nullptr;
    PumpFn pump_ = nullptr;
    DestroyFn destroy_ = nullptr;
    std::atomic<void*> world_{nullptr};
    std::atomic<bool> serverActive_{false}, renderEnabled_{false};
    std::atomic<int> activeCallbacks_{0};
    std::atomic<float> gain_{1.f};
    std::array<std::array<std::atomic<float>, 2>, maxTrackSlots> trackPeaks_{};
    int serverPort_ = serverPort;
    std::string libraryPath_;
    int sampleRate_ = 0;
    int outputChannels_ = 0;
#ifdef _WIN32
    bool winsock_ = false;
#endif

    static constexpr Socket invalidSocket() {
#ifdef _WIN32
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }
    void clearPeaks() noexcept {
        for (auto& track : trackPeaks_)
            for (auto& peak : track) peak.store(0.f, std::memory_order_relaxed);
    }
    void closeSocket() {
        if (socket_ != invalidSocket()) {
#ifdef _WIN32
            closesocket(socket_);
#else
            close(socket_);
#endif
            socket_ = invalidSocket();
        }
    }
#ifdef _WIN32
    void closeWinsock() {
        if (winsock_) { WSACleanup(); winsock_ = false; }
    }
#endif
    static void appendString(std::vector<unsigned char>& out, const std::string& value) {
        out.insert(out.end(), value.begin(), value.end()); out.push_back(0);
        while (out.size() % 4) out.push_back(0);
    }
    static void appendInt(std::vector<unsigned char>& out, std::int32_t value) {
        const std::uint32_t net = htonl(static_cast<std::uint32_t>(value));
        const auto* bytes = reinterpret_cast<const unsigned char*>(&net); out.insert(out.end(), bytes, bytes + 4);
    }
    static void appendFloat(std::vector<unsigned char>& out, float value) {
        std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); bits = htonl(bits);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&bits); out.insert(out.end(), bytes, bytes + 4);
    }
    bool sendMessage(const std::string& address, const std::string& types, const std::vector<unsigned char>& args) {
        if (socket_ == invalidSocket()) return false;
        std::vector<unsigned char> packet; appendString(packet, address); appendString(packet, types);
        packet.insert(packet.end(), args.begin(), args.end());
        sockaddr_in target{}; target.sin_family = AF_INET; target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(languagePort);
#ifdef _WIN32
        const int sent = sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        return sent == static_cast<int>(packet.size());
#else
        const ssize_t sent = sendto(socket_, packet.data(), packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        return sent == static_cast<ssize_t>(packet.size());
#endif
    }
};
