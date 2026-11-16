#include "csound_api_runner.h"
#include "vst_rack.h"
#include "wav_writer.h"
#include "audio_channels.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
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
    LogQueue logs;
    int (*getTable)(void*, void**, int) = nullptr;
    std::vector<VstJob> vstJobs;
    int vstSampleRate = 48000;

    template<class T> bool get(T& fn, const char* name) {
        fn = reinterpret_cast<T>(symbol(lib, name));
        return fn != nullptr;
    }
    void drain(void* cs) {
        for (int n = 0; n < 1024 && messageCount(cs) > 0; ++n) {
            logs.push(firstMessage(cs));
            popMessage(cs);
        }
    }
    void run(std::string path, int meterCount, bool captureEnabled, bool hostOutput, CawAudioSettings audioSettings,
             bool offline, std::string offlinePath, int wavFormat, int outputChannels) {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
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
        std::vector<void*> meterPtrs((size_t)meterCount, nullptr);
        void* serialPtr = nullptr;
        unsigned nchan = 0, blockSize = 0;
        if (status == 0 && !stopRequested.load()) {
            nchan = version == 6 ? channels6(cs) : channels7(cs, 0);
            blockSize = ksmps(cs);
            // Only the engine thread touches Csound's channel memory.
            if (!offline && (channelPtr(cs, &serialPtr, "__caw_cmd_serial", 1 | 16) != 0 || !serialPtr)) {
                logs.push("[caw] Could not open the Csound command channel.\n");
                status = -1;
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
    const char* libnames[] = {"csound64.dll", "csound7.dll", "csound64-7.0.dll"};
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
    for (const char* name : libnames) names.emplace_back(name);
    for (const auto& name : names) if ((d->lib = openLibrary(name))) break;
    if (!d->lib) { error = "Csound API library not found beside the Csound program or in the library path."; return false; }
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
        d->stopRequested = false; d->code = 0; d->done = false; d->live = true;
        d->worker = std::thread(&Impl::run, d.get(), path, meterCount, capture, audio != nullptr,
                                audio ? *audio : CawAudioSettings{}, false, std::string(), 2, 0);
    } catch (...) {
        d->live = false; d->done = true; d->capture.reset(); error = "Could not start the Csound engine thread."; return false;
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
void CsoundApiRunner::setCommandSerial(int serial) {
    d->commandSerial.store(std::max(0, serial), std::memory_order_release);
}
void CsoundApiRunner::stop() {
    d->stopRequested = true;
    if (d->worker.joinable()) d->worker.join();
    d->live = false; d->done = true;
    d->capture.reset();
}
bool CsoundApiRunner::active() const { return d->worker.joinable(); }
bool CsoundApiRunner::running() const { return d->live.load(); }
bool CsoundApiRunner::finished() const { return active() && d->done.load(); }
int CsoundApiRunner::exitCode() const { return d->code.load(); }
std::vector<std::string> CsoundApiRunner::takeOutput() { return d->logs.take(); }
bool CsoundApiRunner::takeSnapshot(MonitorCapture::Snapshot& out, uint64_t lastSequence) const {
    return d->capture && d->capture->take(out, lastSequence);
}
