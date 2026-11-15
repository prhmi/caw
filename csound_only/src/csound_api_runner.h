#pragma once
#include "monitor_capture.h"
#include "audio_output.h"
#include "vst_job.h"
#include <memory>
#include <string>
#include <vector>

// The Csound host owns the engine thread. The analyzer bridge contains no
// Csound types, so future sources can feed the same MonitorCapture.
class CsoundApiRunner {
public:
    CsoundApiRunner();
    ~CsoundApiRunner();
    CsoundApiRunner(const CsoundApiRunner&) = delete;
    CsoundApiRunner& operator=(const CsoundApiRunner&) = delete;
    bool prepare(const std::string& executable, std::string& error);
    bool start(const std::string& csdPath, int sampleRate, int meterCount, bool capture, std::string& error,
               const CawAudioSettings* audio = nullptr);
    // Offline uses the same Csound and VST block pipeline, but writes WAV instead of opening a device.
    bool startOffline(const std::string& csdPath, int sampleRate, int channels, float fullScale,
                      float gain, int wavFormat, const std::string& wavPath, std::string& error,
                      int outputChannels = 0);
    void setMasterGain(float linear);
    // Publish a completed command-file number to the engine thread.
    void setCommandSerial(int serial);
    void setVstJobs(const std::vector<VstJob>& jobs);   // VST3 tracks to run next to Csound (call before start)
    void stop();
    bool active() const;
    bool running() const;
    bool finished() const;
    int exitCode() const;
    std::vector<std::string> takeOutput();
    bool takeSnapshot(MonitorCapture::Snapshot& out, uint64_t lastSequence) const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
