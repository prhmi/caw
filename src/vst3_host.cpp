// VST3 hosting without any editor GUI. Needs the Steinberg VST3 SDK (public.sdk hosting + base + pluginterfaces).
#include "plugin_host.h"
#include "vst_audio_buffers.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <functional>
#include <filesystem>
#include <cstdlib>
#include <cctype>
#include <set>
#include <cstdint>
#include <deque>
#include <chrono>
#include <cmath>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "crash_dump.h"   // cawcrash::sehSwallowRecord: a faulting plugin is recorded, not fatal
#if !defined(_WIN32)
#include "vst_editor_native.h"   // the plugin editor window on macOS (Cocoa) / Linux (X11)
#endif
using namespace Steinberg; using namespace Steinberg::Vst;

// ---- SEH trampolines for the plugin-facing calls -------------------------------------------
// A third-party plugin must never kill CAW: an access violation inside its parameter / state
// entry points (seen with Arturia ARP 2600 V3) used to take the whole app down, so saving,
// loading and exporting all died with it. These small helpers contain ONLY the raw COM call
// (no C++ objects, so __try is legal); on a fault they record caw_crash.txt and report failure.
#ifdef _WIN32
#define CAW_SEH_GETINFO GetExceptionInformation()
#else
// macOS / Linux: no SEH. The guards become plain calls (a faulting plugin crashes CAW there).
#include <functional>
#include <string>
#define __try if (true)
#define __except(x) else if (false)
#define CAW_SEH_GETINFO 0
namespace cawcrash { inline int sehSwallowRecord(int) { return 0; } inline std::function<void(const std::string&)> sehConsole; }
#endif
static bool seh_getParam(IEditController* ctrl, Vst::ParamID id, double* out) {
    __try { *out = ctrl->getParamNormalized(id); return true; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_setParam(IEditController* ctrl, Vst::ParamID id, double v) {
    __try { ctrl->setParamNormalized(id, v); return true; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_getState(IComponent* comp, IBStream* stream) {
    __try { return comp->getState(stream) == kResultTrue; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_setState(IComponent* comp, IBStream* stream) {
    __try { return comp->setState(stream) == kResultTrue; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_setComponentState(IEditController* ctrl, IBStream* stream) {
    __try { ctrl->setComponentState(stream); return true; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
// The render call: the most frequently executed plugin entry point - an access violation here
// (repeated LKD / export runs with a fragile plugin) killed CAW outright. On a fault the block is
// silenced instead.
static bool seh_process(IAudioProcessor* proc, ProcessData* d) {
    __try { return proc->process(*d) == kResultTrue; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
// Lifecycle calls (prepare / activate / processing on/off): a fault during a repeated instance
// build killed CAW too. The caller treats false as "the plugin did not start".
static bool seh_setupProcessing(IAudioProcessor* proc, ProcessSetup* s) {
    __try { return proc->setupProcessing(*s) == kResultOk; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_setActive(IComponent* comp, bool on) {
    __try { return comp->setActive(on) == kResultOk; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_setProcessing(IAudioProcessor* proc, bool on) {
    __try { return proc->setProcessing(on) == kResultOk; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static int32 seh_getLatency(IAudioProcessor* proc) {
    __try { return proc->getLatencySamples(); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return 0; }
}
// Load-path calls: every fresh plugin instance runs these (every LKD / export pass builds one).
// The ARP 2600 still escaped through comp->getState on a repeated lock - all of these are now guarded.
static bool seh_providerInitialize(Steinberg::Vst::PlugProvider* p) {
    __try { return p->initialize(); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static int32 seh_getBusCount(IComponent* comp, MediaType mt, BusDirection dir) {
    __try { return comp->getBusCount(mt, dir); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return -1; }
}
static bool seh_activateBus(IComponent* comp, MediaType mt, BusDirection dir, int32 idx, bool on) {
    __try { return comp->activateBus(mt, dir, idx, on) == kResultOk; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_getBusInfo(IComponent* comp, MediaType mt, BusDirection dir, int32 idx, BusInfo* out) {
    __try { return comp->getBusInfo(mt, dir, idx, *out) == kResultOk; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static bool seh_setBusArrangements(IAudioProcessor* proc, SpeakerArrangement* in, int32 nIn, SpeakerArrangement* out, int32 nOut) {
    __try { return proc->setBusArrangements(in, nIn, out, nOut) == kResultTrue; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
// A speaker arrangement with exactly n channels (the standard ones for 1/2/4/6/8, else n generic bits).
static SpeakerArrangement cawArrangement(int n) {
    using namespace Steinberg::Vst::SpeakerArr;
    switch (n) {
        case 1: return kMono;
        case 2: return kStereo;
        case 4: return k40Music;   // L R Ls Rs (quad)
        case 6: return k51;
        case 8: return k71Music;
        default: { SpeakerArrangement a = 0; for (int i = 0; i < n && i < 32; ++i) a |= (SpeakerArrangement)1 << i; return a; }
    }
}
static int32 seh_getParameterCount(IEditController* ctrl) {
    __try { return ctrl->getParameterCount(); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return 0; }
}
static bool seh_getParameterInfo(IEditController* ctrl, int32 idx, ParameterInfo* out) {
    __try { return ctrl->getParameterInfo(idx, *out) == kResultOk; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static float seh_getParamNormalized(IEditController* ctrl, ParamID id) {
    __try { return ctrl->getParamNormalized(id); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return 0.f; }
}
static bool seh_createView(IEditController* ctrl, FIDString name, Steinberg::IPlugView** out) {
    __try { *out = ctrl->createView(name); return true; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static void seh_setComponentHandler(IEditController* ctrl, IComponentHandler* h) {
    __try { ctrl->setComponentHandler(h); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) {}
}
static bool seh_getMidiControllerAssignment(IMidiMapping* mm, int16 chn, CtrlNumber cn, ParamID* pid) {
    __try { return mm->getMidiControllerAssignment(0, chn, cn, *pid) == kResultTrue; }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) { return false; }
}
static void seh_terminate(Steinberg::IPluginBase* u) {
    __try { u->terminate(); }
    __except (cawcrash::sehSwallowRecord(CAW_SEH_GETINFO)) {}
}

namespace fs = std::filesystem;

namespace caw {
// A VST3 class is identified by its 16-byte CID, not by the bundle file: Arturia / Kilohearts and
// other bundles put several classes in one .vst3, and "load the first class" made every entry of
// the same bundle load the same plugin. The canonical text form is 32 uppercase hex digits.
static std::string cidText(const TUID tuid) {
    static const char* hex = "0123456789ABCDEF";
    std::string s; s.reserve(32);
    for (int i = 0; i < 16; ++i) { const unsigned char b = (unsigned char)tuid[i]; s += hex[b >> 4]; s += hex[b & 15]; }
    return s;
}
static std::string normCid(const std::string& text) {   // accepts {braces}, dashes and lower case
    std::string s;
    for (char c : text) if (std::isxdigit((unsigned char)c)) s += (char)std::toupper((unsigned char)c);
    return s.size() == 32 ? s : std::string();
}
// Minimal base64 (RFC 4648) for the plugin state blob: the full component state is binary, and it
// travels through the app as std::string (VstJob::stateBlob / the editor's pushedState) - base64
// keeps it text-safe everywhere, including a future project-file field.
static const char* const kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64Encode(const std::string& raw) {
    std::string out; out.reserve((raw.size() + 2) / 3 * 4);
    for (size_t i = 0; i < raw.size(); i += 3) {
        const unsigned v = ((unsigned char)raw[i] << 16)
                         | (i + 1 < raw.size() ? (unsigned char)raw[i + 1] << 8 : 0u)
                         | (i + 2 < raw.size() ? (unsigned char)raw[i + 2] : 0u);
        out += kBase64[(v >> 18) & 63]; out += kBase64[(v >> 12) & 63];
        out += i + 1 < raw.size() ? kBase64[(v >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? kBase64[v & 63] : '=';
    }
    return out;
}
static bool base64Decode(const std::string& in, std::string& out) {
    static int rev[256]; static bool ready = false;
    if (!ready) { ready = true; std::fill_n(rev, 256, -1); for (int i = 0; i < 64; ++i) rev[(unsigned char)kBase64[i]] = i; }
    out.clear();
    unsigned buf = 0; int bits = 0;
    for (char c : in) {
        if (c == '=') break;                     // padding: the rest is empty by construction
        const int r = rev[(unsigned char)c];
        if (r < 0) return false;                 // not a base64 character
        buf = buf << 6 | (unsigned)r; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
    }
    return true;
}
// PlugProvider uses this shared context for component/controller initialization.
// Keep it alive for the entire process, including detached scans and plugin teardown.
static Steinberg::Vst::HostApplication& hostApp() { static auto* h = new Steinberg::Vst::HostApplication; return *h; }
static void ensureHostContext() {
    static std::once_flag once;
    std::call_once(once, [] { PluginContextFactory::instance().setPluginContext(&hostApp()); });
}

#ifdef _WIN32
// Use the same awareness context for plugin loading, editor calls and the editor HWND.
class EditorDpiContext {
    using SetContext = void* (WINAPI*)(void*);
    SetContext set_ = nullptr;
    void* previous_ = nullptr;
public:
    explicit EditorDpiContext(bool systemScaling) {
        if (HMODULE u = GetModuleHandleA("user32.dll"))
            set_ = reinterpret_cast<SetContext>(GetProcAddress(u, "SetThreadDpiAwarenessContext"));
        // -1: DPI unaware (Windows bitmap scaling); -4: per-monitor-aware v2.
        if (set_) previous_ = set_(reinterpret_cast<void*>(static_cast<std::intptr_t>(systemScaling ? -1 : -4)));
    }
    ~EditorDpiContext() { if (set_ && previous_) set_(previous_); }
};
#endif

#if defined(__linux__)
// Linux VST3: the host must offer Steinberg::Linux::IRunLoop through the IPlugFrame. Plugin editors
// (VSTGUI, JUCE...) register their X11 connection's file descriptor and their redraw timers here;
// without it they open blank or never animate. CAW runs it from the UI thread every frame.
#include <sys/select.h>
#include <chrono>
class CawRunLoop : public Steinberg::Linux::IRunLoop {
public:
    tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* h, Steinberg::Linux::FileDescriptor fd) override {
        if (!h) return kInvalidArgument;
        h->addRef(); fds_.push_back({h, fd}); return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler* h) override {
        bool any = false;
        for (auto it = fds_.begin(); it != fds_.end();) { if (it->h == h) { it->h->release(); it = fds_.erase(it); any = true; } else ++it; }
        return any ? kResultTrue : kInvalidArgument;
    }
    tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler* h, Steinberg::Linux::TimerInterval ms) override {
        if (!h) return kInvalidArgument;
        h->addRef(); timers_.push_back({h, std::max<Steinberg::uint64>(1, ms), now() + ms}); return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* h) override {
        bool any = false;
        for (auto it = timers_.begin(); it != timers_.end();) { if (it->h == h) { it->h->release(); it = timers_.erase(it); any = true; } else ++it; }
        return any ? kResultTrue : kInvalidArgument;
    }
    // One pass: every ready file descriptor, then every due timer. Never blocks.
    void run() {
        if (!fds_.empty()) {
            fd_set set; FD_ZERO(&set); int maxFd = -1;
            for (const auto& f : fds_) if (f.fd >= 0 && f.fd < FD_SETSIZE) { FD_SET(f.fd, &set); maxFd = std::max(maxFd, f.fd); }
            if (maxFd >= 0) {
                timeval tv{0, 0};
                if (select(maxFd + 1, &set, nullptr, nullptr, &tv) > 0) {
                    const auto snapshot = fds_;   // a handler may unregister itself
                    for (const auto& f : snapshot) if (f.fd >= 0 && f.fd < FD_SETSIZE && FD_ISSET(f.fd, &set)) f.h->onFDIsSet(f.fd);
                }
            }
        }
        const Steinberg::uint64 t = now();
        const auto snapshot = timers_;
        for (const auto& tm : snapshot) {
            if (t < tm.next) continue;
            for (auto& live : timers_) if (live.h == tm.h) live.next = t + live.ms;
            tm.h->onTimer();
        }
    }
    bool empty() const { return fds_.empty() && timers_.empty(); }
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, Steinberg::Linux::IRunLoop)
        QUERY_INTERFACE(_iid, obj, Steinberg::Linux::IRunLoop::iid, Steinberg::Linux::IRunLoop)
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }      // owned by the EditorHandler
    uint32 PLUGIN_API release() override { return 1000; }
private:
    struct Fd { Steinberg::Linux::IEventHandler* h; Steinberg::Linux::FileDescriptor fd; };
    struct Tm { Steinberg::Linux::ITimerHandler* h; Steinberg::uint64 ms; Steinberg::uint64 next; };
    static Steinberg::uint64 now() {
        return (Steinberg::uint64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::vector<Fd> fds_;
    std::vector<Tm> timers_;
};
#endif

// What the plugin calls back while its window is open: which parameter the user touches, and "please resize my window".
class EditorHandler : public IComponentHandler, public IPlugFrame {
public:
    std::atomic<bool> learning{false};
    std::mutex touchMutex;
    std::deque<int> touches;
    std::function<int(ParamID)> indexOf;
    std::function<void(int, double)> onLiveEdit;   // every editor knob turn with its normalized value
    std::function<tresult(IPlugView*, ViewRect*)> resize;
    std::function<void()> onInteract;   // the user changed something in the editor: the full state is due (no timer polls it any more)
    void clearTouches() { std::lock_guard<std::mutex> lock(touchMutex); touches.clear(); }
    int popTouch() { std::lock_guard<std::mutex> lock(touchMutex); if (touches.empty()) return -1; int i = touches.front(); touches.pop_front(); return i; }
    std::atomic<int> gestureDepth{0};   // inside beginEdit / endEdit = the USER is editing
    tresult PLUGIN_API beginEdit(ParamID) override { gestureDepth.fetch_add(1, std::memory_order_relaxed); return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
        const int i = indexOf ? indexOf(id) : -1;
        // A value the plugin moves itself (a Cabbage meter, 25x a second) is not a user edit: it
        // must not mark the state dirty - that started a full getState capture every time (meter CPU).
        if (onInteract && gestureDepth.load(std::memory_order_relaxed) > 0) onInteract();
        if (learning && i >= 0) { std::lock_guard<std::mutex> lock(touchMutex); if (touches.size() < 256) touches.push_back(i); }
        if (i >= 0 && onLiveEdit) onLiveEdit(i, (double)v);   // the editor's value used to be discarded: live sync could never see a knob turn
        return kResultOk;
    }
    // Editor knob turns must also reach the PROCESSOR: the host only passes inputParameterChanges
    // from the code rings, so a plugin's own editor (master FX single instance) never heard its GUI.
    // SPSC ring written here (UI thread), drained in process() (audio thread).
    void pushPendingEdit(int i, double v) {
        const unsigned w = pendW_.load(std::memory_order_relaxed);
        const unsigned n = (w + 1u) % kPendEditN;
        if (n == pendR_.load(std::memory_order_acquire)) return;   // full: drop the oldest-free edit
        pendEdit_[(size_t)w].idx.store(i, std::memory_order_relaxed);
        pendEdit_[(size_t)w].val.store((float)v, std::memory_order_relaxed);
        pendW_.store(n, std::memory_order_release);
    }
    static constexpr unsigned kPendEditN = 256;
    struct PendEdit { std::atomic<int> idx{-1}; std::atomic<float> val{0.f}; };
    PendEdit pendEdit_[kPendEditN];
    std::atomic<unsigned> pendW_{0}, pendR_{0};
    tresult PLUGIN_API endEdit(ParamID) override { int d = gestureDepth.load(std::memory_order_relaxed); while (d > 0 && !gestureDepth.compare_exchange_weak(d, d - 1, std::memory_order_relaxed)) {} return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { if (onInteract) onInteract(); return kResultOk; }    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override { return resize && r ? resize(view, r) : kResultFalse; }
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler)
        QUERY_INTERFACE(_iid, obj, IPlugFrame::iid, IPlugFrame)
#if defined(__linux__)
        if (FUnknownPrivate::iidEqual(_iid, Steinberg::Linux::IRunLoop::iid)) { *obj = static_cast<Steinberg::Linux::IRunLoop*>(&runLoop); return kResultOk; }
#endif
        *obj = nullptr; return kNoInterface;
    }
#if defined(__linux__)
    CawRunLoop runLoop;   // offered to the plugin's editor (see CawRunLoop)
#endif
    uint32 PLUGIN_API addRef() override { return 1000; }      // lives as long as the plugin object: never deleted by the plugin
    uint32 PLUGIN_API release() override { return 1000; }
};

// ONE module (DLL) per .vst3 file for the whole process. Every instance of the same plugin shares it,
// and it is never unloaded while CAW runs: with many plugins loading / unloading on the loader, editor
// and UI threads, a per-instance Module::create / release unloaded a DLL while another instance of the
// same bundle still ran code in it (the crash with many VSTs): every Module object calls the bundle's
// InitDll / ExitDll, and an ExitDll tears the plugin's global state down under every live instance.
static std::mutex& moduleMutex() { static std::mutex m; return m; }
static std::map<std::string, std::shared_ptr<VST3::Hosting::Module>>& moduleCache() {
    static auto* cache = new std::map<std::string, std::shared_ptr<VST3::Hosting::Module>>();   // never destroyed: DLLs stay loaded until exit
    return *cache;
}
static std::shared_ptr<VST3::Hosting::Module> sharedModule(const std::string& path, std::string& err) {
    auto* cache = &moduleCache();
    std::lock_guard<std::mutex> lock(moduleMutex());
    auto it = cache->find(path);
    if (it != cache->end() && it->second) return it->second;
    auto mod = VST3::Hosting::Module::create(path, err);
    if (mod) (*cache)[path] = mod;
    return mod;
}
class Vst3Plugin : public IPlugin {
    std::atomic<int> auxBusCh_{0};                // channels of the plugin's sidechain (auxiliary) input bus, 0 = none
    const float* scIn_ = nullptr;                 // the rack's sidechain block for the next process() (audio thread only)
    std::vector<std::vector<float>> auxScratch_;
    static constexpr int kMaxProcessFrames = 8192;   // no block bigger than this ever reaches a plugin (scratch stays bounded)
    std::atomic<int> inBusCh_{2}, outBusCh_{2};
    int wantChannels_ = 0;
    bool enginePrepared_ = false; double preparedSr_ = 0.0; int preparedBlock_ = 0, preparedWant_ = -1;   // the engine setup in force   // the track's channel count, requested from the plugin at prepare (0 = leave its layout)   // channel count of the plugin's first audio input / output bus (queried at prepare)
    bool editorComponentActive_ = false;
    // The EDITOR instance is touched by two threads: the housekeeping worker (processIdle, state
    // capture) and the UI thread (open / close / re-attach / state restore). kHs-class plugins are
    // not safe against that: one call at a time, and the window never tears down mid-process.
    mutable std::mutex editorApiMutex_;
    bool editorProcessing_ = false;   // SEPARATED ENGINES: the editor instance runs processIdle() ticks
    bool editorLogged_ = false;       // one-shot log: confirms the editor instance began processing (and setProcessing's return)
    double editorSetupSr_ = 0.0;
    int editorSetupBlock_ = 0;
    std::vector<std::vector<float>> inScratch_, outScratch_;   // de-interleaved process scratch for any channel count
    // SEPARATED ENGINES: what this instance heard and dispatched, captured for the EDITOR
    // instance's feed (EQ analyzers, synth meters). A REAL DAW runs one instance and its GUI is
    // fed as the audio runs. CAW runs two, so the engine instance streams its input into a
    // lock-free SPSC ring (tapRing_) and the editor's realtime tick drains it: a 60 Hz tick
    // replays exactly the frames the engine produced, i.e. realtime-speed meters / analyzers.
    // The ring is preallocated and the producer never waits on the consumer. tapEvents_ is
    // drained under tapMutex_ (the producer only try-locks it: it must never block the audio).
    static constexpr size_t kTapCap = 8192;   // frames (interleaved stereo): ~170 ms at 48 kHz
    std::mutex tapMutex_;
    std::vector<float> tapRing_ = std::vector<float>(kTapCap * 2, 0.f);
    std::atomic<size_t> tapWrite_{0};        // total frames ever written (monotonic)
    std::atomic<size_t> tapRead_{0};         // total frames drained (monotonic)
    std::deque<MidiMsg> tapEvents_;          // dispatched MIDI since the last drain (capped)
    // Editor interaction: see IPlugin::stateDirty / windowBusy.
    std::atomic<bool> stateDirty_{false};
    std::atomic<bool> windowBusy_{false};
    std::atomic<bool> closeRequested_{false};   // the user closed the window: destroy it on the next message turn
    std::atomic<bool> hostParam_{false};     // the HOST is setting a parameter (slider / automation): not a user edit
    std::atomic<double> lastTouchMs_{0.0};   // steady_clock ms
    struct HostParamGuard {
        std::atomic<bool>& f;
        explicit HostParamGuard(std::atomic<bool>& a) : f(a) { f.store(true, std::memory_order_relaxed); }
        ~HostParamGuard() { f.store(false, std::memory_order_relaxed); }
    };
    static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void touchEditor() { lastTouchMs_.store(nowMs(), std::memory_order_relaxed); stateDirty_.store(true, std::memory_order_relaxed); }
    std::set<int> heldNotes_;                // channel * 128 + pitch of note-ons with no note-off yet
    // The controller's MIDI CC / bend / aftertouch -> parameter assignment, cached ONCE at load on
    // the UI thread (IMidiMapping is documented "[UI-thread & Connected]" - it must never be
    // queried inside process()). Index: channel * 141 + ctrlNumber; (ParamID)-1 = unmapped.
    std::vector<ParamID> ccMap_;
public:
    void setEditorSystemScaling(bool enabled) override { systemScaling_ = enabled; }
    void closeEditor() override {
        std::lock_guard<std::mutex> api(editorApiMutex_);   // never tear the window down while the worker is inside the plugin
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
        if (view_) { view_->setFrame(nullptr); if (viewAttached_) view_->removed(); viewAttached_ = false; view_ = nullptr; }
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        lastScale_ = 0.f; lastWidth_ = lastHeight_ = -1;
        vstLog("editor: closed (reattach)");
#else
        if (view_) { view_->setFrame(nullptr); if (viewAttached_) view_->removed(); viewAttached_ = false; view_ = nullptr; }
        if (edWin_) { cawed::destroy(edWin_); edWin_ = nullptr; }
        vstLog("editor: closed");
#endif
    }
    ~Vst3Plugin() override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        closeEditor();
        // VST3 split component/controller pairs communicate through IConnectionPoint.
        // Several Kilohearts editors rely on this channel for custom UI state and updates.
        if (comp_ && ctrl_) {
            FUnknownPtr<IConnectionPoint> componentCP(comp_);
            FUnknownPtr<IConnectionPoint> controllerCP(ctrl_);
            if (componentCP && controllerCP) {
                controllerCP->disconnect(componentCP);
                componentCP->disconnect(controllerCP);
                vstLog("controller: connection points disconnected");
            }
        }
        if (ctrl_) seh_setComponentHandler(ctrl_.get(), nullptr);
        // The plugin may fault even on its way out (a repeated lock/export with a fragile plugin
        // killed CAW during teardown too): shut it down through the same guarded trampolines.
        if (proc_) seh_setProcessing(proc_.get(), false);
        if (comp_) seh_setActive(comp_.get(), false);
        if (comp_) seh_terminate(comp_.get());
        if (ctrl_) seh_terminate(ctrl_.get());
    }
    bool load(const std::string& path, const std::string& classId, std::string& err) override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        ensureHostContext();
        module_ = sharedModule(path, err); if (!module_) { vstLog("load failed " + path + ": " + err); return false; }
        auto& fac = module_->getFactory();
        fac.setHostContext(&hostApp());
        vstLog("load " + path + (classId.empty() ? std::string() : (" class " + classId)));
        const std::string wanted = normCid(classId);
        if (!classId.empty() && wanted.empty()) { err = "the plugin class id is invalid"; return false; }
        // Select by CID when one is given: one bundle can hold many plugins, and the first class is
        // not necessarily the one the user picked. Empty = the first audio class (old projects).
        const VST3::Hosting::ClassInfo* chosen = nullptr;
        VST3::Hosting::ClassInfo chosenCopy;
        for (auto& ci : fac.classInfos()) {
            if (ci.category() != kVstAudioEffectClass) continue;
            if (!wanted.empty() && cidText(ci.ID().data()) != wanted) continue;
            chosenCopy = ci; chosen = &chosenCopy; break;
        }
        if (!chosen) { err = "this .vst3 does not contain the selected plugin class (was it updated or uninstalled?)"; vstLog("load: class not found: " + classId); return false; }
        classId_ = cidText(chosen->ID().data());
        instrument_ = chosen->subCategoriesString().find("Instrument") != std::string::npos;
        vstLog("create provider for " + chosen->name() + " [" + classId_ + "]");
        provider_ = Steinberg::owned(new PlugProvider(fac, chosenCopy, true));
        if (!provider_) { err = "no audio class"; return false; }
        vstLog("initialize");
        if (!seh_providerInitialize(provider_.get())) { err = "the plugin faulted while starting (CAW recovered)"; return false; }
        vstLog("initialized");
        // PlugProvider's accessors return already-retained references.
        comp_ = Steinberg::owned(provider_->getComponent());
        ctrl_ = Steinberg::owned(provider_->getController());
        if (!comp_) { err = "no component"; return false; }
        FUnknownPtr<IAudioProcessor> ap(comp_); proc_ = ap; if (!proc_) { err = "no processor"; return false; }
        if (ctrl_) {
            // A VST3 component and its edit controller are separate objects. The host must
            // connect their message channels in BOTH directions; setComponentState alone is
            // not a substitute. Kilohearts uses these messages for custom editor state and
            // interactive/animated UI updates.
            FUnknownPtr<IConnectionPoint> componentCP(comp_);
            FUnknownPtr<IConnectionPoint> controllerCP(ctrl_);
            if (componentCP && controllerCP) {
                const tresult c2e = componentCP->connect(controllerCP);
                const tresult e2c = controllerCP->connect(componentCP);
                vstLog("controller: connection points component->controller=" +
                    std::to_string((int)c2e) + " controller->component=" + std::to_string((int)e2c));
            } else {
                vstLog(std::string("controller: connection points unavailable component=") +
                    (componentCP ? "yes" : "no") + " controller=" + (controllerCP ? "yes" : "no"));
            }
            handler_.indexOf = [this](ParamID id) { const auto it = paramIndexById_.find(id); return it == paramIndexById_.end() ? -1 : it->second; };
            // A real DAW captures the full plugin state when the USER changed something (the
            // editor tick takes it once the mouse settles), never on a timer: a state poll stalls
            // kHs-class editors and re-applies a blob with volatile fields forever. Slice EQ-style
            // plugins echo host-driven setParamNormalized as performEdit too, so a host set is not
            // a user edit.
            handler_.onInteract = [this] { if (!hostParam_.load(std::memory_order_relaxed)) touchEditor(); };
            // The editor's knob turns arrive here as performEdit calls: keep the parameter cache
            // fresh so the UI live sync (syncLiveParameters) sees every editor edit next frame.
            handler_.onLiveEdit = [this](int i, double v) {
                if (i < 0 || i >= (int)params_.size() || params_[(size_t)i].readOnly) return;
                v = std::min(1.0, std::max(0.0, v));
                params_[(size_t)i].value = v;
                handler_.pushPendingEdit(i, v);   // the processor hears the editor's knob on the next block (master FX single instance)
                // Forward EVERY editor edit to the app: the running engine applies any parameter
                // on the same block (not just the 8 picked ones). Deduped per parameter.
                if (lastLive_.size() != params_.size()) lastLive_.assign(params_.size(), -2.0);
                if (liveHook_ && std::fabs(v - lastLive_[(size_t)i]) > 1e-6) {
                    lastLive_[(size_t)i] = v;
                    liveHook_(i, v);
                }
            };
            seh_setComponentHandler(ctrl_.get(), &handler_);
            // Newly initialized split controllers need the processor's initial state.
            Steinberg::MemoryStream state;
            const bool read = seh_getState(comp_.get(), &state);
            if (read) {
                state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                const bool sync = seh_setComponentState(ctrl_.get(), &state);
                vstLog("controller: component state sync result=" + std::to_string(sync ? 1 : 0));
            } else vstLog("controller: component state unavailable (fault or empty)");
            // The controller MAY map MIDI CCs / bend / aftertouch to parameters (IMidiMapping,
            // a UI-thread interface): cache the whole assignment table once here - process()
            // then only looks it up, never calls the interface.
            ccMap_.assign((size_t)16 * 141, (ParamID)-1);
            {
                FUnknownPtr<IMidiMapping> mm(ctrl_);
                if (mm) {
                    int mapped = 0;
                    for (int chn = 0; chn < 16; ++chn)
                        for (int cn = 0; cn <= 140; ++cn) {
                            ParamID pid = 0;
                            if (seh_getMidiControllerAssignment(mm, (int16)chn, (CtrlNumber)cn, &pid)) {
                                ccMap_[(size_t)chn * 141 + (size_t)cn] = pid;
                                ++mapped;
                            }
                        }
                    vstLog("controller: IMidiMapping " + std::to_string(mapped) + " assignment(s) cached");
                } else vstLog("controller: IMidiMapping absent - CC / bend do not drive parameters");
            }
        } else vstLog("controller: unavailable");
        for (int i = 0, n = seh_getBusCount(comp_.get(), kEvent, kInput); i < n; ++i) seh_activateBus(comp_.get(), kEvent, kInput, i, true);
        seh_activateBus(comp_.get(), kAudio, kOutput, 0, true);
        if (seh_getBusCount(comp_.get(), kAudio, kInput) > 0) seh_activateBus(comp_.get(), kAudio, kInput, 0, true);
        if (seh_getBusCount(comp_.get(), kAudio, kInput) > 1) {   // the sidechain: an auxiliary audio input bus
            BusInfo ai{};
            if (seh_getBusInfo(comp_.get(), kAudio, kInput, 1, &ai) && ai.busType == BusTypes::kAux && ai.channelCount > 0) seh_activateBus(comp_.get(), kAudio, kInput, 1, true);
        }
        applyWantedLayout();   // the layout the track wants, BEFORE the plugin's first activation
        {   // the real bus widths right at load: the EDITOR instance never runs prepare(), and its processIdle,
            // the meter layout and the chain fold all read these (a 4-out plugin used to report 2 here)
            BusInfo bi{};
            if (seh_getBusCount(comp_.get(), kAudio, kInput) > 0 && seh_getBusInfo(comp_.get(), kAudio, kInput, 0, &bi) && bi.channelCount > 0)
                inBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed);
            else inBusCh_.store(instrument_ ? 0 : 2, std::memory_order_relaxed);
            if (seh_getBusInfo(comp_.get(), kAudio, kOutput, 0, &bi) && bi.channelCount > 0)
                outBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed);
            if (seh_getBusCount(comp_.get(), kAudio, kInput) > 1 && seh_getBusInfo(comp_.get(), kAudio, kInput, 1, &bi) && bi.busType == BusTypes::kAux && bi.channelCount > 0)
                auxBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed);
        }
        for (int i = 0, n = ctrl_ ? seh_getParameterCount(ctrl_.get()) : 0; i < n; ++i) {
            ParameterInfo pi{}; if (!seh_getParameterInfo(ctrl_.get(), i, &pi)) continue;   // a faulting plugin loses that one parameter, not the load
            PluginParam p; p.index = i; p.id = pi.id; p.def = pi.defaultNormalizedValue; p.steps = pi.stepCount;
            p.readOnly = pi.flags & ParameterInfo::kIsReadOnly;
            p.title = u16to8(pi.title); p.units = u16to8(pi.units);
            double v = pi.defaultNormalizedValue;
            if (!seh_getParam(ctrl_.get(), pi.id, &v)) v = pi.defaultNormalizedValue;   // the load path must survive a faulting plugin
            p.value = v;
            paramIndexById_[p.id] = p.index;
            params_.push_back(p);
        }
        changes_.setMaxParameters((int32)params_.size() + 8);
        vstLog("loaded, " + std::to_string(params_.size()) + " parameters");
        return true;
    }
    bool isInstrument() const override { return instrument_; }
    std::string classId() const override { return classId_; }
    void prepareEditor(double sr, int maxBlock) override {
        if (!proc_ || !comp_ || sr <= 0.0 || maxBlock <= 0) return;
        if (engineOwned.load(std::memory_order_acquire)) return;   // the engine set this object up (prepare) and renders it
        std::lock_guard<std::mutex> api(editorApiMutex_);
        if (editorComponentActive_ && std::fabs(editorSetupSr_ - sr) < 0.01 && editorSetupBlock_ == maxBlock) return;
        // The UI instance is separate from the real-time audio instance. Configure and activate
        // its component only after the host engine has a real format; processIdle() (vstui::
        // queueEditorTasks, on the housekeeping worker) runs small silent blocks on it so sampler UIs stay alive, but it is
        // never connected to the audio callback.
        if (editorProcessing_) { if (!seh_setProcessing(proc_.get(), false)) vstLog("editor: setProcessing(false) faulted (recovered)"); editorProcessing_ = false; }
        editorLogged_ = false;   // a re-prepare (sample-rate / block change, reactivation) gets a fresh confirmation log
        if (editorComponentActive_) { if (!seh_setActive(comp_.get(), false)) vstLog("editor: setActive(false) faulted (recovered)"); }
        applyWantedLayout();
        ProcessSetup setup{kRealtime, kSample32, maxBlock, sr};
        const tresult setupResult = seh_setupProcessing(proc_.get(), &setup) ? kResultOk : kResultFalse;
        if (setupResult != kResultOk) {
            vstLog("editor: setupProcessing failed, result=" + std::to_string((int)setupResult));
            editorComponentActive_ = false;
            return;
        }
        const tresult activeResult = seh_setActive(comp_.get(), true) ? kResultOk : kResultFalse;
        if (activeResult != kResultOk) {
            vstLog("editor: component activation failed, result=" + std::to_string((int)activeResult));
            editorComponentActive_ = false;
            return;
        }
        editorComponentActive_ = true;
        editorSetupSr_ = sr;
        editorSetupBlock_ = maxBlock;
        vstLog("editor: component prepared for engine " + std::to_string((int)sr) + " Hz / " + std::to_string(maxBlock) + " frames");
    }
    // SEPARATED ENGINES: idle tick for the EDITOR instance. Kontakt (and other sampler-style
    // plugins) only finish their loading screen and move their meters when their component is
    // processed. The editor tick (vstui::startEditorWorker) runs a block every ~16 ms; no engine
    // connection: the engine instance on the audio path is a different object and is unaffected.
    // With a feed, the block carries the ENGINE instance's real input audio and dispatched MIDI,
    // drained from the engine's ring at the rate the engine produced it, so EQ analyzers and
    // meters run at realtime speed.
    void processIdle(int frames, const EditorFeed* feed) override {
        if (!proc_ || !comp_ || !editorComponentActive_ || frames <= 0) return;
        if (engineOwned.load(std::memory_order_acquire)) return;   // one instance: the engine processes it, the window just shows it
        std::lock_guard<std::mutex> api(editorApiMutex_);
        // VST3: IAudioProcessor::setProcessing is OPTIONAL. Many plugins (and VST2 adapters)
        // return kResultFalse from setProcessing(true) - the spec says the host must then just
        // call process() anyway. The engine path already does this; the editor instance must
        // too, or every EQ analyzer / scope / meter in a plugin window stays dead while sound
        // plays fine. Call setProcessing once (polite), then ALWAYS process.
        if (!editorProcessing_) {
            editorProcessing_ = true;
            const tresult sp = seh_setProcessing(proc_.get(), true) ? kResultOk : kResultFalse;
            if (!editorLogged_) {
                editorLogged_ = true;
                vstLog("editor instance: processing started (setProcessing returned " + std::to_string((int)sp) + "; " + std::to_string(inBusCh_.load(std::memory_order_relaxed)) + " input channel(s))");
            }
        }
        if (editorSetupBlock_ > 0 && frames > editorSetupBlock_) frames = editorSetupBlock_;   // stay within the prepared block
        // The feed's engine audio replaces the silence. An instrument has no input bus: its GUI
        // lives on the dispatched notes (below).
        // The plugin's own bus widths (see process): a 4-out instrument gets 4 output buffers here too.
        int nIn = std::max(0, std::min(8, inBusCh_.load(std::memory_order_relaxed)));   // every active bus gets buffers (an instrument's are silent)
        const bool haveAudio = feed && feed->frames > 0 && (int)feed->audio.size() >= feed->frames * 2 && !instrument_;
        if (haveAudio && feed->frames < frames) frames = feed->frames;
        const int nOut = std::max(1, std::min(8, outBusCh_.load(std::memory_order_relaxed)));
        if (frames > kMaxProcessFrames) frames = kMaxProcessFrames;
        if ((int)inScratch_.size() < nIn) inScratch_.resize((size_t)nIn);
        if ((int)outScratch_.size() < nOut) outScratch_.resize((size_t)nOut);
        for (int c = 0; c < nIn; ++c) {
            if ((int)inScratch_[(size_t)c].size() < frames) inScratch_[(size_t)c].resize((size_t)frames);
            for (int i = 0; i < frames; ++i)
                inScratch_[(size_t)c][(size_t)i] = haveAudio && c < 2 ? feed->audio[(size_t)i * 2 + (size_t)c] : 0.f;
        }
        for (int c = 0; c < nOut; ++c) {
            if ((int)outScratch_[(size_t)c].size() < frames) outScratch_[(size_t)c].resize((size_t)frames);
            std::fill_n(outScratch_[(size_t)c].begin(), (size_t)frames, 0.f);
        }
        float* ins[8] = {}; float* outs[8] = {};
        for (int c = 0; c < nIn; ++c) ins[c] = inScratch_[(size_t)c].data();
        for (int c = 0; c < nOut; ++c) outs[c] = outScratch_[(size_t)c].data();
        AudioBusBuffers ib{}, ob{}; ib.numChannels = nIn; ib.channelBuffers32 = ins; ob.numChannels = nOut; ob.channelBuffers32 = outs;
        events_.clear(); changes_.clearQueue();
        if (feed) for (const MidiMsg& m : feed->events) mapMidi(m, events_, changes_, 0);
        // The window's own knob turns reach the processor through this instance's performEdit ring
        // exactly like master FX's single instance (Vst3Plugin::process drains it). The editor
        // instance IS the window's DSP: skip this and its processed-audio display (Slice EQ's
        // spectrum analyzer, meters) keeps drawing the patch from load time while the engine -
        // fed through the value sweep - plays the edited one.
        {
            unsigned r = handler_.pendR_.load(std::memory_order_relaxed);
            int drained = 0; int32 qi = 0;
            while (r != handler_.pendW_.load(std::memory_order_acquire) && drained < 128) {
                const int pi = handler_.pendEdit_[r].idx.load(std::memory_order_relaxed);
                const float pv = handler_.pendEdit_[r].val.load(std::memory_order_relaxed);
                if (pi >= 0 && pi < (int)params_.size()) {
                    auto* q = changes_.addParameterData(params_[(size_t)pi].id, qi);
                    int32 point;
                    if (q) q->addPoint(0, (double)pv, point);
                }
                r = (r + 1u) % handler_.kPendEditN; ++drained;
            }
            handler_.pendR_.store(r, std::memory_order_release);
        }
        // Host-set values queued by setParam (the 8 picked slots mirrored from the engine).
        {
            std::lock_guard<std::mutex> lock(initParamMutex_);
            int sent = 0; int32 qi = 0;
            for (auto it = initialParameters_.begin(); it != initialParameters_.end(); ) {
                if (sent >= 128) break;
                if (it->first < 0 || it->first >= (int)params_.size()) { it = initialParameters_.erase(it); continue; }
                auto* q = changes_.addParameterData(params_[it->first].id, qi);
                int32 point;
                if (q) q->addPoint(0, it->second, point);
                it = initialParameters_.erase(it);
                ++sent;
            }
        }
        float* auxp[8] = {};
        const int nAux = nIn > 0 ? fillAux(auxp, frames, false) : 0;   // the editor instance hears no sidechain: silence
        AudioBusBuffers ab{}; ab.numChannels = nAux; ab.channelBuffers32 = auxp;
        AudioBusBuffers inBuses[2] = {ib, ab};
        ProcessData d{}; d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = frames;
        d.numInputs = nIn > 0 ? (nAux > 0 ? 2 : 1) : 0; d.inputs = nIn > 0 ? inBuses : nullptr; d.numOutputs = 1; d.outputs = &ob;
        d.inputEvents = feed && !feed->events.empty() ? &events_ : nullptr;
        d.inputParameterChanges = &changes_;
        if (!seh_process(proc_.get(), &d)) {
            static std::atomic<int> warned{0};
            if (warned.fetch_add(1) < 3) vstLog("process: the editor instance faulted inside the plugin (recovered, block silenced)");
            for (int c = 0; c < nOut && c < (int)outScratch_.size(); ++c)   // was frames*2 on ONE channel buffer: a write past its end
                std::fill_n(outScratch_[(size_t)c].begin(), std::min((size_t)frames, outScratch_[(size_t)c].size()), 0.f);
        }
    }
    // The editor tick drains this instance's engine snapshot for the feed. Copies everything out
    // without ever blocking the render thread: audio comes from the SPSC ring (the producer only
    // stores an index), MIDI from a try-lock. One call returns at most one prepared block, so
    // draining every tick replays the engine's audio at the engine's own rate.
    bool takeEditorFeed(EditorFeed& out) override {
        out.audio.clear();
        out.frames = 0;
        out.events.clear();
        size_t r = tapRead_.load(std::memory_order_relaxed);
        size_t w = tapWrite_.load(std::memory_order_acquire);
        // The window only needs RECENT audio: if more than two blocks wait, jump to the newest ones
        // instead of replaying the backlog (the 2-blocks-per-tick drain can never catch up otherwise).
        { const size_t keepFrames = (size_t)(editorSetupBlock_ > 0 ? editorSetupBlock_ : 512) * 2;
          if (w - r > keepFrames) r = w - keepFrames; }
        const int block = editorSetupBlock_ > 0 ? editorSetupBlock_ : 512;
        int n = (int)std::min<size_t>(w - r, (size_t)block);
        if (n > 0) {
            out.audio.resize((size_t)n * 2);
            for (int i = 0; i < n; ++i) {
                const size_t src = ((r + (size_t)i) & (kTapCap - 1)) * 2;
                out.audio[(size_t)i * 2] = tapRing_[src];
                out.audio[(size_t)i * 2 + 1] = tapRing_[src + 1];
            }
            out.frames = n;
            tapRead_.store(r + (size_t)n, std::memory_order_release);
        }
        {
            std::unique_lock<std::mutex> lock(tapMutex_, std::try_to_lock);
            if (lock.owns_lock() && !tapEvents_.empty()) {
                out.events.assign(tapEvents_.begin(), tapEvents_.end());
                tapEvents_.clear();
            }
        }
        return out.frames > 0 || !out.events.empty();
    }
    bool stateDirty() const override { return stateDirty_.load(std::memory_order_relaxed); }
    void markStateCaptured() override { stateDirty_.store(false, std::memory_order_relaxed); }
    double msSinceLastTouch() const override {
        const double t = lastTouchMs_.load(std::memory_order_relaxed);
        return t <= 0.0 ? 1e9 : nowMs() - t;
    }
    bool windowBusy() const override { return windowBusy_.load(std::memory_order_relaxed); }
    // One MIDI message from a ring (the engine's or the editor tick's replay) into VST3 events /
    // parameter changes. CC 120 / 123 (all sound / all notes off) become real note-offs for every
    // note this instance still sees as held: the transport stop sends exactly those, and a plugin
    // that never receives them keeps its voices forever ("Stop does not stop the sound"). Other
    // CCs / bend / aftertouch drive parameters through the controller's IMidiMapping when the
    // plugin provides one (optional per spec - otherwise they are skipped, as before).
    void mapMidi(const MidiMsg& m, EventList& ev, ParameterChanges& pc, int32 offset) {
        const uint8_t st = m.status & 0xF0, chn = m.status & 0x0F;
        Event e{}; e.busIndex = 0; e.sampleOffset = offset; e.ppqPosition = 0; e.flags = 0;
        if (st == 0x90 && m.d2 > 0) {
            e.type = Event::kNoteOnEvent; e.noteOn = {chn, m.d1, 0.f, m.d2 / 127.f, 0, -1};
            ev.addEvent(e);
            heldNotes_.insert((int)chn * 128 + (int)m.d1);
            return;
        }
        if (st == 0x80 || (st == 0x90 && m.d2 == 0)) {
            e.type = Event::kNoteOffEvent; e.noteOff = {chn, m.d1, m.d2 / 127.f, -1, 0.f};
            ev.addEvent(e);
            heldNotes_.erase((int)chn * 128 + (int)m.d1);
            return;
        }
        if (st == 0xB0 && (m.d1 == 120 || m.d1 == 123)) {
            int sent = 0;
            for (auto it = heldNotes_.begin(); it != heldNotes_.end() && sent < 64; ) {
                const int key = *it;
                if ((key >> 7) != (int)chn) { ++it; continue; }
                Event off{}; off.busIndex = 0; off.sampleOffset = offset; off.ppqPosition = 0; off.flags = 0;
                off.type = Event::kNoteOffEvent; off.noteOff = {(int16)(key >> 7), (int16)(key & 127), 0.f, -1, 0.f};
                ev.addEvent(off);
                it = heldNotes_.erase(it);
                ++sent;
            }
            return;
        }
        if (ccMap_.empty()) return;
        int cn = 0; double value = 0;
        if (st == 0xB0) { cn = (int)m.d1; value = m.d2 / 127.0; }
        else if (st == 0xD0) { cn = kAfterTouch; value = m.d2 / 127.0; }
        else if (st == 0xE0) { cn = kPitchBend; const int v14 = ((m.d2 & 0x7F) << 7) | (m.d1 & 0x7F); value = (v14 / 16383.0) * 2.0 - 1.0; }
        else return;                      // poly aftertouch (0xA0): no mapping path
        if (cn > 140) return;
        const ParamID pid = ccMap_[(size_t)chn * 141 + (size_t)cn];
        if (pid == (ParamID)-1) return;
        for (const PluginParam& p : params_) {
            if (p.id != pid || p.readOnly) continue;
            int32 qi, pi;
            if (auto* q = pc.addParameterData(p.id, qi)) q->addPoint(offset, value, pi);
            params_[(size_t)p.index].value = value;   // keep the cache and the UI in sync
            break;
        }
    }
    void prepare(double sr, int maxBlock) override {
        // Already running with exactly this setup: nothing to do. A shared plugin is wrapped again by
        // the NEW graph while the old one still plays it (a chain re-order / add / remove); calling
        // setupProcessing / setActive again on an ACTIVE plugin breaks VST3's state machine - Cabbage
        // and others then process silence, which was the "next effect receives nothing".
        if (enginePrepared_ && preparedSr_ == sr && preparedBlock_ == maxBlock && preparedWant_ == wantChannels_) return;
        if (enginePrepared_) {   // a real re-setup: the VST3 order is processing off -> inactive -> setup -> active -> processing on
            seh_setProcessing(proc_.get(), false);
            seh_setActive(comp_.get(), false);
            enginePrepared_ = false;
        }
        {   // the same object was set up for the window (prepareEditor): stop that before the engine takes it
            std::lock_guard<std::mutex> api(editorApiMutex_);
            if (editorProcessing_) { seh_setProcessing(proc_.get(), false); editorProcessing_ = false; }
            if (editorComponentActive_) { seh_setActive(comp_.get(), false); editorComponentActive_ = false; }
        }
        audioBuffers_.prepare(maxBlock);
        // ASK the plugin for the host's channel layout (VST3 setBusArrangements, before setup /
        // activation). Plugins that follow the host (Cabbage: nchnls = 4) otherwise stay at their
        // default stereo layout and only ever see / write 2 channels. The plugin may refuse: it then
        // keeps its own layout, and the real widths are read back from getBusInfo below.
        applyWantedLayout();
        ProcessSetup s{kRealtime, kSample32, maxBlock, sr};
        if (!seh_setupProcessing(proc_.get(), &s)) vstLog("prepare: setupProcessing faulted inside the plugin (recovered)");
        if (!seh_setActive(comp_.get(), true)) vstLog("prepare: setActive faulted inside the plugin (recovered)");
        if (!seh_setProcessing(proc_.get(), true)) vstLog("prepare: setProcessing faulted inside the plugin (recovered)");
        maxBlock_ = maxBlock;
        enginePrepared_ = true; preparedSr_ = sr; preparedBlock_ = maxBlock; preparedWant_ = wantChannels_;
        BusInfo bi{};   // the real channel counts: the process buffers must match the plugin's buses
        if (seh_getBusCount(comp_.get(), kAudio, kInput) > 0 && seh_getBusInfo(comp_.get(), kAudio, kInput, 0, &bi) && bi.channelCount > 0) inBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed); else inBusCh_.store(instrument_ ? 0 : 2, std::memory_order_relaxed);
        if (seh_getBusInfo(comp_.get(), kAudio, kOutput, 0, &bi) && bi.channelCount > 0) outBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed); else outBusCh_.store(2, std::memory_order_relaxed);
        auxBusCh_.store(0, std::memory_order_relaxed);
        if (seh_getBusCount(comp_.get(), kAudio, kInput) > 1 && seh_getBusInfo(comp_.get(), kAudio, kInput, 1, &bi) && bi.busType == BusTypes::kAux && bi.channelCount > 0)
            auxBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed);
        latency_.store((int)seh_getLatency(proc_.get()), std::memory_order_relaxed);   // the plugin's own delay (oversampling / look-ahead): the dry/wet alignment uses it
        vstLog("audio buses: in " + std::to_string(inBusCh_.load()) + " ch, out " + std::to_string(outBusCh_.load()) + " ch, latency " + std::to_string(latency_.load()) + " samples"); }
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    void setWantedChannels(int n) override { wantChannels_ = std::max(0, std::min(8, n)); }
    // Ask the plugin for the wanted layout (VST3 setBusArrangements) while it is INACTIVE. Called at
    // load (before the very first activation - Cabbage builds its internal Csound with the layout of
    // its FIRST start; asked later, it keeps that one and fills the extra channels with copies), and
    // again before every setup. A refusal keeps the plugin's own layout.
    void applyWantedLayout() {
        if (wantChannels_ <= 0 || !proc_ || !comp_) return;
        const int nInBus = std::max(0, seh_getBusCount(comp_.get(), kAudio, kInput));
        const int nOutBus = std::max(0, seh_getBusCount(comp_.get(), kAudio, kOutput));
        std::vector<SpeakerArrangement> ins((size_t)nInBus), outs((size_t)nOutBus);
        for (int b = 0; b < nInBus; ++b) {   // main bus = the wanted width; sidechain / aux buses keep their own
            SpeakerArrangement cur = 0;
            if (b > 0 && proc_->getBusArrangement(kInput, b, cur) == kResultTrue) ins[(size_t)b] = cur;
            else ins[(size_t)b] = b == 0 ? cawArrangement(wantChannels_) : SpeakerArr::kStereo;
        }
        for (int b = 0; b < nOutBus; ++b) {
            SpeakerArrangement cur = 0;
            if (b > 0 && proc_->getBusArrangement(kOutput, b, cur) == kResultTrue) outs[(size_t)b] = cur;
            else outs[(size_t)b] = b == 0 ? cawArrangement(wantChannels_) : SpeakerArr::kStereo;
        }
        const bool ok = seh_setBusArrangements(proc_.get(), ins.empty() ? nullptr : ins.data(), nInBus,
                                               outs.empty() ? nullptr : outs.data(), nOutBus);
        BusInfo bi{};
        int outW = 0;
        if (seh_getBusInfo(comp_.get(), kAudio, kOutput, 0, &bi)) outW = (int)bi.channelCount;
        vstLog("bus layout: asked for " + std::to_string(wantChannels_) + " ch -> " + (ok ? "accepted" : "refused") +
               ", output bus now " + std::to_string(outW) + " ch");
        if (outW > 0) outBusCh_.store(std::min(8, outW), std::memory_order_relaxed);
        if (nInBus > 0 && seh_getBusInfo(comp_.get(), kAudio, kInput, 0, &bi) && bi.channelCount > 0) inBusCh_.store(std::min(8, (int)bi.channelCount), std::memory_order_relaxed);
    }
    void setSidechain(const float* sc) override { scIn_ = sc; }
    int sidechainChannels() const override { return auxBusCh_.load(std::memory_order_relaxed); }
    int outputChannels() const override { return std::min(8, outBusCh_.load(std::memory_order_relaxed)); }
    int inputChannels() const override { return std::min(8, inBusCh_.load(std::memory_order_relaxed)); }
    // the aux (sidechain) input bus buffers for one block: the rack's stereo sidechain (or silence)
    int fillAux(float** auxp, int frames, bool withSignal) {
        const int nAux = std::min(8, auxBusCh_.load(std::memory_order_relaxed));
        if ((int)auxScratch_.size() < nAux) auxScratch_.resize((size_t)nAux);
        for (int c = 0; c < nAux; ++c) {
            if ((int)auxScratch_[(size_t)c].size() < frames) auxScratch_[(size_t)c].resize((size_t)frames);
            for (int i = 0; i < frames; ++i) auxScratch_[(size_t)c][(size_t)i] = (withSignal && scIn_ && c < 2) ? scIn_[(size_t)i * 2 + (size_t)c] : 0.f;
            auxp[c] = auxScratch_[(size_t)c].data();
        }
        return nAux;
    }
    void process(const float* in, float* out, int frames, int ch, Endpoint& ep) override {
        // 1. MIDI events from the ring (cawsm, MIDI clips, device). CC / bend / aftertouch are
        //    mapped too now (see mapMidi): the transport stop's all-notes-off MUST reach the
        //    plugin, and a mapped CC drives its parameter live. The raw messages are also
        //    captured for the editor feed; the audio thread never waits on the UI for that.
        events_.clear(); changes_.clearQueue();
        MidiMsg m; int32 qi; int guard = 0;
        while (ep.midiIn.pop(m) && ++guard < 512) {
            { std::unique_lock<std::mutex> lock(tapMutex_, std::try_to_lock); if (lock.owns_lock() && tapEvents_.size() < 256) tapEvents_.push_back(m); }
            mapMidi(m, events_, changes_, 0);
        }
        // 2. parameter changes from the ring (cawsp, automation lanes)
        ParamMsg pm;
        // The editor's own knob turns (SPSC ring from performEdit) reach the processor here:
        // the host never forwards them by itself, so a single-instance plugin (master FX) ignored its GUI.
        {
            unsigned r = handler_.pendR_.load(std::memory_order_relaxed);
            int drained = 0;
            while (r != handler_.pendW_.load(std::memory_order_acquire) && drained < 128) {
                const int pi2 = handler_.pendEdit_[r].idx.load(std::memory_order_relaxed);
                const float pv = handler_.pendEdit_[r].val.load(std::memory_order_relaxed);
                if (pi2 >= 0 && pi2 < (int)params_.size()) {
                    auto* q = changes_.addParameterData(params_[(size_t)pi2].id, qi);
                    int32 point;
                    if (q) q->addPoint(0, (double)pv, point);
                }
                r = (r + 1u) % handler_.kPendEditN; ++drained;
            }
            handler_.pendR_.store(r, std::memory_order_release);
        }
        // Initial editor values (restored before the graph went live) reach the processor across the
        // next blocks, a bounded number per block: a burst of thousands of changes in one block makes
        // some plugins stop processing (Arturia ARP 2600 goes silent). Automation below is applied
        // after the initials of the same block, so the current lane always wins.
        int sentInitial = 0;
        {
        std::lock_guard<std::mutex> lock(initParamMutex_);   // the UI thread's setParam may be filling it
        for (auto it = initialParameters_.begin(); it != initialParameters_.end(); ) {
            if (sentInitial >= 128) break;
            if (it->first < 0 || it->first >= (int)params_.size()) { it = initialParameters_.erase(it); continue; }
            auto* q = changes_.addParameterData(params_[it->first].id, qi); int32 pi;
            if (q) q->addPoint(0, it->second, pi);
            it = initialParameters_.erase(it);
            ++sentInitial;
        }
        }
        while (ep.paramIn.pop(pm) && pm.paramIndex >= 0 && pm.paramIndex < (int)params_.size()) {
            auto* q = changes_.addParameterData(params_[pm.paramIndex].id, qi); int32 pi;
            if (q) q->addPoint(0, pm.value, pi); params_[pm.paramIndex].value = pm.value;
        }
        // 3. audio: de-interleave into per-channel scratch, hand the plugin its real bus sizes, interleave back
        // The buses are handed over at the plugin's OWN widths (what getBusInfo reported, max 8): a
        // plugin always writes every channel of its bus, so declaring a bus with fewer buffers than
        // it has channels made it write through null / foreign pointers (the crash with many or wide
        // plugins). Channels the caller does not carry are fed silence / dropped on interleave.
        if (frames <= 0) return;
        if (frames > kMaxProcessFrames) frames = kMaxProcessFrames;
        const int busIn = std::max(0, std::min(8, inBusCh_.load(std::memory_order_relaxed)));
        // EVERY active bus gets its buffers, also on an instrument: Cabbage (and other) instruments keep
        // an audio input bus, CAW activates it at load - handing the plugin 0 inputs for an ACTIVE bus
        // breaks the VST3 contract and the plugin returns silence. No input audio = silent buffers.
        const int nIn = busIn;
        const int nOut = std::max(1, std::min(8, outBusCh_.load(std::memory_order_relaxed)));
        if ((int)inScratch_.size() < nIn) inScratch_.resize((size_t)nIn);
        if ((int)outScratch_.size() < nOut) outScratch_.resize((size_t)nOut);
        for (int c = 0; c < nIn; ++c) {
            if ((int)inScratch_[(size_t)c].size() < frames) inScratch_[(size_t)c].resize((size_t)frames);
            if (in && c < ch) for (int i = 0; i < frames; ++i) inScratch_[(size_t)c][(size_t)i] = in[(size_t)i * ch + c];
            else std::fill_n(inScratch_[(size_t)c].begin(), (size_t)frames, 0.f);
        }
        for (int c = 0; c < nOut; ++c) {
            if ((int)outScratch_[(size_t)c].size() < frames) outScratch_[(size_t)c].resize((size_t)frames);
            std::fill_n(outScratch_[(size_t)c].begin(), (size_t)frames, 0.f);
        }
        float* ins[8] = {}; float* outs[8] = {};
        for (int c = 0; c < nIn; ++c) ins[c] = inScratch_[(size_t)c].data();
        for (int c = 0; c < nOut; ++c) outs[c] = outScratch_[(size_t)c].data();
        AudioBusBuffers ib{}, ob{}; ib.numChannels = nIn; ib.channelBuffers32 = ins; ob.numChannels = nOut; ob.channelBuffers32 = outs;
        float* auxp[8] = {};
        const int nAux = nIn > 0 ? fillAux(auxp, frames, in != nullptr) : 0;   // an active sidechain bus always gets buffers (silence without a sidechain)
        AudioBusBuffers ab{}; ab.numChannels = nAux; ab.channelBuffers32 = auxp;
        AudioBusBuffers inBuses[2] = {ib, ab};
        ProcessData d{}; d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = frames;
        d.numInputs = nIn > 0 ? (nAux > 0 ? 2 : 1) : 0; d.inputs = nIn > 0 ? inBuses : nullptr; d.numOutputs = 1; d.outputs = &ob;
        d.inputEvents = &events_; d.inputParameterChanges = &changes_;
        if (!seh_process(proc_.get(), &d)) {
            static std::atomic<int> warned{0};
            if (warned.fetch_add(1) < 3) vstLog("process: the plugin faulted inside its render call (recovered, block silenced)");
            for (int c = 0; c < nOut; ++c) std::memset(outScratch_[(size_t)c].data(), 0, (size_t)frames * sizeof(float));
        }
        if (out) for (int c = 0; c < ch; ++c) for (int i = 0; i < frames; ++i)
            out[(size_t)i * ch + c] = c < nOut ? outScratch_[(size_t)c][(size_t)i] : 0.f;   // channels the plugin does not have stay silent (wider buses than `ch` are dropped here, never written past `out`)
        // SEPARATED ENGINES: stream the first stereo pair of this block into the editor's ring
        // (EQ analyzers, scopes). Instruments stream silence - their GUI meters move on the
        // dispatched notes, not on input audio. The audio thread never waits on the editor: a
        // full ring just drops the newest frames, and the consumer paces itself by what is there.
        {
            size_t w = tapWrite_.load(std::memory_order_relaxed);
            const size_t r = tapRead_.load(std::memory_order_acquire);
            const size_t used = w >= r ? w - r : kTapCap;
            const size_t room = used < kTapCap ? kTapCap - used : 0;
            const int ncap = in ? std::min(2, nIn) : 0;
            const int put = (int)std::min<size_t>((size_t)frames, room);
            for (int i = 0; i < put; ++i) {
                const size_t dst = ((w + (size_t)i) & (kTapCap - 1)) * 2;
                tapRing_[dst] = ncap > 0 ? in[(size_t)i * ch] : 0.f;
                tapRing_[dst + 1] = ncap > 1 ? in[(size_t)i * ch + 1] : 0.f;
            }
            if (put > 0) tapWrite_.store(w + (size_t)put, std::memory_order_release);
        }
        // 4. publish the last parameter values so code can read them (cawgp)
        if (ep.paramLast.size() == params_.size()) for (size_t i = 0; i < params_.size(); ++i) ep.paramLast[i].store((float)params_[i].value);
    }
    const std::vector<PluginParam>& params() const override { return params_; }
    void setParam(int i, double v) override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        if (ctrl_ && i >= 0 && i < (int)params_.size() && !params_[i].readOnly) {
            v = std::min(1.0, std::max(0.0, v));
            HostParamGuard hostSet(hostParam_);   // the plugin may echo this as performEdit: not a user edit
            if (!seh_setParam(ctrl_.get(), params_[i].id, v)) vstLog("param: setParamNormalized faulted inside the plugin (recovered)");
            params_[i].value = v;
            { std::lock_guard<std::mutex> lock(initParamMutex_); initialParameters_[i] = v; }
        }
    }
    double getParam(int i) const override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        if (i < 0 || i >= (int)params_.size()) return 0.0;
        if (!ctrl_) return params_[i].value;
        double v = params_[i].value;
        if (!seh_getParam(ctrl_.get(), params_[i].id, &v)) vstLog("param: getParamNormalized faulted inside the plugin (recovered)");
        return v;
    }
    // Bulk normalized values for host-side change detection. ONE DPI context for the whole
    // sweep (a per-parameter context switch would cost more than the getters themselves).
    int readParameterValues(std::vector<double>& out) const override {
        const int n = (int)params_.size();
        if (!ctrl_ || n <= 0) { out.clear(); return 0; }
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        out.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            if (params_[(size_t)i].readOnly) { out[(size_t)i] = 0.0; continue; }
            double v = params_[(size_t)i].value;
            if (!seh_getParam(ctrl_.get(), params_[(size_t)i].id, &v)) v = params_[(size_t)i].value;
            out[(size_t)i] = v;
        }
        return n;
    }
    bool editorVisible() const override {
#ifdef _WIN32
        return hwnd_ && IsWindowVisible(hwnd_);
#else
        return edWin_ && cawed::visible(edWin_);
#endif
    }
    void setLiveEditHook(std::function<void(int, double)> hook) override { liveHook_ = std::move(hook); }
    void setLearnEnabled(bool enabled) override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        handler_.learning = false;
        handler_.clearTouches();
        learnValues_.clear();
        if (!enabled || !ctrl_) return;
        learnValues_.reserve(params_.size());
        for (const auto& p : params_) learnValues_.push_back(seh_getParamNormalized(ctrl_.get(), p.id));
        nextLearnPoll_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
        handler_.learning = true;
        vstLog("learn: armed");
    }
    int takeTouchedParam() override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        if (!handler_.learning || !ctrl_) return -1;
        int i;
        while ((i = handler_.popTouch()) >= 0) {
            if ((size_t)i < params_.size() && !params_[(size_t)i].readOnly) {
                vstLog("learn: edit " + params_[(size_t)i].title); return i;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextLearnPoll_) return -1;
        nextLearnPoll_ = now + std::chrono::milliseconds(120);
        int changed = -1;
        for (size_t n = 0; n < params_.size() && n < learnValues_.size(); ++n) {
            double value = learnValues_[n];
            if (!seh_getParam(ctrl_.get(), params_[n].id, &value)) value = learnValues_[n];
            if (changed < 0 && !params_[n].readOnly && std::isfinite(value) &&
                std::fabs(value - learnValues_[n]) > 1e-6) changed = (int)n;
            learnValues_[n] = value;
        }
        if (changed >= 0) vstLog("learn: polled " + params_[(size_t)changed].title);
        return changed;
    }
    bool showEditor(const std::string& title, std::string& err) override {
        std::lock_guard<std::mutex> api(editorApiMutex_);
#ifdef _WIN32
        EditorDpiContext dpiContext(systemScaling_);
        if (!ctrl_) { err = "this plugin has no controller"; return false; }
        if (!view_) {
            handler_.indexOf = [this](ParamID id) { const auto it = paramIndexById_.find(id); return it == paramIndexById_.end() ? -1 : it->second; };
            seh_setComponentHandler(ctrl_.get(), &handler_);
            Steinberg::IPlugView* view = nullptr;
            if (!seh_createView(ctrl_.get(), ViewType::kEditor, &view)) {
                vstLog("editor: createView faulted inside the plugin (recovered)");
                err = "The plugin's editor could not be created (it faulted). Try its parameters below, or check caw_vst_log.txt.";
                return false;
            }
            view_ = owned(view);
            if (!view_) {
                vstLog("editor: createView returned null; parameters=" + std::to_string(params_.size()));
                err = "The plugin did not provide an editor. Try its parameters below, or check caw_vst_log.txt.";
                return false;
            }
            if (view_->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) { view_ = nullptr; err = "the editor is not supported here"; return false; }
            static bool registered = false;
            if (!registered) {
                WNDCLASSA wc{}; wc.lpfnWndProc = editorProc; wc.hInstance = GetModuleHandle(nullptr); wc.lpszClassName = "CawVstEditor"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
                wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                RegisterClassA(&wc); registered = true;
            }
            vstLog("editor: create window");
            // Resizing is driven by the plugin; this avoids unconstrained host resizing.
            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            // Owned by CAW's main window: an owned window always stays above its owner, so clicking
            // any CAW surface can no longer bury the plugin window behind the maximized main window
            // (it used to look "closed"). It also minimizes/restores together with CAW. The main
            // window is found by its class ("CawWindow", registered in main.cpp) so one plugin editor
            // never becomes the owner of another (closing an owner destroys its owned windows).
            const HWND owner = FindWindowA("CawWindow", nullptr) ? FindWindowA("CawWindow", nullptr) : GetActiveWindow();
            // WS_EX_TOOLWINDOW: the plugin window never gets its own taskbar entry / thumbnail (it is a
            // child panel of CAW, like a DAW's plugin windows).
            hwnd_ = CreateWindowExA(WS_EX_TOOLWINDOW, "CawVstEditor", title.c_str(), style, 100, 100, 400, 300, owner, nullptr, GetModuleHandle(nullptr), nullptr);
            if (!hwnd_) { view_ = nullptr; err = "could not create the window"; return false; }
            SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
            handler_.resize = [this](IPlugView* v, ViewRect* r) -> tresult {
                if (!view_ || v != view_.get() || !r) return kResultFalse;
                return resizeEditor(*r) ? kResultOk : kResultFalse;
            };
            view_->setFrame(&handler_);
            // Size the window to the view BEFORE attached(), scale AFTER: the exact order of the
            // last build where Kilohearts' editors (Phase Plant, Snap Heap) opened and animated
            // correctly. The post-attach scale path also re-queries getSize and sends onSize with
            // the plugin's own (scaled) size, which is what their GUIs lay out on.
            ViewRect initial{};
            if (view_->getSize(&initial) == kResultTrue) resizeEditor(initial);
            vstLog("editor: attach");
            viewAttached_ = true; // A plugin may call resizeView synchronously during attached().
            const tresult attached = view_->attached(hwnd_, kPlatformTypeHWND);
            if (attached != kResultOk) {
                viewAttached_ = false; view_->setFrame(nullptr); view_ = nullptr;
                DestroyWindow(hwnd_); hwnd_ = nullptr; err = "the plugin refused to open its window"; return false;
            }
            applyEditorScale(windowDpi(hwnd_));
            // An explicit onSize with the final client rect: some GUIs only create / lay out their
            // child rendering window here (and animation timers start with it), not in attached().
            { RECT c{}; GetClientRect(hwnd_, &c);
              const int w = (int)(c.right - c.left), h = (int)(c.bottom - c.top);
              if (w > 0 && h > 0) { lastWidth_ = w; lastHeight_ = h; ViewRect r{0, 0, w, h}; view_->onSize(&r); } }
            vstLog("editor: attached, scale " + std::to_string(lastScale_) + ", size " + std::to_string(lastWidth_) + "x" + std::to_string(lastHeight_));
        }
        SetWindowTextA(hwnd_, title.c_str());
        ShowWindow(hwnd_, SW_SHOW); SetForegroundWindow(hwnd_);
        return true;
#else
#if defined(__APPLE__)
        const FIDString platform = kPlatformTypeNSView;
#else
        const FIDString platform = kPlatformTypeX11EmbedWindowID;
#endif
        if (!ctrl_) { err = "this plugin has no controller"; return false; }
        if (!view_) {
            handler_.indexOf = [this](ParamID id) { const auto it = paramIndexById_.find(id); return it == paramIndexById_.end() ? -1 : it->second; };
            seh_setComponentHandler(ctrl_.get(), &handler_);
            Steinberg::IPlugView* view = nullptr;
            if (!seh_createView(ctrl_.get(), ViewType::kEditor, &view) || !view) {
                err = "The plugin did not provide an editor. Use its parameters in CAW's panel.";
                return false;
            }
            view_ = owned(view);
            if (view_->isPlatformTypeSupported(platform) != kResultTrue) {
                view_ = nullptr;
                err = "This plugin's editor does not support this system's windows.";
                return false;
            }
            ViewRect initial{0, 0, 400, 300};
            view_->getSize(&initial);
            int w = initial.right - initial.left, h = initial.bottom - initial.top;
            if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { w = 400; h = 300; }
            edResizable_ = view_->canResize() == kResultTrue;
            vstLog("editor: create window " + std::to_string(w) + "x" + std::to_string(h) + (edResizable_ ? " (resizable)" : ""));
            edWin_ = cawed::create(title.c_str(), w, h, edResizable_);
            if (!edWin_) { view_ = nullptr; err = "could not create the window (no display?)"; return false; }
            lastWidth_ = w; lastHeight_ = h;
            handler_.resize = [this](IPlugView* v, ViewRect* r) -> tresult {
                if (!view_ || v != view_.get() || !r || !edWin_) return kResultFalse;
                const int nw = r->right - r->left, nh = r->bottom - r->top;
                if (nw <= 0 || nh <= 0 || nw > 16384 || nh > 16384) return kResultFalse;
                cawed::resize(edWin_, nw, nh, edResizable_);
                lastWidth_ = nw; lastHeight_ = nh;
                ViewRect done{0, 0, nw, nh};
                if (viewAttached_) view_->onSize(&done);
                return kResultOk;
            };
            view_->setFrame(&handler_);
            viewAttached_ = true;   // a plugin may call resizeView synchronously during attached()
            if (view_->attached(cawed::nativeHandle(edWin_), platform) != kResultOk) {
                viewAttached_ = false; view_->setFrame(nullptr); view_ = nullptr;
                cawed::destroy(edWin_); edWin_ = nullptr;
                err = "the plugin refused to open its window"; return false;
            }
            ViewRect r{0, 0, lastWidth_, lastHeight_};
            view_->onSize(&r);
            vstLog("editor: attached");
        }
        cawed::setTitle(edWin_, title.c_str());
        cawed::show(edWin_);
        return true;
#endif
    }
#if !defined(_WIN32)
    // Called every UI frame (vstui::pumpEditorWindows): the window's close button and a user
    // resize reach the plugin here, on the UI thread, outside the plugin's own callbacks.
    void pumpEditor() override {
#if defined(__linux__)
        handler_.runLoop.run();   // the plugin's display fd + redraw timers (they may also run while closing)
#endif
        if (!edWin_) return;
        int w = 0, h = 0;
        const int f = cawed::poll(edWin_, &w, &h);
        if (f & 1) { touchEditor(); closeEditor(); return; }   // closed: the instance and its patch stay
        if ((f & 2) && view_ && viewAttached_) {
            std::lock_guard<std::mutex> api(editorApiMutex_);
            ViewRect r{0, 0, w, h};
            if (view_->checkSizeConstraint(&r) == kResultTrue) {
                const int cw = r.right - r.left, ch = r.bottom - r.top;
                if (cw != w || ch != h) cawed::resize(edWin_, cw, ch, edResizable_);
                w = cw; h = ch;
            }
            if (w != lastWidth_ || h != lastHeight_) {
                lastWidth_ = w; lastHeight_ = h;
                ViewRect done{0, 0, w, h};
                view_->onSize(&done);
            }
        }
    }
#endif
    // SEPARATED ENGINES: the full component state (Kontakt's loaded .nki, an FX preset, GUI-only
    // state - binary, NOT expressible as parameters). Captured from the EDITOR instance and
    // applied to the ENGINE instance (at engine creation via VstJob::stateBlob, or between blocks
    // via VstRack::pushState). Base64 keeps the blob text-safe on the way through the app.
    std::string getState() const override {
        if (!comp_) return {};
        std::lock_guard<std::mutex> api(editorApiMutex_);
        try {
            Steinberg::MemoryStream state;
            if (!seh_getState(comp_.get(), &state)) { vstLog("state: getState faulted inside the plugin (recovered)"); return {}; }
            Steinberg::int64 size = 0;
            if (state.seek(0, Steinberg::IBStream::kIBSeekEnd, &size) != kResultOk || size <= 0) return {};
            std::string raw; raw.resize((size_t)size);
            state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            int32 got = 0;
            if (state.read(&raw[0], (int32)raw.size(), &got) != kResultOk || got <= 0) return {};
            raw.resize((size_t)got);
            return base64Encode(raw);
        } catch (...) { vstLog("state: capture threw - no state saved"); return {}; }
    }
    void setState(const std::string& blob) override {
        if (!comp_ || blob.empty()) return;
        std::lock_guard<std::mutex> api(editorApiMutex_);
        std::string raw;
        if (!base64Decode(blob, raw)) { vstLog("state: blob is not base64 - skipped (" + std::to_string(blob.size()) + " chars)"); return; }
        if (raw.empty()) return;
        try {
            Steinberg::MemoryStream stream;
            int32 written = 0;
            if (stream.write((void*)raw.data(), (int32)raw.size(), &written) != kResultOk || written != (int32)raw.size()) {
                vstLog("state: could not buffer the blob - skipped"); return;
            }
            stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            if (!seh_setState(comp_.get(), &stream)) { vstLog("state: setState faulted inside the plugin (recovered)"); return; }
            // The controller follows the patch too, so parameter reads / the GUI reflect it.
            if (ctrl_) {
                stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                if (!seh_setComponentState(ctrl_.get(), &stream)) vstLog("state: setComponentState faulted inside the plugin (recovered)");
            }
            vstLog("state: applied " + std::to_string(raw.size()) + " bytes" + (ctrl_ ? " (controller resynced)" : ""));
        } catch (...) { vstLog("state: applying threw - the plugin keeps its previous state"); }
    }
private:
    static std::string u16to8(const TChar* s) { std::string o; for (; s && *s; ++s) o += (*s < 128) ? char(*s) : '?'; return o; }
    std::shared_ptr<VST3::Hosting::Module> module_; Steinberg::IPtr<PlugProvider> provider_;
    Steinberg::IPtr<IComponent> comp_; Steinberg::IPtr<IEditController> ctrl_; Steinberg::IPtr<IAudioProcessor> proc_;
    EventList events_{}; ParameterChanges changes_{16};
    VstAudioBuffers audioBuffers_;
    std::vector<PluginParam> params_; std::map<int, double> initialParameters_; std::mutex initParamMutex_;   // setParam (UI) vs process/processIdle drain
    std::function<void(int, double)> liveHook_;   // app callback for every editor edit (all parameters)
    std::vector<double> lastLive_;                // last editor value forwarded per parameter (dedupe)
    std::map<ParamID, int> paramIndexById_;       // performEdit id -> index (was a linear scan per call)
    bool instrument_ = false; std::string classId_; int maxBlock_ = 512;
    std::atomic<int> latency_{0};
    bool systemScaling_ = false;
    std::vector<double> learnValues_;
    std::chrono::steady_clock::time_point nextLearnPoll_{};
    EditorHandler handler_;
    Steinberg::IPtr<IPlugView> view_;
#if !defined(_WIN32)
    cawed::EdWin* edWin_ = nullptr;   // the native editor window (Cocoa / X11)
    bool edResizable_ = false;
    bool viewAttached_ = false;
    int lastWidth_ = -1, lastHeight_ = -1;
#endif
#ifdef _WIN32
    HWND hwnd_ = nullptr;
    bool viewAttached_ = false, resizing_ = false;
    unsigned resizeSerial_ = 0;
    int lastWidth_ = -1, lastHeight_ = -1;
    float lastScale_ = 0.f;   // last factor sent to IPlugViewContentScaleSupport
    static UINT windowDpi(HWND hwnd) {
        using GetDpi = UINT (WINAPI*)(HWND);
        HMODULE u = GetModuleHandleA("user32.dll");
        auto fn = u ? reinterpret_cast<GetDpi>(GetProcAddress(u, "GetDpiForWindow")) : nullptr;
        const UINT dpi = fn ? fn(hwnd) : 96;
        return dpi ? dpi : 96;
    }
    bool resizeEditor(const ViewRect& requested) {
        EditorDpiContext context(systemScaling_);
        if (!hwnd_ || !view_ || resizing_) return false;
        const int width = requested.right - requested.left, height = requested.bottom - requested.top;
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return false;
        RECT outer{0, 0, width, height};
        const DWORD style = (DWORD)GetWindowLongPtr(hwnd_, GWL_STYLE);
        const DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
        using AdjustForDpi = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        HMODULE u = GetModuleHandleA("user32.dll");
        auto adjust = u ? reinterpret_cast<AdjustForDpi>(GetProcAddress(u, "AdjustWindowRectExForDpi")) : nullptr;
        if (adjust) adjust(&outer, style, FALSE, exStyle, windowDpi(hwnd_));
        else AdjustWindowRectEx(&outer, style, FALSE, exStyle);
        resizing_ = true;
        const BOOL ok = SetWindowPos(hwnd_, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (ok && viewAttached_) {
            RECT actual{}; GetClientRect(hwnd_, &actual);
            ViewRect size{0, 0, static_cast<int32>(actual.right - actual.left), static_cast<int32>(actual.bottom - actual.top)};
            lastWidth_ = size.right; lastHeight_ = size.bottom;
            // VST3 requires onSize even when Windows sends no WM_SIZE (same-size resize).
            view_->onSize(&size);
            vstLog("editor: client " + std::to_string(size.right) + "x" + std::to_string(size.bottom) + " dpi=" + std::to_string(windowDpi(hwnd_)));
        }
        resizing_ = false;
        if (ok) ++resizeSerial_;
        return ok != FALSE;
    }
    // Set the view's content scale (before OR after attached). Redundant calls are skipped: a
    // repeated setContentScaleFactor with the same factor makes some editors apply their display
    // scale twice and draw at the wrong size.
    void setEditorScale(UINT dpi) {
        EditorDpiContext context(systemScaling_);
        if (!view_) return;
        if (systemScaling_) dpi = 96; // Windows scales the complete DPI-unaware window and mouse coordinates.
        const float factor = (float)dpi / 96.f;
        FUnknownPtr<IPlugViewContentScaleSupport> scaler(view_);
        if (!scaler) { lastScale_ = factor; vstLog(std::string("editor: mode=") + (systemScaling_ ? "windows" : "native") + " dpi=" + std::to_string(dpi) + " (no scale support)"); return; }
        if (lastScale_ == factor) return;
        const tresult result = scaler->setContentScaleFactor(factor);
        // A plugin may refuse scaling until its view is attached; keep lastScale_ unset so the
        // post-attach call retries instead of being deduped away.
        if (result == kResultOk) lastScale_ = factor;
        vstLog(std::string("editor: mode=") + (systemScaling_ ? "windows" : "native") + " dpi=" + std::to_string(dpi) + " setContentScaleFactor " + std::to_string(factor) + " -> " + std::to_string((int)result));
    }
    void applyEditorScale(UINT dpi) {
        if (!view_ || !hwnd_ || !viewAttached_) return;
        const unsigned before = resizeSerial_;
        setEditorScale(dpi);
        // Plugins may synchronously request a new size while handling the scale change.
        if (before == resizeSerial_) {
            ViewRect size{};
            if (view_->getSize(&size) == kResultTrue) resizeEditor(size);
        }
    }
    static LRESULT CALLBACK editorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Vst3Plugin*>(GetWindowLongPtr(h, GWLP_USERDATA));
        // A modal move/size loop keeps the plugin's GUI thread busy: no state capture then (some
        // editors serialize getState with their own UI locks and freeze mid-drag). Moving a
        // window changes no sound, so it does not arm a capture either.
        if (self && (m == WM_ENTERSIZEMOVE || m == WM_EXITSIZEMOVE))
            self->windowBusy_.store(m == WM_ENTERSIZEMOVE, std::memory_order_relaxed);
        // Close = really close the window (the plugin instance and its state stay): a merely HIDDEN
        // owned window is still listed by Windows' taskbar thumbnail / Aero Peek, so every plugin
        // ever opened popped up when the mouse hovered CAW's taskbar button. The view is detached
        // and the window destroyed after this message; the next Open re-creates it.
        if (m == WM_CLOSE) {
            if (self) { self->touchEditor(); self->closeRequested_.store(true, std::memory_order_release); }
            ShowWindow(h, SW_HIDE);
            if (self) PostMessageA(h, WM_APP + 41, 0, 0);   // tear down outside the plugin's own close handling
            return 0;
        }
        if (m == WM_APP + 41 && self && self->closeRequested_.exchange(false, std::memory_order_acq_rel)) {
            self->closeEditor();
            return 0;
        }
        if (m == WM_DPICHANGED && self && self->viewAttached_) {
            const RECT* suggested = reinterpret_cast<const RECT*>(l);
            if (suggested) SetWindowPos(h, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            self->applyEditorScale(LOWORD(w));
            return 0;
        }
        if (m == WM_SIZE && w != SIZE_MINIMIZED && self && self->viewAttached_ && self->view_ && !self->resizing_) {
            RECT c{}; GetClientRect(h, &c);
            const int width = c.right - c.left, height = c.bottom - c.top;
            if (width > 0 && height > 0 && (width != self->lastWidth_ || height != self->lastHeight_)) {
                EditorDpiContext context(self->systemScaling_);
                self->lastWidth_ = width; self->lastHeight_ = height;
                ViewRect r{0, 0, width, height}; self->view_->onSize(&r);
            }
        }
        return DefWindowProcA(h, m, w, l);
    }
#endif
};
std::unique_ptr<IPlugin> makeVst3Plugin() { return std::make_unique<Vst3Plugin>(); }
void vstLog(const std::string& line) {
    static std::mutex m; std::lock_guard<std::mutex> lock(m);
    if (FILE* f = std::fopen("caw_vst_log.txt", "ab")) { std::fputs((line + "\n").c_str(), f); std::fclose(f); }
    // A recovered plugin fault surfaces in the CAW console too: the problem must be readable
    // there, not only in caw_vst_log.txt / caw_crash.txt (and never a silent app-kill).
    if (line.find("faulted") != std::string::npos || line.find("(recovered") != std::string::npos) {
        static std::atomic<int> surf{0};
        if (surf.fetch_add(1, std::memory_order_relaxed) < 24 && cawcrash::sehConsole)
            cawcrash::sehConsole("[caw] VST3: " + line);
    }
}

std::vector<std::string> scanVst3(const std::vector<std::string>& extra);
std::vector<PluginInfo> scanVst3Info(const std::vector<std::string>& extra,
    const std::function<void(const std::vector<PluginInfo>&)>& publish) {
    // Cache v3: one line PER CLASS (path, file size/time, kind, name, class id). An unchanged file
    // whose cached classes are all classified needs no module load - and a multi-class bundle
    // replays every class, not just the first one.
    struct Cached { std::string size, time; bool complete = true; std::vector<PluginInfo> infos; };
    std::map<std::string, Cached> cache;
    { std::ifstream in("caw_vst_cache_v3.txt"); std::string line;
      while (std::getline(in, line)) {
          std::vector<std::string> f; std::stringstream ss(line); std::string x; while (std::getline(ss, x, '\t')) f.push_back(x);
          if (f.size() != 6) continue;
          Cached& c = cache[f[0]];
          if (c.size.empty()) { c.size = f[1]; c.time = f[2]; }
          const int kind = std::atoi(f[3].c_str());
          if (kind == 0) c.complete = false;
          c.infos.push_back({f[0], f[4], f[5], kind});
      } }
    const std::vector<std::string> files = scanVst3(extra);
    std::vector<PluginInfo> out;
    for (const std::string& path : files) out.push_back({path, fs::path(path).stem().string(), {}, 0});
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name || (a.name == b.name && a.classId < b.classId); });
    if (publish) publish(out);  // A slow plugin must not hide the rest of the discovered files.
    std::ofstream save("caw_vst_cache_v3.txt", std::ios::trunc);
    std::vector<PluginInfo> classified;
    classified.reserve(files.size());
    for (const std::string& path : files) {
        std::error_code ec; const auto sz = fs::is_directory(path, ec) ? std::uintmax_t(0) : fs::file_size(path, ec);
        const auto tm = fs::last_write_time(path, ec).time_since_epoch().count();
        const std::string ssz = std::to_string(sz), stm = std::to_string((long long)tm);
        std::vector<PluginInfo> entries;
        const auto it = cache.find(path);
        if (it != cache.end() && it->second.size == ssz && it->second.time == stm && it->second.complete && !it->second.infos.empty())
            entries = it->second.infos;
        else {
            try {
                vstLog("scan " + path);
                std::string err; std::shared_ptr<VST3::Hosting::Module> mod;
                {   // a bundle that already has live instances is read from its shared module: a temporary
                    // Module would run the bundle's ExitDll under those instances. Others are opened
                    // only for the scan and released again (no instance can exist for them).
                    std::lock_guard<std::mutex> lock(moduleMutex());
                    const auto known = moduleCache().find(path);
                    if (known != moduleCache().end()) mod = known->second;
                }
                if (!mod) mod = VST3::Hosting::Module::create(path, err);
                if (mod) for (auto& ci : mod->getFactory().classInfos()) {
                    if (ci.category() != kVstAudioEffectClass) continue;
                    const std::string categories = ci.subCategoriesString();
                    const std::string cid = cidText(ci.ID().data());
                    if (std::any_of(entries.begin(), entries.end(), [&](const PluginInfo& e) { return e.classId == cid; })) continue;
                    entries.push_back({path, ci.name(), cid, categories.find("Instrument") != std::string::npos ? 1 : (categories.empty() ? 0 : 2)});
                }
                if (!mod) vstLog("scan failed " + path + ": " + err);
            } catch (...) { vstLog("scan exception " + path); }
        }
        if (entries.empty()) entries.push_back({path, fs::path(path).stem().string(), {}, 0});
        for (const PluginInfo& info : entries) {
            save << info.path << '\t' << ssz << '\t' << stm << '\t' << info.kind << '\t' << info.name << '\t' << info.classId << '\n';
            classified.push_back(info);
        }
        save.flush();
        out = classified;
        if (publish) publish(out);
    }
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name || (a.name == b.name && a.classId < b.classId); });
    return out;
}
// User folders from Settings > Programs > VST3, kept in <home>/.caw_vst3_dirs (one per line).
static std::string vstDirsFile() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? (fs::u8path(home) / ".caw_vst3_dirs").u8string() : std::string();
}
static std::mutex g_vstDirsMutex;
static std::vector<std::string> g_vstDirs;
static bool g_vstDirsLoaded = false;
std::vector<std::string> vst3ExtraDirs() {
    std::lock_guard<std::mutex> lock(g_vstDirsMutex);
    if (!g_vstDirsLoaded) {
        g_vstDirsLoaded = true;
        const std::string file = vstDirsFile();
        if (!file.empty()) {
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line)) if (!line.empty()) g_vstDirs.push_back(line);
        }
    }
    return g_vstDirs;
}
void setVst3ExtraDirs(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lock(g_vstDirsMutex);
    g_vstDirs = dirs; g_vstDirsLoaded = true;
    const std::string file = vstDirsFile();
    if (file.empty()) return;
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& d : dirs) out << d << '\n';
}
std::vector<std::string> scanVst3(const std::vector<std::string>& extra) {
    std::vector<std::string> dirs = extra, out;
#ifdef _WIN32
    if (const char* common = std::getenv("CommonProgramFiles")) dirs.push_back((fs::path(common) / "VST3").string());
    dirs.push_back("C:/Program Files/Common Files/VST3");
    if (const char* local = std::getenv("LOCALAPPDATA")) dirs.push_back((fs::path(local) / "Programs/Common/VST3").string());
#elif defined(__APPLE__)
    dirs.push_back("/Library/Audio/Plug-Ins/VST3");
    if (const char* home = std::getenv("HOME")) dirs.push_back((fs::path(home) / "Library/Audio/Plug-Ins/VST3").string());
#else
    dirs.push_back("/usr/lib/vst3"); dirs.push_back("/usr/local/lib/vst3");
    if (const char* home = std::getenv("HOME")) dirs.push_back((fs::path(home) / ".vst3").string());
#endif
    std::set<std::string> seen;
    for (const auto& d : dirs) {
        std::error_code ec;
        fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            const fs::path p = it->path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".vst3") {
                const auto normalized = fs::weakly_canonical(p, ec);
                const std::string key = ec ? p.lexically_normal().string() : normalized.string();
                ec.clear();
                if (seen.insert(key).second) out.push_back(p.string());
                it.disable_recursion_pending(); // .vst3 folders are bundles, not search directories.
            }
            it.increment(ec);
            if (ec) { vstLog("scan directory error: " + ec.message()); ec.clear(); }
        }
    }
    return out;
}
}
