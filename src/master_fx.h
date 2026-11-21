#pragma once
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "plugin_host.h"

// MASTER FX: up to 8 VST3 effects processed IN ORDER on CAW's final device mix. The chain runs
// in every CAW audio callback (audio_output.h calls the app's master-chain hook after all engines
// have mixed), before the master gain. One plugin instance per slot: it is processed on the
// audio thread and can also show its editor window (most effect plugins support that; a slot
// that misbehaves can simply be switched off with its On checkbox).
namespace masterfx {

constexpr int kSlots = 8;

struct Slot {
    bool on = false, tried = false;      // tried = the plugin was loaded once (lazy load on first draw)
    std::string path, classId, name;     // name = what the row shows
    std::string state;                   // plugin patch (saved into the project)
    std::shared_ptr<caw::IPlugin> plugin;
    std::shared_ptr<caw::Endpoint> ep;
};

struct SlotView {
    bool on = false, tried = false, loaded = false, editorVisible = false;
    std::string path, classId, name;
};

class Rack {
public:
    static Rack& get() { static Rack r; return r; }

    // ---- audio thread: returns true when the chain actually processed the buffer
    bool render(float* out, int frames, int channels) noexcept {
        if (!out || frames <= 0 || channels <= 0 || channels > 16) return false;
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
        if (!lock.owns_lock()) return false;   // the UI thread holds the lock briefly: skip the block
        const size_t count = (size_t)frames * (size_t)channels;
        if (scratch_.size() < count) return false;   // prepared by the UI thread
        bool any = false;
        for (Slot& s : slots_) {
            if (!s.on || !s.plugin) continue;
            try {
                s.plugin->process(out, scratch_.data(), frames, channels, *s.ep);
            } catch (...) { continue; }
            std::memcpy(out, scratch_.data(), count * sizeof(float));
            any = true;
        }
        return any;
    }

    // ---- UI thread
    std::array<SlotView, kSlots> view() {
        std::lock_guard<std::mutex> lock(mu_);
        std::array<SlotView, kSlots> out;
        for (int i = 0; i < kSlots; ++i) {
            const Slot& s = slots_[(size_t)i];
            out[(size_t)i].on = s.on;
            out[(size_t)i].tried = s.tried;
            out[(size_t)i].loaded = s.plugin != nullptr;
            out[(size_t)i].editorVisible = s.plugin && s.plugin->editorVisible();
            out[(size_t)i].path = s.path;
            out[(size_t)i].classId = s.classId;
            out[(size_t)i].name = s.name;
        }
        return out;
    }

    // Loads (or reloads) slot i. Empty name = the slot is unused. Returns an error string ("" = ok).
    std::string load(int i, const std::string& path, const std::string& classId, const std::string& name,
                     double sampleRate, int maxBlock) {
        std::string err;
        auto p = caw::makeVst3Plugin();
        p->setEditorSystemScaling(true);
        try {
            if (!p->load(path, classId, err)) {
                std::lock_guard<std::mutex> lock(mu_);
                slots_[(size_t)i].plugin.reset();
                slots_[(size_t)i].ep.reset();
                return err.empty() ? "the plugin failed to load" : err;
            }
            p->prepare(sampleRate, std::max(64, maxBlock));
        } catch (...) { return "the plugin failed while loading"; }
        std::lock_guard<std::mutex> lock(mu_);
        Slot& s = slots_[(size_t)i];
        s.plugin = std::move(p);
        s.ep = std::make_shared<caw::Endpoint>();   // paramLast stays empty, like the VST rack's endpoints (vector<atomic> cannot resize)
        s.path = path;
        s.classId = s.plugin->classId().empty() ? classId : s.plugin->classId();
        s.name = name;
        s.tried = true;
        s.on = true;
        // restore the saved patch (state was kept across the load)
        if (!s.state.empty()) {
            try { s.plugin->setState(s.state); } catch (...) {}
        }
        return {};
    }

    void clear(int i) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot& s = slots_[(size_t)i];
        if (s.plugin) try { s.plugin->closeEditor(); } catch (...) {}
        s = Slot{};
    }

    void setOn(int i, bool on) {
        std::lock_guard<std::mutex> lock(mu_);
        slots_[(size_t)i].on = on;
    }

    bool showEditor(int i, const std::string& title, std::string& err) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot& s = slots_[(size_t)i];
        if (!s.plugin) { err = "no plugin in this slot"; return false; }
        try { return s.plugin->showEditor(title, err); } catch (...) { err = "the editor failed to open"; return false; }
    }

    void closeEditors() {
        std::lock_guard<std::mutex> lock(mu_);
        for (Slot& s : slots_)
            if (s.plugin) try { s.plugin->closeEditor(); } catch (...) {}
    }

    // ---- persistence (project file)
    std::string getState(int i) {
        std::lock_guard<std::mutex> lock(mu_);
        Slot& s = slots_[(size_t)i];
        if (!s.plugin) return s.state;
        try { s.state = s.plugin->getState(); } catch (...) {}
        return s.state;
    }
    void setState(int i, const std::string& blob) {
        std::lock_guard<std::mutex> lock(mu_);
        slots_[(size_t)i].state = blob;
        if (slots_[(size_t)i].plugin) {
            try { slots_[(size_t)i].plugin->setState(blob); } catch (...) {}
        }
    }
    // Raw slot data for saving (without forcing a state capture).
    struct SaveSlot { bool on = false; std::string path, classId, name, state; };
    std::array<SaveSlot, kSlots> save() {
        std::lock_guard<std::mutex> lock(mu_);
        std::array<SaveSlot, kSlots> out;
        for (int i = 0; i < kSlots; ++i) {
            const Slot& s = slots_[(size_t)i];
            out[(size_t)i].on = s.on;
            out[(size_t)i].path = s.path;
            out[(size_t)i].classId = s.classId;
            out[(size_t)i].name = s.name;
            out[(size_t)i].state = s.state;
        }
        return out;
    }
    void restore(const std::array<SaveSlot, kSlots>& in) {
        std::lock_guard<std::mutex> lock(mu_);
        for (int i = 0; i < kSlots; ++i) {
            slots_[(size_t)i].on = in[(size_t)i].on;
            slots_[(size_t)i].tried = false;
            slots_[(size_t)i].path = in[(size_t)i].path;
            slots_[(size_t)i].classId = in[(size_t)i].classId;
            slots_[(size_t)i].name = in[(size_t)i].name;
            slots_[(size_t)i].state = in[(size_t)i].state;
        }
    }
    void clearAll() {
        std::lock_guard<std::mutex> lock(mu_);
        for (Slot& s : slots_) {
            if (s.plugin) try { s.plugin->closeEditor(); } catch (...) {}
            s = Slot{};
        }
    }

private:
    Rack() { scratch_.resize(8192 * 16); }
    std::mutex mu_;
    std::array<Slot, kSlots> slots_;
    std::vector<float> scratch_;
};

inline Rack& rack() { return Rack::get(); }

}  // namespace masterfx
