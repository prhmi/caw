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
    bool processTick(float* output, std::string& error);
    bool bang(const std::string& receiver, std::string& error);
    bool number(const std::string& receiver, float value, std::string& error);
    bool numberLocal(const std::string& receiver, float value, std::string& error);
    bool bangLocal(const std::string& receiver, std::string& error);
    bool noteOn(int channel, int pitch, int velocity, std::string& error);
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
    int channels_ = 0;
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
    Init init_ = nullptr; BlockSize blockSize_ = nullptr; InitAudio initAudio_ = nullptr;
    Open open_ = nullptr; Close close_ = nullptr; Process process_ = nullptr;
    DollarZero dollarZeroFn_ = nullptr;
    Bang bang_ = nullptr; Number number_ = nullptr; StartMessage startMessage_ = nullptr;
    NoteOn noteOn_ = nullptr;
    AddFloat addFloat_ = nullptr; FinishMessage finishMessage_ = nullptr;
    Bind bind_=nullptr;Unbind unbind_=nullptr;SetFloatHook setFloatHook_=nullptr;
    bool setDsp(bool enabled) noexcept;
};
