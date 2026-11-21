#pragma once
#include "monitor_capture.h"
#include "audio_output.h"
#include "caw_hub.h"
#include "vst_job.h"
#include "pd_timeline.h"
#include "pd_control_queue.h"
#include <memory>
#include <array>
#include <set>
#include <string>
#include <utility>
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
    // true = the CAW master chain applies the master gain on the device buffer (the engine
    // must not apply it again). The offline renderer keeps its own gain.
    void setHostGainExternal(bool external);
    // Publish a completed command-file number to the engine thread.
    void setCommandSerial(int serial);
    void setVstJobs(const std::vector<VstJob>& jobs);   // the whole VST graph: instruments AND effects (call before start)
    // Live VST graph update while the engine runs: the rack reloads the plugins when a track was
    // added / removed or its .vst3 changed, otherwise it only re-points the effect inputs and the
    // AUDIO OUTPUT. Pushed by the UI on every wiring / plugin change - no Run needed.
    void setVstWiring(const std::vector<VstJob>& jobs);
    // SEPARATED ENGINES: Csound loads NO VST plugins. It only captures these named clip tap
    // channels (engine 0 sources, cawtap_*) once per ksmps cycle into the vsttaps rings, so the
    // 4th engine's rack can consume Csound clip audio without touching Csound's thread.
    void setVstTaps(const std::vector<std::string>& channels);    // Live M / S: the set of VST track ids that should keep sounding. Updated by the UI whenever a
    // track's mute or solo changes; the rack skips every loaded track whose id is not in the set.
    void setVstActiveTracks(const std::set<int>& ids);
    // Live VSTE input on/off: bit i set = input slot i of that track feeds the plugin. The rack
    // packs the enabled slots from line 1, so toggling never leaves silent gaps. Pushed by the UI
    // whenever an input checkbox changes; a missing entry means all configured inputs are on.
    void setVstInputMasks(const std::map<int, unsigned>& masks);
    // Manual parameter override: reaches the rack DIRECTLY (no Csound channel involved) and is
    // applied on the same block, so a slider drag is always heard immediately.
    void pushVstParamOverride(int trackId, int slot, double value);
    // Live re-pick of the P1..P8 mapping (the UI picked / cleared a slot while running): the rack
    // updates its job so the next edit maps to the right parameter without a restart.
    void refreshVstPicks(int trackId, const std::vector<int>& pick);
    // Live parameter edit from the plugin's own GUI for ANY parameter (not just the 8 picked):
    // queued to the rack, applied to the engine instance on the same block.
    void pushVstParamValue(int trackId, int paramIndex, double value);
    // Live code editing: recompile ONE clip's instrument inside the running engine
    // (csoundCompileOrc, applied on the engine thread between two audio blocks). Csound replaces
    // the definition; instances that are already playing keep the old code, so the caller can
    // restart the clip to hear the edit at once. Does nothing while no engine runs - the next Run
    // compiles the code anyway. label = clip name, used in the console lines.
    void pushLiveInstrument(const std::string& label, std::string code);
    // AUDIO OUTPUT switched live (VST panel): the rack replaces (or stops replacing) the VST
    // sources it consumes and the raw PD / SC mix suppression is updated - no restart. The CSD
    // source-clip gates are written separately by the app (vstGateValues) on the same change.
    void setVstOutModeLive(int trackId, int mode);
    // SuperCollider mix capture (filled by CAW's audio callback) for VSTe "SuperCollider" inputs.
    void setScAudioRing(CawScRing* ring);
    // Set to true by the engine while an ACTIVE master-mode VSTe consumes the SuperCollider mix
    // (the processed output replaces the raw mix, so CAW's audio callback must not add it too).
    // The Pure Data raw mix is suppressed inside the engine thread itself, no flag needed.
    void setScRawSuppress(std::atomic<bool>* flag);
    // The parameter values the engine is actually using (normalized kP1..kP8, per track id).
    // Returns false when the engine is off or the track has no VST rack entry.
    bool vstEngineParams(int trackId, double out[8]) const;
    struct VstTrackPeak { int trackId = 0; float l = 0.f, r = 0.f; };
    // Per-track output peaks of the rack (every VST track, instrument or effect), max-held since
    // the last call: the lane meters read them while Csound hosts the plugins.
    std::vector<VstTrackPeak> takeVstPeaks();
    // Live parameter edit: write each value into the named Csound control channel once (the clip
    // code reads it on the next block). Used by the VSTE panel sliders so a dragged P1..P8 value
    // reaches the running plugin without restarting Csound.
    void setControlChannels(const std::vector<std::pair<std::string, double>>& values);
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
