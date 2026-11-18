#pragma once
#include "monitor_capture.h"
#include "audio_output.h"
#include "caw_hub.h"
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
    // Ground-truth check of the basic math opcodes: compiles tiny abs / sqrt / pow probe orchestras
    // through the same API library the engine would use beside `executable`. A flag is set false ONLY
    // when Csound's own message says the opcode does not exist ("Unable to find opcode entry" /
    // "unknown opcode"); any other probe failure is reported in `error` as inconclusive and keeps the
    // default true, so a probe problem can never block a real run. Returns false only when the Csound
    // library itself could not be loaded or bound.
    bool probeMath(const std::string& executable, bool& hasAbs, bool& hasSqrt, bool& hasPow, std::string& error);
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
    // CAW hub (caws/cawr): the shared bus lives in the app; the engine thread polls the
    // "caws/<name>" channels into it and writes changed values back + delivers to PD.
    // `names` = every caws/cawr channel name found in the clip code (without the prefix).
    void setHub(CawHub* hub);
    void setHubNames(const std::vector<std::string>& names);
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
