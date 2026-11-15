#pragma once
// The window of a VSTi / VSTe track (the track header has a Show button that opens it again):
//   choose the plugin by name (search) -> "Show plugin window" (its real interface) -> fill P1 .. P8: Learn (turn a knob in the plugin) or tick in the list.
// The clips of the track then get one automation lane per picked parameter and code (CAW code, see vst_lang.h) that drives them.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <vector>
#include "imgui.h"
#include "arrow_widgets.h"
#include "model.h"
#ifdef CAW_VST3
#include "plugin_host.h"
#include "vst_lang.h"
#ifdef _WIN32
#include <objbase.h>
#include <ole2.h>
#endif
#endif

namespace vstui {
struct Slot {
    bool open = false, autoTried = false;
    bool systemScaling = true, loadedSystemScaling = true;
    int learn = -1;                       // slot (0..7) that waits for a knob turn in the plugin window, -1 = none
    std::string err, pluginFilter, paramFilter, loadedPath;
#ifdef CAW_VST3
    std::unique_ptr<caw::IPlugin> plugin;
#endif
};
inline std::map<int, Slot>& slots() { static std::map<int, Slot> s; return s; }
inline void openWindow(int trackId) { slots()[trackId].open = true; }

inline std::string lower(const std::string& s) { std::string o; for (char c : s) o += (char)std::tolower((unsigned char)c); return o; }

#ifdef CAW_VST3
// Discovered paths appear immediately; classification continues in the background.
struct Catalog { std::vector<caw::PluginInfo> list; std::mutex mutex; std::atomic<bool> done{false}, started{false}; };
// The detached scanner can outlive UI teardown, so its catalog has process lifetime.
inline Catalog& catalog() { static Catalog* c = new Catalog; return *c; }
inline std::vector<caw::PluginInfo> catalogSnapshot() {
    Catalog& c = catalog(); std::lock_guard<std::mutex> lock(c.mutex); return c.list;
}
inline void startScan(bool rescan = false) {
    Catalog& c = catalog();
    if (rescan) { if (!c.done) return; c.started = false; c.done = false; }
    if (c.started.exchange(true)) return;
    std::thread([&c]() {
#ifdef _WIN32
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        auto publish = [&c](const std::vector<caw::PluginInfo>& list) {
            std::lock_guard<std::mutex> lock(c.mutex); c.list = list;
        };
        try { publish(caw::scanVst3Info({}, publish)); }
        catch (const std::exception& e) { caw::vstLog(std::string("scan error: ") + e.what()); }
        catch (...) { caw::vstLog("scan error: unknown exception"); }
#ifdef _WIN32
        if (SUCCEEDED(com)) CoUninitialize();
#endif
        c.done = true;
    }).detach();
}
inline std::string niceName(const std::string& path) {
    for (const auto& i : catalogSnapshot()) if (i.path == path) return i.name;
    return std::filesystem::path(path).stem().string();
}

inline void captureTrackParameters(Track& t, Slot& s) {
    if (!s.plugin || s.loadedPath != t.vstPath) return;
    std::map<uint32_t, double> snapshot;
    for (const caw::PluginParam& p : s.plugin->params()) {
        if (p.readOnly) continue;
        const double v = s.plugin->getParam(p.index);
        if (std::isfinite(v) && v >= 0. && v <= 1.) snapshot[p.id] = v;
    }
    // Do not erase a previously captured snapshot if a plugin reports no parameters.
    if (!snapshot.empty()) t.vstParameterState = std::move(snapshot);
}

inline bool loadPlugin(Slot& s, Track& t, const std::string& path, double sr) {
    (void)sr; s.err.clear();
    if (path == t.vstPath) captureTrackParameters(t, s);
#ifdef _WIN32
    static bool com = false; if (!com) { OleInitialize(nullptr); com = true; }   // many plugins expect COM on the thread that loads them
#endif
    auto p = caw::makeVst3Plugin(); std::string err;
    p->setEditorSystemScaling(s.systemScaling);
    // This copy only shows the plugin; the sound is made by a separate engine instance.
    bool ok = false;
    try { ok = p->load(path, err); } catch (...) { err = "the plugin failed while loading"; }
    if (ok) {
        if (path != t.vstPath) t.vstParameterState.clear();
        for (const caw::PluginParam& param : p->params()) {
            const auto saved = t.vstParameterState.find(param.id);
            if (!param.readOnly && saved != t.vstParameterState.end()) p->setParam(param.index, saved->second);
        }
        s.plugin = std::move(p); t.vstPath = path; s.loadedPath = path;
        s.loadedSystemScaling = s.systemScaling; s.learn = -1;
    }
    else { s.err = err.empty() ? "could not load that plugin" : err; }
    return ok;
}
// the name of the automation lane of a picked parameter: letters, digits and _ only, never twice on one track
inline std::string laneName(const Track& t, const std::string& title, size_t slot) {
    std::string n;
    for (char c : title) n += (std::isalnum((unsigned char)c) ? c : '_');
    while (!n.empty() && n.front() == '_') n.erase(n.begin());
    while (!n.empty() && n.back() == '_') n.pop_back();
    if (n.empty()) n = "P" + std::to_string(slot + 1);
    std::string out = n; int k = 2;
    auto taken = [&](const std::string& x) { for (size_t i = 0; i < t.vstPickName.size(); ++i) if (i != slot && t.vstPickName[i] == x) return true; return false; };
    while (taken(out)) out = n + std::to_string(k++);
    return out;
}
inline void assign(Track& t, caw::IPlugin& plugin, size_t slot, const caw::PluginParam& p) {
    while (t.vstPickName.size() < t.vstPick.size()) t.vstPickName.push_back("P" + std::to_string(t.vstPickName.size() + 1));
    while (t.vstInit.size() < t.vstPick.size()) t.vstInit.push_back(0.5);
    const double init = plugin.getParam(p.index);
    if (slot >= t.vstPick.size()) { slot = t.vstPick.size(); t.vstPick.push_back(p.index); t.vstInit.push_back(init); t.vstPickName.push_back(""); }
    else { t.vstPick[slot] = p.index; t.vstInit[slot] = init; }
    t.vstPickName[slot] = laneName(t, p.title, slot);
    CawParam cp{t.vstPickName[slot], 0.0, 1.0, 0.001, init};
    t.automationRanges[cp.chn] = cp;                     // the lane starts at the plugin's current value
}
inline void clearSlot(Track& t, size_t k) {
    if (k < t.vstPick.size()) t.vstPick.erase(t.vstPick.begin() + (long)k);
    if (k < t.vstInit.size()) t.vstInit.erase(t.vstInit.begin() + (long)k);
    if (k < t.vstPickName.size()) t.vstPickName.erase(t.vstPickName.begin() + (long)k);
}

inline void processLearn(Track& t, Slot& s) {
    if (!s.plugin || s.learn < 0) return;
    const auto& params = s.plugin->params();
    int touched;
    while ((touched = s.plugin->takeTouchedParam()) >= 0) {
        auto existing = std::find(t.vstPick.begin(), t.vstPick.end(), touched);
        if (existing != t.vstPick.end() && (size_t)(existing - t.vstPick.begin()) != (size_t)s.learn) continue;
        if (touched < (int)params.size() && !params[(size_t)touched].readOnly) {
            const int next = s.learn + 1;
            assign(t, *s.plugin, (size_t)s.learn, params[(size_t)touched]);
            s.learn = next < 8 ? next : -1;
            // Clear queued edits and reset the polling baseline between slots.
            s.plugin->setLearnEnabled(s.learn >= 0);
            break;
        }
    }
}

// A compact mixer-strip layout: fixed-size slot numbers, flexible names, visible actions.
inline void drawMappings(Project& proj, Track& t, Slot& s, float scale) {
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "AUTOMATION SLOTS");
    ImGui::SameLine(); ImGui::TextDisabled("%d / 8", (int)t.vstPick.size());
    if (ImGui::BeginTable("##slots", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34.f * scale);
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 42.f * scale);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 132.f * scale);
        const auto& params = s.plugin->params();
        for (size_t k = 0; k < 8; ++k) {
            ImGui::PushID((int)k); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(ImVec4(1.f, .72f, .30f, 1.f), "P%d", (int)k + 1);
            const bool filled = k < t.vstPick.size();
            std::string name = filled ? "(parameter unavailable)" : "(empty)";
            if (filled) for (const auto& p : params) if (p.index == t.vstPick[k]) { name = p.title; break; }
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
            ImGui::TableSetColumnIndex(2);
            if (filled && t.vstPick[k] >= 0 && (size_t)t.vstPick[k] < params.size()) {
                ImGui::TextDisabled("%.2f", s.plugin->getParam(t.vstPick[k]));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Current editor value (0 to 1)");
            }
            ImGui::TableSetColumnIndex(3);
            const bool learning = s.learn == (int)k, unavailable = k > t.vstPick.size();
            if (unavailable) ImGui::BeginDisabled();
            if (learning) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.80f, .34f, .08f, 1.f));
            if (ImGui::SmallButton(learning ? "Listening..." : "Learn")) {
                s.learn = learning ? -1 : (int)k; s.plugin->setLearnEnabled(s.learn >= 0);
                if (s.learn >= 0) {
                    // Clicking CAW activates its main HWND, which can cover a standalone editor.
                    // Bring the same editor back to the foreground without reloading it.
                    std::string err;
                    if (!s.plugin->showEditor(niceName(t.vstPath) + " (" + t.name + ")", err)) s.err = err;
                    else s.err.clear();
                }
            }
            if (learning) ImGui::PopStyleColor();
            if (unavailable) ImGui::EndDisabled();
            if (filled) { ImGui::SameLine(); if (ImGui::SmallButton("Clear")) {
                clearSlot(t, k); s.learn = -1; s.plugin->setLearnEnabled(false);
            }}
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "AUDIO OUTPUT");
    std::string cur = t.vstOut.empty() ? "Master" : "Bus: " + t.vstOut;
    ImGui::SetNextItemWidth(-1);
    if (caw_ui::beginCombo("##audio_out", cur.c_str())) {
        if (ImGui::Selectable("Master", t.vstOut.empty())) t.vstOut.clear();
        for (const Track& o : proj.tracks) {
            if (o.kind != 4 || o.id == t.id) continue;
            const std::string bn = vstlang::busName(o);
            if (ImGui::Selectable(("VSTe: " + o.name + "##out" + std::to_string(o.id)).c_str(), t.vstOut == bn)) t.vstOut = bn;
        }
        if (ImGui::Selectable("Csound bus (type a name)...", false)) t.vstOut = t.vstOut.empty() || t.vstOut == "mix" ? "mix" : t.vstOut;
        ImGui::EndCombo();
    }
    if (!t.vstOut.empty()) {
        char ob[64]; std::snprintf(ob, sizeof ob, "%s", t.vstOut.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##outbus", "Bus name", ob, sizeof ob)) t.vstOut = vstlang::cleanBus(ob);
        ImGui::Checkbox("Also send to master", &t.vstOutMaster);
        ImGui::TextWrapped("Csound: aL, aR cawra \"%s\"", t.vstOut.c_str());
    }
#ifdef _WIN32
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "EDITOR SCALING");
    ImGui::Checkbox("Windows compatibility scaling", &s.systemScaling);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use Windows scaling if the editor is cropped or mouse clicks are offset.");
    if (s.systemScaling != s.loadedSystemScaling) {
        ImGui::TextWrapped("Reload the editor to apply. Unsaved plugin settings may reset.");
        if (ImGui::Button("Reload editor for DPI mode")) loadPlugin(s, t, t.vstPath, proj.cs.sr);
    }
#endif
}

inline void drawParameterBrowser(Track& t, Slot& s) {
    const auto& params = s.plugin->params();
    ImGui::TextColored(ImVec4(1.f, .66f, .20f, 1.f), "PARAMETER LIBRARY");
    char fb[128]; std::snprintf(fb, sizeof fb, "%s", s.paramFilter.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter_params", "Search parameters...", fb, sizeof fb)) s.paramFilter = fb;
    const std::string low = lower(s.paramFilter);
    std::vector<const caw::PluginParam*> matches;
    for (const auto& p : params) if (!p.readOnly && (low.empty() || lower(p.title).find(low) != std::string::npos)) matches.push_back(&p);
    ImGui::TextDisabled("%d shown", (int)matches.size());
    if (params.empty()) ImGui::TextWrapped("No parameters were reported by this plugin. Check the plugin log if its editor also fails.");
    else if (matches.empty()) ImGui::TextDisabled("No writable parameters match your search.");
    ImGui::BeginChild("##parameter_list", ImVec2(0, 0), true);
    ImGuiListClipper clipper; clipper.Begin((int)matches.size());
    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const auto& p = *matches[(size_t)row];
        auto it = std::find(t.vstPick.begin(), t.vstPick.end(), p.index);
        bool on = it != t.vstPick.end();
        const bool full = !on && t.vstPick.size() >= 8;
        if (full) ImGui::BeginDisabled();
        const std::string label = (on ? "P" + std::to_string((int)(it - t.vstPick.begin()) + 1) + "  " : std::string("     ")) + p.title + "##parameter" + std::to_string(p.index);
        if (ImGui::Checkbox(label.c_str(), &on)) {
            if (on) assign(t, *s.plugin, t.vstPick.size(), p);
            else clearSlot(t, (size_t)(it - t.vstPick.begin()));
            s.learn = -1; s.plugin->setLearnEnabled(false);
        }
        if (full) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.title.c_str());
    }
    ImGui::EndChild();
}
#endif

inline void captureParameters(Project& project) {
#ifdef CAW_VST3
    for (Track& track : project.tracks) {
        auto it = slots().find(track.id);
        if ((track.kind == 3 || track.kind == 4) && it != slots().end())
            captureTrackParameters(track, it->second);
    }
#else
    (void)project;
#endif
}

// Continue parameter learning while CAW's main window is minimized, even
// though its ImGui windows are not being laid out.
inline void serviceLearn(Project& project) {
#ifdef CAW_VST3
    for (Track& track : project.tracks) {
        if (track.kind != 3 && track.kind != 4) continue;
        auto it = slots().find(track.id);
        if (it != slots().end() && it->second.open && it->second.plugin)
            processLearn(track, it->second);
    }
#else
    (void)project;
#endif
}

inline void drawWindows(Project& proj) {
    for (Track& t : proj.tracks) {
        if (t.kind != 3 && t.kind != 4) continue;
        Slot& s = slots()[t.id];
        if (!s.open) continue;
        const bool inst = t.kind == 3;
        const float scale = ImGui::GetFontSize() / 17.f;
        ImGui::SetNextWindowSize(ImVec2(530.f * scale, 460.f * scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(500.f * scale, 320.f * scale), ImVec2(10000, 10000));
        const std::string title = std::string(inst ? "VSTi: " : "VSTe: ") + t.name + "##vst" + std::to_string(t.id);
        if (ImGui::Begin(title.c_str(), &s.open)) {
#ifdef CAW_VST3
            startScan();
            if (!s.autoTried && !t.vstPath.empty()) { s.autoTried = true; loadPlugin(s, t, t.vstPath, proj.cs.sr); }   // a saved project: load the plugin again
            // The top strip stays visible while the mapping and library panels scroll independently.
            const auto plugins = catalogSnapshot();
            const bool scanDone = catalog().done;
            const std::string current = t.vstPath.empty() ? std::string("Choose a plugin...") : niceName(t.vstPath);
            ImGui::SetNextItemWidth(-1);
            if (caw_ui::beginCombo("##plugin", current.c_str())) {
                char fb[128]; std::snprintf(fb, sizeof fb, "%s", s.pluginFilter.c_str());
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputTextWithHint("##pf", "search...", fb, sizeof fb)) s.pluginFilter = fb;
                const std::string low = lower(s.pluginFilter);
                if (!scanDone) ImGui::TextDisabled("Scanning plugin types... unknown types appear in both lists.");
                int visible = 0;
                for (const caw::PluginInfo& info : plugins) {
                    if (info.kind != 0 && info.kind != (inst ? 1 : 2)) continue;
                    const std::string nm = info.name, f = info.path;
                    if (!low.empty() && lower(nm).find(low) == std::string::npos) continue;
                    ++visible;
                    const std::string label = nm + (info.kind == 0 ? " (type unknown)" : "") + "##" + f;
                    if (ImGui::Selectable(label.c_str(), f == t.vstPath)) {
                        const bool changed = f != t.vstPath;
                        if (loadPlugin(s, t, f, proj.cs.sr) && changed) {
                            t.vstPick.clear(); t.vstInit.clear(); t.vstPickName.clear(); s.learn = -1;
                        }
                    }
                }
                if (scanDone && plugins.empty()) ImGui::TextDisabled("No .vst3 found in the standard VST3 folders.");
                else if (scanDone && visible == 0) ImGui::TextDisabled("No plugins match this type or search.");
                ImGui::EndCombo();
            }
            if (!s.plugin) ImGui::BeginDisabled();
            if (ImGui::Button(s.plugin && s.plugin->editorVisible() ? "Show plugin editor" : "Open plugin editor")) {
                std::string err; if (!s.plugin->showEditor(niceName(t.vstPath) + " (" + t.name + ")", err)) s.err = err; else s.err.clear();
            }
            if (!s.plugin) ImGui::EndDisabled();
            if (!scanDone) { ImGui::SameLine(); ImGui::TextDisabled("Scanning..."); }
            if (!s.err.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .45f, .40f, 1.f));
                ImGui::TextWrapped("%s", s.err.c_str()); ImGui::PopStyleColor();
            }
            if (!s.plugin) { ImGui::TextDisabled("Choose a plugin to start."); ImGui::End(); continue; }
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            // Learn keeps running regardless of which pane or tab is visible.
            processLearn(t, s);
            const float width = ImGui::GetContentRegionAvail().x;
            if (width >= 760.f * scale) {
                const float height = std::max(120.f * scale, ImGui::GetContentRegionAvail().y - 4.f * scale);
                if (ImGui::BeginTable("##workbench", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, height))) {
                    ImGui::TableSetupColumn("Mappings", ImGuiTableColumnFlags_WidthStretch, 1.05f);
                    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 1.f);
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    ImGui::BeginChild("##mappings_panel", ImVec2(0, height), true); drawMappings(proj, t, s, scale); ImGui::EndChild();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::BeginChild("##library_panel", ImVec2(0, height), true); drawParameterBrowser(t, s); ImGui::EndChild();
                    ImGui::EndTable();
                }
            } else if (ImGui::BeginTabBar("##plugin_tabs")) {
                if (ImGui::BeginTabItem("Mappings & routing")) {
                    ImGui::BeginChild("##mappings_compact", ImVec2(0, 0), true); drawMappings(proj, t, s, scale); ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Parameters")) {
                    ImGui::BeginChild("##library_compact", ImVec2(0, 0), true); drawParameterBrowser(t, s); ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
#else
            ImGui::TextWrapped("This build of CAW has no VST3 support (built with CAW_VST3=OFF).");
#endif
        }
        ImGui::End();
    }
}
}
