#include "csound_api_runner.h"
#include "vst_rack.h"
#include "wav_writer.h"
#include "audio_channels.h"
#include "platform.h"
#include "pd_host_mixer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#else
#include <dlfcn.h>
#endif

namespace {
#ifdef _WIN32
using Library = HMODULE;
Library openLibrary(const std::string& name) {
    return LoadLibraryExA(name.c_str(), nullptr, name.find_first_of("\\/") == std::string::npos ? 0 : LOAD_WITH_ALTERED_SEARCH_PATH);
}
void* symbol(Library lib, const char* name) { return reinterpret_cast<void*>(GetProcAddress(lib, name)); }
void closeLibrary(Library lib) { if (lib) FreeLibrary(lib); }
#else
using Library = void*;
Library openLibrary(const std::string& name) { return dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* symbol(Library lib, const char* name) { return dlsym(lib, name); }
void closeLibrary(Library lib) { if (lib) dlclose(lib); }
#endif

// Why a library would not load: the system's own text, plus DLLs of its folder that another engine already holds.
std::string loadFailure(const std::string& file) {
#ifdef _WIN32
    const DWORD code = GetLastError();
    char text[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof(text), nullptr);
    std::string reason = text;
    while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r' || reason.back() == ' ')) reason.pop_back();
    std::string message = "Could not load " + file + " (Windows error " + std::to_string(code) +
                          (reason.empty() ? "" : ": " + reason) + ").";
    const std::string clash = plat::dllConflictReport(std::filesystem::path(file).parent_path().string());
    if (!clash.empty()) message += " Another engine (SuperCollider / Pure Data) already uses its own copy of:" + clash;
    return message;
#else
    const char* why = dlerror();
    return "Could not load " + file + (why ? std::string(": ") + why : std::string());
#endif
}

// Fixed-capacity SPSC lines; a slow UI never blocks the engine thread.
class LogQueue {
    static constexpr size_t count = 512, lineSize = 2048;
    std::array<std::array<char, lineSize>, count> lines_{};
    std::atomic<size_t> head_{0}, tail_{0};
    std::atomic<unsigned> dropped_{0};
    std::array<char, lineSize> partial_{};
    size_t used_ = 0;
public:
    void push(const char* text) noexcept {
        if (!text) return;
        while (*text) {
            const char ch = *text++;
            if (ch == '\n') { publish(); continue; }
            if (ch == '\r') continue;
            if (used_ + 1 >= lineSize) publish();
            partial_[used_++] = ch;
        }
    }
    void flush() noexcept { if (used_) publish(); }
    void publish() noexcept {
        const size_t head = head_.load(std::memory_order_relaxed), next = (head + 1) % count;
        if (next == tail_.load(std::memory_order_acquire)) { ++dropped_; used_ = 0; return; }
        partial_[used_] = 0;
        std::memcpy(lines_[head].data(), partial_.data(), used_ + 1);
        used_ = 0;
        head_.store(next, std::memory_order_release);
    }
    std::vector<std::string> take() {
        std::vector<std::string> out;
        size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        while (tail != head) {
            out.emplace_back(lines_[tail].data());
            tail = (tail + 1) % count;
        }
        tail_.store(tail, std::memory_order_release);
        const unsigned n = dropped_.exchange(0, std::memory_order_relaxed);
        if (n) out.push_back("[caw] Some Csound messages were dropped because the UI was busy.");
        return out;
    }
};
}

struct CsoundApiRunner::Impl {
    Library lib = nullptr;
    int version = 0, sampleBytes = 0;
    int (*initialize)(int) = nullptr;
    int (*getVersion)() = nullptr;
    int (*getSampleSize)() = nullptr;
    void* (*create6)(void*) = nullptr;
    void* (*create7)(void*, const char*) = nullptr;
    void (*destroy)(void*) = nullptr;
    int (*cleanup6)(void*) = nullptr;
    int (*compile6)(void*, const char*) = nullptr;
    int (*compile7)(void*, const char*, int, int) = nullptr;
    int (*startEngine)(void*) = nullptr;
    void (*hostAudio6)(void*, int, int) = nullptr;
    void (*hostAudio7)(void*) = nullptr;
    int (*perform)(void*) = nullptr;
    unsigned (*ksmps)(void*) = nullptr;
    unsigned (*channels6)(void*) = nullptr;
    unsigned (*channels7)(void*, int) = nullptr;
    const void* (*spout)(void*) = nullptr;
    int (*channelPtr)(void*, void**, const char*, int) = nullptr;
    void (*createMessages)(void*, int) = nullptr;
    int (*messageCount)(void*) = nullptr;
    const char* (*firstMessage)(void*) = nullptr;
    void (*popMessage)(void*) = nullptr;
    void (*destroyMessages)(void*) = nullptr;
    std::thread worker;
    std::atomic<bool> stopRequested{false}, live{false}, done{true};
    std::atomic<int> commandSerial{0};
    std::atomic<int> code{0};
    std::atomic<float> masterGain{1.f};
    std::unique_ptr<MonitorCapture> capture;
    std::unique_ptr<MonitorCapture> outputCapture;
    int outputMonitorChannels = 0;
    mutable uint64_t workerMonitorSequence = 0;
    LogQueue logs;
    int (*getTable)(void*, void**, int) = nullptr;
    std::vector<VstJob> vstJobs;
    std::string pdLibrary;
    std::vector<pd_timeline::Event> pdPlan;
    std::unique_ptr<PdHostMixer> pd;
    std::atomic<bool> pdPrepared{false};
    int vstSampleRate = 48000;
    // CAW hub (caws/cawr). hubNames/hubChans are guarded by hubMutex: setHubNames runs on the UI
    // thread, the engine thread polls and delivers. Channel pointers resolve lazily - a clip's
    // chnget/chnset creates "caws/<name>" at any moment.
    CawHub* hub = nullptr;
    mutable std::mutex hubMutex;
    std::set<std::string> hubNames;
    std::map<std::string, void*> hubChans;
    std::atomic<std::uint64_t> hubSeen{0};
    unsigned hubResolveCountdown = 0;

    template<class T> bool get(T& fn, const char* name) {
        fn = reinterpret_cast<T>(symbol(lib, name));
        return fn != nullptr;
    }
    std::string drainedText;   // the engine's own messages of the current run, for targeted advice on failure
    void drain(void* cs) {
        for (int n = 0; n < 1024 && messageCount(cs) > 0; ++n) {
            const std::string text = firstMessage(cs);
            logs.push(text.c_str());
            drainedText += text;
            popMessage(cs);
        }
    }
    void run(std::string path, int meterCount, bool captureEnabled, bool hostOutput, CawAudioSettings audioSettings,
             bool offline, std::string offlinePath, int wavFormat, int outputChannels) {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        drainedText.clear();
        void* cs = version == 6 ? create6(nullptr) : create7(nullptr, nullptr);
        if (!cs) { logs.push("[caw] Could not create the Csound API engine.\n"); code = -1; live = false; done = true; return; }
        createMessages(cs, 0);
        // Prevent Csound from opening a second hardware stream. CAW owns I/O.
        if (hostOutput || offline) {
            if (version == 6) hostAudio6(cs, 1, 0);
            else hostAudio7(cs);
        }
        int status = version == 6 ? compile6(cs, path.c_str()) : compile7(cs, path.c_str(), 0, 0);
        drain(cs);
        if (status == 0 && !stopRequested.load()) status = startEngine(cs);
        const bool started = status == 0 && !stopRequested.load();
        drain(cs);
        if (!started && !stopRequested.load()) {
            // Csound's own message above names the file, line and opcode. CAW
            // remaps the line to "instr <name>, clip line N" in the console. If
            // that message still shows code that was already fixed, an older
            // caw.exe is still running: close every CAW instance and rebuild.
            logs.push("[caw] Csound could not compile or start the generated project. The engine message above names the file, line and opcode; CAW shows the line as 'instr <name>, clip line N'. If it still shows code you already fixed, close every CAW window and rebuild - an older caw.exe is still running.");
            if (drainedText.find("Unable to find opcode entry for") != std::string::npos)
                logs.push("[caw] The opcode named above does not exist in this Csound. CAW builds its meters from the real opcode list (abs first, then sqrt(x*x)), so a missing abs or sqrt means this Csound copy is incomplete or a very early Csound 7 build: point CAW at the installed Csound (Settings > Programs) or re-copy it with 1-PREPARE. Any other missing opcode comes from clip code.");
#ifdef _WIN32
            {   // A missing-opcode error with a Csound copy inside CAW's folder usually means a plugin DLL
                // lost the one-copy-per-name race against libpd (Csound + PD mode only). Show what ran.
                std::vector<wchar_t> name(32768);
                const DWORD n = GetModuleFileNameW(lib, name.data(), static_cast<DWORD>(name.size()));
                std::string libDir;
                if (n && n < name.size()) {
                    const std::filesystem::path libPath(std::wstring(name.data(), n));
                    libDir = libPath.parent_path().u8string();
                    logs.push(("[caw] Csound API library: " + libPath.u8string() + "\n").c_str());
                }
                for (const char* var : {"OPCODE6DIR64", "OPCODE7DIR64"}) {
                    char value[1024] = {};
                    const DWORD len = GetEnvironmentVariableA(var, value, sizeof(value));
                    logs.push((std::string("[caw] ") + var + " = " + (len && len < sizeof(value) ? value : "(not set)") + "\n").c_str());
                }
                const std::string clash = plat::dllConflictReport(libDir);
                if (!clash.empty())
                    logs.push(("[caw] These Csound DLLs came from another engine's folder instead of Csound's own:" + clash +
                               "\nRestart CAW and try Csound + PD again - CAW now preloads Csound's own DLLs first. If it still fails, send these lines.\n").c_str());
            }
#endif
        }
        std::vector<void*> meterPtrs((size_t)meterCount, nullptr);
        void* serialPtr = nullptr;
        void* pdAckPtr = nullptr;
        unsigned nchan = 0, blockSize = 0;
        if (status == 0 && !stopRequested.load()) {
            nchan = version == 6 ? channels6(cs) : channels7(cs, 0);
            blockSize = ksmps(cs);
            // Only the engine thread touches Csound's channel memory.
            if (!offline && (channelPtr(cs, &serialPtr, "__caw_cmd_serial", 1 | 16) != 0 || !serialPtr)) {
                logs.push("[caw] Could not open the Csound command channel.\n");
                status = -1;
            }
            if (pd && hostOutput && (channelPtr(cs,&pdAckPtr,"__caw_loaded_serial",1 | 32)!=0 || !pdAckPtr)) {
                logs.push("[caw] Could not open the shared transport acknowledgement channel.\n");
                status=-1;
            }
            if ((captureEnabled || hostOutput || offline) && (nchan == 0 || nchan > 64 || blockSize == 0 || blockSize > 65536)) {
                logs.push("[caw] Csound returned an unsupported audio block layout.\n");
                status = -1;
            }
            for (int i = 0; status == 0 && captureEnabled && i < meterCount; ++i) {
                const std::string name = "__caw_meter_" + std::to_string(i);
                // Csound control channel + output channel. Only this engine
                // thread reads the pointer, after csoundPerformKsmps.
                if (channelPtr(cs, &meterPtrs[(size_t)i], name.c_str(), 1 | 32) != 0 || !meterPtrs[(size_t)i]) {
                    logs.push("[caw] Could not open a Csound meter channel.\n");
                    status = -1;
                }
            }
        }
        if (status == 0 && !stopRequested.load() && pd && hostOutput && !offline) {
            std::string error;
            bool prepared=false;
            try {prepared=pd->prepare(pdLibrary,pdPlan,audioSettings.sampleRate,(int)nchan,blockSize,error);}
            catch(const std::exception& e){error=e.what();}
            catch(...){error="Unexpected exception while preparing PD.";}
            if (!prepared) {
                logs.push(("[caw] Shared PD preparation failed: " + error + "\n").c_str());
                status=-1;
            } else {
                pdPrepared.store(true,std::memory_order_release);
                logs.push("[caw] PD and Csound share CAW's audio stream.\n");
            }
        }
        CawAudioOutput output;
        CawWavWriter wav;
        if (status == 0 && !stopRequested.load() && offline) {
            std::string error;
            if (nchan != (unsigned)audioSettings.channels ||
                !wav.open(offlinePath, audioSettings.sampleRate, outputChannels, wavFormat, error)) {
                logs.push(("[caw] Offline WAV output failed: " + error + "\n").c_str());
                status = -1;
            }
        }
        if (status == 0 && !stopRequested.load() && hostOutput) {
            if (outputCapture) output.setMonitorCapture(outputCapture.get());
            if (nchan != (unsigned)audioSettings.channels) {
                logs.push("[caw] Csound output channel count differs from the CAW soundcard setup.\n");
                status = -1;
            } else {
                std::string outputError;
                if (!output.open(audioSettings, outputError)) {
                    logs.push(("[caw] Audio output failed: " + outputError + "\n").c_str());
                    status = -1;
                } else {
                    logs.push(("[caw] CAW audio output: " + output.description() + "\n").c_str());
                }
            }
        }
        VstRack vstRack;
        if (status == 0 && !stopRequested.load() && !vstJobs.empty() && nchan > 0 && blockSize > 0) {
#ifdef _WIN32
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // VST3 plugins expect COM on the thread that loads them
#endif
            bool ok = false;
            try {
                ok = vstRack.init(vstJobs, vstSampleRate, (int)blockSize, (int)nchan, sampleBytes,
                    [&](const std::string& n, int flags) -> void* { void* p = nullptr; return channelPtr(cs, &p, n.c_str(), flags) == 0 ? p : nullptr; },
                    [&](int number) -> const void* { void* t = nullptr; return (getTable && number > 0 && getTable(cs, &t, number) > 0) ? t : nullptr; },
                    [&](const std::string& s) { logs.push(s.c_str()); }, offline);
            } catch (...) { logs.push("[caw] VST initialization threw an exception.\n"); }
            if (!ok) { logs.push("[caw] VST initialization failed; export stopped.\n"); status = -1; }
        }
        if (status == 0 && !stopRequested.load()) {
#ifdef _WIN32
            {   // Say WHICH Csound library is running: with several Csound folders around, a wrong or old
                // copy hides version problems (for example an opcode that only exists in one of them).
                std::vector<wchar_t> name(32768);
                const DWORD n = GetModuleFileNameW(lib, name.data(), static_cast<DWORD>(name.size()));
                if (n && n < name.size()) {
                    const int full = getVersion();
                    logs.push(("[caw] Csound API library: " + std::filesystem::path(std::wstring(name.data(), n)).u8string()
                               + " (Csound " + std::to_string(full / 1000) + "." + std::to_string((full % 1000) / 10) + ")\n").c_str());
                }
            }
#endif
            logs.push("[caw] Csound API engine started.\n");
            unsigned logFrames = 0;
            bool firstBlockDone = false;
            std::vector<double> convertedD;
            std::vector<float> convertedF;
            while (!stopRequested.load(std::memory_order_relaxed)) {
                const int serial = commandSerial.load(std::memory_order_acquire);
                if (!offline) {
                    if (sampleBytes == 8) *static_cast<double*>(serialPtr) = static_cast<double>(serial);
                    else *static_cast<float*>(serialPtr) = static_cast<float>(serial);
                }
                status = perform(cs);
                if (status != 0) break;
                const void* buffer = (hostOutput || captureEnabled || offline) ? spout(cs) : nullptr;
                if (buffer) {
                    try { vstRack.process(const_cast<void*>(buffer), captureEnabled ? capture.get() : nullptr, audioSettings.fullScale); }
                    catch (...) { logs.push("[caw] VST processing failed.\n"); status = -1; break; }
                }
                if ((hostOutput || captureEnabled || offline) && !buffer) {
                    logs.push("[caw] Csound did not provide an audio output buffer.\n");
                    status = -1;
                    break;
                }
                if (pd && hostOutput) {
                    const float* pdAudio=nullptr;
                    std::string error;
                    const double ack=sampleBytes==8 ? *static_cast<const double*>(pdAckPtr) :
                                                     *static_cast<const float*>(pdAckPtr);
                    const int ackSerial=std::isfinite(ack) ? (int)std::max(0.0,std::min(ack,static_cast<double>(INT32_MAX))) : 0;
                    bool rendered=false;
                    try {rendered=pd->render(blockSize,pdAudio,error,ackSerial);}
                    catch(const std::exception& e){error=e.what();}
                    catch(...){error="Unexpected exception while processing PD.";}
                    for(const auto& message:pd->takeRuntimeNotes())
                        logs.push(("[caw] "+message+"\n").c_str());
                    if(!rendered) {
                        logs.push(("[caw] Shared PD processing failed: "+error+"\n").c_str());
                        status=-1;break;
                    }
                    for(const auto& warning:pd->takeAuditionErrors())
                        logs.push(("[caw] "+warning+"\n").c_str());
                    const size_t count=(size_t)blockSize*nchan;
                    if(sampleBytes==8) {
                        auto* data=static_cast<double*>(const_cast<void*>(buffer));
                        for(size_t i=0;i<count;++i)data[i]+=static_cast<double>(pdAudio[i])*audioSettings.fullScale;
                    } else {
                        auto* data=static_cast<float*>(const_cast<void*>(buffer));
                        for(size_t i=0;i<count;++i)data[i]+=pdAudio[i]*audioSettings.fullScale;
                    }
                }
                if (hub) {
                    // CAW hub (caws/cawr): poll the "caws/<name>" channels Csound clips wrote with
                    // the caws command into the shared bus, then write changed bus values back into
                    // the channels (clips read them with cawr) and deliver them to the PD instances.
                    if (hubResolveCountdown == 0) {   // re-resolve pointers about once a second
                        hubResolveCountdown = 250;
                        std::lock_guard<std::mutex> lock(hubMutex);
                        for (const auto& name : hubNames) {
                            const std::string full = "caws/" + name;
                            void* p = nullptr;
                            if (channelPtr(cs, &p, full.c_str(), 1 | 32) == 0 && p) hubChans[name] = p;
                        }
                    } else --hubResolveCountdown;
                    {   // Csound clips (caws) -> shared bus
                        std::lock_guard<std::mutex> lock(hubMutex);
                        for (const auto& entry : hubChans) {
                            const double value = sampleBytes == 8
                                ? *static_cast<const double*>(entry.second)
                                : static_cast<double>(*static_cast<const float*>(entry.second));
                            if (std::isfinite(value)) hub->send(entry.first, value);
                        }
                    }
                    std::map<std::string,double> hubValues;
                    std::uint64_t hubSeenLocal = hubSeen.load(std::memory_order_acquire);
                    if (hub->take(hubSeenLocal, hubValues)) {
                        hubSeen.store(hubSeenLocal, std::memory_order_release);
                        {   // shared bus -> Csound clips (cawr reads "caws/<name>")
                            std::lock_guard<std::mutex> lock(hubMutex);
                            for (const auto& entry : hubValues) {
                                auto it = hubChans.find(entry.first);
                                if (it == hubChans.end()) {   // created by a late chnget: try once now
                                    void* p = nullptr;
                                    const std::string full = "caws/" + entry.first;
                                    if (channelPtr(cs, &p, full.c_str(), 1 | 32) == 0 && p) {
                                        hubChans[entry.first] = p;
                                        it = hubChans.find(entry.first);
                                    }
                                }
                                if (it != hubChans.end()) {
                                    if (sampleBytes == 8) *static_cast<double*>(it->second) = entry.second;
                                    else *static_cast<float*>(it->second) = static_cast<float>(entry.second);
                                }
                            }
                        }
                        if (pd) pd->deliverHub(hubValues);
                    }
                }
                const float gain = masterGain.load(std::memory_order_relaxed);
                if (hostOutput) {
                    const size_t sampleCount = (size_t)blockSize * nchan;
                    if (sampleBytes == 8) {
                        double* data = static_cast<double*>(const_cast<void*>(buffer));
                        for (size_t i = 0; i < sampleCount; ++i) data[i] *= gain;
                    } else {
                        float* data = static_cast<float*>(const_cast<void*>(buffer));
                        for (size_t i = 0; i < sampleCount; ++i) data[i] *= gain;
                    }
                    const bool ok = sampleBytes == 8
                        ? output.push(static_cast<const double*>(buffer), blockSize, stopRequested)
                        : output.push(static_cast<const float*>(buffer), blockSize, stopRequested);
                    if (!ok) {
                        if (!stopRequested.load()) { logs.push("[caw] Soundcard output stopped unexpectedly.\n"); status = -1; }
                        break;
                    }
                }
                if (offline) {
                    std::string error;
                    bool ok = false;
                    if (sampleBytes == 8) {
                        mapExportChannels(static_cast<const double*>(buffer), blockSize, (int)nchan, outputChannels, convertedD);
                        ok = wav.write(convertedD.data(), blockSize, audioSettings.fullScale, gain, error);
                    } else {
                        mapExportChannels(static_cast<const float*>(buffer), blockSize, (int)nchan, outputChannels, convertedF);
                        ok = wav.write(convertedF.data(), blockSize, audioSettings.fullScale, gain, error);
                    }
                    if (!ok) { logs.push(("[caw] Offline output failed: " + error + "\n").c_str()); status = -1; break; }
                }
                if (!firstBlockDone) {   // the first block really went through Csound and out to the soundcard: only now is the engine live
                    firstBlockDone = true;
                    logs.push("[caw] Csound is running: first audio block delivered.\n");
                }
                if (captureEnabled && capture) {
                    for (int i = 0; i < meterCount; ++i) {
                        const void* ptr = meterPtrs[(size_t)i];
                        float value = sampleBytes == 8 ? (float)*static_cast<const double*>(ptr)
                                                       : *static_cast<const float*>(ptr);
                        if (hostOutput && i < (int)nchan) {
                            value = 0.f;
                            for (unsigned frame = 0; frame < blockSize; ++frame) {
                                const size_t ix = (size_t)frame * nchan + (size_t)i;
                                const float sample = sampleBytes == 8 ? (float)static_cast<const double*>(buffer)[ix] : static_cast<const float*>(buffer)[ix];
                                if (std::isfinite(sample)) value = std::max(value, std::fabs(sample) / audioSettings.fullScale);
                            }
                        }
                        capture->addMeter(i, value);
                    }
                    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    if (sampleBytes == 8)
                        capture->pushInterleaved(static_cast<const double*>(buffer), (int)blockSize, (int)nchan, now);
                    else
                        capture->pushInterleaved(static_cast<const float*>(buffer), (int)blockSize, (int)nchan, now);
                }
                logFrames += blockSize;
                if (logFrames >= 1024) { drain(cs); logFrames = 0; }
            }
        }
        if (hostOutput) {
            output.close();
            if (output.underflows() > 0)
                logs.push(("[caw] Audio buffer underruns: " + std::to_string(output.underflows()) + "\n").c_str());
        }
        if (offline && status > 0 && !stopRequested.load()) {
            std::string error;
            if (!wav.finish(error)) { logs.push(("[caw] " + error + "\n").c_str()); status = -1; }
        }
        if (version == 6 && started && cleanup6) cleanup6(cs);
        drain(cs);
        destroyMessages(cs);
        destroy(cs);
        logs.flush();
        pdPrepared.store(false,std::memory_order_release);
        if(pd && hostOutput && !offline)pd->shutdown();
        // A positive perform status means normal score completion, but a
        // positive compile/start status is still an error.
        code = offline ? ((started && status > 0 && !stopRequested.load()) ? 0 : -1)
                       : (status > 0 ? 0 : status);
        live = false;
        done = true;
    }
};

CsoundApiRunner::CsoundApiRunner() : d(new Impl) {}
CsoundApiRunner::~CsoundApiRunner() { stop(); closeLibrary(d->lib); }
bool CsoundApiRunner::prepare(const std::string& executable, std::string& error) {
    stop(); error.clear();
    closeLibrary(d->lib); d->lib = nullptr;
    std::vector<std::string> names;
#ifdef _WIN32
    const char* libnames[] = {"csound64.dll", "csound7_64.dll", "csound7.dll", "csound64-7.0.dll"};
#elif defined(__APPLE__)
    const char* libnames[] = {"libcsound64.dylib", "libcsound64.7.0.dylib", "libcsound64.6.0.dylib"};
#else
    const char* libnames[] = {"libcsound64.so", "libcsound64.so.7.0", "libcsound64.so.6.0"};
#endif
    std::filesystem::path path(executable);
#ifdef _WIN32
    if (!path.has_parent_path()) {
        std::array<char, 32768> found{};
        const DWORD n = SearchPathA(nullptr, executable.c_str(), ".exe", (DWORD)found.size(), found.data(), nullptr);
        if (n > 0 && n < found.size()) path = found.data();
    }
#endif
    if (path.has_parent_path()) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(path.parent_path(), ec);
        const auto dir = ec ? path.parent_path() : absolute;
        for (const char* name : libnames) names.push_back((dir / name).string());
    }
    // 1. The library that sits beside csound.exe. If one exists but will not load, say why: silently using some
    //    other Csound found on PATH (e.g. an installed Csound 6) hides the real problem and mixes versions.
    std::string failure;
    bool foundBeside = false;
    for (const auto& file : names) {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) continue;
        foundBeside = true;
        if ((d->lib = openLibrary(file))) break;
        failure = loadFailure(file);
    }
    // 2. Only when nothing was found beside csound.exe (e.g. plain "csound" on PATH): the library search path.
    if (!d->lib && !foundBeside) for (const char* name : libnames) if ((d->lib = openLibrary(name))) break;
    if (!d->lib) {
        error = !failure.empty() ? failure : "Csound API library not found beside the Csound program or in the library path.";
        return false;
    }
    if (!d->get(d->getVersion, "csoundGetVersion") || !d->get(d->getSampleSize, "csoundGetSizeOfMYFLT")) {
        error = "Csound API version or sample format is unavailable."; closeLibrary(d->lib); d->lib = nullptr; return false;
    }
    d->version = d->getVersion() / 1000;
    d->sampleBytes = d->getSampleSize();
    bool ok = (d->version == 6 || d->version == 7) && (d->sampleBytes == 4 || d->sampleBytes == 8);
    ok = d->get(d->initialize, "csoundInitialize") && ok;
    ok = d->get(d->destroy, "csoundDestroy") && ok;
    ok = d->get(d->startEngine, "csoundStart") && ok;
    d->hostAudio6 = nullptr; d->hostAudio7 = nullptr;
    if (d->version == 6) d->get(d->hostAudio6, "csoundSetHostImplementedAudioIO");
    else if (d->version == 7) d->get(d->hostAudio7, "csoundSetHostAudioIO");
    ok = d->get(d->perform, "csoundPerformKsmps") && ok;
    ok = d->get(d->ksmps, "csoundGetKsmps") && ok;
    ok = d->get(d->spout, "csoundGetSpout") && ok;
    ok = d->get(d->channelPtr, "csoundGetChannelPtr") && ok;
    d->get(d->getTable, "csoundGetTable");   // optional: only the VST tracks need it
    ok = d->get(d->createMessages, "csoundCreateMessageBuffer") && ok;
    ok = d->get(d->messageCount, "csoundGetMessageCnt") && ok;
    ok = d->get(d->firstMessage, "csoundGetFirstMessage") && ok;
    ok = d->get(d->popMessage, "csoundPopFirstMessage") && ok;
    ok = d->get(d->destroyMessages, "csoundDestroyMessageBuffer") && ok;
    if (d->version == 6) {
        ok = d->get(d->create6, "csoundCreate") && d->get(d->compile6, "csoundCompileCsd") &&
             d->get(d->channels6, "csoundGetNchnls") && d->get(d->cleanup6, "csoundCleanup") && ok;
    } else if (d->version == 7) {
        ok = d->get(d->create7, "csoundCreate") && d->get(d->compile7, "csoundCompileCSD") &&
             d->get(d->channels7, "csoundGetChannels") && ok;
    }
    if (!ok) { error = "Csound API version or required functions are unsupported."; closeLibrary(d->lib); d->lib = nullptr; return false; }
    if (d->initialize(1 | 2) < 0) { error = "Csound API initialization failed."; closeLibrary(d->lib); d->lib = nullptr; return false; }
    return true;
}

bool CsoundApiRunner::probeMath(const std::string& executable, bool& hasAbs, bool& hasSqrt, bool& hasPow, std::string& error) {
    hasAbs = hasSqrt = hasPow = true;   // defaults; only Csound's own "no such opcode" error clears one
    error.clear();
    // Loads exactly the API library the engine would use beside this csound.exe (or from the library path).
    if (!prepare(executable, error)) return false;
    // Compile-only probe orchestras with the exact forms the meter/spectrum code generates.
    // "aM init 0.25" makes an a-rate variable without depending on any signal opcode; the probes use
    // no audio device and no output file, they only have to COMPILE. No <CsOptions> section (compiling
    // needs none) and every CSD tag on its own line - Csound's CSD reader is line-based: a same-line
    // "<CsOptions>-n -d</CsOptions>" was NOT recognized as closed, the options stream ran on into the
    // orchestra body ("Invalid arguments in <CsOptions>: <CsInstruments>", "Additional string \"=\"")
    // and all three probes failed although the Csound was perfectly fine.
    static const char* absProbe =
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "sr = 48000\nksmps = 32\nnchnls = 1\n0dbfs = 1\n"
        // The three forms the meter code really generates: simple a-rate, the parenthesized lane sum,
        // and k-rate. A fresh Csound 7 build rejected abs((0)) - an i-rate constant - so the generator
        // never emits that; the probe checks exactly the forms that remain.
        "instr 1\naM init 0.25\naA = abs(aM)\naS2 = abs((aM + aM))\nkM init 0.25\nkA = abs(kM)\nendin\n"
        "</CsInstruments>\n</CsoundSynthesizer>\n";
    static const char* sqrtProbe =
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "sr = 48000\nksmps = 32\nnchnls = 1\n0dbfs = 1\n"
        "instr 1\naM init 0.25\naS = sqrt(aM)\nkM init 0.25\nkS = sqrt(kM)\nendin\n"
        "</CsInstruments>\n</CsoundSynthesizer>\n";
    static const char* powProbe =
        "<CsoundSynthesizer>\n<CsInstruments>\n"
        "sr = 48000\nksmps = 32\nnchnls = 1\n0dbfs = 1\n"
        "instr 1\nkM init 0.25\nkP = pow(kM, 0.5)\nendin\n"
        "</CsInstruments>\n</CsoundSynthesizer>\n";
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    auto tryCompile = [&](const char* text) -> std::string {
        void* cs = d->version == 6 ? d->create6(nullptr) : d->create7(nullptr, nullptr);
        if (!cs) return "could not create a Csound instance";
        d->createMessages(cs, 0);
        std::string message;
        if (dir.empty()) message = "no temp folder for the probe orchestra";
        else {
            static std::atomic<unsigned> serial{0};
            const std::filesystem::path file = dir / ("caw_math_probe_" + std::to_string(serial++) + ".csd");
            { std::ofstream f(file, std::ios::binary); f << text; }
            const int status = d->version == 6 ? d->compile6(cs, file.u8string().c_str())
                                               : d->compile7(cs, file.u8string().c_str(), 0, 0);
            if (status != 0) for (int n = 0; n < 8 && d->messageCount(cs) > 0; ++n) {
                const char* m = d->firstMessage(cs);
                if (m && *m) { if (!message.empty()) message += " | "; message += m; }
                d->popMessage(cs);
            }
            if (message.size() > 300) { message.resize(300); message += " ..."; }
            std::error_code rm; std::filesystem::remove(file, rm);
        }
        d->destroy(cs);
        return message;
    };
    const std::string absError = tryCompile(absProbe);
    const std::string sqrtError = tryCompile(sqrtProbe);
    const std::string powError = tryCompile(powProbe);
    // A probe failure only counts as "opcode missing" when Csound itself says so. Any other failure
    // (option parsing, file I/O, an internal error) is a PROBE problem, not proof about the opcode:
    // keep the default (true) and log it as inconclusive - a real compile that fails shows its own
    // error live anyway. Csound's definite "no such opcode" messages are "Unable to find opcode entry
    // for 'abs'..." and "unknown opcode 'abs'".
    auto definiteMissing = [](const std::string& message) {
        std::string low = message;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return low.find("unable to find opcode entry") != std::string::npos
            || low.find("unknown opcode") != std::string::npos;
    };
    std::string detail;
    if (!absError.empty())
        detail += definiteMissing(absError) ? (hasAbs = false, "abs does not compile (" + absError + "). ")
                                            : "abs probe inconclusive (" + absError + "). ";
    if (!sqrtError.empty())
        detail += definiteMissing(sqrtError) ? (hasSqrt = false, "sqrt does not compile (" + sqrtError + "). ")
                                             : "sqrt probe inconclusive (" + sqrtError + "). ";
    if (!powError.empty())
        detail += definiteMissing(powError) ? (hasPow = false, "pow does not compile (" + powError + "). ")
                                            : "pow probe inconclusive (" + powError + "). ";
    if (!detail.empty()) error = detail;
    return true;
}
bool CsoundApiRunner::start(const std::string& path, int sampleRate, int meterCount, bool capture, std::string& error,
                            const CawAudioSettings* audio) {
    error.clear();
    if (!d->lib || d->worker.joinable() || meterCount < 0) { error = "Csound API engine is not ready."; return false; }
    if (audio && ((d->version == 6 && !d->hostAudio6) || (d->version == 7 && !d->hostAudio7))) {
        error = "This Csound library does not support host-managed audio. Use the OSC launcher."; return false;
    }
    try {
        d->vstSampleRate = sampleRate;
        d->capture.reset(capture ? new MonitorCapture(sampleRate, meterCount) : nullptr);
        d->outputCapture.reset(audio ? new MonitorCapture(sampleRate, meterCount) : nullptr);
        d->outputMonitorChannels = audio ? audio->channels : 0;
        d->workerMonitorSequence = 0;
        d->stopRequested = false; d->code = 0; d->done = false; d->live = true;
        d->worker = std::thread(&Impl::run, d.get(), path, meterCount, capture, audio != nullptr,
                                audio ? *audio : CawAudioSettings{}, false, std::string(), 2, 0);
    } catch (...) {
        d->live = false; d->done = true; d->capture.reset(); d->outputCapture.reset(); d->outputMonitorChannels = 0;
        error = "Could not start the Csound engine thread."; return false;
    }
    return true;
}
bool CsoundApiRunner::startOffline(const std::string& path, int sampleRate, int channels, float fullScale,
                                   float gain, int wavFormat, const std::string& wavPath, std::string& error,
                                   int outputChannels) {
    error.clear();
    if (!d->lib || d->worker.joinable() || ((d->version == 6 && !d->hostAudio6) || (d->version == 7 && !d->hostAudio7))) {
        error = "Csound host audio is unavailable."; return false;
    }
    if (channels < 1 || channels > 64 || !std::isfinite(fullScale) || fullScale <= 0 || wavFormat < 0 || wavFormat > 2) {
        error = "Unsupported offline audio format."; return false;
    }
    CawAudioSettings audio;
    audio.sampleRate = sampleRate; audio.channels = channels; audio.fullScale = fullScale;
    if (outputChannels == 0) outputChannels = channels;
    if (outputChannels < 1 || outputChannels > 64) { error = "Unsupported export channel count."; return false; }
    try {
        d->vstSampleRate = sampleRate;
        d->capture.reset();
        d->outputCapture.reset();
        d->outputMonitorChannels = 0;
        d->masterGain = std::max(0.f, std::min(2.f, gain));
        d->stopRequested = false; d->code = 0; d->done = false; d->live = true;
        d->worker = std::thread(&Impl::run, d.get(), path, 0, false, false, audio, true, wavPath, wavFormat, outputChannels);
    } catch (...) {
        d->live = false; d->done = true; error = "Could not start the offline renderer."; return false;
    }
    return true;
}
void CsoundApiRunner::setMasterGain(float linear) {
    if (std::isfinite(linear)) d->masterGain.store(std::max(0.f, std::min(2.f, linear)), std::memory_order_relaxed);
}
void CsoundApiRunner::setVstJobs(const std::vector<VstJob>& jobs) { d->vstJobs = jobs; }
void CsoundApiRunner::setPdSource(const std::string& library, const std::vector<pd_timeline::Event>& plan) {
    if (d->worker.joinable()) return;
    d->pdLibrary=library;d->pdPlan=plan;
    d->pdPrepared.store(false,std::memory_order_release);
    d->pd=library.empty()?nullptr:std::make_unique<PdHostMixer>();
}
void CsoundApiRunner::setHub(CawHub* hub) { d->hub = hub; }
void CsoundApiRunner::setHubNames(const std::vector<std::string>& names) {
    std::lock_guard<std::mutex> lock(d->hubMutex);
    d->hubNames.clear();
    d->hubNames.insert(names.begin(), names.end());
}
void CsoundApiRunner::pdTransport(bool playing,double time,double loopStart,double loopEnd,bool loop,int rate,int commandSerial) {
    if(d->pd)d->pd->transport(playing,time,loopStart,loopEnd,loop,rate,commandSerial);
}
bool CsoundApiRunner::pdReady() const {return d->pdPrepared.load(std::memory_order_acquire);}
std::int64_t CsoundApiRunner::pdPosition() const {return d->pd?d->pd->position():-1;}
bool CsoundApiRunner::auditionPd(const pd_timeline::Event& event,bool playing) {
    if(!pdReady()||!d->pd)return false;
    d->pd->audition(event,playing);return true;
}
bool CsoundApiRunner::auditioningPd(int clipId) const {
    return pdReady()&&d->pd&&d->pd->auditionRequested(clipId);
}
bool CsoundApiRunner::replanPd(const std::vector<pd_timeline::Event>& plan) {
    if(!pdReady()||!d->pd)return false;
    d->pd->replan(plan);return true;
}
std::string CsoundApiRunner::takePdPlanError() {
    return d->pd?d->pd->takePlanError():std::string();
}
std::vector<std::string> CsoundApiRunner::takePdPlanNotes() {
    return d->pd?d->pd->takePlanNotes():std::vector<std::string>{};
}
std::vector<PdControlOutput> CsoundApiRunner::takePdControlOutputs() {
    return d->pd?d->pd->takeControlOutputs():std::vector<PdControlOutput>{};
}
unsigned CsoundApiRunner::takePdDroppedControls() {return d->pd?d->pd->takeDroppedControls():0;}
std::vector<CsoundApiRunner::PdTrackPeak> CsoundApiRunner::takePdPeaks() {
    std::vector<PdTrackPeak> result;
    if(!pdReady()||!d->pd)return result;
    for(const auto& peak:d->pd->takePeaks())result.push_back({peak.trackId,peak.value});
    return result;
}
void CsoundApiRunner::setCommandSerial(int serial) {
    d->commandSerial.store(std::max(0, serial), std::memory_order_release);
}
void CsoundApiRunner::stop() {
    d->stopRequested = true;
    if (d->worker.joinable()) d->worker.join();
    d->live = false; d->done = true;
    d->capture.reset();
    d->outputCapture.reset();
    d->outputMonitorChannels = 0;
    d->workerMonitorSequence = 0;
}
bool CsoundApiRunner::active() const { return d->worker.joinable(); }
bool CsoundApiRunner::running() const { return d->live.load(); }
bool CsoundApiRunner::finished() const { return active() && d->done.load(); }
int CsoundApiRunner::exitCode() const { return d->code.load(); }
std::vector<std::string> CsoundApiRunner::takeOutput() { return d->logs.take(); }
bool CsoundApiRunner::takeSnapshot(MonitorCapture::Snapshot& out, uint64_t lastSequence) const {
    if (d->outputCapture) {
        if (!d->outputCapture->take(out, lastSequence)) return false;
        if (d->capture) {
            MonitorCapture::Snapshot worker;
            if (d->capture->take(worker, d->workerMonitorSequence)) {
                d->workerMonitorSequence = worker.sequence;
                const size_t firstTrackMeter = static_cast<size_t>(std::max(0, d->outputMonitorChannels));
                for (size_t i = firstTrackMeter; i < out.meters.size() && i < worker.meters.size(); ++i)
                    out.meters[i] = worker.meters[i];
            }
        }
        return true;
    }
    return d->capture && d->capture->take(out, lastSequence);
}
