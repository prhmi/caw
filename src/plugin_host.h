#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "caw_bus.h"

namespace caw {
struct PluginParam { int index = 0; unsigned id = 0; std::string title, units; double def = 0, value = 0; int steps = 0; bool readOnly = false; };
struct MidiMsg;   // caw_bus.h

// SEPARATED ENGINES: one block of what the ENGINE instance just rendered - its input audio and
// the MIDI it dispatched - drained by the editor tick (vstui::startEditorWorker) and replayed on
// the EDITOR instance (processIdle) so plugin GUIs show live content: an EQ gets real audio for
// its analyzer waves, a synth gets the notes for its meters. The engine streams into a ring and
// the tick drains it at the rate the audio runs, so the GUI moves at realtime speed.
struct EditorFeed {
    std::vector<float> audio;        // interleaved stereo of the block (empty = silence)
    int frames = 0;                  // frames in audio (audio.size() == frames * 2)
    std::vector<MidiMsg> events;     // the MIDI messages dispatched on the captured blocks
};

// Format-neutral. VST3 now; CLAP / LV2 / AU can implement the same thing later.
struct IPlugin {
    virtual ~IPlugin() = default;
    // Set before load. Windows scaling uses DPI-unaware coordinates for legacy editors.
    virtual void setEditorSystemScaling(bool enabled) = 0;
    // One .vst3 file can contain SEVERAL plugin classes (Arturia bundles, Kilohearts bundles ...).
    // classId selects the class (canonical 32-hex CID); empty = the first audio class, as before.
    virtual bool load(const std::string& path, const std::string& classId, std::string& err) = 0;
    virtual std::string classId() const = 0;               // the class the plugin actually loaded
    virtual bool isInstrument() const = 0;
    virtual void prepare(double sr, int maxBlock) = 0;
    // The channel count the host wants on the main buses (the track's channel format). Set BEFORE
    // prepare(): prepare asks the plugin for that layout (VST3 setBusArrangements). 0 = keep its own.
    virtual void setWantedChannels(int n) { (void)n; }
    // The plugin's reported processing latency in samples (0 for most plugins; oversampled / look-ahead
    // FX report their internal delay). The host uses it to align the raw source with the effect in mix mode.
    virtual int latencySamples() const { return 0; }
    // Configure/activate the editor-side component when the host audio engine becomes ready.
    // This is deliberately separate from prepare(): the editor instance must not start audio processing.
    virtual void prepareEditor(double sr, int maxBlock) { (void)sr; (void)maxBlock; }
    // ONE INSTANCE PER PLUGIN: true while the live engine renders THIS object. The window's own
    // idle processing (prepareEditor / processIdle / the feed) is then skipped - the window already
    // shows the playing plugin, and process() must never run on two threads at once.
    std::atomic<int> engineOwnedCount{0};   // how many live rack Insts wrap this object (a graph swap briefly has two)
    std::atomic<bool> engineOwned{false};
    // in = bus audio (null for instruments), out = result. MIDI and param events are pulled from ep.
    virtual void process(const float* in, float* out, int frames, int channels, Endpoint& ep) = 0;
    // The plugin's first audio OUTPUT bus channel count (queried from the VST3 bus info).
    // CAW is the virtual soundcard: a 4-channel instrument plays 4 meters and folds into the
    // master with the same k mod N rule every engine follows.
    virtual int outputChannels() const { return 2; }
    // The main audio INPUT bus width (VSTe): a chain stage folds the previous stage's output
    // into this many lines (channel c plays on input c mod M - the virtual-soundcard rule).
    virtual int inputChannels() const { return 2; }
    // SIDECHAIN: interleaved stereo (frames * 2) for the plugin's auxiliary (sidechain) input bus, valid for the
    // NEXT process() call only (nullptr = silence). sidechainChannels() is 0 when the plugin has no such bus.
    virtual void setSidechain(const float* interleavedStereo) { (void)interleavedStereo; }
    virtual int sidechainChannels() const { return 0; }
    virtual const std::vector<PluginParam>& params() const = 0;
    virtual void setParam(int index, double normalized) = 0;
    virtual double getParam(int index) const = 0;
    // The plugin's own window (Windows: HWND, macOS: NSView, Linux: X11). Used to look at the plugin and to "learn" a parameter: turn a knob, takeTouchedParam() says which one.
    virtual bool showEditor(const std::string& title, std::string& err) = 0;
    // Close the plugin's own window; showEditor() opens it again. The plugin keeps its settings
    // (used to reattach an editor after a DPI mode change, or when its window drew wrong).
    virtual void closeEditor() {}
    // macOS / Linux: service the editor window (its close button, a user resize). Called every UI
    // frame on the UI thread. Windows windows get their messages through the message loop instead.
    virtual void pumpEditor() {}
    virtual bool editorVisible() const = 0;
    virtual void setLearnEnabled(bool enabled) = 0;
    virtual int takeTouchedParam() = 0;                    // plugin parameter index touched in the editor since the last call, or -1
    // Live editor edits: the hook is called with (parameter index, normalized value) EVERY time
    // the plugin's own window changes a parameter. This is how ANY editor knob reaches the
    // running engine in real time (not just the 8 picked parameters).
    virtual void setLiveEditHook(std::function<void(int, double)> hook) { (void)hook; }
    // SEPARATED ENGINES: idle tick for the EDITOR instance (not the audio one). Kontakt
    // and other sampler-style plugins only finish their loading screen and move their meters when
    // their component is processed. Called ~60 Hz from the editor tick (vstui::startEditorWorker);
    // the engine instance on the audio path is untouched. `feed` (optional) carries the engine's
    // next block - real input audio for an EQ's analyzer, the dispatched notes for a synth's
    // meters - so the plugin window shows what the engine actually does. Default no-op.
    virtual void processIdle(int frames, const EditorFeed* feed = nullptr) { (void)frames; (void)feed; }
    // The engine-side snapshot for the editor feed, drained by the editor's realtime tick. One
    // call returns at most one prepared block (the ring only ever holds what the engine
    // produced, so draining at 60 Hz replays the engine's audio at exactly realtime speed).
    // Returns false when nothing was captured yet. Default: no feed.
    virtual bool takeEditorFeed(EditorFeed& out) { (void)out; return false; }
    // Editor interaction state. A real DAW captures the full plugin state when the USER changed
    // something, not on a timer: polling getState() on a kHs-class editor stalls its GUI and
    // re-applies a blob that embeds volatile data forever. The editor marks itself dirty on
    // every knob / preset message (setParamNormalized, restartComponent) and reports how long
    // ago the user last touched it and whether its window is being moved / resized.
    virtual bool stateDirty() const { return false; }
    virtual void markStateCaptured() {}
    virtual double msSinceLastTouch() const { return 1e9; }
    virtual bool windowBusy() const { return false; }
    virtual std::string getState() const = 0;              // for saving into the project
    virtual void setState(const std::string& blob) = 0;
    // Every parameter's current normalized value, in parameter-index order. Cheap getters only:
    // this is how the host detects an editor knob turn for plugins whose GUI does not report
    // edits through IComponentHandler::performEdit (kilohearts' Slice EQ does not) - the value
    // then still reaches the engine instance, exactly like a single-instance host. Never
    // serializes state. Returns the number of values stored.
    virtual int readParameterValues(std::vector<double>& out) const { out.clear(); return 0; }
};
std::unique_ptr<IPlugin> makeVst3Plugin();
std::vector<std::string> scanVst3(const std::vector<std::string>& extraDirs);   // finds *.vst3
// One plugin CLASS (read from the factory without creating the plugin). A bundle file can appear
// several times, once per class, with different classId / name / kind.
struct PluginInfo { std::string path, name, classId; int kind = 0; };   // kind: 1 instrument, 2 effect, 0 unknown
// Extra folders the user added in Settings > Programs > VST3 (persisted per user). The standard
// VST3 system folders are always scanned in addition to these.
std::vector<std::string> vst3ExtraDirs();
void setVst3ExtraDirs(const std::vector<std::string>& dirs);
// Publish discovered paths first, then classifications as they arrive.
std::vector<PluginInfo> scanVst3Info(const std::vector<std::string>& extraDirs,
    const std::function<void(const std::vector<PluginInfo>&)>& publish = {});
void vstLog(const std::string& line);                          // caw_vst_log.txt: what the host was doing last (for crash reports)
}
