#pragma once
#include "caw_audio_bus.h"
#include "caw_audio_in.h"
#include <mutex>

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
#include <fstream>
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
    static constexpr int maxTrackSlots = 32;
    // Audio channels per track slot (the meter follows the clip's channel count, up to 8)
    static constexpr int slotStride = 8;
    static constexpr int serverOutputChannels = maxTrackSlots * slotStride;
    // CAW AUDIO BUSES: the last kBusSlots track slots carry ~cawbsend (they never reach the master; CAW
    // moves them into the named bus). The server's 8 input buses carry ~cawbrecv: 4 stereo pairs.
    static constexpr int kBusSlots = 4;
    static constexpr int kFirstBusSlot = maxTrackSlots - kBusSlots;   // SC tracks use slots 0 .. kFirstBusSlot-1
    // Server inputs: buses 0..7 = the SOUNDCARD input (SoundIn.ar(0..7), the microphone), then 4
    // stereo CAW cables for ~cawbrecv (input buses 8..15).
    static constexpr int kMicChannels = 8;
    static constexpr int kRecvPairs = 4;
    static constexpr int kInputChannels = kMicChannels + kRecvPairs * 2;

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
        if (!abiVersion || abiVersion() < 5) {
            error = "This libscsynth has the old CAW audio ABI (the per-track audio streams are missing). Rebuild it with the latest supplied script.";
            stopEmbedded();
            return false;
        }
        create_ = reinterpret_cast<CreateFn>(symbol("caw_sc_world_new"));
        lastError_ = reinterpret_cast<LastErrorFn>(symbol("caw_sc_last_error"));
        pump_ = reinterpret_cast<PumpFn>(symbol("caw_sc_world_process"));
        pumpEx_ = reinterpret_cast<PumpExFn>(symbol("caw_sc_world_process_ex"));
        destroy_ = reinterpret_cast<DestroyFn>(symbol("caw_sc_world_free"));
        if (!create_ || !pump_ || !pumpEx_ || !destroy_) {
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

    // configArgument: optional path to a private sclang_conf.yaml. When given, sclang is started
    // with -l <config> and compiles ONLY the SCClassLibrary shipped next to sclang.exe - the user's
    // own SuperCollider installation (quarks, Extensions, sclang_conf.yaml) is never read, so broken
    // quark folders on the machine (Invalid utf8 / Duplicate identifier errors) cannot stop CAW,
    // and CAW cannot modify the user's IDE setup.
    bool start(const std::string& executable, const std::string& bootstrap, std::string& error,
               const std::string& configArgument = std::string()) {
        stopLanguage();
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == invalidSocket()) {
            error = "Could not create the local OSC socket for SuperCollider.";
            closeSocket();
            return false;
        }
        process_.setHighPriority(true);
        const std::string command = processArg(executable) +
            (configArgument.empty() ? std::string() : " -l " + processArg(configArgument)) +
            " -D -u " +
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
        if (slot < 0 || slot >= maxTrackSlots || channel < 0 || channel >= slotStride) return 0.f;
        return trackPeaks_[static_cast<size_t>(slot)][static_cast<size_t>(channel)].exchange(0.f, std::memory_order_acq_rel);
    }
    bool running() const { return process_.running(); }
    bool active() const { return process_.active(); }
    bool finished() const { return process_.finished(); }
    int exitCode() const { return process_.exitCode(); }
    std::vector<std::string> takeOutput() { return process_.takeOutput(); }
    void setGain(float gain) { gain_.store(std::isfinite(gain) ? gain : 1.f, std::memory_order_relaxed); }

    // Per-track audio handoff (VSTe "SuperCollider" inputs, engine 2): after every pumped block the
    // engine hands each track slot's raw stereo mix to this callback (the app pushes it into the
    // per-slot rings the rack pops). Called with the pump done, on the audio thread: lock-free.
    void setTrackFeed(std::function<void(int slot, const float* interleaved, int frames, int stride)> fn) {
        trackFeed_ = std::move(fn);
    }
    // Bit s set = track slot s does NOT mix into the device output (a consumed FX-only source:
    // its only path is through the consuming effect). The meter and the per-track feed still see
    // the slot's audio. Set from the UI whenever the routing / M / S / mode changes.
    void setSlotSuppress(std::uint32_t mask) { slotSuppress_.store(mask, std::memory_order_release); }
    // UI thread: which bus each send slot / receive pair carries (empty = unused). Rare; the audio
    // thread picks it up with a try-lock, never waits.
    void setBusRouting(const std::array<std::string, kBusSlots>& sends, const std::array<std::string, kRecvPairs>& recvs) {
        std::lock_guard<std::mutex> lock(busMutex_);
        busSendNames_ = sends; busRecvNames_ = recvs; busDirty_ = true;
    }
    // The track fader x volume lane per slot (UI thread, every frame). CAW mixes the slots into the device
    // output itself, so this rides each slot on its way out; the VSTe feed keeps the raw (pre-fader) signal.
    void setSlotGain(int slot, float gain) noexcept {
        if (slot < 0 || slot >= maxTrackSlots) return;
        slotGain_[static_cast<size_t>(slot)].v.store(std::isfinite(gain) ? std::max(0.f, gain) : 1.f, std::memory_order_relaxed);
    }

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
        const auto pumpEx = pumpEx_;
        void* world = world_.load(std::memory_order_acquire);
        std::array<float, maxTrackSlots * slotStride> trackMeters{};
        if (pump && world) {
            if (pumpEx) {   // always per slot: CAW mixes the slots itself, so the track fader applies
                // Per-track pass: each slot's raw stereo bus lands in its scratch buffer, handed to
                // the app's feed (the per-slot rings) after the pump; suppressed slots skip the
                // device mixdown but keep feeding their ring and meter.
                const size_t need = static_cast<size_t>(frames) * slotStride;
                for (int slot = 0; slot < maxTrackSlots; ++slot) {
                    if (slotScratch_[static_cast<size_t>(slot)].size() < need)
                        slotScratch_[static_cast<size_t>(slot)].resize(need, 0.f);
                    slotPtrs_[static_cast<size_t>(slot)] = slotScratch_[static_cast<size_t>(slot)].data();
                }
                const std::uint32_t suppress = slotSuppress_.load(std::memory_order_acquire);
                // The server mixes NO slot into the device output; CAW adds each one below with its gain.
                const unsigned char mask[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
                {   // CAW AUDIO BUSES: adopt new routing (never wait on the UI)
                    std::unique_lock<std::mutex> lock(busMutex_, std::try_to_lock);
                    if (lock.owns_lock() && busDirty_) {
                        for (int k = 0; k < kBusSlots; ++k) busSend_[(size_t)k].rename(busSendNames_[(size_t)k]);
                        for (int k = 0; k < kRecvPairs; ++k) busRecv_[(size_t)k].rename(busRecvNames_[(size_t)k]);
                        busDirty_ = false;
                    }
                }
                const size_t inNeed = static_cast<size_t>(frames) * kInputChannels;
                if (busInput_.size() < inNeed) busInput_.assign(inNeed, 0.f);
                if (busLr_.size() < static_cast<size_t>(frames) * 2) busLr_.assign(static_cast<size_t>(frames) * 2, 0.f);
                bool anyIn = false;
                std::fill_n(busInput_.begin(), inNeed, 0.f);
                if (cawin::isOpen()) {   // the soundcard input -> SoundIn.ar(0..7)
                    if (micBlock_.size() < static_cast<size_t>(frames) * kMicChannels) micBlock_.assign(static_cast<size_t>(frames) * kMicChannels, 0.f);
                    micReader_.read(micBlock_.data(), frames, kMicChannels);
                    for (unsigned long f = 0; f < frames; ++f)
                        for (int c = 0; c < kMicChannels; ++c) busInput_[f * kInputChannels + c] = micBlock_[f * kMicChannels + c];
                    anyIn = true;
                }
                for (int k = 0; k < kRecvPairs; ++k) {   // cable -> server input pair k (In.ar(NumOutputBuses.ir + 8 + 2k, 2))
                    cawbus::Port& p = busRecv_[(size_t)k];
                    if (p.busName.empty()) continue;
                    cawbus::read(p, busLr_.data(), frames);
                    for (unsigned long f = 0; f < frames; ++f) {
                        busInput_[f * kInputChannels + kMicChannels + k * 2] = busLr_[f * 2];
                        busInput_[f * kInputChannels + kMicChannels + k * 2 + 1] = busLr_[f * 2 + 1];
                    }
                    anyIn = true;
                }
                pumpEx(world, anyIn ? busInput_.data() : nullptr, anyIn ? kInputChannels : 0, output, channels, static_cast<int>(frames),
                       trackMeters.data(), maxTrackSlots, slotPtrs_.data(), maxTrackSlots, mask, 4);
                for (int k = 0; k < kBusSlots; ++k) {   // send slots -> their bus (channels 1-2 of the slot), never the master
                    cawbus::Port& p = busSend_[(size_t)k];
                    if (p.busName.empty()) continue;
                    const float* src = slotScratch_[static_cast<size_t>(kFirstBusSlot + k)].data();
                    for (unsigned long f = 0; f < frames; ++f) { busLr_[f * 2] = src[f * slotStride]; busLr_[f * 2 + 1] = src[f * slotStride + 1]; }
                    cawbus::send(p, busLr_.data(), frames);
                }
                if (trackFeed_)
                    for (int slot = 0; slot < maxTrackSlots; ++slot)
                        trackFeed_(slot, slotScratch_[static_cast<size_t>(slot)].data(), frames, slotStride);
                for (int slot = 0; slot < maxTrackSlots; ++slot) {
                    const float g = slotGain_[static_cast<size_t>(slot)].v.load(std::memory_order_relaxed);
                    for (int channel = 0; channel < slotStride; ++channel)
                        trackMeters[static_cast<size_t>(slot) * slotStride + channel] *= g;   // meters show the post-fader level
                    if (slot >= kFirstBusSlot) continue;   // a bus send slot: its audio went into the bus, not the master
                    if ((suppress & (1u << slot)) || g == 0.f) continue;   // consumed by an FX-only VSTe / fader down
                    const float* src = slotScratch_[static_cast<size_t>(slot)].data();
                    // CAW is the virtual soundcard: every slot channel c plays on the project mix's
                    // channel (c mod N) - a 2-channel project hears a 4-channel clip as ch1+ch3 L,
                    // ch2+ch4 R, and a wide project maps wide clips 1:1. The device fold happens
                    // once at the master output (CawAudioOutput), the same for every engine.
                    for (unsigned long f = 0; f < frames; ++f)
                        for (int c = 0; c < slotStride; ++c)
                            output[f * channels + (c % channels)] += src[f * slotStride + c] * g;
                }
            } else {
                pump(world, nullptr, 0, output, channels, static_cast<int>(frames),
                     trackMeters.data(), maxTrackSlots);
            }
        }
        const float gain = gain_.load(std::memory_order_relaxed);
        if (gain != 1.f) for (size_t i = 0; i < static_cast<size_t>(frames) * channels; ++i) output[i] *= gain;
        for (int slot = 0; slot < maxTrackSlots; ++slot)
            for (int channel = 0; channel < slotStride; ++channel) {
                const float peak = trackMeters[static_cast<size_t>(slot) * slotStride + channel] * gain;
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
    // CAW hub (caws/cawr): one shared value. The bootstrap's /caw/hub handler
    // stores it into the ~cawr dictionary; SC clips read ~cawr[\\name].
    // CAW AUDIO BUSES: tell the language which bus name lives on which send slot / receive pair
    // (kind 0 = send slot k, 1 = receive pair k, -1 = forget every mapping).
    bool sendBusMap(const std::string& name, int kind, int index) {
        std::vector<unsigned char> args; appendString(args, name.empty() ? std::string("-") : name); appendInt(args, kind); appendInt(args, index);
        return sendMessage("/caw/busmap", ",sii", args);
    }
    bool sendHub(const std::string& name, float value) {
        std::vector<unsigned char> args; appendString(args, name); appendFloat(args, value);
        return sendMessage("/caw/hub", ",sf", args);
    }

private:
    using CreateFn = void* (*)(int, int, int, int, int*);
    using AbiVersionFn = int (*)();
    using LastErrorFn = int (*)();
    using PumpFn = void (*)(void*, const float*, int, float*, int, int, float*, int);
    using PumpExFn = void (*)(void*, const float*, int, float*, int, int, float*, int, float* const*, int, const unsigned char*, int);
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
    std::array<std::array<std::atomic<float>, slotStride>, maxTrackSlots> trackPeaks_{};
    PumpExFn pumpEx_ = nullptr;                       // the per-track pump (caw_sc_world_process_ex)
    std::function<void(int slot, const float* interleaved, int frames, int stride)> trackFeed_;
    std::atomic<std::uint32_t> slotSuppress_{0};      // bit s = slot s does not mix into the device output
    struct SlotGain { std::atomic<float> v{1.f}; };   // atomics cannot be copied: unity via the member initializer
    std::array<SlotGain, maxTrackSlots> slotGain_{};  // the track fader x volume lane per slot
    std::array<std::vector<float>, maxTrackSlots> slotScratch_{};
    std::mutex busMutex_;                                          // guards the names below (UI writes, audio try-locks)
    std::array<std::string, kBusSlots> busSendNames_{};
    std::array<std::string, kRecvPairs> busRecvNames_{};
    bool busDirty_ = false;
    std::array<cawbus::Port, kBusSlots> busSend_{};                // audio thread only
    std::array<cawbus::Port, kRecvPairs> busRecv_{};
    std::vector<float> busInput_, busLr_, micBlock_;
    cawin::Reader micReader_;   // audio thread only   // per-slot stereo scratch (audio thread)
    std::array<float*, maxTrackSlots> slotPtrs_{};
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
