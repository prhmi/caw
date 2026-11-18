#pragma once
#include "audio_output.h"
#include "pd_backend.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A persistent output stream and a single libpd instance. File loading and
// every libpd call stay on the worker, never on the UI or audio callback.
// One patch can be prepared in advance; arbitrary patches cannot safely be
// preloaded together in one shared PD instance (loadbangs and globals run).
class PdTimelineEngine {
public:
    struct Patch { int id = 0; std::string path; long long stamp = 0; unsigned long long epoch = 0; };
    PdTimelineEngine() = default;
    ~PdTimelineEngine() { stop(); }
    PdTimelineEngine(const PdTimelineEngine&) = delete;
    PdTimelineEngine& operator=(const PdTimelineEngine&) = delete;
    bool start(const std::string& lib, const Patch& first, const CawAudioSettings& settings) {
        if (active_.load()) return false;
        if (worker_.joinable()) worker_.join();
        { std::lock_guard<std::mutex> lock(mutex_); requested_ = first; playing_ = false; status_ = "Preparing PD and audio output..."; }
        stop_.store(false); ready_.store(false); active_.store(true);
        try { worker_ = std::thread(&PdTimelineEngine::run, this, lib, settings); }
        catch (...) { active_.store(false); setStatus("Could not start the PD worker."); return false; }
        return true;
    }
    void select(const Patch& patch, bool playing) {
        std::lock_guard<std::mutex> lock(mutex_);
        requested_ = patch; playing_ = playing;
    }
    void silence() { std::lock_guard<std::mutex> lock(mutex_); playing_ = false; }
    void stop() { stop_.store(true); if (worker_.joinable()) worker_.join(); }
    bool active() const { return active_.load(); }
    bool ready() const { return ready_.load(); }
    std::string status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
private:
    static bool same(const Patch& a, const Patch& b) {
        return a.id == b.id && a.path == b.path && a.stamp == b.stamp && a.epoch == b.epoch;
    }
    void setStatus(const std::string& text) { std::lock_guard<std::mutex> lock(mutex_); status_ = text; }
    void run(std::string library, CawAudioSettings settings) noexcept {
        try {
            PdBackend backend; CawAudioOutput output; std::string error;
            settings.fullScale = 1.f;
            Patch prepared, request; bool playing;
            { std::lock_guard<std::mutex> lock(mutex_); request = requested_; }
            if (!request.path.empty()) {
                if (!backend.open(library, request.path, settings.sampleRate, settings.channels, error)) {
                    setStatus("PD: " + error); active_.store(false); return;
                }
                prepared = request;
            }
            if (stop_.load()) { active_.store(false); return; }
            if (!output.open(settings, error)) {
                setStatus("PD audio: " + error); active_.store(false); return;
            }
            setStatus("PD ready."); ready_.store(true);
            std::vector<float> samples(static_cast<size_t>(PdBackend::tickFrames()) * settings.channels);
            while (!stop_.load()) {
                { std::lock_guard<std::mutex> lock(mutex_); request = requested_; playing = playing_; }
                if (!request.path.empty() && !same(request, prepared)) {
                    if (!backend.open(library, request.path, settings.sampleRate, settings.channels, error)) {
                        setStatus("PD: " + error); break;
                    }
                    prepared = request;
                    // The request may have changed while the patch loaded.
                    continue;
                }
                std::fill(samples.begin(), samples.end(), 0.f);
                if (playing && !request.path.empty() && same(request, prepared)) {
                    if (!backend.processTick(samples.data(), error)) { setStatus("PD: " + error); break; }
                    // Unity gain: PD in CAW must be exactly as loud as the same patch in Pure Data.
                    for (float& value : samples) value = std::max(-1.f, std::min(1.f, value));
                }
                if (!output.push(samples.data(), PdBackend::tickFrames(), stop_)) {
                    if (!stop_.load()) setStatus("PD audio output stopped unexpectedly.");
                    break;
                }
            }
            output.close();
            if (stop_.load()) setStatus("PD stopped.");
        } catch (const std::exception& e) { setStatus(std::string("PD error: ") + e.what()); }
        catch (...) { setStatus("PD failed unexpectedly."); }
        ready_.store(false); active_.store(false);
    }
    std::atomic<bool> stop_{false}, ready_{false}, active_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    Patch requested_;
    bool playing_ = false;
    std::string status_ = "PD is off.";
};
