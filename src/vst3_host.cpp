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
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
using namespace Steinberg; using namespace Steinberg::Vst;
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

// What the plugin calls back while its window is open: which parameter the user touches, and "please resize my window".
class EditorHandler : public IComponentHandler, public IPlugFrame {
public:
    std::atomic<bool> learning{false};
    std::mutex touchMutex;
    std::deque<int> touches;
    std::function<int(ParamID)> indexOf;
    std::function<void(int, double)> onLiveEdit;   // every editor knob turn with its normalized value
    std::function<tresult(IPlugView*, ViewRect*)> resize;
    void clearTouches() { std::lock_guard<std::mutex> lock(touchMutex); touches.clear(); }
    int popTouch() { std::lock_guard<std::mutex> lock(touchMutex); if (touches.empty()) return -1; int i = touches.front(); touches.pop_front(); return i; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue v) override {
        const int i = indexOf ? indexOf(id) : -1;
        if (learning && i >= 0) { std::lock_guard<std::mutex> lock(touchMutex); if (touches.size() < 256) touches.push_back(i); }
        if (i >= 0 && onLiveEdit) onLiveEdit(i, (double)v);   // the editor's value used to be discarded: live sync could never see a knob turn
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
    std::atomic<int> inBusCh_{2}, outBusCh_{2};   // channel count of the plugin's first audio input / output bus (queried at prepare)
    bool editorComponentActive_ = false;
    bool editorProcessing_ = false;   // SEPARATED ENGINES: the editor instance runs processIdle() ticks
    bool editorLogged_ = false;       // one-shot log: confirms the editor instance began processing (and setProcessing's return)
    double editorSetupSr_ = 0.0;
    int editorSetupBlock_ = 0;
    std::vector<std::vector<float>> inScratch_, outScratch_;   // de-interleaved process scratch for any channel count
    // SEPARATED ENGINES: what this instance heard and dispatched, captured for the EDITOR
    // instance's feed (EQ analyzers, synth meters). tapAudio_ / tapEvents_ are written on the
    // thread that runs process() (the rack's render thread) and drained by the UI thread in
    // takeEditorFeed(); heldNotes_ only lives on the process() thread.
    std::mutex tapMutex_;
    std::vector<float> tapAudio_;            // interleaved stereo (2 ch) of the last rendered block
    int tapFrames_ = 0;
    std::deque<MidiMsg> tapEvents_;          // dispatched MIDI since the last drain (capped)
    std::set<int> heldNotes_;                // channel * 128 + pitch of note-ons with no note-off yet
    // The controller's MIDI CC / bend / aftertouch -> parameter assignment, cached ONCE at load on
    // the UI thread (IMidiMapping is documented "[UI-thread & Connected]" - it must never be
    // queried inside process()). Index: channel * 141 + ctrlNumber; (ParamID)-1 = unmapped.
    std::vector<ParamID> ccMap_;
public:
    void setEditorSystemScaling(bool enabled) override { systemScaling_ = enabled; }
    void closeEditor() override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
        if (view_) { view_->setFrame(nullptr); if (viewAttached_) view_->removed(); viewAttached_ = false; view_ = nullptr; }
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        lastScale_ = 0.f; lastWidth_ = lastHeight_ = -1;
        vstLog("editor: closed (reattach)");
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
        if (ctrl_) ctrl_->setComponentHandler(nullptr);
    }
    bool load(const std::string& path, const std::string& classId, std::string& err) override {
#ifdef _WIN32
        EditorDpiContext dpi(systemScaling_);
#endif
        ensureHostContext();
        module_ = VST3::Hosting::Module::create(path, err); if (!module_) { vstLog("load failed " + path + ": " + err); return false; }
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
        if (!provider_->initialize()) { err = "the plugin did not start"; return false; }
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
            handler_.indexOf = [this](ParamID id) { for (const PluginParam& p : params_) if (p.id == id) return p.index; return -1; };
            // The editor's knob turns arrive here as performEdit calls: keep the parameter cache
            // fresh so the UI live sync (syncLiveParameters) sees every editor edit next frame.
            handler_.onLiveEdit = [this](int i, double v) {
                if (i < 0 || i >= (int)params_.size() || params_[(size_t)i].readOnly) return;
                v = std::min(1.0, std::max(0.0, v));
                params_[(size_t)i].value = v;
                // Forward EVERY editor edit to the app: the running engine applies any parameter
                // on the same block (not just the 8 picked ones). Deduped per parameter.
                if (lastLive_.size() != params_.size()) lastLive_.assign(params_.size(), -2.0);
                if (liveHook_ && std::fabs(v - lastLive_[(size_t)i]) > 1e-6) {
                    lastLive_[(size_t)i] = v;
                    liveHook_(i, v);
                }
            };
            ctrl_->setComponentHandler(&handler_);
            // Newly initialized split controllers need the processor's initial state.
            Steinberg::MemoryStream state;
            const tresult read = comp_->getState(&state);
            if (read == kResultTrue) {
                state.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                const tresult sync = ctrl_->setComponentState(&state);
                vstLog("controller: component state sync result=" + std::to_string((int)sync));
            } else vstLog("controller: component state unavailable result=" + std::to_string((int)read));
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
                            if (mm->getMidiControllerAssignment(0, (int16)chn, (CtrlNumber)cn, pid) == kResultTrue) {
                                ccMap_[(size_t)chn * 141 + (size_t)cn] = pid;
                                ++mapped;
                            }
                        }
                    vstLog("controller: IMidiMapping " + std::to_string(mapped) + " assignment(s) cached");
                } else vstLog("controller: IMidiMapping absent - CC / bend do not drive parameters");
            }
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
    std::string classId() const override { return classId_; }
    void prepareEditor(double sr, int maxBlock) override {
        if (!proc_ || !comp_ || sr <= 0.0 || maxBlock <= 0) return;
        if (editorComponentActive_ && std::fabs(editorSetupSr_ - sr) < 0.01 && editorSetupBlock_ == maxBlock) return;
        // The UI instance is separate from the real-time audio instance. Configure and activate
        // its component only after the host engine has a real format; processIdle() (vstui::
        // queueEditorTasks, on the housekeeping worker) runs small silent blocks on it so sampler UIs stay alive, but it is
        // never connected to the audio callback.
        if (editorProcessing_) { proc_->setProcessing(false); editorProcessing_ = false; }
        editorLogged_ = false;   // a re-prepare (sample-rate / block change, reactivation) gets a fresh confirmation log
        if (editorComponentActive_) comp_->setActive(false);
        ProcessSetup setup{kRealtime, kSample32, maxBlock, sr};
        const tresult setupResult = proc_->setupProcessing(setup);
        if (setupResult != kResultOk) {
            vstLog("editor: setupProcessing failed, result=" + std::to_string((int)setupResult));
            editorComponentActive_ = false;
            return;
        }
        const tresult activeResult = comp_->setActive(true);
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
    // SEPARATED ENGINES: UI-thread idle tick for the EDITOR instance. Kontakt (and other
    // sampler-style plugins) only finish their loading screen and move their meters when their
    // component is processed. Runs a small block at ~12 Hz (vstui::queueEditorTasks); no engine
    // connection: the engine instance on the audio path is a different object and is unaffected.
    // With a feed, the block carries the ENGINE instance's real input audio and dispatched MIDI,
    // so EQ analyzers draw the actual signal and synth meters follow the notes that sound.
    void processIdle(int frames, const EditorFeed* feed) override {
        if (!proc_ || !comp_ || !editorComponentActive_ || frames <= 0) return;
        // VST3: IAudioProcessor::setProcessing is OPTIONAL. Many plugins (and VST2 adapters)
        // return kResultFalse from setProcessing(true) - the spec says the host must then just
        // call process() anyway. The engine path already does this; the editor instance must
        // too, or every EQ analyzer / scope / meter in a plugin window stays dead while sound
        // plays fine. Call setProcessing once (polite), then ALWAYS process.
        if (!editorProcessing_) {
            editorProcessing_ = true;
            const tresult sp = proc_->setProcessing(true);
            if (!editorLogged_) {
                editorLogged_ = true;
                vstLog("editor instance: processing started (setProcessing returned " + std::to_string((int)sp) + "; " + std::to_string(inBusCh_.load(std::memory_order_relaxed)) + " input channel(s))");
            }
        }
        if (editorSetupBlock_ > 0 && frames > editorSetupBlock_) frames = editorSetupBlock_;   // stay within the prepared block
        // The feed's engine audio replaces the silence. An instrument has no input bus: its GUI
        // lives on the dispatched notes (below).
        int nIn = instrument_ ? 0 : std::min(2, inBusCh_.load(std::memory_order_relaxed));
        const bool haveAudio = feed && feed->frames > 0 && (int)feed->audio.size() >= feed->frames * 2 && !instrument_;
        if (haveAudio && feed->frames < frames) frames = feed->frames;
        const int nOut = std::min(2, outBusCh_.load(std::memory_order_relaxed));
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
        ProcessData d{}; d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = frames;
        d.numInputs = nIn > 0 ? 1 : 0; d.inputs = nIn > 0 ? &ib : nullptr; d.numOutputs = 1; d.outputs = &ob;
        d.inputEvents = feed && !feed->events.empty() ? &events_ : nullptr;
        d.inputParameterChanges = &changes_;
        proc_->process(d);
    }
    // The UI drains this instance's engine snapshot for the editor feed. Copies everything out
    // under a short lock - the render thread must never wait on the UI.
    bool takeEditorFeed(EditorFeed& out) override {
        std::lock_guard<std::mutex> lock(tapMutex_);
        out.audio = tapAudio_;
        out.frames = tapFrames_;
        out.events.assign(tapEvents_.begin(), tapEvents_.end());
        tapEvents_.clear();
        return out.frames > 0 || !out.events.empty();
    }
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
        audioBuffers_.prepare(maxBlock);
        ProcessSetup s{kRealtime, kSample32, maxBlock, sr}; proc_->setupProcessing(s);
        comp_->setActive(true); proc_->setProcessing(true); maxBlock_ = maxBlock;
        BusInfo bi{};   // the real channel counts: the process buffers must match the plugin's buses
        if (comp_->getBusInfo(kAudio, kInput, 0, bi) == kResultOk && bi.channelCount > 0) inBusCh_.store((int)bi.channelCount, std::memory_order_relaxed); else inBusCh_.store(2, std::memory_order_relaxed);
        if (comp_->getBusInfo(kAudio, kOutput, 0, bi) == kResultOk && bi.channelCount > 0) outBusCh_.store((int)bi.channelCount, std::memory_order_relaxed); else outBusCh_.store(2, std::memory_order_relaxed);
        vstLog("audio buses: in " + std::to_string(inBusCh_.load()) + " ch, out " + std::to_string(outBusCh_.load()) + " ch"); }
    void process(const float* in, float* out, int frames, int ch, Endpoint& ep) override {
        // 1. MIDI events from the ring (cawsm, MIDI clips, device). CC / bend / aftertouch are
        //    mapped too now (see mapMidi): the transport stop's all-notes-off MUST reach the
        //    plugin, and a mapped CC drives its parameter live. The raw messages are also
        //    captured for the editor feed; the audio thread never waits on the UI for that.
        events_.clear(); changes_.clearQueue();
        MidiMsg m; int guard = 0;
        while (ep.midiIn.pop(m) && ++guard < 512) {
            { std::unique_lock<std::mutex> lock(tapMutex_, std::try_to_lock); if (lock.owns_lock() && tapEvents_.size() < 256) tapEvents_.push_back(m); }
            mapMidi(m, events_, changes_, 0);
        }
        // 2. parameter changes from the ring (cawsp, automation lanes)
        ParamMsg pm; int32 qi;
        // Initial editor values (restored before the graph went live) reach the processor across the
        // next blocks, a bounded number per block: a burst of thousands of changes in one block makes
        // some plugins stop processing (Arturia ARP 2600 goes silent). Automation below is applied
        // after the initials of the same block, so the current lane always wins.
        int sentInitial = 0;
        for (auto it = initialParameters_.begin(); it != initialParameters_.end(); ) {
            if (sentInitial >= 128) break;
            if (it->first < 0 || it->first >= (int)params_.size()) { it = initialParameters_.erase(it); continue; }
            auto* q = changes_.addParameterData(params_[it->first].id, qi); int32 pi;
            if (q) q->addPoint(0, it->second, pi);
            it = initialParameters_.erase(it);
            ++sentInitial;
        }
        while (ep.paramIn.pop(pm) && pm.paramIndex >= 0 && pm.paramIndex < (int)params_.size()) {
            auto* q = changes_.addParameterData(params_[pm.paramIndex].id, qi); int32 pi;
            if (q) q->addPoint(0, pm.value, pi); params_[pm.paramIndex].value = pm.value;
        }
        // 3. audio: de-interleave into per-channel scratch, hand the plugin its real bus sizes, interleave back
        const int nIn = in ? std::min(ch, inBusCh_.load(std::memory_order_relaxed)) : 0;
        const int nOut = std::min(ch, outBusCh_.load(std::memory_order_relaxed));
        if ((int)inScratch_.size() < nIn) inScratch_.resize((size_t)nIn);
        if ((int)outScratch_.size() < nOut) outScratch_.resize((size_t)nOut);
        for (int c = 0; c < nIn; ++c) {
            if ((int)inScratch_[(size_t)c].size() < frames) inScratch_[(size_t)c].resize((size_t)frames);
            for (int i = 0; i < frames; ++i) inScratch_[(size_t)c][(size_t)i] = in[(size_t)i * ch + c];
        }
        for (int c = 0; c < nOut; ++c) {
            if ((int)outScratch_[(size_t)c].size() < frames) outScratch_[(size_t)c].resize((size_t)frames);
            std::fill_n(outScratch_[(size_t)c].begin(), (size_t)frames, 0.f);
        }
        float* ins[8] = {}; float* outs[8] = {};
        for (int c = 0; c < nIn; ++c) ins[c] = inScratch_[(size_t)c].data();
        for (int c = 0; c < nOut; ++c) outs[c] = outScratch_[(size_t)c].data();
        AudioBusBuffers ib{}, ob{}; ib.numChannels = nIn; ib.channelBuffers32 = ins; ob.numChannels = nOut; ob.channelBuffers32 = outs;
        ProcessData d{}; d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = frames;
        d.numInputs = nIn > 0 ? 1 : 0; d.inputs = nIn > 0 ? &ib : nullptr; d.numOutputs = 1; d.outputs = &ob;
        d.inputEvents = &events_; d.inputParameterChanges = &changes_;
        proc_->process(d);
        if (out) for (int c = 0; c < ch; ++c) for (int i = 0; i < frames; ++i)
            out[(size_t)i * ch + c] = c < nOut ? outScratch_[(size_t)c][(size_t)i] : 0.f;   // channels the plugin does not have stay silent
        // SEPARATED ENGINES: capture the first stereo pair of this block for the EDITOR
        // instance's feed (EQ analyzers, scopes). Instruments capture silence - their GUI
        // meters move on the dispatched notes, not on input audio. Never waits on the UI.
        {
            std::unique_lock<std::mutex> lock(tapMutex_, std::try_to_lock);
            if (lock.owns_lock()) {
                if (tapAudio_.size() != (size_t)frames * 2) tapAudio_.assign((size_t)frames * 2, 0.f);
                const int ncap = in ? std::min(2, nIn) : 0;
                for (int i = 0; i < frames; ++i)
                    for (int c = 0; c < 2; ++c)
                        tapAudio_[(size_t)i * 2 + (size_t)c] = c < ncap ? in[(size_t)i * ch + c] : 0.f;
                tapFrames_ = frames;
            }
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
            // Owned by CAW's main window: an owned window always stays above its owner, so clicking
            // any CAW surface can no longer bury the plugin window behind the maximized main window
            // (it used to look "closed"). It also minimizes/restores together with CAW. The main
            // window is found by its class ("CawWindow", registered in main.cpp) so one plugin editor
            // never becomes the owner of another (closing an owner destroys its owned windows).
            const HWND owner = FindWindowA("CawWindow", nullptr) ? FindWindowA("CawWindow", nullptr) : GetActiveWindow();
            hwnd_ = CreateWindowExA(0, "CawVstEditor", title.c_str(), style, 100, 100, 400, 300, owner, nullptr, GetModuleHandle(nullptr), nullptr);
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
        err = "the plugin window is only available on Windows for now"; return false;
#endif
    }
    // SEPARATED ENGINES: the full component state (Kontakt's loaded .nki, an FX preset, GUI-only
    // state - binary, NOT expressible as parameters). Captured from the EDITOR instance and
    // applied to the ENGINE instance (at engine creation via VstJob::stateBlob, or between blocks
    // via VstRack::pushState). Base64 keeps the blob text-safe on the way through the app.
    std::string getState() const override {
        if (!comp_) return {};
        try {
            Steinberg::MemoryStream state;
            if (comp_->getState(&state) != kResultTrue) return {};
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
            const tresult applied = comp_->setState(&stream);
            if (applied != kResultTrue) { vstLog("state: component refused the state, result=" + std::to_string((int)applied)); return; }
            // The controller follows the patch too, so parameter reads / the GUI reflect it.
            if (ctrl_) {
                stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                ctrl_->setComponentState(&stream);
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
    std::vector<PluginParam> params_; std::map<int, double> initialParameters_;
    std::function<void(int, double)> liveHook_;   // app callback for every editor edit (all parameters)
    std::vector<double> lastLive_;                // last editor value forwarded per parameter (dedupe)
    bool instrument_ = false; std::string classId_; int maxBlock_ = 512;
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
                std::string err; auto mod = VST3::Hosting::Module::create(path, err);
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
    const char* home = std::getenv(_WIN32 ? "USERPROFILE" : "HOME");
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
