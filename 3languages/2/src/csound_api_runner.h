#pragma once
#include "monitor_capture.h"
#include "audio_output.h"
#include "vst_job.h"
#include "pd_timeline.h"
#include "pd_control_queue.h"
#include <memory>
#include <array>
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
    // Configure before start. An empty library disables the shared PD source.
    void setPdSource(const std::string& library, const std::vector<pd_timeline::Event>& plan);
    void pdTransport(bool playing, double time, double loopStart, double loopEnd, bool loop, int rate, int commandSerial = 0);
    bool pdReady() const;
    std::int64_t pdPosition() const;
    bool auditionPd(const pd_timeline::Event& event, bool playing);
    bool auditioningPd(int clipId) const;
    bool replanPd(const std::vector<pd_timeline::Event>& plan);
    std::string takePdPlanError();
    std::vector<std::string> takePdPlanNotes();
    std::vector<PdControlOutput> takePdControlOutputs();
    unsigned takePdDroppedControls();
    struct PdTrackPeak {int trackId; std::array<float,2> value;};
    std::vector<PdTrackPeak> takePdPeaks();
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
