#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// Experimental offline-tested bridge, NOT yet connected to the application.
// Load all patches on a single worker before processing. Every method and the
// destructor must run on that same worker. Requires libpd built MULTI=true.
class PdMultiBackend {
public:
    PdMultiBackend() = default;
    PdMultiBackend(const PdMultiBackend&) = delete;
    PdMultiBackend& operator=(const PdMultiBackend&) = delete;
    ~PdMultiBackend(){close();}
    bool load(const std::string& file,std::string& error) {
        close(); error.clear();
#ifdef _WIN32
        library_=reinterpret_cast<void*>(LoadLibraryExW(std::filesystem::u8path(file).c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
#else
        library_=dlopen(file.c_str(),RTLD_NOW|RTLD_LOCAL);
#endif
        if(!library_){error="Could not load libpd.";return false;}
#define PD_MULTI_BIND(member,name) member=reinterpret_cast<decltype(member)>(symbol(name)); if(!member){error=std::string("Missing libpd symbol: ")+name;close();return false;}
        PD_MULTI_BIND(init_,"libpd_init"); PD_MULTI_BIND(block_,"libpd_blocksize");
        PD_MULTI_BIND(new_,"libpd_new_instance"); PD_MULTI_BIND(set_,"libpd_set_instance");
        PD_MULTI_BIND(free_,"libpd_free_instance"); PD_MULTI_BIND(main_,"libpd_main_instance");
        PD_MULTI_BIND(audio_,"libpd_init_audio");
        PD_MULTI_BIND(open_,"libpd_openfile"); PD_MULTI_BIND(closePatch_,"libpd_closefile");
        PD_MULTI_BIND(process_,"libpd_process_float"); PD_MULTI_BIND(float_,"libpd_float");
        PD_MULTI_BIND(note_,"libpd_noteon"); PD_MULTI_BIND(start_,"libpd_start_message");
        PD_MULTI_BIND(add_,"libpd_add_float"); PD_MULTI_BIND(finish_,"libpd_finish_message");
#undef PD_MULTI_BIND
        int r=init_();
        if((r!=0 && r!=-1)||block_()!=64){error="Could not initialize a 64-frame libpd.";close();return false;}
        // Non-MULTI builds may export the symbol but return nullptr.
        void* probe=new_();
        if(!probe){error="This libpd was not built with MULTI=true.";close();return false;}
        set_(probe);free_(probe);set_(main_());return true;
    }
    bool addPatch(int id,const std::string& file,int rate,int channels,std::string& error) {
        if(!library_||id<=0||rate<8000||rate>192000||channels<1||channels>8||find(id)){
            error="Invalid or duplicate PD instance.";return false;
        }
        std::error_code ec;auto path=std::filesystem::absolute(std::filesystem::u8path(file),ec);
        if(ec||path.extension()!=".pd"||!std::filesystem::is_regular_file(path,ec)||ec){
            error="Patch is missing or is not a .pd file.";return false;
        }
        Instance item{id,new_(),nullptr,channels};
        if(!item.instance){error="Cannot create an isolated PD instance.";return false;}
        set_(item.instance);
        if(audio_(0,channels,rate)!=0){free_(item.instance);set_(main_());error="Cannot configure PD audio.";return false;}
        const auto name=path.filename().u8string(),dir=path.parent_path().u8string();
        item.patch=open_(name.c_str(),dir.c_str());
        if(!item.patch){free_(item.instance);set_(main_());error="Cannot open PD patch.";return false;}
        if(start_(1)!=0){closePatch_(item.patch);free_(item.instance);set_(main_());error="Cannot start PD DSP.";return false;}
        add_(1.f);
        if(finish_("pd","dsp")!=0){closePatch_(item.patch);free_(item.instance);set_(main_());error="Cannot enable PD DSP.";return false;}
        instances_.push_back(item);error.clear();return true;
    }
    bool tick(int id,float* output,std::string& error) {
        Instance* item=find(id);
        if(!item||!output){error="Unknown PD instance.";return false;}
        set_(item->instance);std::array<float,64> input{};
        if(process_(1,input.data(),output)!=0){error="PD processing failed.";return false;}
        for(int i=0;i<64*item->channels;++i)if(!std::isfinite(output[i]))output[i]=0.f;
        return true;
    }
    bool number(int id,const std::string& receiver,float number) {
        Instance* item=find(id);if(!item||receiver.empty()||!std::isfinite(number))return false;
        set_(item->instance);return float_(receiver.c_str(),number)==0;
    }
    bool noteOn(int id,int channel,int pitch,int velocity) {
        Instance* item=find(id);if(!item||channel<0||pitch<0||pitch>127||velocity<0||velocity>127)return false;
        set_(item->instance);return note_(channel,pitch,velocity)==0;
    }
    void close() noexcept {
        for(auto& item:instances_){
            if(set_)set_(item.instance);
            if(start_&&add_&&finish_&&start_(1)==0){add_(0.f);finish_("pd","dsp");}
            if(item.patch&&closePatch_)closePatch_(item.patch);
            if(item.instance&&free_)free_(item.instance);
        }
        instances_.clear();
        if(library_&&set_&&main_)set_(main_());
        if(library_){
#ifdef _WIN32
            FreeLibrary(reinterpret_cast<HMODULE>(library_));
#else
            dlclose(library_);
#endif
        }
        library_=nullptr;
        init_=nullptr;block_=nullptr;new_=nullptr;set_=nullptr;free_=nullptr;main_=nullptr;
        audio_=nullptr;open_=nullptr;closePatch_=nullptr;process_=nullptr;float_=nullptr;
        note_=nullptr;start_=nullptr;add_=nullptr;finish_=nullptr;
    }
private:
    struct Instance {int id;void* instance;void* patch;int channels;};
    std::vector<Instance> instances_;
    Instance* find(int id){for(auto& item:instances_)if(item.id==id)return &item;return nullptr;}
    void* symbol(const char* name) {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library_),name));
#else
        return dlsym(library_,name);
#endif
    }
    void* library_=nullptr;
    int (*init_)()=nullptr; int (*block_)()=nullptr;
    void* (*new_)()=nullptr;void (*set_)(void*)=nullptr;void (*free_)(void*)=nullptr;void* (*main_)()=nullptr;
    int (*audio_)(int,int,int)=nullptr;void* (*open_)(const char*,const char*)=nullptr;
    void (*closePatch_)(void*)=nullptr;int (*process_)(int,const float*,float*)=nullptr;
    int (*float_)(const char*,float)=nullptr;int (*note_)(int,int,int)=nullptr;
    int (*start_)(int)=nullptr;void (*add_)(float)=nullptr;int (*finish_)(const char*,const char*)=nullptr;
};
