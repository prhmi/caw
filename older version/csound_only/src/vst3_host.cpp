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
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
using namespace Steinberg; using namespace Steinberg::Vst;
namespace fs = std::filesystem;

namespace caw {
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

// What the plugin calls back while its window is open: which parameter the user touches, and "please resize my window".
class EditorHandler : public IComponentHandler, public IPlugFrame {
public:
    std::atomic<bool> learning{false};
    std::mutex touchMutex;
    std::deque<int> touches;
    std::function<int(ParamID)> indexOf;
    std::function<tresult(IPlugView*, ViewRect*)> resize;
    void clearTouches() { std::lock_guard<std::mutex> lock(touchMutex); touches.clear(); }
    int popTouch() { std::lock_guard<std::mutex> lock(touchMutex); if (touches.empty()) return -1; int i = touches.front(); touches.pop_front(); return i; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue) override {
        if (learning && indexOf) {
            int i = indexOf(id);
            if (i >= 0) { std::lock_guard<std::mutex> lock(touchMutex); if (touches.size() < 256) touches.push_back(i); }
        }
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override { return resize && r ? resize(view, r) : kResultFalse; }
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override {
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler)
        QUERY_INTERFACE(_iid, obj, IPlugFrame::iid, IPlugFrame)
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1000; }      // lives as long as the plugin object: never deleted by the plugin
    uint32 PLUGIN_API release() override { return 1000; }
};

class Vst3Plugin : public IPlugin {
public:
    void setEditorSystemScaling(bool enabled) override { systemScaling_ = enabled; }
    ~Vst3Plugin() override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        if (ctrl_) ctrl_->setComponentHandler(nullptr);
#ifdef _WIN32
        if (view_) { view_->setFrame(nullptr); if (viewAttached_) view_->removed(); viewAttached_ = false; view_ = nullptr; }
        if (hwnd_) DestroyWindow(hwnd_);
#endif
    }
    bool load(const std::string& path, std::string& err) override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        ensureHostContext();
        module_ = VST3::Hosting::Module::create(path, err); if (!module_) { vstLog("load failed " + path + ": " + err); return false; }
        auto& fac = module_->getFactory();
        fac.setHostContext(&hostApp());
        vstLog("load " + path);
        for (auto& ci : fac.classInfos()) if (ci.category() == kVstAudioEffectClass) {
            instrument_ = ci.subCategoriesString().find("Instrument") != std::string::npos;
            vstLog("create provider for " + ci.name());
            provider_ = Steinberg::owned(new PlugProvider(fac, ci, true)); break; }
        if (!provider_) { err = "no audio class"; return false; }
        vstLog("initialize");
        if (!provider_->initialize()) { err = "the plugin did not start"; return false; }
        vstLog("initialized");
        // PlugProvider's accessors return already-retained references.
        comp_ = Steinberg::owned(provider_->getComponent());
        ctrl_ = Steinberg::owned(provider_->getController());
        if (!comp_) { err = "no component"; return false; }
        FUnknownPtr<IAudioProcessor> ap(comp_); proc_ = ap; if (!proc_) { err = "no processor"; return false; }
        if (ctrl_) {
            handler_.indexOf = [this](ParamID id) { for (const PluginParam& p : params_) if (p.id == id) return p.index; return -1; };
            ctrl_->setComponentHandler(&handler_);
            // Newly initialized split controllers need the processor's initial state.
            Steinberg::MemoryStream state;
            const tresult read = comp_->getState(&state);
            if (read == kResultTrue) {
                state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                const tresult sync = ctrl_->setComponentState(&state);
                vstLog("controller: component state sync result=" + std::to_string((int)sync));
            } else vstLog("controller: component state unavailable result=" + std::to_string((int)read));
        } else vstLog("controller: unavailable");
        for (int i = 0, n = comp_->getBusCount(kEvent, kInput); i < n; ++i) comp_->activateBus(kEvent, kInput, i, true);
        comp_->activateBus(kAudio, kOutput, 0, true);
        if (comp_->getBusCount(kAudio, kInput) > 0) comp_->activateBus(kAudio, kInput, 0, true);
        for (int i = 0, n = ctrl_ ? ctrl_->getParameterCount() : 0; i < n; ++i) {
            ParameterInfo pi{}; ctrl_->getParameterInfo(i, pi);
            PluginParam p; p.index = i; p.id = pi.id; p.def = pi.defaultNormalizedValue; p.steps = pi.stepCount;
            p.readOnly = pi.flags & ParameterInfo::kIsReadOnly;
            p.title = u16to8(pi.title); p.units = u16to8(pi.units); p.value = ctrl_->getParamNormalized(pi.id);
            params_.push_back(p);
        }
        changes_.setMaxParameters((int32)params_.size() + 8);
        vstLog("loaded, " + std::to_string(params_.size()) + " parameters");
        return true;
    }
    bool isInstrument() const override { return instrument_; }
    void prepare(double sr, int maxBlock) override {
        audioBuffers_.prepare(maxBlock);
        ProcessSetup s{kRealtime, kSample32, maxBlock, sr}; proc_->setupProcessing(s);
        comp_->setActive(true); proc_->setProcessing(true); maxBlock_ = maxBlock; }
    void process(const float* in, float* out, int frames, int ch, Endpoint& ep) override {
        // 1. MIDI events from the ring (cawsm, MIDI clips, device)
        events_.clear(); MidiMsg m; int guard = 0;
        while (ep.midiIn.pop(m) && ++guard < 512) {
            Event e{}; e.busIndex = 0; e.sampleOffset = 0; e.ppqPosition = 0; e.flags = 0;
            uint8_t st = m.status & 0xF0, chn = m.status & 0x0F;
            if (st == 0x90 && m.d2 > 0) { e.type = Event::kNoteOnEvent; e.noteOn = {chn, m.d1, 0.f, m.d2 / 127.f, 0, -1}; }
            else if (st == 0x80 || st == 0x90) { e.type = Event::kNoteOffEvent; e.noteOff = {chn, m.d1, m.d2 / 127.f, -1, 0.f}; }
            else continue;                      // CC / bend: map to a plugin parameter via the IMidiMapping interface (TODO phase 2)
            events_.addEvent(e);
        }
        // 2. parameter changes from the ring (cawsp, automation lanes)
        changes_.clearQueue(); ParamMsg pm; int32 qi;
        // Restore the initial editor values on the first processing block.
        // Automation below is applied afterwards, so the current lane wins.
        for (const auto& initial : initialParameters_) {
            if (initial.first < 0 || initial.first >= (int)params_.size()) continue;
            auto* q = changes_.addParameterData(params_[initial.first].id, qi); int32 pi;
            if (q) q->addPoint(0, initial.second, pi);
        }
        initialParameters_.clear();
        while (ep.paramIn.pop(pm) && pm.paramIndex >= 0 && pm.paramIndex < (int)params_.size()) {
            auto* q = changes_.addParameterData(params_[pm.paramIndex].id, qi); int32 pi;
            if (q) q->addPoint(0, pm.value, pi); params_[pm.paramIndex].value = pm.value;
        }
        // 3. audio
        audioBuffers_.begin(in, frames, ch);
        float* ins[2] = {audioBuffers_.left(), audioBuffers_.right()};
        float* outs[2] = {audioBuffers_.outLeft(), audioBuffers_.outRight()};
        AudioBusBuffers ib{}, ob{}; ib.numChannels = 2; ib.channelBuffers32 = ins; ob.numChannels = 2; ob.channelBuffers32 = outs;
        ProcessData d{}; d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = frames;
        d.numInputs = in ? 1 : 0; d.inputs = in ? &ib : nullptr; d.numOutputs = 1; d.outputs = &ob;
        d.inputEvents = &events_; d.inputParameterChanges = &changes_;
        proc_->process(d);
        audioBuffers_.finish(out, frames, ch);
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
            ctrl_->setParamNormalized(params_[i].id, v);
            params_[i].value = v;
            initialParameters_[i] = v;
        }
    }
    double getParam(int i) const override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        if (i < 0 || i >= (int)params_.size()) return 0.0;
        return ctrl_ ? ctrl_->getParamNormalized(params_[i].id) : params_[i].value;
    }
    bool editorVisible() const override {
#ifdef _WIN32
        return hwnd_ && IsWindowVisible(hwnd_);
#else
        return false;
#endif
    }
    void setLearnEnabled(bool enabled) override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        handler_.learning = false;
        handler_.clearTouches();
        learnValues_.clear();
        if (!enabled || !ctrl_) return;
        learnValues_.reserve(params_.size());
        for (const auto& p : params_) learnValues_.push_back(ctrl_->getParamNormalized(p.id));
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
            const double value = ctrl_->getParamNormalized(params_[n].id);
            if (changed < 0 && !params_[n].readOnly && std::isfinite(value) &&
                std::fabs(value - learnValues_[n]) > 1e-6) changed = (int)n;
            learnValues_[n] = value;
        }
        if (changed >= 0) vstLog("learn: polled " + params_[(size_t)changed].title);
        return changed;
    }
    bool showEditor(const std::string& title, std::string& err) override {
#ifdef _WIN32
        EditorDpiContext dpiContext(systemScaling_);
        if (!ctrl_) { err = "this plugin has no controller"; return false; }
        if (!view_) {
            handler_.indexOf = [this](ParamID id) { for (const PluginParam& p : params_) if (p.id == id) return p.index; return -1; };
            ctrl_->setComponentHandler(&handler_);
            view_ = owned(ctrl_->createView(ViewType::kEditor));
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
            hwnd_ = CreateWindowExA(0, "CawVstEditor", title.c_str(), style, 100, 100, 400, 300, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
            if (!hwnd_) { view_ = nullptr; err = "could not create the window"; return false; }
            SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
            handler_.resize = [this](IPlugView* v, ViewRect* r) -> tresult {
                if (!view_ || v != view_.get() || !r) return kResultFalse;
                return resizeEditor(*r) ? kResultOk : kResultFalse;
            };
            view_->setFrame(&handler_);
            ViewRect initial{};
            if (view_->getSize(&initial) == kResultTrue) resizeEditor(initial);
            vstLog("editor: attach");
            viewAttached_ = true; // A plugin may call resizeView synchronously during attached().
            const tresult attached = view_->attached(hwnd_, kPlatformTypeHWND);
            if (attached != kResultOk) {
                viewAttached_ = false; view_->setFrame(nullptr); view_ = nullptr;
                DestroyWindow(hwnd_); hwnd_ = nullptr; err = "the plugin refused to open its window"; return false;
            }
            vstLog("editor: attached");
            applyEditorScale(windowDpi(hwnd_));
        }
        SetWindowTextA(hwnd_, title.c_str());
        ShowWindow(hwnd_, SW_SHOW); SetForegroundWindow(hwnd_);
        return true;
#else
        err = "the plugin window is only available on Windows for now"; return false;
#endif
    }
    std::string getState() const override { return {}; }      // TODO: IComponent::getState via MemoryStream, base64 into the project file
    void setState(const std::string&) override {}
private:
    static std::string u16to8(const TChar* s) { std::string o; for (; s && *s; ++s) o += (*s < 128) ? char(*s) : '?'; return o; }
    std::shared_ptr<VST3::Hosting::Module> module_; Steinberg::IPtr<PlugProvider> provider_;
    Steinberg::IPtr<IComponent> comp_; Steinberg::IPtr<IEditController> ctrl_; Steinberg::IPtr<IAudioProcessor> proc_;
    EventList events_{}; ParameterChanges changes_{16};
    VstAudioBuffers audioBuffers_;
    std::vector<PluginParam> params_; std::map<int, double> initialParameters_;
    bool instrument_ = false; int maxBlock_ = 512;
    bool systemScaling_ = false;
    std::vector<double> learnValues_;
    std::chrono::steady_clock::time_point nextLearnPoll_{};
    EditorHandler handler_;
    Steinberg::IPtr<IPlugView> view_;
#ifdef _WIN32
    HWND hwnd_ = nullptr;
    bool viewAttached_ = false, resizing_ = false;
    unsigned resizeSerial_ = 0;
    int lastWidth_ = -1, lastHeight_ = -1;
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
    void applyEditorScale(UINT dpi) {
        EditorDpiContext context(systemScaling_);
        if (!view_ || !hwnd_ || !viewAttached_) return;
        const unsigned before = resizeSerial_;
        FUnknownPtr<IPlugViewContentScaleSupport> scaler(view_);
        if (systemScaling_) dpi = 96; // Windows scales the complete DPI-unaware window and mouse coordinates.
        vstLog(std::string("editor: mode=") + (systemScaling_ ? "windows" : "native") + " scale dpi=" + std::to_string(dpi) + (scaler ? " supported" : " unsupported"));
        if (scaler) scaler->setContentScaleFactor((float)dpi / 96.f);
        // Plugins may synchronously request a new size while handling the scale change.
        if (before == resizeSerial_) {
            ViewRect size{};
            if (view_->getSize(&size) == kResultTrue) resizeEditor(size);
        }
    }
    static LRESULT CALLBACK editorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Vst3Plugin*>(GetWindowLongPtr(h, GWLP_USERDATA));
        if (m == WM_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }
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
}
std::vector<std::string> scanVst3(const std::vector<std::string>& extra);
std::vector<PluginInfo> scanVst3Info(const std::vector<std::string>& extra,
    const std::function<void(const std::vector<PluginInfo>&)>& publish) {
    struct Cached { std::string size, time; PluginInfo info; };
    std::map<std::string, Cached> cache;
    { std::ifstream in("caw_vst_cache_v2.txt"); std::string line;
      while (std::getline(in, line)) {
          std::vector<std::string> f; std::stringstream ss(line); std::string x; while (std::getline(ss, x, '\t')) f.push_back(x);
          if (f.size() == 5) cache[f[0]] = {f[1], f[2], {f[0], f[4], std::atoi(f[3].c_str())}};
      } }
    std::vector<PluginInfo> out;
    for (const std::string& path : scanVst3(extra)) out.push_back({path, fs::path(path).stem().string(), 0});
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    if (publish) publish(out);  // A slow plugin must not hide the rest of the discovered files.
    std::ofstream save("caw_vst_cache_v2.txt", std::ios::trunc);
    for (PluginInfo& info : out) {
        const std::string path = info.path;
        std::error_code ec; const auto sz = fs::is_directory(path, ec) ? std::uintmax_t(0) : fs::file_size(path, ec);
        const auto tm = fs::last_write_time(path, ec).time_since_epoch().count();
        const std::string ssz = std::to_string(sz), stm = std::to_string((long long)tm);
        auto it = cache.find(path);
        if (it != cache.end() && it->second.size == ssz && it->second.time == stm && it->second.info.kind != 0) info = it->second.info;
        else {
            try {
                vstLog("scan " + path);
                std::string err; auto mod = VST3::Hosting::Module::create(path, err);
                if (mod) for (auto& ci : mod->getFactory().classInfos()) if (ci.category() == kVstAudioEffectClass) {
                    info.name = ci.name();
                    const std::string categories = ci.subCategoriesString();
                    info.kind = categories.find("Instrument") != std::string::npos ? 1 : (categories.empty() ? 0 : 2);
                    break;
                }
                if (!mod) vstLog("scan failed " + path + ": " + err);
            } catch (...) { vstLog("scan exception " + path); }
        }
        save << info.path << '\t' << ssz << '\t' << stm << '\t' << info.kind << '\t' << info.name << '\n';
        save.flush();
        if (publish) publish(out);
    }
    std::sort(out.begin(), out.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    return out;
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
