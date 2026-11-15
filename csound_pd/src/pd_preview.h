#pragma once
#include "audio_output.h"
#include "pd_backend.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One patch per worker. The timeline starts and stops this worker at clip
// boundaries. All libpd calls happen here, never in the audio callback.
class PdPreview {
public:
    PdPreview() = default;
    ~PdPreview() { stop(); }
    PdPreview(const PdPreview&) = delete;
    PdPreview& operator=(const PdPreview&) = delete;
    bool start(const std::string& library, const std::string& patch, const CawAudioSettings& settings) {
        if (active_.load()) return false;
        if (thread_.joinable()) thread_.join();
        stop_.store(false); ready_.store(false); active_.store(true);
        setStatus("Starting PD patch...");
        try { thread_ = std::thread(&PdPreview::run,this,library,patch,settings); }
        catch (...) { active_.store(false); setStatus("Could not start PD worker thread."); return false; }
        return true;
    }
    void stop() {
        stop_.store(true);
        if(thread_.joinable())thread_.join();
    }
    // Live coding: ask the running worker to load the saved patch into its
    // running instance (no restart, no gap). The worker reports the result in status().
    void reload(const std::string& patch) {
        if(!active_.load())return;
        {std::lock_guard<std::mutex> guard(mutex_);reloadPatch_=patch;}
        reloadSeq_.fetch_add(1,std::memory_order_release);
    }
    bool active() const { return active_.load(); }
    bool ready() const { return ready_.load(); }
    std::string status() const { std::lock_guard<std::mutex> guard(mutex_); return status_; }
private:
    void setStatus(const std::string& s) { std::lock_guard<std::mutex> guard(mutex_); status_=s; }
    void run(std::string library,std::string patch,CawAudioSettings settings) noexcept {
        try {
            PdBackend backend; CawAudioOutput output; std::string error;
            settings.fullScale=1.f;
            if (!backend.open(library,patch,settings.sampleRate,settings.channels,error)) setStatus("PD: "+error);
            else if (stop_.load()) setStatus("PD stopped.");
            else if (!output.open(settings,error)) setStatus("PD audio: "+error);
            else {
                setStatus("PD patch is running.");
                ready_.store(true);
                std::vector<float> samples(static_cast<std::size_t>(PdBackend::tickFrames())*settings.channels);
                std::uint64_t seenReload=reloadSeq_.load(std::memory_order_acquire);
                while(!stop_.load()) {
                    if(reloadSeq_.load(std::memory_order_acquire)!=seenReload) {
                        std::string path;
                        {std::lock_guard<std::mutex> guard(mutex_);path=reloadPatch_;seenReload=reloadSeq_.load(std::memory_order_relaxed);}
                        std::string why;
                        if(backend.reloadPatch(path,{},why))setStatus("PD patch reloaded.");
                        else setStatus("PD reload failed, the previous patch keeps playing: "+why);
                    }
                    if(!backend.processTick(samples.data(),error)) {setStatus("PD: "+error);break;}
                    // Preview is independent of CAW's mixer. Use a conservative
                    // fixed gain and clamp to avoid sending over-range samples.
                    for(float& value : samples) value=std::max(-1.f,std::min(1.f,value*0.25f));
                    if(!output.push(samples.data(),PdBackend::tickFrames(),stop_)) {
                        if(!stop_.load())setStatus("PD audio output stopped unexpectedly.");
                        break;
                    }
                }
                if(stop_.load())setStatus("PD stopped.");
                output.close();
            }
        } catch(const std::exception& e) {setStatus(std::string("PD error: ")+e.what());}
        catch(...) {setStatus("PD failed unexpectedly.");}
        ready_.store(false);
        active_.store(false);
    }
    std::atomic<bool> stop_{false},active_{false},ready_{false};
    std::atomic<std::uint64_t> reloadSeq_{0};
    std::string reloadPatch_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string status_ = "PD is off.";
};
