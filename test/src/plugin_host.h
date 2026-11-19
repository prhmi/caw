#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "caw_bus.h"

namespace caw {
struct PluginParam { int index = 0; unsigned id = 0; std::string title, units; double def = 0, value = 0; int steps = 0; bool readOnly = false; };

// Format-neutral. VST3 now; CLAP / LV2 / AU can implement the same thing later.
struct IPlugin {
    virtual ~IPlugin() = default;
    // Set before load. Windows scaling uses DPI-unaware coordinates for legacy editors.
    virtual void setEditorSystemScaling(bool enabled) = 0;
    virtual bool load(const std::string& path, std::string& err) = 0;
    virtual bool isInstrument() const = 0;
    virtual void prepare(double sr, int maxBlock) = 0;
    // in = bus audio (null for instruments), out = result. MIDI and param events are pulled from ep.
    virtual void process(const float* in, float* out, int frames, int channels, Endpoint& ep) = 0;
    virtual const std::vector<PluginParam>& params() const = 0;
    virtual void setParam(int index, double normalized) = 0;
    virtual double getParam(int index) const = 0;
    // The plugin's own window (Windows). Used to look at the plugin and to "learn" a parameter: turn a knob, takeTouchedParam() says which one.
    virtual bool showEditor(const std::string& title, std::string& err) = 0;
    virtual bool editorVisible() const = 0;
    virtual void setLearnEnabled(bool enabled) = 0;
    virtual int takeTouchedParam() = 0;                    // plugin parameter index touched in the editor since the last call, or -1
    // Live editor edits: the hook is called with (parameter index, normalized value) EVERY time
    // the plugin's own window changes a parameter. This is how ANY editor knob reaches the
    // running engine in real time (not just the 8 picked parameters).
    virtual void setLiveEditHook(std::function<void(int, double)> hook) { (void)hook; }
    virtual std::string getState() const = 0;              // for saving into the project
    virtual void setState(const std::string& blob) = 0;
};
std::unique_ptr<IPlugin> makeVst3Plugin();
std::vector<std::string> scanVst3(const std::vector<std::string>& extraDirs);   // finds *.vst3
// Name and kind of every plugin file (read from the plugin factory without creating the plugin).
struct PluginInfo { std::string path, name; int kind = 0; };   // kind: 1 instrument, 2 effect, 0 unknown
// Publish discovered paths first, then classifications as they arrive.
std::vector<PluginInfo> scanVst3Info(const std::vector<std::string>& extraDirs,
    const std::function<void(const std::vector<PluginInfo>&)>& publish = {});
void vstLog(const std::string& line);                          // caw_vst_log.txt: what the host was doing last (for crash reports)
}
