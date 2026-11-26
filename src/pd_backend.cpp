#include "pd_backend.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace {
void* loadLibrary(const std::string& path) {
#ifdef _WIN32
    // Search the selected DLL's own directory for its dependencies. Plain
    // LoadLibraryW does not reliably search that directory for dependent DLLs.
    return reinterpret_cast<void*>(LoadLibraryExW(std::filesystem::u8path(path).c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void unloadLibrary(void* library) {
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}
void* symbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library),name));
#else
    return dlsym(library,name);
#endif
}
}
PdBackend::~PdBackend() { close(); }
thread_local PdBackend* PdBackend::activeBackend_=nullptr;
void PdBackend::flushPrintLine() {
    while(!partialPrint_.empty()&&std::isspace(static_cast<unsigned char>(partialPrint_.back())))
        partialPrint_.pop_back();
    std::size_t first=0;
    while(first<partialPrint_.size()&&std::isspace(static_cast<unsigned char>(partialPrint_[first])))++first;
    if(first<partialPrint_.size()) {
        if(printLines_.size()>=256)printLines_.erase(printLines_.begin(),printLines_.begin()+64);
        printLines_.push_back(partialPrint_.substr(first,4096));
    }
    partialPrint_.clear();
}
void PdBackend::printHook(const char* text) {
    PdBackend* self=activeBackend_;
    if(!self||!text)return;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(text);*p;++p) {
        if(*p=='\n')self->flushPrintLine();
        else if(*p!='\r'&&self->partialPrint_.size()<4096)
            self->partialPrint_.push_back(static_cast<char>(*p));
    }
}
void PdBackend::floatHook(const char* receiver,float value) {
    auto* self=activeBackend_;
    if(!self||!receiver||!std::isfinite(value))return;
    for(std::size_t i=0;i<self->bindings_.size();++i)
        if(self->bindings_[i].receiver==receiver) {
            if(self->pendingCount_<self->pending_.size())self->pending_[self->pendingCount_++]={i,value};
            else ++self->pendingDropped_;
            return;
        }
}
void PdBackend::unbindOutputs() noexcept {
    if(unbind_)for(auto& binding:bindings_)if(binding.handle)unbind_(binding.handle);
    bindings_.clear();boundOutputs_.clear();pendingCount_=0;pendingDropped_=0;
}
std::vector<std::string> PdBackend::takeDiagnostics() {
    flushPrintLine();
    std::vector<std::string> result;
    for(std::size_t i=0;i<printLines_.size();++i) {
        const std::string& line=printLines_[i];
        std::string low=line;
        std::transform(low.begin(),low.end(),low.begin(),[](unsigned char c){return (char)std::tolower(c);});
        const bool diagnostic=low.find("error:")!=std::string::npos ||
            low.rfind("error",0)==0 || low.find("couldn't create")!=std::string::npos ||
            low.find("cannot create")!=std::string::npos || low.find("can't create")!=std::string::npos ||
            low.find("no such file")!=std::string::npos || low.find("failed to load")!=std::string::npos ||
            low.find("not found")!=std::string::npos;
        if(!diagnostic)continue;
        std::string message=line;
        const bool creationError=low.find("couldn't create")!=std::string::npos ||
            low.find("cannot create")!=std::string::npos || low.find("can't create")!=std::string::npos;
        // Pd logs the full object text immediately before its generic
        // "... couldn't create" error. Keep that compiler context so CAW can
        // match the exact readable line instead of highlighting every object.
        if(creationError&&i>0&&!printLines_[i-1].empty()) {
            std::string objectText=printLines_[i-1];
            // stderr-mode Pd prefixes logpost() output with its verbosity.
            // libpd sends the same prefix through libpd_set_printhook().
            if(objectText.rfind("verbose(",0)==0) {
                const auto end=objectText.find("):");
                if(end!=std::string::npos)objectText.erase(0,end+2);
            }
            while(!objectText.empty()&&std::isspace(static_cast<unsigned char>(objectText.front())))
                objectText.erase(objectText.begin());
            // Shared Csound/PD logs are transported as newline-delimited
            // records. Keep context on the SAME record as the error.
            if(!objectText.empty())message+=" | PD object text: "+objectText;
        }
        result.push_back(std::move(message));
    }
    printLines_.clear();
    return result;
}
bool PdBackend::reloadPatch(const std::string& patchPath,const std::vector<pd_control_syntax::Output>& outputs,std::string& error) {
    error.clear();
    if(!dsp_||!patch_||!open_||!close_||!dollarZeroFn_){error="PD is not running.";return false;}
    std::error_code ec;
    auto patch=std::filesystem::absolute(std::filesystem::u8path(patchPath),ec);
    if(patchPath.empty()||ec||!std::filesystem::is_regular_file(patch,ec)||ec||patch.extension()!=".pd") {
        error="The .pd patch does not exist or is not a .pd file.";return false;
    }
    HookScope scope(this);
    const std::string name=patch.filename().u8string(),dir=patch.parent_path().u8string();
    void* fresh=open_(name.c_str(),dir.c_str());
    if(!fresh){error="libpd could not open the patch.";return false;}
    const int freshZero=dollarZeroFn_(fresh);
    if(freshZero<=0){close_(fresh);error="Could not determine the patch's $0 identifier.";return false;}
    void* oldPatch=patch_;const int oldZero=dollarZero_;
    const auto oldOutputs=boundOutputs_;
    unbindOutputs();
    patch_=fresh;dollarZero_=freshZero;
    if(!bindOutputs(outputs,error)) {   // roll back: the previous patch keeps running
        unbindOutputs();
        close_(fresh);
        patch_=oldPatch;dollarZero_=oldZero;
        std::string ignored;bindOutputs(oldOutputs,ignored);
        return false;
    }
    close_(oldPatch);
    return true;
}
// The CAW CABLE bridge: a tiny patch CAW writes for this clip and opens in the SAME instance as the
// user's patch. It maps every cable name to its fixed channels: [adc~ 9..24] -> [s~ cawcable-<name>-L/R]
// for the [cawr~ <name>] receivers, [r~ cawcable-<name>-out-L/R] -> [dac~ 9..24] for the [caws~ <name>]
// senders. The user's patch never touches adc~ / dac~ for cables: adc~ stays the microphone.
bool PdBackend::openBridge(const std::string& bridgePath,std::string& error) {
    if(bridge_ && close_){close_(bridge_);bridge_=nullptr;}
    if(bridgePath.empty())return true;
    HookScope scope(this);
    const auto p=std::filesystem::u8path(bridgePath);
    bridge_=open_(p.filename().u8string().c_str(),p.parent_path().u8string().c_str());
    if(!bridge_){error="libpd could not open the CAW cable bridge.";return false;}
    return true;
}
bool PdBackend::bindOutputs(const std::vector<pd_control_syntax::Output>& outputs,std::string& error) {
    if(outputs.empty())return true;
    boundOutputs_=outputs;
    if(!bind_||!unbind_||!setFloatHook_){error="libpd does not expose float-output binding hooks.";return false;}
    if(dollarZero_<=0){error="PD patch-local namespace is unavailable.";return false;}
    setFloatHook_(floatHook);
    for(const auto& output:outputs) {
        if(output.name.size()>=128){error="PD output name exceeds 127 characters.";return false;}
        Binding b;b.kind=output.kind;b.name=output.name;
        b.receiver=std::to_string(dollarZero_)+"-"+output.kind+"-"+output.name;
        b.handle=bind_(b.receiver.c_str());
        if(!b.handle){error="Could not bind PD output "+b.receiver;return false;}
        bindings_.push_back(std::move(b));
    }
    error.clear();return true;
}
bool PdBackend::setDsp(bool enabled) noexcept {
    if (!startMessage_ || !addFloat_ || !finishMessage_ || startMessage_(1) != 0) return false;
    addFloat_(enabled ? 1.f : 0.f);
    return finishMessage_("pd","dsp") == 0;
}
bool PdBackend::open(const std::string& libraryPath,const std::string& patchPath,int rate,int channels,std::string& error) {
    error.clear();
    std::error_code ec;
    auto patch=std::filesystem::absolute(std::filesystem::u8path(patchPath),ec);
    if(patchPath.empty() || ec || !std::filesystem::is_regular_file(patch,ec) || ec || patch.extension() != ".pd") {
        error="The selected .pd patch does not exist or is not a .pd file.";return false;
    }
    if(!openEngine(libraryPath,rate,channels,error))return false;
    HookScope scope(this);
    const std::string name=patch.filename().u8string(), dir=patch.parent_path().u8string();
    patch_=open_(name.c_str(),dir.c_str());
    if(!patch_) {error="libpd could not open the patch.";close();return false;}
    dollarZero_=dollarZeroFn_(patch_);
    if(dollarZero_<=0) {error="Could not determine the patch's $0 identifier.";close();return false;}
    if(!setDsp(true)) {error="Could not enable PD DSP after loading the patch.";close();return false;}
    return true;
}
bool PdBackend::openEngine(const std::string& libraryPath,int rate,int channels,std::string& error) {
    close(); error.clear();
    if(libraryPath.empty() || rate < 8000 || rate > 192000 || channels < 1 || channels > 8) {
        error="Select libpd and use a supported audio format.";return false;
    }
    std::error_code ec;
    auto lib=std::filesystem::absolute(std::filesystem::u8path(libraryPath),ec);
    if(ec || !std::filesystem::is_regular_file(lib,ec) || ec) {error="The selected libpd library does not exist.";return false;}
    library_=loadLibrary(lib.u8string());
    if(!library_) {
#ifdef _WIN32
        const auto code = GetLastError();
        error="Could not load libpd (Windows error " + std::to_string(code) + ": " +
            std::system_category().message(static_cast<int>(code)) +
            "). Check 64-bit architecture and that dependent DLLs are beside libpd.dll.";
#else
        const char* detail = dlerror();
        error=std::string("Could not load libpd: ") + (detail ? detail : "unknown loader error");
#endif
        return false;
    }
#define CAW_PD_BIND(member, name) member = reinterpret_cast<decltype(member)>(symbol(library_,name)); if(!member){error=std::string("libpd is missing ")+name;close();return false;}
    CAW_PD_BIND(init_,"libpd_init"); CAW_PD_BIND(blockSize_,"libpd_blocksize");
    CAW_PD_BIND(initAudio_,"libpd_init_audio"); CAW_PD_BIND(open_,"libpd_openfile");
    CAW_PD_BIND(dollarZeroFn_,"libpd_getdollarzero");
    CAW_PD_BIND(close_,"libpd_closefile"); CAW_PD_BIND(process_,"libpd_process_float");
    CAW_PD_BIND(bang_,"libpd_bang"); CAW_PD_BIND(number_,"libpd_float");
    CAW_PD_BIND(startMessage_,"libpd_start_message"); CAW_PD_BIND(addFloat_,"libpd_add_float");
    CAW_PD_BIND(finishMessage_,"libpd_finish_message");
#undef CAW_PD_BIND
    setPrintHook_=reinterpret_cast<SetPrintHook>(symbol(library_,"libpd_set_printhook"));
    HookScope scope(this);
    if(setPrintHook_)setPrintHook_(printHook);
    noteOn_ = reinterpret_cast<NoteOn>(symbol(library_,"libpd_noteon"));
    bind_=reinterpret_cast<Bind>(symbol(library_,"libpd_bind"));
    unbind_=reinterpret_cast<Unbind>(symbol(library_,"libpd_unbind"));
    setFloatHook_=reinterpret_cast<SetFloatHook>(symbol(library_,"libpd_set_floathook"));
    const int initialized=init_();
    if((initialized != 0 && initialized != -1) || blockSize_()!=tickFrames() || initAudio_(kInputChannels,kTrackSlots+kCableOutChannels,rate)!=0) {
        error="libpd initialization or audio configuration failed.";close();return false;
    }
    if(!setDsp(true)) {error="Could not enable libpd DSP.";close();return false;}
    dsp_=true; channels_=channels;return true;
}
bool PdBackend::processTick(float* output,std::string& error,const float* input,float* cableOut) {
    if(!dsp_||!patch_||!output){error="PD is not running.";return false;}
    // inputs: mic (adc~ 1..8) + cables (adc~ 9..24), interleaved kInputChannels wide, or silence
    std::array<float,64*kInputChannels> silence{};
    // libpd writes channels_ + kCableOutChannels per frame: render wide, then split track / cables
    const int wide=kTrackSlots+kCableOutChannels;   // dac~ 1..8 track, dac~ 9..24 cables (fixed, whatever the track width)
    if(wideOut_.size()<(size_t)(64*wide))wideOut_.assign((size_t)(64*wide),0.f);
    HookScope scope(this);
    if(process_(1,input?input:silence.data(),wideOut_.data())!=0){error="libpd audio processing failed.";return false;}
    for(int f=0;f<tickFrames();++f) {
        const float* src=wideOut_.data()+(size_t)f*wide;
        for(int c=0;c<channels_;++c){const float v=src[c];output[(size_t)f*channels_+c]=std::isfinite(v)?v:0.f;}
        if(cableOut)for(int c=0;c<kCableOutChannels;++c){const float v=src[kTrackSlots+c];cableOut[(size_t)f*kCableOutChannels+c]=std::isfinite(v)?v:0.f;}
    }
    return true;
}
bool PdBackend::bang(const std::string& receiver,std::string& error) {
    HookScope scope(this);
    if(!dsp_ || receiver.empty() || bang_(receiver.c_str())!=0){error="PD receiver is unavailable.";return false;}
    return true;
}
bool PdBackend::number(const std::string& receiver,float value,std::string& error) {
    HookScope scope(this);
    if(!dsp_ || receiver.empty() || !std::isfinite(value) || number_(receiver.c_str(),value)!=0){error="PD receiver is unavailable or the value is invalid.";return false;}
    return true;
}
bool PdBackend::numberLocal(const std::string& receiver,float value,std::string& error) {
    if(dollarZero_<=0){error="PD patch-local namespace is unavailable.";return false;}
    return number(std::to_string(dollarZero_)+"-"+receiver,value,error);
}
bool PdBackend::bangLocal(const std::string& receiver,std::string& error) {
    if(dollarZero_<=0){error="PD patch-local namespace is unavailable.";return false;}
    return bang(std::to_string(dollarZero_)+"-"+receiver,error);
}
bool PdBackend::noteOn(int channel,int pitch,int velocity,std::string& error) {
    HookScope scope(this);
    if(!dsp_||!noteOn_||channel<0||pitch<0||pitch>127||velocity<0||velocity>127||noteOn_(channel,pitch,velocity)!=0){
        error="PD MIDI input is unavailable or invalid.";return false;
    }
    return true;
}
void PdBackend::close() noexcept {
    HookScope scope(this);
    unbindOutputs();
    if(dsp_) {setDsp(false);dsp_=false;}
    if(bridge_ && close_) close_(bridge_);
    bridge_=nullptr;
    if(patch_ && close_) close_(patch_);
    patch_=nullptr;channels_=0;dollarZero_=0;
    if(setPrintHook_)setPrintHook_(nullptr);
    if(library_) unloadLibrary(library_);
    library_=nullptr;
    init_=nullptr;blockSize_=nullptr;initAudio_=nullptr;open_=nullptr;close_=nullptr;process_=nullptr;dollarZeroFn_=nullptr;
    bang_=nullptr;number_=nullptr;noteOn_=nullptr;startMessage_=nullptr;addFloat_=nullptr;finishMessage_=nullptr;
    bind_=nullptr;unbind_=nullptr;setFloatHook_=nullptr;setPrintHook_=nullptr;
    partialPrint_.clear();printLines_.clear();
}
