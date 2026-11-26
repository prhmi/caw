#pragma once
#include <string>
#include <array>
#include <vector>
#include "pd_control_syntax.h"

// Experimental, single-instance libpd wrapper. All methods, including close,
// must be called serially by ONE non-real-time worker thread. Libpd itself
// may lock and allocate; never call this from the PortAudio callback.
class PdBackend {
public:
    PdBackend() = default;
    ~PdBackend();
    PdBackend(const PdBackend&) = delete;
    PdBackend& operator=(const PdBackend&) = delete;
    bool open(const std::string& libraryPath, const std::string& patchPath,
              int sampleRate, int outputChannels, std::string& error);
    bool openEngine(const std::string& libraryPath, int sampleRate, int outputChannels, std::string& error);
    // PD's audio inputs: [adc~ 1..8] = the soundcard inputs (the microphone), [adc~ 9..24] = 8
    // stereo CAW CABLES (written by [cawr~ name], an abstraction CAW provides). The patch's
    // outputs: [dac~ 1..N] = the track's sound, then 16 more channels that [caws~ name] writes
    // to send a stereo pair into a cable (never heard on the master).
    static constexpr int kMicChannels = 8;
    static constexpr int kCablePairs = 8;
    static constexpr int kInputChannels = kMicChannels + kCablePairs * 2;
    static constexpr int kCableOutChannels = kCablePairs * 2;
    static constexpr int kTrackSlots = 8;   // [dac~ 1..8] = the track; [dac~ 9..24] = the 8 cable sends (fixed numbers)
    // input: 64 * kInputChannels interleaved frames, or nullptr = silence.
    // output: 64 * (outputChannels + kCableOutChannels) frames: the first outputChannels are the
    // track, the rest the cable sends (cableOut, may be null to discard them).
    bool processTick(float* output, std::string& error, const float* input = nullptr, float* cableOut = nullptr);
    std::vector<std::string> takeDiagnostics();
    bool bang(const std::string& receiver, std::string& error);
    bool number(const std::string& receiver, float value, std::string& error);
    bool numberLocal(const std::string& receiver, float value, std::string& error);
    bool bangLocal(const std::string& receiver, std::string& error);
    bool noteOn(int channel, int pitch, int velocity, std::string& error);
    bool openBridge(const std::string& bridgePath, std::string& error);   // CAW cables: see pd_backend.cpp
    bool bindOutputs(const std::vector<pd_control_syntax::Output>& outputs,std::string& error);
    // Live coding: load the (re-saved or different) patch into the SAME running
    // libpd instance, without stopping DSP. The new patch is opened before the
    // old one is closed; on any failure the old patch keeps playing.
    bool reloadPatch(const std::string& patchPath,const std::vector<pd_control_syntax::Output>& outputs,std::string& error);
    template<class F> void drainOutputs(F&& fn) {
        for(std::size_t i=0;i<pendingCount_;++i) {
            const auto& p=pending_[i];const auto& b=bindings_[p.index];
            fn(b.kind,b.name,p.value);
        }
        pendingCount_=0;
    }
    unsigned takeDroppedOutputs(){unsigned n=pendingDropped_;pendingDropped_=0;return n;}
    void close() noexcept;
    static constexpr int tickFrames() noexcept { return 64; }
private:
    void* library_ = nullptr;
    void* patch_ = nullptr;
    void* bridge_ = nullptr;   // the CAW cable bridge patch (cawr~ / caws~ channel map)
    int channels_ = 0;
    std::vector<float> wideOut_;   // libpd's full output block: track channels + cable sends
    bool dsp_ = false;
    int dollarZero_ = 0;
    struct Binding {std::string receiver,kind,name;void* handle=nullptr;};
    struct Pending {std::size_t index=0;float value=0;};
    std::vector<Binding> bindings_;
    std::vector<pd_control_syntax::Output> boundOutputs_;
    void unbindOutputs() noexcept;
    std::array<Pending,256> pending_{};
    std::size_t pendingCount_=0;
    unsigned pendingDropped_=0;
    static thread_local PdBackend* activeBackend_;
    static void floatHook(const char* receiver,float value);
    static void printHook(const char* text);
    void flushPrintLine();
    struct HookScope {
        PdBackend* previous;
        explicit HookScope(PdBackend* current):previous(activeBackend_){activeBackend_=current;}
        ~HookScope(){activeBackend_=previous;}
    };
    using Init = int (*)();
    using BlockSize = int (*)();
    using InitAudio = int (*)(int,int,int);
    using Open = void* (*)(const char*,const char*);
    using DollarZero = int (*)(void*);
    using Close = void (*)(void*);
    using Process = int (*)(int,const float*,float*);
    using Bang = int (*)(const char*);
    using Number = int (*)(const char*,float);
    using NoteOn = int (*)(int,int,int);
    using StartMessage = int (*)(int);
    using AddFloat = void (*)(float);
    using FinishMessage = int (*)(const char*,const char*);
    using Bind = void* (*)(const char*);
    using Unbind = void (*)(void*);
    using SetFloatHook = void (*)(void (*)(const char*,float));
    using SetPrintHook = void (*)(void (*)(const char*));
    Init init_ = nullptr; BlockSize blockSize_ = nullptr; InitAudio initAudio_ = nullptr;
    Open open_ = nullptr; Close close_ = nullptr; Process process_ = nullptr;
    DollarZero dollarZeroFn_ = nullptr;
    Bang bang_ = nullptr; Number number_ = nullptr; StartMessage startMessage_ = nullptr;
    NoteOn noteOn_ = nullptr;
    AddFloat addFloat_ = nullptr; FinishMessage finishMessage_ = nullptr;
    Bind bind_=nullptr;Unbind unbind_=nullptr;SetFloatHook setFloatHook_=nullptr;SetPrintHook setPrintHook_=nullptr;
    std::string partialPrint_;
    std::vector<std::string> printLines_;
    bool setDsp(bool enabled) noexcept;
};
