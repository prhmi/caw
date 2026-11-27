#include "app_shared.h"
#include <mutex>
#include <ctime>
#include <vector>
#include "platform.h"

// src/ui_panels.cpp (phase 2: extracted from app.cpp; the shared state is app_state.h, the
//  internal declarations are app_internal.h - all included through app_shared.h.)

int scopeDiv() {
    const double period = (double)std::max(1, g_proj.cs.ksmps) / (double)std::max(1, g_proj.cs.sr);
    return std::max(1, (int)std::lround(g_scopeMs / 1000.0 / (kScopePoints * period)));
}
#include "app.h"
int scopeWindowMs() {
    return (int)std::lround(scopeDiv() * kScopePoints * (double)std::max(1, g_proj.cs.ksmps) / (double)std::max(1, g_proj.cs.sr) * 1000.0);
}
#include "app.h"
std::string clipLineMessage(const std::string& l);
#include "app.h"
void logCsoundLine(const std::string& raw);
#include "app.h"
void helpTip(const char* fmt, ...) {
    if (!g_helpOn || !fmt) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_helpNext = buf;
}
#include "app.h"
void logLine(const std::string& s);
#include "app.h"
void logLine(const std::string& s);
#include "app.h"
void logScriptError(const char* what, int clipId, const std::string& clipName, const vstscript::Program& prog) {
    if (prog.error.empty()) return;
    static std::map<int, std::string> lastLogged;
    auto it = lastLogged.find(clipId);
    if (it != lastLogged.end() && it->second == prog.error) return;
    lastLogged[clipId] = prog.error;
    logLine(std::string("[caw] CAW code error (") + what + " '" + clipName + "'): " + prog.error);
}
#include "app.h"
LogLine classifyLog(const std::string& text) {
    static const std::regex clipRe(R"re((?:instr|clip)\s+(\w+))re");
    LogLine l;
    l.text = text;
    std::string low = text;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::smatch m;
    if (std::regex_search(text, m, clipRe)) l.clip = m[1].str();
    const bool app = text.rfind("[caw]", 0) == 0;
    l.level = app ? kLogMessage : kLogDetail;
    if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos || low.find("invalid") != std::string::npos) l.level = kLogError;
    else if (!app && (low.find("couldn't create") != std::string::npos || low.find("not understood") != std::string::npos || low.find("no method for") != std::string::npos ||
                      low.find("used before defined") != std::string::npos || low.find("undefined variable") != std::string::npos || low.find("syntax") != std::string::npos)) l.level = kLogError;
    else if (low.find("warning") != std::string::npos || (!app && low.find("extra arguments") != std::string::npos)) l.level = kLogImportant;
    else if (app)
        for (const char* key : {"run ", "rendered", "render", "playback", "stopped", "saved", "opened",
                                "build", "vst", "sample engine", "input", "output", "silent", "silence", "not active", "problem"})
            if (low.find(key) != std::string::npos) { l.level = kLogImportant; break; }
    return l;
}
#include "app.h"
void pushLog(LogLine&& l) {
    g_log.push_back(std::move(l));
    if (g_log.size() > 5000) {
        g_log.erase(g_log.begin(), g_log.begin() + 1000);
        if (g_scErrorLogIndex >= 0) {
            if (g_scErrorLogIndex < 1000) g_scErrorLogIndex = -1;
            else g_scErrorLogIndex -= 1000;
        }
    }
    g_logDirty = true;
}
#include "app.h"
void fillLoc(LogLine& l, const std::string& clipName, int line, const std::string& markText) {
    const Clip* first = nullptr;
    std::vector<double> times;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips)
            if (c.name == clipName) { if (!first) first = &c; times.push_back(c.start); }
    if (!first) return;
    std::sort(times.begin(), times.end());
    l.clip = clipName; l.clipId = first->id; l.line = line; l.times = times;
    if (g_playing) l.playhead = g_play;
    if (l.level == kLogError && line > 0) {
        std::string mk = markText;
        while (!mk.empty() && std::isspace((unsigned char)mk.front())) mk.erase(mk.begin());
        while (!mk.empty() && std::isspace((unsigned char)mk.back())) mk.pop_back();
        if (mk.size() > 300) mk.resize(300);
        auto& marks = g_diagMarks[first->id];
        if (!marks.count(line)) marks[line] = mk.empty() ? std::string("error") : mk;
    }
}
#include "app.h"
void logClipError(const char* language, const Clip& clip, const std::string& message) {
    LogLine entry = classifyLog(std::string("[") + language + "] ERROR: " + message);
    entry.level = kLogError;
    fillLoc(entry, clip.name, 0, message);
    pushLog(std::move(entry));
}
#include "app.h"
void logClipErrorById(const char* language, int clipId, const std::string& message) {
    for (const Track& track : g_proj.tracks)
        for (const Clip& clip : track.clips)
            if (clip.id == clipId) { logClipError(language, clip, message); return; }
    LogLine entry = classifyLog(std::string("[") + language + "] ERROR: " + message);
    entry.level = kLogError;
    pushLog(std::move(entry));
}
#include "app.h"
void logPdClipMessage(int clipId, const std::string& message) {
    std::string low = message;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const bool warning = low.rfind("warning:", 0) == 0 || low.rfind("notice:", 0) == 0;
    LogLine entry = classifyLog(std::string("[PureData] ") + (warning ? "WARNING: " : "ERROR: ") + message);
    entry.level = warning ? kLogImportant : kLogError;
    if (Clip* clip = findClip(clipId)) {
        const auto location = inferPdEditorLocation(*clip, message);
        fillLoc(entry, clip->name, location.line, message);
        entry.lineApprox = location.approximate;
        entry.lineCandidates = location.candidates;
        if (location.line > 0) {
            entry.lineApprox = location.approximate;
            g_gotoLine[clip->id] = location.line;
        }
    }
    pushLog(std::move(entry));
}
#include "app.h"
void logPdNote(const std::string& note);
#include "app.h"
void logPdFailureStatus(const std::string& status) {
    static const std::regex clipError(R"re(PD clip\s+(\d+):\s*(.*))re");
    std::smatch match;
    if (std::regex_search(status, match, clipError)) {
        logPdClipMessage(std::atoi(match[1].str().c_str()), match[2].str());
    } else {
        LogLine entry = classifyLog("[PureData] ERROR: " + status);
        entry.level = kLogError;
        pushLog(std::move(entry));
    }
}
#include "app.h"
void logLine(const std::string& s) {
    {   // the crash-proof trail: the console log is memory-only and a hard crash loses it; every
        // line also lands in caw_log.txt next to caw.exe, flushed per line
        static std::mutex s_logMu;
        std::lock_guard<std::mutex> lock(s_logMu);
        static FILE* s_logFile = nullptr;
        if (!s_logFile) {
            const std::string own = plat::cawOwnFolder();   // the folder of caw.exe / caw (portable)
            {
                const std::string base = own.empty() ? std::string() : own + "/";
                s_logFile = fopen((base + "caw_log.txt").c_str(), "a");
                if (s_logFile) {
                    time_t now = time(nullptr);
                    char stamp[32] = {};
                    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
                    fprintf(s_logFile, "---- CAW started %s ----\n", stamp);
                }
            }
        }
        if (s_logFile) {
            long long size = ftell(s_logFile);
            if (size > 8 * 1024 * 1024) {   // rotate: a runaway log must not grow forever
                fclose(s_logFile);
                const std::string own = plat::cawOwnFolder();
                const std::string base = own.empty() ? std::string() : own + "/";
                s_logFile = fopen((base + "caw_log.txt").c_str(), "w");
                if (s_logFile) fprintf(s_logFile, "---- log rotated ----\n");
            }
            fprintf(s_logFile, "%s\n", s.c_str());
            fflush(s_logFile);
        }
    }
    LogLine l = classifyLog(s);
    if (l.level == kLogError && !l.clip.empty() && l.clipId < 0) fillLoc(l, l.clip, 0, s);
    pushLog(std::move(l));
}
#include "app.h"
void logPdNote(const std::string& note) {
    static const std::regex clipMessage(R"re(PD clip\s+\d+:)re");
    if (std::regex_search(note, clipMessage)) logPdFailureStatus(note);
    else logLine("[caw] " + note);
}
#include "app.h"
void playNotice(const std::string& text, const std::string& shortText);
#include "app.h"
void drawMasterFxWindow();   // defined near the clip editors: the 8-slot master effects chain (FX button under the master meter)
#include "app.h"
const char* renderExtension(int format) {
    return format == 3 ? ".flac" : format == 4 ? ".mp3" : ".wav";
}
#include "app.h"
std::filesystem::path renderFilePath() {
    std::filesystem::path file = g_proj.cs.renderFile.empty() ? "render.wav" : g_proj.cs.renderFile;
    file.replace_extension(renderExtension(g_proj.cs.renderFormat));
    if (file.is_absolute()) return file;
    return (g_projectPath.empty() ? workDir() : std::filesystem::path(g_projectPath).parent_path()) / file;
}
#include "app.h"
void drawScoreMarkList(const char* idPrefix, std::vector<ScoreMark>& list, bool section) {
    int erase = -1;
    for (int i = 0; i < (int)list.size(); ++i) {
        ScoreMark& mk = list[(size_t)i];
        ImGui::PushID(i);
        float t = (float)mk.t;
        ImGui::SetNextItemWidth(S(90));
        if (ImGui::DragFloat("##t", &t, 0.1f, 0.f, 36000.f, "%.1f s")) mk.t = std::max(0.f, t);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(section ? 44 : 150));
        inputStr("##label", mk.label, 0);
        if (section) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(200));
            inputStr("##text", mk.text, 0);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) erase = i;
        ImGui::PopID();
    }
    if (erase >= 0) list.erase(list.begin() + erase);
    (void)idPrefix;
}
#include "app.h"
void drawScoreRoutingMap() {
    struct Route { std::string from, to; bool toMaster; };
    std::vector<Route> routes;
    std::set<std::pair<std::string, std::string>> codeRoutes;
    const std::regex sendRe(R"re(\bcawsa\s+"([^"]+)")re");
    const std::regex receiveRe(R"re(\bcawra\s+"([^"]+)")re");
    auto destination = [&](const std::string& bus) {
        for (const Track& other : g_proj.tracks)
            if (other.kind == 4 && vstlang::busName(other) == bus) return std::string("VSTe: ") + other.name;
        return std::string("BUS: ") + bus;
    };
    for (const Track& t : g_proj.tracks) {
        if (t.kind == 0) {
            for (const Clip& clip : t.clips) {
                std::istringstream lines(clip.code);
                std::string line;
                while (std::getline(lines, line)) {
                    const size_t semicolon = line.find(';');
                    if (semicolon != std::string::npos) line.erase(semicolon);
                    std::smatch match;
                    const std::string clipName = "Csound: " + clip.name;
                    if (std::regex_search(line, match, sendRe))
                        codeRoutes.insert({clipName, destination(match[1].str())});
                    if (std::regex_search(line, match, receiveRe))
                        codeRoutes.insert({"BUS: " + match[1].str(), clipName});
                }
            }
        }
        if (t.kind != 3 && t.kind != 4) continue;
        // VSTe output always goes to the master now (master = replace the source, mix = alongside it).
        routes.push_back({t.name, "MASTER", true});
        // Each enabled track / clips input feeds the VSTe; master inputs have no named source node.
        if (t.kind == 4) for (const auto& in : t.vstInputs)
            if (in.on && in.mode != 2 && !in.name.empty())
                routes.push_back({in.name, t.name, false});
    }
    for (const auto& route : codeRoutes) routes.push_back({route.first, route.second, false});
    ImGui::TextUnformatted("SCORE PATCH BOARD");
    ImGui::TextDisabled("VST outputs and literal cawsa/cawra in Csound clips. Dynamic code and other routes are not shown.");
    if (routes.empty()) { ImGui::TextDisabled("No recognized routes yet."); return; }
    ImGui::BeginChild("##score_patchboard", ImVec2(0, S(400)), true, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(S(570), ImGui::GetContentRegionAvail().x);
    const float rowH = S(64), left = origin.x + S(20), right = origin.x + width - S(210);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(width, rowH * routes.size() + S(20)));
    for (size_t i = 0; i < routes.size(); ++i) {
        const float y = origin.y + S(12) + i * rowH;
        const ImU32 wire = routes[i].toMaster ? IM_COL32(246, 151, 58, 255) : IM_COL32(67, 193, 200, 255);
        dl->AddRectFilled(ImVec2(left, y), ImVec2(left + S(180), y + S(45)), IM_COL32(39, 42, 46, 255), S(4));
        dl->AddRect(ImVec2(left, y), ImVec2(left + S(180), y + S(45)), IM_COL32(111, 105, 91, 255), S(4));
        dl->PushClipRect(ImVec2(left + S(8), y), ImVec2(left + S(174), y + S(45)), true);
        dl->AddText(ImVec2(left + S(10), y + S(13)), IM_COL32(235, 232, 219, 255), routes[i].from.c_str());
        dl->PopClipRect();
        dl->AddBezierCubic(ImVec2(left + S(180), y + S(22)), ImVec2(left + S(230), y + S(22)),
                           ImVec2(right - S(45), y + S(22)), ImVec2(right, y + S(22)), wire, S(3));
        dl->AddCircleFilled(ImVec2(left + S(180), y + S(22)), S(5), wire);
        dl->AddRectFilled(ImVec2(right, y), ImVec2(right + S(180), y + S(45)), IM_COL32(40, 43, 45, 255), S(4));
        dl->AddRect(ImVec2(right, y), ImVec2(right + S(180), y + S(45)), IM_COL32(111, 105, 91, 255), S(4));
        dl->AddCircleFilled(ImVec2(right, y + S(22)), S(5), wire);
        dl->PushClipRect(ImVec2(right + S(10), y), ImVec2(right + S(178), y + S(45)), true);
        dl->AddText(ImVec2(right + S(12), y + S(13)), IM_COL32(235, 232, 219, 255), routes[i].to.c_str());
        dl->PopClipRect();
    }
    ImGui::EndChild();
}
#include "app.h"
void drawScoreMapWindow() {
    if (!g_scoreMapOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(700), S(500)), ImGuiCond_FirstUseEver);
    winMaxPrepareT("Map score");
    if (ImGui::Begin("Map score", &g_scoreMapOpen, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Map score");
        drawScoreRoutingMap();
    }
    ImGui::End();
}
#include "app.h"
void drawScoreExportControls() {
    ImGui::TextUnformatted("Score");
    ImGui::SetNextItemWidth(S(260));
    caw_ui::combo("##score_export_type", &g_scoreExportType, "Overview (SVG)\0Full score (HTML)\0");
    ImGui::TextUnformatted("Color");
    ImGui::SetNextItemWidth(S(260));
    caw_ui::combo("##score_export_color", &g_scoreExportColor, "Colour\0Black and white\0");
    if (ImGui::Button("Export score")) generateScore(g_scoreExportType == 1, g_scoreExportColor == 1);
}
#include "app.h"
void drawScoreExportWindow() {
    if (!g_scoreExportOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(360), S(210)), ImGuiCond_FirstUseEver);
    winMaxPrepareT("Export score");
    if (ImGui::Begin("Export score", &g_scoreExportOpen, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Export score");
        drawScoreExportControls();
    }
    ImGui::End();
}
#include "app.h"
void drawCodeExportWindow() {
    if (!g_codeExportOpen) return;
    static int mode = 0, filter = 0;
    ImGui::SetNextWindowSize(ImVec2(S(570), S(330)), ImGuiCond_FirstUseEver);
    winMaxPrepareT("Export code");
    if (ImGui::Begin("Export code", &g_codeExportOpen, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Export code");
        ImGui::TextUnformatted("Format");
        ImGui::SetNextItemWidth(-1);
        caw_ui::combo("##code_export_format", &mode,
            "Separate raw clip files\0All raw clip code in one TXT\0Csound clip code in one ORC\0Csound instruments in one ORC\0Whole project as CSD\0");
        if (mode <= 1) {
            ImGui::TextUnformatted("Include");
            ImGui::SetNextItemWidth(-1);
            caw_ui::combo("##code_export_filter", &filter, "All clip code\0Csound clips only\0VST CAW clips only\0");
        }
        if (mode == 0) ImGui::TextWrapped("Writes one original source file per clip. VST CAW code stays separate from Csound.");
        else if (mode == 1) ImGui::TextWrapped("Combines the original clip text into one labelled TXT file.");
        else if (mode == 2) ImGui::TextWrapped("Combines raw Csound clip bodies. The file is not a standalone orchestra.");
        else if (mode == 3) ImGui::TextWrapped("Wraps Csound clips as instruments. Some code may still depend on project opcodes, files or CAW helpers.");
        else ImGui::TextWrapped("Generates a Csound project and score. SuperCollider and Pure Data clips are not included in Csound export. External files may be needed; CAW-hosted VST plugins do not run standalone in Csound.");
        ImGui::Spacing();
        ImGui::TextDisabled("SuperCollider and Pure Data audio export is not implemented yet.");
        ImGui::TextDisabled("A new folder is created, so existing exports are not overwritten.");
        if (ImGui::Button("Export code")) {
            exportCodeClips(static_cast<code_export::Mode>(mode), static_cast<code_export::Filter>(filter));
        }
    }
    ImGui::End();
}
#include "app.h"
void drawLogo(ImDrawList* dl, ImVec2 origin, float size) {
    const LogoVariant* best = nullptr;
    for (int i = 0; i < g_logoVariantsReady; ++i)
        if (!best || std::fabs(g_logoVariants[i].px - size) < std::fabs(best->px - size)) best = &g_logoVariants[i];
    if (best && std::fabs(best->px - size) < 2.f) {
        const ImVec2 p(std::floor(origin.x + .5f), std::floor(origin.y + .5f));   // whole pixels: 1:1, no blur
        dl->AddImage(ImGui::GetIO().Fonts->TexID, p, ImVec2(p.x + best->px, p.y + best->px), best->uv0, best->uv1);
        return;
    }
    if (g_logoReady)
        dl->AddImage(ImGui::GetIO().Fonts->TexID, origin, ImVec2(origin.x + size, origin.y + size), g_logoUv0, g_logoUv1);
}
#include "app.h"
void drawMainMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New project", "Ctrl+N")) newProject();
        if (ImGui::MenuItem("Open project...", "Ctrl+O")) openProject();
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveProject(false);
        if (ImGui::MenuItem("Save As...")) saveProject(true);
        // A regular selectable opens the side popup on click, not hover.
        // Unlike BeginMenu, it never draws ImGui's arrow underneath the plus.
        const bool exportClicked = ImGui::Selectable("Export", false, ImGuiSelectableFlags_DontClosePopups);
        const ImVec2 exportA = ImGui::GetItemRectMin(), exportB = ImGui::GetItemRectMax();
        caw_ui::disclosure(ImGui::GetWindowDrawList(),
                           ImVec2(exportB.x - S(12), (exportA.y + exportB.y) * .5f),
                           ImGui::GetFontSize() * .19f, ImGui::GetColorU32(ImGuiCol_Text), false);
        if (exportClicked && !ImGui::IsPopupOpen("##export_flyout")) ImGui::OpenPopup("##export_flyout");
        ImGui::SetNextWindowPos(ImVec2(exportB.x, exportA.y), ImGuiCond_Appearing);
        bool exportAction = false;
        if (ImGui::BeginPopup("##export_flyout")) {
            if (ImGui::MenuItem("Audio export...", nullptr, false, true)) { g_audioExportOpen = true; exportAction = true; }
            if (g_renderActive && ImGui::MenuItem("Cancel audio export")) { cancelRender(); exportAction = true; }
            if (ImGui::MenuItem("Code clips...")) { g_codeExportOpen = true; exportAction = true; }
            if (ImGui::MenuItem("Score...")) { g_scoreExportOpen = true; exportAction = true; }
            ImGui::EndPopup();
        }
        if (exportAction) ImGui::CloseCurrentPopup();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Ctrl+Q")) appRequestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Select tool", "1", g_tool == Tool::Select)) g_tool = Tool::Select;
        if (ImGui::MenuItem("Pen 1 (draw automation, simplified to few dots)", "8", g_tool == Tool::Pen)) g_tool = Tool::Pen;
        if (ImGui::MenuItem("Pen 2 (draw automation, keeps every dot)", "9", g_tool == Tool::Pen2)) g_tool = Tool::Pen2;
        if (ImGui::MenuItem("Cut tool (split MIDI regions)", "3", g_tool == Tool::Cut)) g_tool = Tool::Cut;
        if (ImGui::MenuItem("Join MIDI regions", "4", g_tool == Tool::Join)) runJoinTool();
        ImGui::Separator();
        int laneIndex = -1;
        Clip* clip = selectedClip(&laneIndex);
        if (ImGui::MenuItem("Duplicate selected clips (same instrument)", "Ctrl+D", false, !g_selClips.empty())) duplicateSelectedClips();
        if (ImGui::MenuItem("Duplicate clip as new instrument", nullptr, false, clip != nullptr)) duplicateClipAsNew(laneIndex, g_selClip);
        if (ImGui::MenuItem("Edit selected clip code", nullptr, false, clip != nullptr) && clip) clip->editorOpen = true;
        validateAutoSelection();
        if (ImGui::MenuItem("Delete selected automation points", "Del", false, hasAutoSelection())) deleteSelectedAutoPoints();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Pool")) {
        if (ImGui::MenuItem("File")) g_audioPoolOpen = true;
        if (g_langCsound) {
            if (ImGui::MenuItem("UDO")) g_udoPoolOpen = true;
            if (ImGui::MenuItem("Instrument")) g_instrPoolOpen = true;
            if (ImGui::MenuItem("Orchestra")) g_orchestraOpen = true;
            if (ImGui::MenuItem("Includes")) g_includePoolOpen = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Monitor panel", nullptr, &g_showMonitor);
        ImGui::MenuItem("Help panel (bottom)", nullptr, &g_helpOn);
        ImGui::Separator();
        ImGui::MenuItem("Master meter and gain (right panel)", nullptr, &g_viewMeter);
        ImGui::MenuItem("Spectrum", nullptr, &g_viewSpectrum);
        ImGui::MenuItem("Signal scope", nullptr, &g_viewScope);
        ImGui::MenuItem("Console", nullptr, &g_viewConsole);
        ImGui::Separator();
        ImGui::TextDisabled("Timeline");
        if (ImGui::MenuItem("Hide / show all automation lanes", "Alt+H")) toggleHideAllAutomation();
        if (ImGui::MenuItem("Show all automation lanes", "Alt+J")) showAllAutomation();
        if (ImGui::MenuItem("Hide / show all MIDI lanes", "Alt+M")) toggleHideAllMidi();
        if (ImGui::MenuItem("Show all MIDI lanes", "Alt+N")) showAllMidi();
        ImGui::Separator();
        ImGui::TextDisabled("Keyboard shortcuts");
        ImGui::TextDisabled("1 Select   8 Pen   3 Cut   4 Join   Esc = Select");
        ImGui::TextDisabled("M mute / S solo the selected track");
        ImGui::TextDisabled("Space play / stop timeline. MIDI editor: - / + move playhead, Shift = fast");
        ImGui::TextDisabled("Ctrl+D duplicate clips   Del delete");
        ImGui::TextDisabled("Ctrl+A select all dots of a lane");
        ImGui::TextDisabled("Ctrl+Z undo   Ctrl+Y redo   Ctrl+S save");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Score")) {
        if (ImGui::MenuItem("Edit score")) g_scoreOpen = true;
        if (ImGui::MenuItem("Map score")) g_scoreMapOpen = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Engines...")) g_chooseLanguages = true;
        ImGui::Separator();
        static const char* tabs[] = {"General", "Audio", "Programs", "Editor", "Advanced", "Files"};
        static const int ids[] = {6, 0, 1, 3, 4, 5};
        for (int i = 0; i < 6; ++i)
            if ((ids[i] != 4 || g_langCsound) && ImGui::MenuItem(tabs[i])) { g_settingsOpen = true; g_settingsGoto = ids[i]; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About CAW")) g_aboutOpen = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
    if (g_chooseLanguages) { ImGui::OpenPopup("Choose engines###caw_languages"); g_chooseLanguages = false; }
    if (ImGui::BeginPopupModal("Choose engines###caw_languages", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(0.93f, 0.50f, 0.15f, 1.f), "Select the engines");
        ImGui::Separator();
        ImGui::Checkbox("Csound", &g_langCsound);
        ImGui::Checkbox("Pure Data", &g_langPd);
        ImGui::Checkbox("SuperCollider", &g_langSc);
        ImGui::Checkbox("CAW: VST", &g_langVst);
        ImGui::Checkbox("CAW: sample", &g_langAudio);
        ImGui::Spacing();
        if (!g_langCsound && !g_langPd && !g_langSc && !g_langVst && !g_langAudio)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.f), "DSP off: score only.");
        if (ImGui::Button("Continue", ImVec2(S(110), 0))) {
            // The engine set changed while DSP is running: stop EVERYTHING so the DSP button
            // honestly reads "off". The user presses DSP again to run the new set (a green DSP
            // that no longer matches the checked engines is what hid this before).
            const bool ranCsound = g_liveCsound;
            const bool ranPd = g_mixedPd || g_pdEngineOn;
            const bool ranSc = g_scEngineOn;
            const bool ranVst = g_vstEngineOn;
            const bool ranSample = g_sampleEngineOn;
            const bool anyRunning = ranCsound || ranPd || ranSc || ranVst || ranSample;
            const bool setChanged = anyRunning && (g_langCsound != ranCsound || g_langPd != ranPd || g_langSc != ranSc || g_langVst != ranVst || g_langAudio != ranSample);
            if (setChanged) {
                stopPlayback();   // Csound (+ the mixed PD engine on it)
                if (g_scEngineOn) stopScEngine();
                if (g_vstEngineOn) stopVstEngine();
                if (g_sampleEngineOn) {
                    g_sampleEngineOn = false;
                    cawaudio::engine().clearAll();
                    g_audioVoiceSig.clear();
                    closeSampleOutput();
                }
                if (g_pdTimelineMode) { pdTransport(false); g_timelineRun = false; }
                if (g_pdEngineOn || g_pdTimelineMode) {
                    g_pdTimelineEngine.stop(); g_pdPlayingClip = 0;
                    g_pdEngineOn = false; g_pdTimelineMode = false; g_pdTimelineEvents.clear();
                    g_pdResumeAfterReload = false;
                }
                logLine("[caw] The engine selection changed - DSP stopped. Press DSP to run the new set.");
                playNotice("Engine selection changed - DSP stopped. Press DSP to run the new set.", "DSP stopped");
            }
            else {
            if (!g_langSc && g_scEngineOn) stopScEngine();
            if (!g_langVst && g_vstEngineOn) stopVstEngine();   // unchecked: stop the sound, keep the plugins loaded
            if (!g_langAudio && g_sampleEngineOn) {   // unchecked: stop the sample engine like the others
                g_sampleEngineOn = false;
                cawaudio::engine().clearAll();
                g_audioVoiceSig.clear();
                closeSampleOutput();
                logLine("[caw] CAW: sample unchecked: the sample engine stopped.");
            }
            }
            g_languageChoiceMade = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (g_welcomeOpen) { ImGui::OpenPopup("Welcome to CAW###welcome_caw"); g_welcomeOpen = false; }
    if (ImGui::BeginPopupModal("Welcome to CAW###welcome_caw", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float logoSize = S(220), bodyWidth = S(300);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        drawLogo(ImGui::GetWindowDrawList(), ImVec2(start.x + (bodyWidth - logoSize) * .5f, start.y), logoSize);
        ImGui::Dummy(ImVec2(bodyWidth, logoSize));
        const char* title = "CAW - Code Audio Workstation";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (bodyWidth - ImGui::CalcTextSize(title).x) * .5f));
        ImGui::TextUnformatted(title);
        const char* version = "0.0.34 beta";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (bodyWidth - ImGui::CalcTextSize(version).x) * .5f));
        ImGui::TextUnformatted(version);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (bodyWidth - S(100)) * .5f);
        if (ImGui::Button("Close##welcome", ImVec2(S(100), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_aboutOpen) { ImGui::OpenPopup("About CAW###about_caw"); g_aboutOpen = false; }
    if (ImGui::BeginPopupModal("About CAW###about_caw", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // logo on the left, name / version / author on its right
        const float aboutLogo = S(112);
        drawLogo(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), aboutLogo);
        ImGui::Dummy(ImVec2(aboutLogo, aboutLogo));
        ImGui::SameLine(0.f, S(18));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (aboutLogo - ImGui::GetTextLineHeightWithSpacing() * 3.f) * .5f));
        ImGui::TextColored(ImVec4(0.93f, 0.50f, 0.15f, 1.f), "CAW - Code Audio Workstation");
        ImGui::TextDisabled("Version 0.0.57 beta");
        ImGui::TextDisabled("Parham Izadyar  -  parhamizadyar.net");
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::TextUnformatted("simple audio workstation written in C++, with a focus on code-based compositions and audio programming.");
        ImGui::TextUnformatted("Csound, SuperCollider and Pure Data can run at the same time in one project, with data passing between them.");
        ImGui::TextUnformatted("VSTs and audio files run and modulated by CAW’s own code, with project export to a musical score");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(S(100), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_confirmDeleteSelected) {
        ImGui::OpenPopup("Confirm track deletion");
        g_confirmDeleteSelected = false;
    }
    if (ImGui::BeginPopupModal("Confirm track deletion", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int selectedCount = 0;
        for (const Track& t : g_proj.tracks) if (t.deleteSelected) ++selectedCount;
        ImGui::Text("Delete %d selected track(s)? Clips and automation on those tracks will be removed.", selectedCount);
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(100, 0))) {
            const int draggedId = g_autoDragTrackId;
            const int selectedMidiTrack = g_midiSelectedTrackId;
            const bool removesDragged = std::any_of(g_proj.tracks.begin(), g_proj.tracks.end(),
                [draggedId](const Track& t) { return t.id == draggedId && t.deleteSelected; });
            const bool removesMidiTrack = std::any_of(g_proj.tracks.begin(), g_proj.tracks.end(),
                [selectedMidiTrack](const Track& t) { return t.id == selectedMidiTrack && t.deleteSelected; });
            g_proj.tracks.erase(std::remove_if(g_proj.tracks.begin(), g_proj.tracks.end(),
                [](const Track& t) { return t.deleteSelected; }), g_proj.tracks.end());
            pruneFolders();
            if (removesDragged) { g_autoDragTrackId = -1; g_autoDragPoint = -1; g_autoDragChannel.clear(); }
            if (removesMidiTrack) { g_midiSelectedTrackId = -1; g_midiSelectedNote = -1; g_midiDragTrackId = -1; g_midiDragNote = -1; }
            g_selTrack = g_selClip = -1;
            g_confirmDeleteSelected = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            g_confirmDeleteSelected = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
#include "app.h"
void analyzeCurrentRaw() {
    // DC / infrasonic blocker on the ANALYSIS input only (the audio is untouched): analog-modeled
    // plugins put DC on their output, and the FFT window smears it across the lowest bins - a wall
    // of fake low-frequency bars that is not in the sound (user report). A one-pole blocker at
    // ~35 Hz removes it, exactly what a standard analyzer does before displaying.
    static float dcLx = 0.f, dcLy = 0.f, dcRx = 0.f, dcRy = 0.f;
    float blockedL[monitor_fft::size], blockedR[monitor_fft::size];
    for (int i = 0; i < monitor_fft::size; ++i) {
        const float l = g_rawLeft[i], r = g_rawRight[i];
        dcLy = (l - dcLx) + 0.995f * dcLy; dcLx = l; blockedL[i] = dcLy;
        dcRy = (r - dcRx) + 0.995f * dcRy; dcRx = r; blockedR[i] = dcRy;
    }
    float input[monitor_fft::size];
    for (int i = 0; i < monitor_fft::size; ++i)
        input[i] = g_fftChannel == 1 ? blockedR[i] : g_fftChannel == 2 ? (blockedL[i] + blockedR[i]) * 0.5f : blockedL[i];
    g_fftDb = monitor_fft::spectrum(input, monitor_fft::size);
}
#include "app.h"
void applyRawSamples(const float* left, const float* right) {
    const float inv = 1.f / g_rawFullScale;
    for (int i = 0; i < monitor_fft::size; ++i) {
        g_rawLeft[i] = left[i] * inv;
        g_rawRight[i] = right[i] * inv;
    }
    g_haveRaw = true;
    g_rawAge = 0.f;
    const auto work = monitor_work::select(g_showMonitor && g_viewSpectrum, g_specFreeze, g_analyzerMode, g_specStyle);
    if (!work.fft || g_specFreeze) return;
    // The analyzer pipeline runs at ~30 Hz, not per arriving frame. A standard RTA updates
    // 20-30 times a second; this cuts the FFT / band / peak work by several times with no
    // visual change (much less CPU, user request).
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0 && now - last < std::chrono::milliseconds(33)) return;
    last = now;
    analyzeCurrentRaw();
    if (g_analyzerMode == 0 && g_specStyle == 2) {   // the FFT line gets the same attack/release smoothing as the band view
        if (g_fftDbDisp.size() != g_fftDb.size()) g_fftDbDisp.assign(g_fftDb.size(), g_specDbFloor);
        // BOTH directions speed-limited (user request: smooth motion): a raw frame-to-frame jump
        // would read as flicker; the attack is fast but not instant.
        const float rise = g_specBetter ? 240.f : 800.f;     // dB per second
        const float fall = g_specBetter ? 45.f : 160.f;      // dB per second
        for (size_t k = 0; k < g_fftDb.size(); ++k) {
            const float v = g_fftDb[k];
            float& d = g_fftDbDisp[k];
            d = v > d ? std::min(v, d + rise * 0.033f) : std::max(v, d - fall * 0.033f);   // the pipeline is throttled to ~30 Hz
        }
    }
    if (work.thirdBands || work.columnBands || work.tonalPeaks) updateSpectrumDerived();
    if (work.history) {
        g_fftHistory.push_back(g_fftDb);
        if (g_fftHistory.size() > 120) g_fftHistory.pop_front();
    }
}
#include "app.h"
void applyMonitor(char tag, int start, const std::vector<float>& values) {
    if (start < 0) return;
    if (tag == 'B') {
        if (values.size() == (size_t)monitor_fft::size * 2)
            applyRawSamples(values.data(), values.data() + monitor_fft::size);
        return;
    }
    if (tag == 'U' || tag == 'V') {
        if (g_rawAssembler.push(tag, start, values, 1.f))
            applyRawSamples(g_rawAssembler.left, g_rawAssembler.right);
        return;
    }
    const float inv = 1.f / (float)std::max(0.0001, g_proj.cs.zdbfs);
    for (size_t i = 0; i < values.size(); ++i) {
        float v = values[i];
        if (tag == 'C') { recFeed(start + (int)i, v); continue; }   // live Rec value of a clip: used as it is
        if (!std::isfinite(v)) v = 0.f;
        v *= inv;
        const int idx = start + (int)i;
        if (tag == 'M') {
            if ((size_t)idx >= g_peakIn.size()) continue;
            g_peakIn[(size_t)idx] = v;
            if (v >= 0.99426f && (size_t)idx < g_meterClipHold.size())
                g_meterClipHold[(size_t)idx] = 1.5f;
        } else if (tag == 'S' && idx < kMeterBands) { g_bandAmp[idx] = v; g_haveBands = true; }
        else if (tag == 'L' && idx < kScopePoints) g_scopeStagingL[idx] = std::min(1.f, std::max(-1.f, v));
        else if (tag == 'R' && idx < kScopePoints) g_scopeStagingR[idx] = std::min(1.f, std::max(-1.f, v));
    }
    if (tag == 'R' && start + (int)values.size() >= kScopePoints) {
        std::copy(g_scopeStagingL, g_scopeStagingL + kScopePoints, g_scopeL);
        std::copy(g_scopeStagingR, g_scopeStagingR + kScopePoints, g_scopeR);
        g_scopeHasFrame = true;
    }
    g_monAge = 0.f;
    g_monSeen = true;
}
#include "app.h"
bool handleMonitorLine(const std::string& l) {
    size_t at = std::string::npos;
    for (const char* tagText : {"CAWM ", "CAWS ", "CAWL ", "CAWR ", "CAWC ", "CAWU ", "CAWV "}) {
        const size_t f = l.find(tagText);
        if (f != std::string::npos) { at = f; break; }
    }
    if (at == std::string::npos) return false;
    MonFrame fr;
    fr.tag = l[at + 3];
    if (fr.tag == 'U' || fr.tag == 'V') {
        if (!parseRawMonitorValues(l.data() + at + 5, l.data() + l.size(), fr.start, fr.values)) {
            // Queue an invalid chunk so the assembler discards the incomplete frame.
            fr.start = 0;
            fr.values.clear();
        }
    } else {
        std::istringstream in(l.substr(at + 5));
        if (!(in >> fr.start)) return true;
        float v = 0.f;
        while (in >> v) fr.values.push_back(v);
    }
    const auto due = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(engineOutputLatency() * 1e6));
    g_monHeld.emplace_back(due, std::move(fr));
    if (g_monHeld.size() > 100000) g_monHeld.clear();
    return true;
}
#include "app.h"
void pollMonitor() {
    if (g_monitorSocket != INVALID_SOCKET) {
        unsigned char data[65536];
        for (int n = 0; n < 256; ++n) {
            sockaddr_in from{};
            SockLen fromLen = sizeof(from);
            const int got = (int)recvfrom(g_monitorSocket, reinterpret_cast<char*>(data), sizeof(data), 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (got == SOCKET_ERROR) break;
            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
            monitor_osc::Packet packet;
            if (!monitor_osc::decode(data, (size_t)got, packet)) continue;
            g_sectionSeen = true;
            bool& have = packet.kind == 'B' ? g_haveOscRawSeq : g_haveOscMeterSeq;
            uint32_t& last = packet.kind == 'B' ? g_oscRawSeq : g_oscMeterSeq;
            if (have && packet.sequence != last && !monitor_osc::newer(packet.sequence, last)) continue;
            if (have && packet.kind == 'B' && packet.sequence == last) continue;
            have = true; last = packet.sequence;
            MonFrame fr;
            fr.tag = packet.kind; fr.start = packet.start; fr.values = std::move(packet.values);
            const auto due = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(engineOutputLatency() * 1e6));
            g_monHeld.emplace_back(due, std::move(fr));
        }
    }
    const auto now = std::chrono::steady_clock::now();
    MonFrame newestRaw;
    bool haveNewestRaw = false;
    while (!g_monHeld.empty() && g_monHeld.front().first <= now) {
        MonFrame& frame = g_monHeld.front().second;
        if (frame.tag == 'B') { newestRaw = std::move(frame); haveNewestRaw = true; }
        else applyMonitor(frame.tag, frame.start, frame.values);
        g_monHeld.pop_front();
    }
    // If the UI paused, don't calculate FFTs for stale snapshots.
    if (haveNewestRaw) applyMonitor(newestRaw.tag, newestRaw.start, newestRaw.values);
}
#include "app.h"
void drawExportOptions(CsoundSettings& s) {
    s.renderFormat = std::min(4, std::max(0, s.renderFormat));
    // Render mode: offline (faster than real time) or real-time capture - global for every engine
    // (LKD freezes AND whole-project exports). Offline renders through each engine's own offline
    // pipeline; real-time plays the transport once and captures the final master mix.
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Render mode", s.renderMode == 0 ? "Offline (faster than real time)" : "Real-time capture")) {
        if (ImGui::Selectable("Offline (faster than real time)", s.renderMode == 0)) s.renderMode = 0;
        if (ImGui::Selectable("Real-time capture", s.renderMode == 1)) s.renderMode = 1;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) helpTip(s.renderMode == 0
        ? "Offline: every engine renders through its own offline pipeline (Csound host, PD C++ host, SC NRT, sample engine) at full CPU speed. SuperCollider tracks need the NRT tools."
        : "Real-time: the transport plays once from the start and the final master mix (every engine live) is captured - works for all engines, but takes as long as the song.");
    const int kind = s.renderFormat < 3 ? 0 : s.renderFormat == 3 ? 1 : 2;
    const char* formats[] = {"WAV", "FLAC", "MP3"};
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Format", formats[kind])) {
        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(formats[i], i == kind)) {
            s.renderFormat = i == 0 ? 1 : i == 1 ? 3 : 4;
            std::filesystem::path p = s.renderFile.empty() ? "render.wav" : s.renderFile;
            p.replace_extension(renderExtension(s.renderFormat));
            s.renderFile = p.string();
        }
        ImGui::EndCombo();
    }
    if (s.renderFormat < 3) {
        const char* bits[] = {"16-bit PCM", "24-bit PCM", "32-bit float"};
        ImGui::SetNextItemWidth(S(180));
        if (caw_ui::beginCombo("Bit depth", bits[s.renderFormat])) {
            for (int i = 0; i < 3; ++i) if (ImGui::Selectable(bits[i], i == s.renderFormat)) s.renderFormat = i;
            ImGui::EndCombo();
        }
    } else if (s.renderFormat == 3) {
        ImGui::SetNextItemWidth(S(180));
        if (caw_ui::beginCombo("Bit depth", s.exportFlacBits == 16 ? "16-bit lossless" : "24-bit lossless")) {
            if (ImGui::Selectable("16-bit lossless", s.exportFlacBits == 16)) s.exportFlacBits = 16;
            if (ImGui::Selectable("24-bit lossless", s.exportFlacBits == 24)) s.exportFlacBits = 24;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) helpTip("FLAC is lossless: its bitrate varies with the audio. Bit depth sets its precision.");
    } else {
        static const int rates[] = {8,16,24,32,40,48,56,64,80,96,112,128,144,160,192,224,256,320};
        if (cawMp3RateSupported(s.sr) && !cawMp3BitrateSupported(s.sr, s.exportMp3Kbps)) {
            int best = 0, delta = 100000;
            for (int rate : rates) if (cawMp3BitrateSupported(s.sr, rate) && std::abs(rate - s.exportMp3Kbps) < delta) {
                best = rate; delta = std::abs(rate - s.exportMp3Kbps);
            }
            if (best) s.exportMp3Kbps = best;
        }
        const std::string current = std::to_string(s.exportMp3Kbps) + " kbps";
        ImGui::SetNextItemWidth(S(180));
        if (caw_ui::beginCombo("Bitrate", current.c_str())) {
            for (int rate : rates) if (cawMp3BitrateSupported(s.sr, rate)) {
                const std::string label = std::to_string(rate) + " kbps";
                if (ImGui::Selectable(label.c_str(), s.exportMp3Kbps == rate)) s.exportMp3Kbps = rate;
            }
            ImGui::EndCombo();
        }
    }
    // EXPORT SOURCE: the master mixdown (as virtual) or the whole virtual bus (as real routing)
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Source", s.exportSource == 0 ? "as virtual (master mixdown)" : "as real routing (virtual bus)")) {
        if (ImGui::Selectable("as virtual (master mixdown)", s.exportSource == 0)) s.exportSource = 0;
        if (ImGui::Selectable("as real routing (virtual bus)", s.exportSource == 1)) s.exportSource = 1;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) helpTip("as virtual: the export is the master mixdown (the Channels combo below decides its fold).\nas real routing: the export is the virtual soundcard bus - the whole project at the audio settings width N, every track at its routed positions (out<n> lines, Out source folds), like the live master but not folded to stereo.\nOffline export only; the real-time capture always captures the stereo master.");
    // CONVERT: when checked, the rendered channels fold to mono / stereo / quadro
    ImGui::Checkbox("Convert", &s.exportConvertOn);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(120));
    if (caw_ui::beginCombo("##conv_to", s.exportConvert == 1 ? "Mono" : s.exportConvert == 2 ? "Stereo" : "Quadro")) {
        if (ImGui::Selectable("Mono", s.exportConvert == 1)) s.exportConvert = 1;
        if (ImGui::Selectable("Stereo", s.exportConvert == 2)) s.exportConvert = 2;
        if (ImGui::Selectable("Quadro", s.exportConvert == 4)) s.exportConvert = 4;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) helpTip("When checked, the rendered channels fold to the chosen width (channel k -> k mod M summed, the virtual soundcard rule): Mono / Stereo / Quadro. Off = the source width passes through.");
    // ONE FILE vs SEPARATED
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Files", s.exportSplit ? "Each channel separated" : "One file (all channels)")) {
        if (ImGui::Selectable("One file (all channels)", !s.exportSplit)) s.exportSplit = false;
        if (ImGui::Selectable("Each channel separated", s.exportSplit)) s.exportSplit = true;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) helpTip("One file: every channel in one file.\nEach channel separated: the export writes one mono file per channel, named <file>_1, <file>_2 ... (WAV formats).");
    const int channelChoice = s.exportChannels == 1 ? 0 : s.exportChannels == 2 ? 1 : s.exportChannels == 4 ? 2 : 3;
    const char* channels[] = {"Mono", "Stereo", "Quadro", "Other"};
    ImGui::SetNextItemWidth(S(180));
    if (caw_ui::beginCombo("Channels", channels[channelChoice])) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(channels[i], i == channelChoice))
            s.exportChannels = i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 4 : 6;
        ImGui::EndCombo();
    }
    if (channelChoice == 3) {
        ImGui::SetNextItemWidth(S(100));
        if (ImGui::InputInt("Channel count", &s.exportChannels, 0, 0))
            s.exportChannels = std::min(64, std::max(1, s.exportChannels));
    }
    if (ImGui::IsItemHovered()) helpTip("Mono/stereo downmix the project; higher counts preserve channel order and add silence for missing channels.");
    if (s.renderFormat == 4 && s.exportChannels > 2) ImGui::TextDisabled("MP3 supports mono or stereo.");
    if (s.renderFormat == 3 && s.exportChannels > 8) ImGui::TextDisabled("FLAC supports up to 8 channels.");
}
#include "app.h"
void drawAudioExport() {
    if (!g_audioExportOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(560), S(410)), ImGuiCond_FirstUseEver);
    winMaxPrepareT("Audio export");
    if (ImGui::Begin("Audio export", &g_audioExportOpen, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Audio export");
        CsoundSettings& s = g_proj.cs;
        ImGui::TextUnformatted("Output file");
        ImGui::SetNextItemWidth(-S(105));
        inputStr("##export_file", s.renderFile, 0);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            const std::string ext = renderExtension(s.renderFormat) + 1;
            const std::string chosen = plat::chooseFile(true, "Choose audio export file", "Audio file", {ext}, renderFilePath().string());
            if (!chosen.empty()) {
                std::filesystem::path path(chosen);
                path.replace_extension(renderExtension(s.renderFormat));
                s.renderFile = path.string();
            }
        }
        ImGui::PushID("audio_export_options");
        drawExportOptions(s);
        ImGui::PopID();
        ImGui::SetNextItemWidth(S(120));
        if (ImGui::InputDouble("Tail (seconds)", &s.tail, .5, 2, "%.1f"))
            s.tail = std::min(60.0, std::max(0.0, s.tail));
        ImGui::SetNextItemWidth(S(120));
        double expStart = s.exportStart;
        if (ImGui::InputDouble("Start (seconds)", &expStart, 0, 0, "%.2f")) s.exportStart = std::max(0.0, expStart);
        if (ImGui::IsItemHovered()) helpTip("The export begins here. 0 = the project start.");
        ImGui::SetNextItemWidth(S(120));
        double expEnd = s.exportEnd;
        if (ImGui::InputDouble("End (seconds)", &expEnd, 0, 0, "%.2f")) s.exportEnd = std::max(0.0, expEnd);
        if (ImGui::IsItemHovered()) helpTip("The export stops here. 0 = the project end (automatic).\nA value greater than 0 renders exactly that range - useful when the project runs long past the last clip.");
        ImGui::TextDisabled("Destination: %s", renderFilePath().string().c_str());
        if (!g_exportError.empty()) ImGui::TextColored(ImVec4(.98f, .43f, .34f, 1.f), "%s", g_exportError.c_str());
        if (g_renderActive) {
            if (ImGui::Button("Cancel export")) cancelRender();
        } else if (ImGui::Button("Export audio")) startRender();
    }
    ImGui::End();
}
#include "app.h"
void playNotice(const std::string& text, const std::string& shortText )
{
    logLine("[caw] " + text);
    g_playNotice = shortText.empty() ? text : shortText; g_playNoticeT = 10.0;
}
#include "app.h"
void logCsoundLine(const std::string& raw) {
    // Live-code results: the line numbers of a runtime-compiled snippet are not the CSD's line
    // numbers, so they must not be mapped onto clips / editor markers.
    if (raw.rfind("[caw] live code", 0) == 0) { logLine(raw); return; }
    static const std::regex pdClipError(R"re(PD clip\s+(\d+):\s*(.*))re");
    std::smatch pdMatch;
    if (std::regex_search(raw, pdMatch, pdClipError)) {
        logPdClipMessage(std::atoi(pdMatch[1].str().c_str()), pdMatch[2].str());
        return;
    }
    std::string text, name;
    int src = 0;
    const bool mapped = resolveOrcLine(raw, text, name, src);
    LogLine l = classifyLog(mapped ? text : raw);
    const bool errLine = l.level == kLogError;
    if (mapped && g_errCtx.left > 0) l.level = kLogError;
    const std::string mark = errLine ? raw : g_errCtx.text;
    if (errLine) { g_errCtx.left = 6; g_errCtx.text = raw; }
    else if (g_errCtx.left > 0) --g_errCtx.left;
    if (l.level == kLogError) {
        if (mapped) fillLoc(l, name, src, mark);
        else if (!l.clip.empty()) fillLoc(l, l.clip, 0, raw);
    }
    pushLog(std::move(l));
}
#include "app.h"
std::string clipLineMessage(const std::string& l) {
    std::string text, name;
    int src = 0;
    return resolveOrcLine(l, text, name, src) ? text : l;
}
#include "app.h"
void drawMarkItem(ImDrawList* dl, int trackIdx, const Track& track, const TrackMark& mk, float x0, float x1, float top, float bot, uint32_t rgb) {
    const ImU32 line = trackColor(trackIdx, 235, rgb), soft = trackColor(trackIdx, 80, rgb);
    const float rowH = bot - top;
    const float scale = (float)std::min(400, std::max(40, mk.size)) / 100.f;
    // on the timeline a shape or box is at most as big as its row; the score shows the full size
    const float h = std::min(rowH, rowH * scale);
    const float sTop = top + (rowH - h) * 0.5f, sBot = sTop + h;
    ImFont* font = g_markUiFont ? g_markUiFont : ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * (mk.font > 0 ? (float)mk.font / 9.f : 1.f);
    auto centered = [&](float cx, float cy, const char* t) {
        const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, t);
        dl->AddText(font, fs, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), IM_COL32(240, 241, 245, 255), t);
    };
    if (track.kind == 2) {   // graph track: graphic-score shapes only
        const float tw = mk.text.empty() ? 0.f : font->CalcTextSizeA(fs, FLT_MAX, 0.f, mk.text.c_str()).x + S(8);
        const float textX = (float)markgfx::shapeRight(mk.shape, x0, sTop, x1, sBot, mk.ratio / 100.0) + S(4);
        dl->PushClipRect(ImVec2(x0, std::min(top, sTop)), ImVec2(std::max(std::max(x1, x0 + h) + S(2), textX + tw), std::max(bot, sBot)), true);
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), trackColor(trackIdx, 26, rgb), S(3));   // always shown on the timeline: light background + thin border
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bot), trackColor(trackIdx, 110, rgb), S(3), 0, S(1.f));
        drawShapePrims(dl, mk.shape, x0, sTop, x1, sBot, line, soft, mk.ratio / 100.0);
        if (!mk.text.empty()) dl->AddText(font, fs, ImVec2(textX, (top + bot) * 0.5f - fs * 0.5f), IM_COL32(235, 236, 240, 255), mk.text.c_str());
        dl->PopClipRect();
        return;
    }
    const std::string label = mk.text;
    if (mk.type == 1) {   // letter box
        const float w = std::max(h, font->CalcTextSizeA(fs, FLT_MAX, 0.f, label.c_str()).x + S(12));
        dl->AddRectFilled(ImVec2(x0, sTop), ImVec2(x0 + w, sBot), soft, S(2));
        dl->AddRect(ImVec2(x0, sTop), ImVec2(x0 + w, sBot), line, S(2), 0, S(1.8f));
        centered(x0 + w * 0.5f, (sTop + sBot) * 0.5f, label.c_str());
    } else if (mk.type == 2) {   // cue
        const float cx = x0 + h * 0.5f, cy = (sTop + sBot) * 0.5f;
        if (mk.shape == 0) { dl->AddCircleFilled(ImVec2(cx, cy), h * 0.5f, soft); dl->AddCircle(ImVec2(cx, cy), h * 0.5f, line, 0, S(1.8f)); }
        else if (mk.shape == 1) { dl->AddRectFilled(ImVec2(x0, sTop), ImVec2(x0 + h, sBot), soft, S(2)); dl->AddRect(ImVec2(x0, sTop), ImVec2(x0 + h, sBot), line, S(2), 0, S(1.8f)); }
        else if (mk.shape == 2) {
            const ImVec2 d[4] = { ImVec2(x0, cy), ImVec2(cx, sTop), ImVec2(x0 + h, cy), ImVec2(cx, sBot) };
            dl->AddConvexPolyFilled(d, 4, soft); dl->AddPolyline(d, 4, line, ImDrawFlags_Closed, S(1.8f));
        } else if (mk.shape == 4) {   // [ Q ]
            const float t = std::min(S(5), h * 0.3f), xl = x0 + S(1), xr = x0 + h - S(1);
            const ImVec2 lb[4] = { ImVec2(xl + t, sTop), ImVec2(xl, sTop), ImVec2(xl, sBot), ImVec2(xl + t, sBot) };
            const ImVec2 rb[4] = { ImVec2(xr - t, sTop), ImVec2(xr, sTop), ImVec2(xr, sBot), ImVec2(xr - t, sBot) };
            dl->AddPolyline(lb, 4, line, 0, S(1.8f)); dl->AddPolyline(rb, 4, line, 0, S(1.8f));
        } else dl->AddLine(ImVec2(x0 + S(2), sBot - S(1)), ImVec2(x0 + h - S(2), sBot - S(1)), line, S(2));
        centered(cx, cy - (mk.shape == 3 ? S(1) : 0.f), "Q");
        if (!label.empty()) dl->AddText(font, fs, ImVec2(x0 + h + S(5), cy - fs * 0.5f), IM_COL32(235, 236, 240, 255), label.c_str());
    } else if (mk.type == 4) {   // span bracket: horizontal line, 90 degree ends, comment centred above (ends down) or below (ends up)
        const float tk = std::min(rowH * 0.35f, S(7.f) * scale), up = mk.shape == 1 ? 1.f : 0.f;
        const float yl = up > 0.5f ? top + tk + S(1) : bot - tk - S(1), ye = up > 0.5f ? top + S(1) : bot - S(1);
        const float xe = std::max(x1, x0 + S(24));
        const ImVec2 br[4] = { ImVec2(x0, ye), ImVec2(x0, yl), ImVec2(xe, yl), ImVec2(xe, ye) };
        dl->AddPolyline(br, 4, line, 0, S(1.8f));
        if (!label.empty()) {
            const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label.c_str());
            dl->AddText(font, fs, ImVec2((x0 + xe) * 0.5f - sz.x * 0.5f, up > 0.5f ? yl + S(2) : yl - sz.y - S(2)), IM_COL32(235, 236, 240, 255), label.c_str());
        }
    } else if (mk.type == 3) {   // code, coding-style font, colors of the chosen language
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), IM_COL32(24, 25, 30, 255), S(3));
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bot), line, S(3), 0, S(1.2f));
        ImFont* mono = g_markCodeFont ? g_markCodeFont : (g_codeFonts[0] ? g_codeFonts[0] : font);
        const float cfs = fs * 0.85f;
        dl->PushClipRect(ImVec2(x0 + S(3), top), ImVec2(x1 - S(2), bot), true);
        float y = top + S(2);
        std::istringstream lines(label);
        std::string ln;
        while (std::getline(lines, ln) && y + cfs <= bot + 1.f) {
            float x = x0 + S(6);
            for (const markgfx::Tok& tk : markgfx::highlightLine(ln, mk.lang)) {
                dl->AddText(mono, cfs, ImVec2(x, y), codeTokColor(tk.kind), tk.text.c_str());
                x += mono->CalcTextSizeA(cfs, FLT_MAX, 0.f, tk.text.c_str()).x;
            }
            y += cfs + S(1);
        }
        dl->PopClipRect();
    } else {   // comment
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bot), soft, S(3));
        dl->AddRect(ImVec2(x0, top), ImVec2(x1, bot), line, S(3), 0, S(1.5f));
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + S(4), bot), trackColor(trackIdx, 255, rgb), S(2));
        dl->PushClipRect(ImVec2(x0 + S(6), top), ImVec2(x1 - S(2), bot), true);
        dl->AddText(font, fs, ImVec2(x0 + S(8), top + S(3)), IM_COL32(235, 236, 240, 255), label.c_str(), nullptr, std::max(S(20), x1 - x0 - S(12)));
        dl->PopClipRect();
    }
}
#include "app.h"
void drawConsolePanel(ImDrawList* dl, const PRect& r, ImU32 panel, ImU32 frame, ImU32 label) {
    int errCount = 0;
    for (const LogLine& l : g_log) if (l.level == kLogError) ++errCount;
    dl->AddRectFilled(r.a, r.b, panel, S(4));
    dl->AddRect(r.a, r.b, frame, S(4));
    dl->AddText(ImVec2(r.a.x + S(6), r.a.y + S(3)), label, "CONSOLE");

    if (r.b.x - r.a.x > S(430)) {   // Problems: only the errors, each with its clip, line and timeline time (click the red tag to jump there)
        ImGui::SetCursorScreenPos(ImVec2(r.b.x - S(330), r.a.y + S(1)));
        if (errCount > 0) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.38f, 1.f));
        // Pre-click snapshot: the pop must match the push on the click frame (post-click pops corrupted the style).
        const bool problemsOn = g_logProblems;
        if (problemsOn) pushBtnOn(ImVec4(0.55f, 0.22f, 0.18f, 1.f));
        if (ImGui::SmallButton((std::string("Problems ") + std::to_string(errCount) + "##logproblems").c_str())) g_logProblems = !g_logProblems;
        if (problemsOn) ImGui::PopStyleColor(3);
        if (errCount > 0) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) helpTip("Show only the errors. Every error tells the clip, the line of its code and where the clip starts on the timeline. Click the tag to open the clip at that line.");
    }
    ImGui::SetCursorScreenPos(ImVec2(r.b.x - S(196), r.a.y + S(1)));
    if (ImGui::SmallButton("Filter")) ImGui::OpenPopup("##consolefilter");
    if (ImGui::IsItemHovered()) helpTip("How much the console tells, and from which language or clip");
    ImGui::SameLine();
    // Pre-click snapshot: the pop must match the push on the click frame (post-click pops corrupted the style).
    const bool logAutoOn = g_logAuto;
    if (logAutoOn) pushBtnOn(ImVec4(0.93f, 0.50f, 0.15f, 0.85f));
    if (ImGui::SmallButton("Auto")) g_logAuto = !g_logAuto;
    if (logAutoOn) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) helpTip("Auto-scroll: the console follows the newest line");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        g_log.clear();
        g_scErrorLogIndex = -1;
    }
    if (ImGui::BeginPopup("##consolefilter")) {
        ImGui::TextDisabled("Detail");
        ImGui::RadioButton("Important: errors, warnings, what runs", &g_logDetail, kLogImportant);
        ImGui::RadioButton("Messages: + everything the app says", &g_logDetail, kLogMessage);
        ImGui::RadioButton("Everything: + raw output of Csound", &g_logDetail, kLogDetail);
        ImGui::MenuItem("Problems only (errors with clip, line and time)", nullptr, &g_logProblems);
        ImGui::Separator();
        ImGui::TextDisabled("Clip");
        ImGui::SetNextItemWidth(S(200));
        if (caw_ui::beginCombo("##logclip", g_logClip.empty() ? "All clips" : g_logClip.c_str())) {
            if (ImGui::Selectable("All clips", g_logClip.empty())) g_logClip.clear();
            for (const Track& t : g_proj.tracks)
                for (const Clip& c : t.clips)
                    if (ImGui::Selectable(c.name.c_str(), g_logClip == c.name)) g_logClip = c.name;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(S(200));
        ImGui::InputTextWithHint("##logsearch", "search text", g_logSearch, sizeof g_logSearch);
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(r.a.x + S(6), r.a.y + S(24)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 1.f));
    ImGui::BeginChild("##consolelog", ImVec2(r.b.x - r.a.x - S(12), r.b.y - r.a.y - S(30)), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    std::string needle = g_logSearch;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (size_t li = 0; li < g_log.size(); ++li) {
        const LogLine& l = g_log[li];
        if (l.level < g_logDetail || (!g_logClip.empty() && l.clip != g_logClip)) continue;
        if (g_logProblems && l.level != kLogError) continue;
        if (!needle.empty()) {
            std::string low = l.text;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find(needle) == std::string::npos) continue;
        }
        ImVec4 col(0.80f, 0.80f, 0.83f, 1.f);
        if (l.level == kLogError) col = ImVec4(0.95f, 0.40f, 0.35f, 1.f);
        else if (l.text.find("arning") != std::string::npos || l.text.find("WARNING") != std::string::npos) col = ImVec4(0.95f, 0.80f, 0.35f, 1.f);
        else if (l.level >= kLogMessage) col = ImVec4(0.55f, 0.78f, 0.95f, 1.f);
        if (!l.clip.empty()) { ImGui::TextDisabled("%s", l.clip.c_str()); ImGui::SameLine(0, S(6)); }
        if (l.clipId >= 0 && l.level >= kLogImportant) {   // clip-linked error/warning, line when known, and timeline time
            ImGui::PushID((int)li);
            const bool isError = l.level == kLogError;
            ImGui::PushStyleColor(ImGuiCol_Button, isError ? ImVec4(0.55f, 0.22f, 0.12f, 0.55f) : ImVec4(0.50f, 0.38f, 0.10f, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isError ? ImVec4(0.75f, 0.30f, 0.15f, 0.85f) : ImVec4(0.72f, 0.52f, 0.12f, 0.82f));
            if (l.line > 0) {
                const std::string chip = "line " + std::string(l.lineApprox ? "~" : "") + std::to_string(l.line);
                if (ImGui::SmallButton(chip.c_str())) gotoClipLine(l.clipId, l.line);
                if (ImGui::IsItemHovered()) {
                    std::string tip = "Open clip " + l.clip;
                    tip += l.lineApprox
                        ? " at likely line " + std::to_string(l.line) + " (inferred from the engine error)"
                        : " at line " + std::to_string(l.line);
                    tip += " and show it on the timeline.";
                    if (!l.times.empty()) { tip += "\nThe clip starts at:"; for (size_t k = 0; k < l.times.size() && k < 8; ++k) tip += "\n  " + timeText(l.times[k]); }
                    if (l.playhead >= 0.0) tip += "\nPlayhead when this came: " + timeText(l.playhead);
                    helpTip("%s", tip.c_str());
                }
            } else if (!l.lineCandidates.empty()) {
                for (size_t candidateIndex = 0; candidateIndex < l.lineCandidates.size(); ++candidateIndex) {
                    const int candidate = l.lineCandidates[candidateIndex];
                    if (candidateIndex) ImGui::SameLine(0, S(3));
                    ImGui::PushID(candidate);
                    const std::string chip = "line ~" + std::to_string(candidate) + "?";
                    if (ImGui::SmallButton(chip.c_str())) gotoClipLine(l.clipId, candidate);
                    if (ImGui::IsItemHovered())
                        helpTip("Possible location only: Pure Data did not name the failing object. Open %s at line %d to inspect it.",
                                l.clip.c_str(), candidate);
                    ImGui::PopID();
                }
                ImGui::SameLine(0, S(6));
                ImGui::TextDisabled("possible");
            } else {
                if (ImGui::SmallButton("clip")) gotoClipLine(l.clipId, 0);
            }
            if (!l.times.empty()) {
                ImGui::SameLine(0, S(6));
                ImGui::TextDisabled("@ %s%s", timeText(l.times.front()).c_str(),
                    l.times.size() > 1 ? (" +" + std::to_string(l.times.size() - 1)).c_str() : "");
            }
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            ImGui::SameLine(0, S(6));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();
    }
    if (g_logAuto && g_logDirty) ImGui::SetScrollHereY(1.0f);
    g_logDirty = false;
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
#include "app.h"
void drawMeterPanel(float dt, float width, float belowH) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.f));
    ImGui::BeginChild("meterpanel", ImVec2(width, -belowH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const int masterN = g_liveCsound ? g_engineMaster : masterChannels(g_proj.cs);
    const ImU32 panel = IM_COL32(14, 14, 16, 255), frame = IM_COL32(40, 40, 46, 255), label = IM_COL32(130, 130, 140, 255);

    if ((int)g_dispDb.size() != masterN) {
        g_dispDb.assign((size_t)masterN, -90.f);
        g_holdDb.assign((size_t)masterN, -90.f);
        g_holdT.assign((size_t)masterN, 0.f);
    }
    for (int ch = 0; ch < masterN; ++ch) {
        const float pk = (g_monAge < 0.5f && (size_t)ch < g_peakIn.size()) ? g_peakIn[(size_t)ch] : 0.f;
        const float d = 20.f * std::log10(std::max(pk, 1e-5f));
        g_dispDb[(size_t)ch] = std::max(d, g_dispDb[(size_t)ch] - 36.f * dt);
        if (d >= g_holdDb[(size_t)ch]) { g_holdDb[(size_t)ch] = d; g_holdT[(size_t)ch] = 1.5f; }
        else {
            g_holdT[(size_t)ch] -= dt;
            if (g_holdT[(size_t)ch] <= 0.f) g_holdDb[(size_t)ch] = std::max(d, g_holdDb[(size_t)ch] - 20.f * dt);
        }
    }

    {   // drag the left edge to resize the panel, double-click = automatic width again
        ImGui::SetCursorScreenPos(org);
        ImGui::InvisibleButton("##meter_resize", ImVec2(S(6), av.y));
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) g_meterW = 0.f;
        if (ImGui::IsItemActivated() && g_meterW <= 0.f) g_meterW = width / g_ui;
        if (active) g_meterW = std::min(700.f, std::max(120.f, g_meterW - ImGui::GetIO().MouseDelta.x / g_ui));
        if (active || ImGui::IsItemHovered()) dl->AddLine(ImVec2(org.x + S(2), org.y + S(6)), ImVec2(org.x + S(2), org.y + av.y - S(6)), active ? IM_COL32(237, 128, 38, 255) : IM_COL32(150, 150, 160, 255), S(2));
    }
    const ImVec2 mA(org.x + S(8), org.y + S(8)), mB(org.x + av.x - S(4), org.y + av.y - S(8));
    dl->AddRectFilled(mA, mB, panel, S(4));
    dl->AddRect(mA, mB, frame, S(4));
    const float fh = ImGui::GetTextLineHeight();
    const float gap = S(masterN > 8 ? 2.f : 8.f);
    const float bx0 = mA.x + S(34);
    const float availBars = std::max(S(8), (mB.x - mA.x) - S(34) - S(8));
    const float barW = std::max(S(2), std::min(S(20), (availBars - gap * (float)(masterN - 1)) / (float)masterN));
    const float barGroupW = barW * masterN + gap * std::max(0, masterN - 1);
    const float gainBoxW = std::min(std::max(S(42), barW * std::min(masterN, 2) + (masterN > 1 ? gap : 0.f)),
                                    std::max(S(42), mB.x - bx0));
    const bool legacyOutput = g_playing && !g_useCsoundApi;
    if (legacyOutput) ImGui::BeginDisabled();
    const float dbValue = g_proj.cs.masterGain <= 0.f ? -80.f : 20.f * std::log10(g_proj.cs.masterGain);
    int db = std::max(-80, (int)std::lround(dbValue));
    ImGui::SetCursorScreenPos(ImVec2(bx0 + (barGroupW - gainBoxW) * .5f, mA.y + S(6)));
    if (dbIntBox("##master_gain_db", db, -80, kMaxVolumeDb, gainBoxW, "")) {   // whole dB text box: double-click, then type
        g_proj.cs.masterGain = db <= -80 ? 0.f : std::pow(10.f, db / 20.f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        helpTip(legacyOutput ? "Master gain requires CAW host audio. Restart with the Csound API." : "Master gain in whole dB: double-click, then type (-80 = silence, 0 is unity, no upper limit).");
    if (legacyOutput) ImGui::EndDisabled();
    ImGui::SameLine(0, S(3));
    ImGui::TextDisabled("dB");
    const float barTop = mA.y + S(12) + ImGui::GetFrameHeight(), barBot = mB.y - (fh + S(34));   // the peak numbers row, then the FX button (no L / R labels)
    auto dbY = [&](float d) {
        const float f = std::min(1.f, std::max(0.f, (d + 60.f) / 60.f));
        return barBot - std::pow(f, 1.8f) * std::max(0.f, barBot - barTop - S(12.f));
    };
    const bool roomForNumbers = barW + gap >= S(26);
    for (int ch = 0; ch < masterN; ++ch) {
        const float x0 = bx0 + ch * (barW + gap), x1 = x0 + barW;
        const bool clipped = (size_t)ch < g_meterClipHold.size() && g_meterClipHold[(size_t)ch] > 0.f;
        drawLedMeter(dl, x0, x1, barTop, barBot, g_dispDb[(size_t)ch], clipped, false, true);
        if (roomForNumbers) {
            char buf[16];
            if (g_holdDb[(size_t)ch] < -90.f) std::snprintf(buf, sizeof buf, "-inf");
            else std::snprintf(buf, sizeof buf, "%.1f", g_holdDb[(size_t)ch]);
            dl->AddText(ImVec2(x0, barBot + S(4)), IM_COL32(200, 200, 205, 255), buf);
        }
    }
    for (float tk : {0.f, -6.f, -12.f, -24.f, -48.f}) {
        const float y = dbY(tk);
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", (int)tk);
        if (y - fh * 0.5f > barTop) dl->AddText(ImVec2(mA.x + S(6), y - fh * 0.5f), label, buf);
        dl->AddLine(ImVec2(bx0 - S(4), y), ImVec2(bx0, y), label);
    }
    ImGui::SetCursorScreenPos(ImVec2(mA.x, barTop));
    if (ImGui::InvisibleButton("##meterreset", ImVec2(mB.x - mA.x, std::max(S(1), mB.y - barTop - S(30)))))
        for (int ch = 0; ch < masterN && (size_t)ch < g_meterClipHold.size(); ++ch) g_meterClipHold[(size_t)ch] = 0.f;
    if (ImGui::IsItemHovered()) helpTip("%d output channel(s). Red clip LEDs hold for 1.5 seconds.", masterN);

    // the FX button: under the master meter, same width and centering as the master gain box
    ImGui::SetCursorScreenPos(ImVec2(bx0 + (barGroupW - gainBoxW) * .5f - S(2), mB.y - S(28)));
    if (g_masterFxOpen) pushBtnOn(ImVec4(0.20f, 0.42f, 0.24f, 1.f));
    if (ImGui::Button("FX##master", ImVec2(gainBoxW + S(4), 0))) g_masterFxOpen = !g_masterFxOpen;
    if (g_masterFxOpen) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) helpTip("Master FX: up to 8 VST effects, processed in order on the master mix (after every engine, before the master gain). Only for CAW's audio streams - a command-line Csound keeps its own output.");
    endChildTight();
    ImGui::PopStyleColor();
}
#include "app.h"
void drawMonitorSplitter() {
    ImGui::InvisibleButton("##monitor_splitter", ImVec2(-1, S(kMonitorSplitterH)));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImU32 color = ImGui::IsItemActive() ? IM_COL32(237, 128, 38, 255) :
                        (hovered ? IM_COL32(150, 150, 160, 255) : IM_COL32(75, 75, 84, 255));
    const float midY = (a.y + b.y) * 0.5f, midX = (a.x + b.x) * 0.5f;
    dl->AddLine(ImVec2(a.x, midY), ImVec2(b.x, midY), color, S(2));
    for (int k = -2; k <= 2; ++k) dl->AddCircleFilled(ImVec2(midX + k * S(8), midY), S(2), color);   // grip dots
    if (ImGui::IsItemActive()) {
        // the monitor may grow up to most of the window
        const float maxH = std::max(kMonitorMaxH, ImGui::GetIO().DisplaySize.y / g_ui - 230.f);
        g_monitorH = std::min(maxH, std::max(kMonitorMinH, g_monitorH - ImGui::GetIO().MouseDelta.y / g_ui));
    }
}
#include "app.h"
void drawMonitor(float dt) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.f));
    ImGui::BeginChild("monitor", ImVec2(0, S(g_monitorH)), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 org = ImGui::GetCursorScreenPos();
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const int sr = g_proj.cs.sr;

    const ImU32 panel = IM_COL32(14, 14, 16, 255), frame = IM_COL32(40, 40, 46, 255);
    const ImU32 label = IM_COL32(130, 130, 140, 255);

    // ---------------- panel layout (only the visible ones, in the order the user arranged them)
    const float pad = S(8);
    const bool visible[kPanelCount] = {g_viewSpectrum, g_viewScope, g_viewConsole};
    std::vector<int> order;
    for (int k = 0; k < kPanelCount; ++k)
        if (visible[g_panelOrder[k]]) order.push_back(g_panelOrder[k]);
    const int n = (int)order.size();
    const float minW[kPanelCount] = {S(120), S(100), S(160)};
    const float flexWeight[kPanelCount] = {3.f, 2.f, 0.f};
    const float total = av.x - 2 * pad - pad * (float)std::max(0, n - 1);
    float pw[kPanelCount] = {0.f, 0.f, 0.f};
    {
        float fixedSum = 0.f, weightSum = 0.f;
        for (int id : order) {
            float fw = g_panelW[id] > 0.f ? S(g_panelW[id]) : 0.f;
            if (fw <= 0.f) {
                if (id == kPanelConsole) fw = std::max(S(260), av.x * 0.28f);
            }
            if (fw > 0.f) { pw[id] = std::max(minW[id], fw); fixedSum += pw[id]; }
            else weightSum += flexWeight[id];
        }
        const float flex = total - fixedSum;
        if (weightSum > 0.f) {
            for (int id : order) if (pw[id] <= 0.f) pw[id] = std::max(minW[id], flex * flexWeight[id] / weightSum);
        } else if (n > 0) {
            const int last = order.back();   // everything has a fixed width: the last panel takes what is left
            pw[last] = std::max(minW[last], pw[last] + flex);
        }
        float sum = 0.f;
        for (int id : order) sum += pw[id];
        if (sum > total && sum > 0.f) for (int id : order) pw[id] *= total / sum;   // window too small: shrink everything evenly
    }
    const float y0 = org.y + pad, y1 = org.y + av.y - pad;
    PRect pr[kPanelCount] = {};
    {
        float cx = org.x + pad;
        for (int id : order) {
            pr[id] = PRect{ImVec2(cx, y0), ImVec2(cx + pw[id], y1)};
            cx += pw[id] + pad;
        }
    }
    const PRect rs = pr[kPanelSpectrum], rc = pr[kPanelScope], rk = pr[kPanelConsole];

    // drag the gap between two neighbouring panels to resize them (double-click = automatic widths again)
    for (int k = 0; k + 1 < n; ++k) {
        const int ia = order[(size_t)k], ib = order[(size_t)k + 1];
        ImGui::SetCursorScreenPos(ImVec2(pr[ia].b.x, y0));
        ImGui::PushID(ia * 16 + ib + 7000);
        ImGui::InvisibleButton("##panel_split", ImVec2(pad, std::max(S(4), y1 - y0)));
        ImGui::PopID();
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            for (int id = 0; id < kPanelCount; ++id) g_panelW[id] = 0.f;
        if (ImGui::IsItemActivated())
            for (int id : order) g_panelW[id] = pw[id] / g_ui;   // freeze the current widths, then move only the two neighbours
        if (active) {
            float dx = ImGui::GetIO().MouseDelta.x / g_ui;
            dx = std::max(dx, minW[ia] / g_ui - g_panelW[ia]);
            dx = std::min(dx, g_panelW[ib] - minW[ib] / g_ui);
            g_panelW[ia] += dx;
            g_panelW[ib] -= dx;
        }
        const float lx = pr[ia].b.x + pad * 0.5f;
        dl->AddLine(ImVec2(lx, y0 + S(6)), ImVec2(lx, y1 - S(6)),
                    active ? IM_COL32(237, 128, 38, 255) : (ImGui::IsItemHovered() ? IM_COL32(150, 150, 160, 255) : IM_COL32(60, 60, 68, 255)), S(2));
    }

    // ---------------- audio-rate analyzer: FFT, waveform, or spectrogram
    if (g_viewSpectrum) {
        const ImVec2 sA = rs.a, sB = rs.b;
        dl->AddRectFilled(sA, sB, IM_COL32(18, 21, 24, 255), S(5));
        dl->AddRect(sA, sB, IM_COL32(43, 49, 53, 255), S(5));
        dl->AddText(ImVec2(sA.x + S(10), sA.y + S(5)), IM_COL32(192, 200, 202, 255),
                    g_analyzerMode == 0 ? "SPECTRUM" : g_analyzerMode == 1 ? "WAVEFORM" : "SPECTROGRAM");
        const bool spectrumAvailable = (int)g_fftDb.size() == monitor_fft::bins && (g_rawAge < 0.6f || g_specFreeze);
        if (sB.x - sA.x > S(180)) {
            const char* state = g_specFreeze ? "HOLD" : g_rawAge < 0.6f ? "LIVE" : "WAIT";
            dl->AddText(ImVec2(sB.x - S(58), sA.y + S(5)), IM_COL32(111, 181, 171, 255), state);
        }
        const float x0 = sA.x + S(36), x1 = sB.x - S(18), py0 = sA.y + S(30), py1 = sB.y - S(24);
        const float w = x1 - x0, h = py1 - py0;
        const float analysisMax = std::max(100.f, std::min(18000.f, g_rawSampleRate * 0.5f));
        const float fHi = std::max(50.f, std::min(g_specMaxHz, analysisMax));
        const float fLo = std::max(25.f, std::min(g_specMinHz, fHi - 1.f));
        const bool logarithmic = g_specScale == 0;
        auto fx = [&](float f) { return x0 + monitor_math::frequencyPosition(f, fLo, fHi, logarithmic) * w; };
        auto dy = [&](float d) { return py1 - monitor_math::clamp01((d - g_specDbFloor) / -g_specDbFloor) * h; };
        if (w > S(40) && h > S(35)) {
            ImGui::PushClipRect(sA, sB, true);
            if (g_analyzerMode == 0) {
            if (g_specGrid) {
                const int gridStep = h >= S(230) ? 12 : 24;
                for (int d = 0; d >= (int)g_specDbFloor; d -= gridStep) {
                    const float yy = dy((float)d);
                    dl->AddLine(ImVec2(x0, yy), ImVec2(x1, yy), IM_COL32(48, 55, 58, 140));
                    char txt[12]; std::snprintf(txt, sizeof txt, "%d", d);
                    dl->AddText(ImVec2(sA.x + S(6), yy - S(6)), IM_COL32(103, 115, 118, 255), txt);
                }
                static const float ticks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 18000};
                float prevX = -10000.f;
                for (float f : ticks) {
                    if (f < fLo || f > fHi) continue;
                    const float px = fx(f);
                    if (px - prevX < S(34)) continue;
                    dl->AddLine(ImVec2(px, py0), ImVec2(px, py1), IM_COL32(44, 51, 54, 120));
                    char txt[12];
                    if (f >= 1000.f) std::snprintf(txt, sizeof txt, "%gk", f / 1000.f);
                    else std::snprintf(txt, sizeof txt, "%.0f", f);
                    const float textW = ImGui::CalcTextSize(txt).x;
                    const float textX = std::max(sA.x + S(3), std::min(px - textW * 0.5f, sB.x - textW - S(3)));
                    dl->AddText(ImVec2(textX, py1 + S(4)), IM_COL32(125, 137, 139, 255), txt);
                    prevX = px;
                }
            }
            // Smoothed FFT line: a MAX envelope over a +-1/6-octave window per point. The raw line
            // wiggles with window lobes and reads as noise; the envelope is what you can trust.
            const int samples = std::max(32, std::min(1200, (int)(w / S(2.f))));   // one point per ~2 px: the spline does the rest
            std::vector<ImVec2> curve;
            if (g_specStyle == 2 && spectrumAvailable) {
              curve.reserve((size_t)samples + 1);
              const float oct6 = std::pow(2.f, 1.f / 12.f);   // +-1 semitone max window
              for (int j = 0; j <= samples; ++j) {
                const float u = (float)j / samples;
                const float freq = monitor_math::frequencyAt(u, fLo, fHi, logarithmic);
                const int a = std::max(0, (int)std::ceil((freq / oct6) * monitor_fft::fftSize / g_rawSampleRate));
                const int b = std::min(monitor_fft::bins - 1, (int)std::floor((freq * oct6) * monitor_fft::fftSize / g_rawSampleRate));
                int wa = a, wb = b;
                if (wb < wa) {
                    // At low frequencies the +-1-semitone window is narrower than one FFT bin: an
                    // empty window used to drop the curve to the floor, which read as narrow
                    // spikes below ~100 Hz. Take the bin that owns the frequency instead.
                    wa = wb = std::max(0, std::min(monitor_fft::bins - 1,
                            (int)std::lround(freq * monitor_fft::fftSize / g_rawSampleRate)));
                }
                const float* src = g_fftDbDisp.size() == g_fftDb.size() ? g_fftDbDisp.data() : g_fftDb.data();
                float v = g_specDbFloor;
                if (b < a) {
                    // Narrower than one FFT bin (the low end on a log axis): interpolate BETWEEN the bins
                    // (cubic, no overshoot) instead of holding one bin - holding it drew flat steps.
                    v = monitor_math::sample(src, monitor_fft::bins, freq * monitor_fft::fftSize / g_rawSampleRate, true);
                } else {
                    for (int k = wa; k <= wb; ++k) v = std::max(v, src[(size_t)k]);
                }
                v = std::max(g_specDbFloor, std::min(0.f, v));   // the line never leaves the plot (below the floor or above 0 dB)
                curve.push_back(ImVec2(x0 + u * w, dy(v)));
              }
              if (curve.size() > 4) {   // a light 5-point smoothing along the (log) frequency axis: no visible stairs
                  std::vector<ImVec2> sm = curve;
                  for (size_t k = 2; k + 2 < curve.size(); ++k)
                      sm[k].y = (curve[k - 2].y + 2.f * curve[k - 1].y + 3.f * curve[k].y + 2.f * curve[k + 1].y + curve[k + 2].y) / 9.f;
                  curve.swap(sm);
              }
            }
            if (spectrumAvailable) {
                if (g_specStyle == 0) {
                    // Whole-spectrum underlay (user request): EVERY semitone slot is measured
                    // straight from the FFT (strongest bin in the slot), in BOTH Better and
                    // Realistic. With noise - where no tonal peak exists - the axis still fills
                    // completely, so the display never shows empty stretches. Fixed positions,
                    // one hue at low opacity: a backdrop for the peaks, not a second data source.
                    // The backdrop also moves calmly: its level is speed-limited in BOTH
                    // directions (attack AND release), slow in Better, quick in Realistic -
                    // raw noise bins would otherwise flicker frame to frame.
                    {
                        static std::vector<float> s_slotDisp;   // display level per semitone slot, indexed [n - kSlotBase]
                        static constexpr int kSlotBase = -64;   // semitone index of 25 Hz is about -62
                        static constexpr int kSlotCount = 192;
                        if (s_slotDisp.size() != (size_t)kSlotCount) s_slotDisp.assign((size_t)kSlotCount, g_specDbFloor);
                        const float slotDt = std::max(0.001f, ImGui::GetIO().DeltaTime);
                        const float up = g_specBetter ? 240.f : 620.f;    // dB per second rise
                        const float down = g_specBetter ? 90.f : 230.f;   // dB per second fall
                        const double slotHalf = std::pow(2.0, 0.5 / 12.0);
                        const int n0 = (int)std::ceil(12.0 * std::log2(std::max(1.0, (double)fLo) / 440.0));
                        const int n1 = (int)std::floor(12.0 * std::log2(std::max(1.0, (double)fHi) / 440.0));
                        for (int n = n0; n <= n1; ++n) {
                            const int si = n - kSlotBase;
                            if (si < 0 || si >= kSlotCount) continue;
                            const double slotHz = 440.0 * std::pow(2.0, n / 12.0);
                            const double blo = slotHz / slotHalf, bhi = slotHz * slotHalf;
                            if (bhi <= fLo || blo >= fHi) continue;
                            int a = std::max(1, (int)std::ceil(blo * monitor_fft::fftSize / g_rawSampleRate));
                            int b = std::min(monitor_fft::bins - 1, (int)std::floor(bhi * monitor_fft::fftSize / g_rawSampleRate));
                            float v = -160.f;
                            if (b < a) {
                                // narrower than one bin: take the bin that owns the slot center
                                const int c = std::max(1, std::min(monitor_fft::bins - 1,
                                        (int)std::lround(slotHz * monitor_fft::fftSize / g_rawSampleRate)));
                                v = g_fftDb[(size_t)c];
                            } else {
                                for (int i = a; i <= b; ++i) v = std::max(v, g_fftDb[(size_t)i]);
                            }
                            float& d = s_slotDisp[(size_t)si];
                            if (!g_specFreeze)
                                d = v > d ? std::min(v, d + up * slotDt) : std::max(v, d - down * slotDt);
                            if (d <= g_specDbFloor) continue;
                            const float sa = fx((float)std::max<double>(fLo, blo));
                            const float sb = fx((float)std::min<double>(fHi, bhi));
                            if (sb <= sa) continue;
                            const float top = dy(d);
                            if (top < py1) dl->AddRectFilled(ImVec2(sa, top), ImVec2(sb, py1), IM_COL32(121, 215, 192, 55));
                        }
                    }
                    // ONE color, always (designer pass): a two-tone display reads as two different
                    // data sources, which is exactly what confused the reading before.
                    const ImU32 peakCol = IM_COL32(121, 215, 192, 235);
                    if (g_specBetter) {
                        // Better view = the gliding cluster (user mockup, clean mode): ONE anchor
                        // column on the dominant tone, gliding calmly (its position comes from the
                        // tracker), and every detected harmonic LOCKED to the anchor's position
                        // (k x anchor) so the whole cluster glides together - never one bar down
                        // while the next goes up. One hue, two opacities: attention levels, not a
                        // second data source. (Everything else - the full spectrum - is the
                        // underlay above; no loose singles, they read as clutter in dense material.)
                        if (!g_tonePeaks.empty()) {
                            // Anchor stability (designer pass): the strongest peak flickers frame to
                            // frame in dense material, and every harmonic column is positioned
                            // relative to the anchor - an unstable anchor scatters the cluster. The
                            // anchor only changes when another tone is clearly stronger.
                            const monitor_peaks::Peak* anchor = &g_tonePeaks.front();
                            static double s_anchorHz = 0.0;
                            const monitor_peaks::Peak* strongest = anchor;
                            for (const auto& p : g_tonePeaks) if (p.db > strongest->db) strongest = &p;
                            if (s_anchorHz > 0.0) {
                                for (const auto& p : g_tonePeaks) {
                                    if (std::abs(std::log(std::max(1.0, p.frequency) / s_anchorHz)) < std::log(1.02)) {
                                        if (p.db >= strongest->db - 3.0) anchor = &p;   // stays unless beaten by > 3 dB
                                        break;
                                    }
                                }
                            }
                            s_anchorHz = anchor->frequency;
                            const double anchorHalf = std::pow(2.0, 0.75 / 12.0);   // +-3/4 semitone anchor column
                            const double harmHalf = std::pow(2.0, 0.5 / 12.0);      // +-1/2 semitone harmonic columns
                            const ImU32 anchorCol = IM_COL32(121, 215, 192, 242), harmCol = IM_COL32(121, 215, 192, 118);
                            for (size_t pi = 0; pi < g_tonePeaks.size(); ++pi) {
                                const monitor_peaks::Peak& peak = g_tonePeaks[pi];
                                if (peak.db <= g_specDbFloor) continue;
                                double lo, hi;
                                ImU32 col;
                                if (&peak == anchor) {
                                    lo = anchor->frequency / anchorHalf;
                                    hi = anchor->frequency * anchorHalf;
                                    col = anchorCol;
                                } else {
                                    const double r = peak.frequency / anchor->frequency;
                                    const int k = (int)std::lround(r);
                                    if (k < 2 || std::abs(r - (double)k) > 0.03) continue;   // not a harmonic of the anchor: the underlay already shows it
                                    lo = anchor->frequency * k / harmHalf;
                                    hi = anchor->frequency * k * harmHalf;
                                    col = harmCol;
                                }
                                if (hi <= fLo || lo >= fHi) continue;
                                const float a = fx((float)std::max<double>(fLo, lo));
                                const float b = fx((float)std::min<double>(fHi, hi));
                                if (b <= a) continue;
                                const float top = dy((float)peak.db);
                                if (top >= py1) continue;
                                dl->AddRectFilled(ImVec2(a, top), ImVec2(b, py1), col);
                            }
                        }
                    } else {
                        // Realistic: the raw estimated positions, every peak, one color.
                        for (const auto& peak : g_tonePeaks) {
                            const auto bounds = monitor_peaks::visualBounds(peak);
                            if (bounds.second <= fLo || bounds.first >= fHi || peak.db <= g_specDbFloor) continue;
                            const float a = fx((float)std::max<double>(fLo, bounds.first));
                            const float b = fx((float)std::min<double>(fHi, bounds.second));
                            if (b > a) dl->AddRectFilled(ImVec2(a, dy((float)peak.db)), ImVec2(b, py1), peakCol);
                        }
                    }
                } else if (g_specStyle == 3 && !g_columnBands.empty()) {
                    // STATIC band columns: fixed 1/6-octave slots whose x positions never move. Each
                    // slot is measured (nothing interpolated) and drawn as a separate column with a
                    // visible gap and a display smoother (fast attack, release-limited fall) so the
                    // picture reads like a standard RTA instead of flickering (designer pass).
                    if (g_columnDisp.size() != g_columnBands.size()) g_columnDisp.assign(g_columnBands.size(), g_specDbFloor);
                    if (!g_specFreeze) {
                        const float dt = std::max(0.001f, ImGui::GetIO().DeltaTime);
                        // Better: the fall takes its time (user request). Realistic: slower in
                        // general than raw, but responsive.
                        const float release = g_specBetter ? 35.f : 60.f;   // dB per second
                        for (size_t i = 0; i < g_columnBands.size(); ++i) {
                            const float v = (float)g_columnBands[i].db;
                            float& d = g_columnDisp[i];
                            d = v > d ? v : std::max(v, d - release * dt);  // attack instant, release smoothed
                        }
                    }
                    for (size_t i = 0; i < g_columnBands.size(); ++i) {
                        const auto& band = g_columnBands[i];
                        if (band.hi <= fLo || band.lo >= fHi) continue;
                        const float a = fx((float)std::max<double>(fLo, band.lo));
                        const float b = fx((float)std::min<double>(fHi, band.hi));
                        const float gap = std::min(S(1.5f), (b - a) * 0.12f);   // a visible gap between columns
                        if (b - a <= gap * 2) continue;
                        const float db = g_columnDisp[i];
                        if (db <= g_specDbFloor) continue;
                        const float top = dy(db);
                        // One uniform color for every column: the gaps make them noticeable, a
                        // second shade would read as different data (designer pass).
                        dl->AddRectFilled(ImVec2(a + gap, top), ImVec2(b - gap, py1), IM_COL32(121, 215, 192, 225));
                    }
                } else if (curve.size() > 1) {
                    // Smoothed FFT line: a soft fill under the max-envelope curve. The quads are
                    // hard-edged and share EXACT edges - neighbors partition the area, so no pixel
                    // is ever covered twice (an overlap would blend the fill twice and read as a
                    // lighter band of "different data" - the artifact this replaces). Each segment
                    // is split into sub-columns with interpolated tops so the staircase never
                    // visibly pokes above the polyline on descents.
                    for (size_t i = 0; i + 1 < curve.size(); ++i) {   // fill under the line: one exact trapezoid per segment
                        const ImVec2& p0 = curve[i];
                        const ImVec2& p1 = curve[i + 1];
                        if (std::min(p0.y, p1.y) >= py1) continue;
                        const ImVec2 quad[4] = {ImVec2(p0.x, py1), p0, p1, ImVec2(p1.x, py1)};
                        dl->AddConvexPolyFilled(quad, 4, IM_COL32(121, 215, 192, 60));
                    }
                    // Modern-style curve (user request): a Catmull-Rom spline through the envelope
                    // points rendered as one path stroke - smooth joins instead of straight
                    // segments, and the curve still passes exactly through every measured point.
                    dl->PathClear();
                    dl->PathLineTo(curve.front());
                    for (size_t i = 0; i + 1 < curve.size(); ++i) {
                        const ImVec2& p1 = curve[i];
                        const ImVec2& p2 = curve[i + 1];
                        const ImVec2& p0 = i > 0 ? curve[i - 1] : p1;
                        const ImVec2& p3 = i + 2 < curve.size() ? curve[i + 2] : p2;
                        // control points stay inside the plot, so the spline never dips under the floor (or above 0 dB)
                        const ImVec2 c1(p1.x + (p2.x - p0.x) / 6.f, std::min(py1, std::max(py0, p1.y + (p2.y - p0.y) / 6.f)));
                        const ImVec2 c2(p2.x - (p3.x - p1.x) / 6.f, std::min(py1, std::max(py0, p2.y - (p3.y - p1.y) / 6.f)));
                        dl->PathBezierCubicCurveTo(c1, c2, p2);
                    }
                    dl->PathStroke(IM_COL32(170, 240, 218, 245), 0, S(1.5f));
                }
                const int loBin = std::max(1, (int)std::ceil(fLo * monitor_fft::fftSize / g_rawSampleRate));
                const int hiBin = std::min(monitor_fft::bins - 2, (int)std::floor(fHi * monitor_fft::fftSize / g_rawSampleRate));
                if (hiBin >= loBin) {
                    const auto it = std::max_element(g_fftDb.begin() + loBin, g_fftDb.begin() + hiBin + 1);
                    const int peak = (int)(it - g_fftDb.begin());
                    if (*it > g_specDbFloor + 3.f) {
                        const float a = g_fftDb[(size_t)peak - 1], b = *it, c = g_fftDb[(size_t)peak + 1];
                        const float denom = a - 2 * b + c;
                        const float delta = std::abs(denom) > 1e-9f ? std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / denom)) : 0.f;
                        const float peakHz = (peak + delta) * g_rawSampleRate / monitor_fft::fftSize;
                        const float peakDb = b - 0.25f * (a - c) * delta;
                        char readout[80];
                        std::snprintf(readout, sizeof readout, "%s  %.1f Hz   %.1f dBFS", g_fftChannel == 0 ? "L" : g_fftChannel == 1 ? "R" : "MID", peakHz, peakDb);
                        const float tw = ImGui::CalcTextSize(readout).x;
                        if (tw + S(12) < w) dl->AddText(ImVec2(x1 - tw, py0 + S(5)), IM_COL32(166, 191, 184, 255), readout);
                    }
                }
            } else {
                dl->AddText(ImVec2(x0 + S(8), py0 + S(8)), IM_COL32(115, 129, 128, 255), "Waiting for complete audio frame");
            }
            } else if (g_analyzerMode == 1 && g_haveRaw && (g_rawAge < 0.6f || g_specFreeze)) {
                std::vector<ImVec2> points;
                points.reserve(monitor_fft::size);
                const float* waveformLeft = g_specFreeze ? g_analyzerFrozenLeft : g_rawLeft;
                const float* waveformRight = g_specFreeze ? g_analyzerFrozenRight : g_rawRight;
                for (int i = 0; i < monitor_fft::size; ++i) {
                    const float v = g_fftChannel == 1 ? waveformRight[i] : g_fftChannel == 2 ? (waveformLeft[i] + waveformRight[i]) * 0.5f : waveformLeft[i];
                    points.emplace_back(x0 + w * i / (monitor_fft::size - 1.f), (py0 + py1) * 0.5f - std::max(-1.f, std::min(1.f, v)) * h * 0.48f);
                }
                dl->AddPolyline(points.data(), (int)points.size(), IM_COL32(135, 223, 204, 240), 0, S(1));
                char duration[64]; std::snprintf(duration, sizeof duration, "%.1f ms, sample-level", 1000.0 * monitor_fft::size / g_rawSampleRate);
                dl->AddText(ImVec2(x0, py1 + S(3)), IM_COL32(103, 115, 118, 255), duration);
            } else if (g_analyzerMode == 2 && !g_fftHistory.empty() && (g_playing || g_specFreeze)) {
                const int rows = std::min(100, std::max(16, (int)h));
                for (size_t col = 0; col < g_fftHistory.size(); ++col) {
                    const float xa = x0 + w * (float)col / g_fftHistory.size();
                    const float xb = x0 + w * (float)(col + 1) / g_fftHistory.size() + 1;
                    for (int row = 0; row < rows; ++row) {
                        const float u = (float)row / rows;
                        const float hz = monitor_math::frequencyAt(u, fLo, fHi, logarithmic);
                        const float hz2 = monitor_math::frequencyAt((float)(row + 1) / rows, fLo, fHi, logarithmic);
                        const int a = std::max(0, (int)std::floor(hz * monitor_fft::fftSize / g_rawSampleRate));
                        const int b = std::min(monitor_fft::bins - 1, (int)std::ceil(hz2 * monitor_fft::fftSize / g_rawSampleRate));
                        float db = g_specDbFloor;
                        for (int k = a; k <= b; ++k) db = std::max(db, g_fftHistory[col][(size_t)k]);
                        const float q = monitor_math::clamp01((db - g_specDbFloor) / -g_specDbFloor);
                        const ImU32 color = IM_COL32((int)(q * 240), (int)(q * q * 180), (int)(q * 110), 255);
                        dl->AddRectFilled(ImVec2(xa, py1 - h * (row + 1) / rows), ImVec2(xb, py1 - h * row / rows + 1), color);
                    }
                }
                dl->AddText(ImVec2(x0, py1 + S(3)), IM_COL32(103, 115, 118, 255), "older                         now");
                char high[16], low[16];
                std::snprintf(high, sizeof high, "%.0fk", fHi / 1000.f);
                std::snprintf(low, sizeof low, "%.0f", fLo);
                dl->AddText(ImVec2(sA.x + S(4), py0), IM_COL32(145, 155, 158, 255), high);
                dl->AddText(ImVec2(sA.x + S(4), py1 - S(12)), IM_COL32(145, 155, 158, 255), low);
            }
            ImGui::PopClipRect();
        }
        ImGui::SetCursorScreenPos(ImVec2(sA.x, sA.y + S(22)));
        ImGui::InvisibleButton("##specmenu", ImVec2(std::max(1.f, sB.x - sA.x), std::max(1.f, sB.y - sA.y - S(22))));
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (g_analyzerMode == 0 && spectrumAvailable && ImGui::IsItemHovered() &&
            mouse.x >= x0 && mouse.x <= x1 && mouse.y >= py0 && mouse.y <= py1) {
            const float freq = monitor_math::frequencyAt((mouse.x - x0) / w, fLo, fHi, logarithmic);
            dl->AddLine(ImVec2(mouse.x, py0), ImVec2(mouse.x, py1), IM_COL32(165, 190, 182, 105));
            if (g_specStyle == 0) {
                const monitor_peaks::Peak* closest = nullptr;
                double distance = 1e30;
                for (const auto& peak : g_tonePeaks) {
                    const auto bounds = monitor_peaks::visualBounds(peak);
                    if (freq < bounds.first || freq > bounds.second) continue;
                    const double d = std::abs(std::log(freq / peak.frequency));
                    if (d < distance) { distance = d; closest = &peak; }
                }
                if (closest) ImGui::SetTooltip("%.1f Hz  |  %.1f dBFS\nEstimated tonal peak; visual width is not measured bandwidth.",
                                               closest->frequency, closest->db);
            } else if (g_specStyle == 3) {
                const auto found = std::find_if(g_columnBands.begin(), g_columnBands.end(),
                    [freq](const monitor_bands::Band& band) { return freq >= band.lo && freq < band.hi; });
                if (found != g_columnBands.end())
                    ImGui::SetTooltip("%.1f Hz band  |  %.1f dBFS\n%.1f to %.1f Hz; peak-sine reference%s", found->center, found->db, found->lo, found->hi,
                                  found->resolved ? "" : "\nNarrower than the analyzer resolves: level is for the whole window lobe.");
            } else {
                const float bin = freq * monitor_fft::fftSize / g_rawSampleRate;
                const float db = monitor_math::sample(g_fftDb.data(), monitor_fft::bins, bin, false);
                ImGui::SetTooltip("%.1f Hz   %.1f dBFS\nHann-window FFT sample", freq, db);
            }
        }
        if (ImGui::BeginPopupContextItem("##specmenu_popup")) {
            ImGui::TextDisabled("AUDIO ANALYZER");
            ImGui::Separator();
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Spectrum: Tonal peaks", nullptr, g_analyzerMode == 0 && g_specStyle == 0)) { g_analyzerMode = 0; g_specStyle = 0; updateSpectrumDerived(); }
                if (ImGui::MenuItem("Spectrum: Static band columns", nullptr, g_analyzerMode == 0 && g_specStyle == 3)) { g_analyzerMode = 0; g_specStyle = 3; updateSpectrumDerived(); }
                if (ImGui::MenuItem("Spectrum: Smoothed FFT line", nullptr, g_analyzerMode == 0 && g_specStyle == 2)) { g_analyzerMode = 0; g_specStyle = 2; updateSpectrumDerived(); }
                ImGui::Separator();
                if (ImGui::MenuItem("Waveform", nullptr, g_analyzerMode == 1)) g_analyzerMode = 1;
                if (ImGui::MenuItem("Spectrogram", nullptr, g_analyzerMode == 2)) { if (g_analyzerMode != 2) g_fftHistory.clear(); g_analyzerMode = 2; }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Detail")) {
                if (ImGui::MenuItem("Better view", "calm, readable", g_specBetter)) { g_specBetter = true; monitor_peaks::resetTracking(); updateSpectrumDerived(); }
                if (ImGui::MenuItem("Realistic", "raw and fast", !g_specBetter)) { g_specBetter = false; monitor_peaks::resetTracking(); updateSpectrumDerived(); }
                ImGui::Separator();
                ImGui::TextDisabled("Better: calm motion, readable amounts of data.");
                ImGui::TextDisabled("Realistic: every peak, fast fall, short holds.");
                ImGui::EndMenu();
            }
            bool hold = g_specFreeze;
            if (ImGui::MenuItem("Hold display", nullptr, &hold)) {
                if (hold) {
                    std::copy(g_rawLeft, g_rawLeft + monitor_fft::size, g_analyzerFrozenLeft);
                    std::copy(g_rawRight, g_rawRight + monitor_fft::size, g_analyzerFrozenRight);
                    if (g_haveRaw) { analyzeCurrentRaw(); updateSpectrumDerived(); }
                }
                g_specFreeze = hold;
            }
            ImGui::BeginDisabled(g_specFreeze);
            if (ImGui::BeginMenu("Input")) {
                if (ImGui::MenuItem("Left", nullptr, g_fftChannel == 0)) { g_fftChannel = 0; g_fftHistory.clear(); g_fftDb.clear(); g_spectrumBands.clear(); g_columnBands.clear(); g_columnHold.clear(); g_columnDisp.clear(); g_tonePeaks.clear(); monitor_peaks::resetTracking(); }
                if (ImGui::MenuItem("Right", nullptr, g_fftChannel == 1)) { g_fftChannel = 1; g_fftHistory.clear(); g_fftDb.clear(); g_spectrumBands.clear(); g_columnBands.clear(); g_columnHold.clear(); g_columnDisp.clear(); g_tonePeaks.clear(); monitor_peaks::resetTracking(); }
                if (ImGui::MenuItem("Mid (L + R) / 2", nullptr, g_fftChannel == 2)) { g_fftChannel = 2; g_fftHistory.clear(); g_fftDb.clear(); g_spectrumBands.clear(); g_columnBands.clear(); g_columnHold.clear(); g_columnDisp.clear(); g_tonePeaks.clear(); monitor_peaks::resetTracking(); }
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();
            if (ImGui::BeginMenu("Frequency scale")) {
                if (ImGui::MenuItem("Logarithmic", nullptr, g_specScale == 0)) g_specScale = 0;
                if (ImGui::MenuItem("Linear", nullptr, g_specScale == 1)) g_specScale = 1;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Frequency range")) {
                ImGui::SetNextItemWidth(S(170));
                ImGui::SliderFloat("Min Hz", &g_specMinHz, 25.f, 2000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
                ImGui::SetNextItemWidth(S(170));
                ImGui::SliderFloat("Max Hz", &g_specMaxHz, 1000.f, 18000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
                g_specMinHz = std::min(g_specMinHz, g_specMaxHz - 1.f);
                ImGui::TextDisabled("Available up to %.0f Hz", analysisMax);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Level range")) {
                for (int floor : {-60, -90, -120}) {
                    char text[24]; std::snprintf(text, sizeof text, "%d to 0 dB", floor);
                    if (ImGui::MenuItem(text, nullptr, g_specDbFloor == (float)floor)) g_specDbFloor = (float)floor;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Appearance")) {
                ImGui::MenuItem("Grid", nullptr, &g_specGrid);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled("%d-sample Hann, zero padded to %d.", monitor_fft::size, monitor_fft::fftSize);
            ImGui::TextDisabled("Observation spacing %.2f Hz; zero padding does not improve resolution.", (float)g_rawSampleRate / monitor_fft::size);
            ImGui::TextDisabled("A new sound fades in over about %.0f ms; quiet high partials appear later.", 1000.0 * monitor_fft::size / g_rawSampleRate * 0.6);
            ImGui::TextDisabled("Tonal peaks are estimates; use band energy for unsuppressed data.");
            ImGui::EndPopup();
        }
    }

    // ---------------- signal: minimal multi-channel monitor
    if (g_viewScope) {
        const ImVec2 cA = rc.a, cB = rc.b;
        dl->AddRectFilled(cA, cB, IM_COL32(18, 21, 24, 255), S(5));
        dl->AddRect(cA, cB, IM_COL32(43, 49, 53, 255), S(5));
        dl->AddText(ImVec2(cA.x + S(10), cA.y + S(5)), IM_COL32(192, 200, 202, 255), "SIGNAL");
        if (cB.x - cA.x > S(150)) {
            const char* state = g_scopeFreeze ? "HOLD" : g_rawAge >= 0.6f ? "WAIT" : g_scopeDisplay ? "TRIG" : "LIVE";
            dl->AddText(ImVec2(cB.x - S(58), cA.y + S(5)), IM_COL32(111, 181, 171, 255), state);
        }
        const float x0 = cA.x + S(10), x1 = cB.x - S(10), py0 = cA.y + S(28), py1 = cB.y - S(20);
        const float w = x1 - x0, mid = (py0 + py1) * 0.5f, hh = (py1 - py0) * 0.47f;
        if (w > S(35) && hh > S(10)) {
            if (g_scopeGrid) {
                for (int i = 0; i <= 4; ++i) {
                    const float xx = x0 + w * (float)i / 4.f;
                    dl->AddLine(ImVec2(xx, py0), ImVec2(xx, py1), IM_COL32(44, 51, 54, 125));
                }
                for (int i = -1; i <= 1; ++i) {
                    const float yy = mid + hh * 0.5f * (float)i;
                    dl->AddLine(ImVec2(x0, yy), ImVec2(x1, yy),
                                i == 0 ? IM_COL32(68, 79, 81, 180) : IM_COL32(44, 51, 54, 125));
                }
            }
            const bool available = g_scopeFreeze ? g_scopeFrozenHasFrame : (g_haveRaw && g_rawAge < 0.6f);
            const float* left = g_scopeFreeze ? g_rawFrozenLeft : g_rawLeft;
            const float* right = g_scopeFreeze ? g_rawFrozenRight : g_rawRight;
            if (available) {
                const auto window = monitor_math::scopeWindow(left, right, monitor_fft::size, g_rawSampleRate,
                    (float)g_scopeMs, g_scopeDisplay == 1, g_scopeChannels);
                const int count = window.samples - 1;
                ImGui::PushClipRect(ImVec2(x0, py0), ImVec2(x1, py1), true);
                const int traces = g_scopeChannels == 0 ? 2 : 1;
                for (int trace = 0; trace < traces; ++trace) {
                    const int ch = g_scopeChannels == 0 ? 1 - trace : g_scopeChannels == 2 ? 1 : 0;
                    std::vector<ImVec2> points;
                    points.reserve((size_t)count + 1);
                    for (int i = 0; i <= count; ++i) {
                        const float u = (float)i / count;
                        const float pos = window.start + u * count;
                        const float l = monitor_math::sample(left, monitor_fft::size, pos, false);
                        const float r = monitor_math::sample(right, monitor_fft::size, pos, false);
                        const float v = g_scopeChannels == 3 ? (l + r) * 0.5f : ch == 0 ? l : r;
                        points.emplace_back(x0 + u * w, mid - std::max(-1.f, std::min(1.f, v)) * hh);
                    }
                    const ImU32 color = ch == 0 ? IM_COL32(135, 223, 204, 235) : IM_COL32(241, 167, 112, 195);
                    const ImU32 glow = ch == 0 ? IM_COL32(135, 223, 204, 30) : IM_COL32(241, 167, 112, 25);
                    dl->AddPolyline(points.data(), (int)points.size(), glow, 0, S(4.f));
                    dl->AddPolyline(points.data(), (int)points.size(), color, 0, S(1.5f));
                }
                ImGui::PopClipRect();
                char timeText[32];
                const float actual = 1000.f * (window.samples - 1.f) / g_rawSampleRate;
                if (actual >= 1000.f) std::snprintf(timeText, sizeof timeText, "%.2f s", actual / 1000.f);
                else std::snprintf(timeText, sizeof timeText, "%.1f ms", actual);
                dl->AddText(ImVec2(x0, py1 + S(3)), IM_COL32(103, 115, 118, 255), timeText);
                if (g_scopeDisplay == 1 && !window.triggered && w > S(125)) {
                    const char* text = "no trigger";
                    dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(text).x, py1 + S(3)), IM_COL32(103, 115, 118, 255), text);
                }
            } else {
                dl->AddText(ImVec2(x0 + S(6), mid - S(7)), IM_COL32(101, 113, 116, 255), "Waiting for signal");
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(cA.x, cA.y + S(22)));
        ImGui::InvisibleButton("##scopemenu", ImVec2(std::max(1.f, cB.x - cA.x), std::max(1.f, cB.y - cA.y - S(22))));
        if (ImGui::BeginPopupContextItem("##scopemenu_popup")) {
            ImGui::TextDisabled("SIGNAL");
            ImGui::Separator();
            bool freeze = g_scopeFreeze;
            if (ImGui::MenuItem("Hold display", nullptr, &freeze)) {
                if (freeze) {
                    std::copy(g_rawLeft, g_rawLeft + monitor_fft::size, g_rawFrozenLeft);
                    std::copy(g_rawRight, g_rawRight + monitor_fft::size, g_rawFrozenRight);
                    g_scopeFrozenHasFrame = g_haveRaw && g_rawAge < 0.6f;
                }
                g_scopeFreeze = freeze;
            }
            if (ImGui::BeginMenu("Display")) {
                if (ImGui::MenuItem("Free-running sweep", nullptr, g_scopeDisplay == 0)) g_scopeDisplay = 0;
                if (ImGui::MenuItem("Triggered frames", nullptr, g_scopeDisplay == 1)) g_scopeDisplay = 1;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Channels")) {
                if (ImGui::MenuItem("Stereo overlay", nullptr, g_scopeChannels == 0)) g_scopeChannels = 0;
                if (ImGui::MenuItem("Left", nullptr, g_scopeChannels == 1)) g_scopeChannels = 1;
                if (ImGui::MenuItem("Right", nullptr, g_scopeChannels == 2)) g_scopeChannels = 2;
                if (ImGui::MenuItem("Mid (L + R)", nullptr, g_scopeChannels == 3)) g_scopeChannels = 3;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Time window")) {
                for (int ms : {2, 4, 6, 10, 20, 40}) {
                    char text[32]; std::snprintf(text, sizeof text, "%d ms", ms);
                    if (ImGui::MenuItem(text, nullptr, g_scopeMs == ms)) g_scopeMs = ms;
                }
                ImGui::TextDisabled("Max %.1f ms at this sample rate.", 1000.f * monitor_fft::size / g_rawSampleRate);
                ImGui::EndMenu();
            }
            ImGui::MenuItem("Grid", nullptr, &g_scopeGrid);
            ImGui::Separator();
            ImGui::TextDisabled("2048 consecutive audio-rate samples; up to 30 frames/s.");
            ImGui::EndPopup();
        }
    }

    // ---------------- console
    if (g_viewConsole) drawConsolePanel(dl, rk, panel, frame, label);

    if (n == 0) {
        dl->AddText(ImVec2(org.x + pad, org.y + pad), label, "All panels are hidden - use View to show them.");
    }

    // drag a panel title onto another panel to change the order
    {
        int dragFrom = -1, dragTo = -1;
        for (int id : order) {
            ImGui::SetCursorScreenPos(ImVec2(pr[id].a.x, pr[id].a.y));
            ImGui::PushID(id + 9000);
            const float panelWidth = pr[id].b.x - pr[id].a.x;
            const float gripW = id == kPanelConsole ? std::max(S(10), std::min(S(86), panelWidth - S(156))) : std::min(S(86), panelWidth);   // keep the console's Auto / Clear reachable
            ImGui::InvisibleButton("##panel_title", ImVec2(gripW, S(20)));
            ImGui::PopID();
            if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); helpTip("Drag the title onto another panel to change the order"); }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload("CAW_PANEL", &id, sizeof(int));
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CAW_PANEL")) { dragFrom = *(const int*)payload->Data; dragTo = id; }
                ImGui::EndDragDropTarget();
            }
        }
        if (dragFrom >= 0 && dragTo >= 0 && dragFrom != dragTo) {
            int posFrom = 0, posTo = 0;
            for (int k = 0; k < kPanelCount; ++k) { if (g_panelOrder[k] == dragFrom) posFrom = k; if (g_panelOrder[k] == dragTo) posTo = k; }
            const int moved = g_panelOrder[posFrom];
            if (posFrom < posTo) for (int k = posFrom; k < posTo; ++k) g_panelOrder[k] = g_panelOrder[k + 1];
            else for (int k = posFrom; k > posTo; --k) g_panelOrder[k] = g_panelOrder[k - 1];
            g_panelOrder[posTo] = moved;
        }
    }

    endChildTight();
    ImGui::PopStyleColor();
}
#include "app.h"
void drawSettings() {
    if (!g_settingsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(600), S(560)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(260), S(90)), ImGuiCond_FirstUseEver);
    winMaxPrepareT("Settings");
    if (ImGui::Begin("Settings", &g_settingsOpen, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Settings");
        CsoundSettings& s = g_proj.cs;
        const float labelW = S(170);
        auto row = [&](const char* name) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name);
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(S(260));
        };
        const ImVec4 head(0.95f, 0.60f, 0.20f, 1.f);
        if (ImGui::BeginTabBar("##settings_tabs")) {

            if (ImGui::BeginTabItem("General", nullptr, g_settingsGoto == 6 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "AUTOSAVE");
                ImGui::Checkbox("Autosave recovery copy", &s.autosaveEnabled);
                row("Every (minutes)");
                ImGui::SetNextItemWidth(S(100));
                if (ImGui::InputInt("##autosave_minutes", &s.autosaveMinutes, 1, 5))
                    s.autosaveMinutes = std::min(120, std::max(1, s.autosaveMinutes));
                ImGui::TextWrapped("A separate recovery file is saved next to the project. Unsaved projects use the CAW Autosaves folder in your home directory. The original project is never overwritten.");
                ImGui::TextColored(head, "PLUGIN WINDOWS");
                ImGui::Checkbox("Send audio & notes to plugin windows", &vstui::g_vstGuiSignal);
                ImGui::TextWrapped("EQ analyzers, scopes, synth meters and Kontakt keys in the plugin's own window follow what the engine plays. Turn this off if a plugin's window misbehaves - loading screens and sound are not affected, only the signal display.");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Audio: sound card, sample rate, buffers, latency
            if (ImGui::BeginTabItem("Audio", nullptr, g_settingsGoto == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!g_audioDevicesReady && !g_pdEngineOn && !g_playing) refreshAudioDevices(false);
                ImGui::TextColored(head, "SOUND CARD");
                if (s.rtaudio != "portaudio") {   // one audio system only: PortAudio (ASIO / WASAPI / DirectSound ...)
                    s.rtaudio = "portaudio";
                    s.outDev = s.inDev = -1;
                    syncOptionsFromSettings(s);
                    refreshAudioDevices(g_languageChoiceMade && g_langCsound);
                }
                // Like Cabbage: first the device TYPE (ASIO / WASAPI / Core Audio / ALSA ...), then only the devices of that type.
                // EVERY driver type of this operating system is always listed (even when Csound's PortAudio found no device for it), so none is missing.
                struct DriverType { const char* name; const char* tag; bool win, mac, lin; };
                static const DriverType kDrivers[] = {
                    {"All",         "",            true,  true,  true },
                    {"ASIO",        "ASIO",        true,  false, false},
                    {"WASAPI",      "WASAPI",      true,  false, false},
                    {"DirectSound", "DirectSound", true,  false, false},
                    {"MME",         "MME",         true,  false, false},
                    {"WDM-KS",      "WDM-KS",      true,  false, false},
                    {"Core Audio",  "Core Audio",  false, true,  false},
                    {"ALSA",        "ALSA",        false, false, true },
                    {"JACK",        "JACK",        false, false, true },
                    {"OSS",         "OSS",         false, false, true },
                };
                static std::vector<int> osDrivers;   // indices into kDrivers that exist on this OS
                if (osDrivers.empty())
                    for (int i = 0; i < (int)(sizeof(kDrivers) / sizeof(kDrivers[0])); ++i)
                        if (plat::isWindows ? kDrivers[i].win : plat::isMac ? kDrivers[i].mac : kDrivers[i].lin) osDrivers.push_back(i);
                const int driverCount = (int)(sizeof(kDrivers) / sizeof(kDrivers[0]));
                static int devType = -1;   // index into kDrivers; 0 = All
                bool driverHasDevices[16] = {true};
                for (const AudioDevice& d : g_outDevs)
                    for (int t = 1; t < driverCount; ++t)
                        if (cawDeviceHasHostApi(d.name, kDrivers[t].tag)) driverHasDevices[t] = true;
                // Windows: the Windows default card lives in WASAPI. Other systems start with All (PortAudio's default device).
                const int defaultType = plat::isWindows ? 2 : 0;
                if (devType < 0) {
                    devType = (s.outDev < 0 && driverHasDevices[defaultType]) ? defaultType : 0;
                    for (const AudioDevice& d : g_outDevs)
                        if (d.id == s.outDev) for (int t = 1; t < driverCount; ++t)
                            if (cawDeviceHasHostApi(d.name, kDrivers[t].tag)) devType = t;
                }
                if (s.rtaudio == "portaudio") {
                    row("Audio device type");
                    auto typeLabel = [&](int t) { return std::string(kDrivers[t].name) + (t > 0 && !driverHasDevices[t] ? "  (no devices found)" : ""); };
                    if (caw_ui::beginCombo("##devtype", kDrivers[devType].name)) {
                        for (int t : osDrivers) {
                            if (!ImGui::Selectable(typeLabel(t).c_str(), devType == t)) continue;
                            devType = t;
                            const std::string tag = kDrivers[t].tag;
                            const bool isSystemDefaultType = plat::isWindows && tag == "WASAPI";   // -1 follows the Windows default card
                            bool okOut = s.outDev < 0 && (isSystemDefaultType || t == 0), okIn = s.inDev < 0;
                            for (const AudioDevice& d : g_outDevs) if (d.id == s.outDev && cawDeviceHasHostApi(d.name, tag)) okOut = true;
                            for (const AudioDevice& d : g_inDevs) if (d.id == s.inDev && cawDeviceHasHostApi(d.name, tag)) okIn = true;
                            if (t > 0 && !okOut) s.outDev = isSystemDefaultType ? -1 : pickDefaultDevice(g_outDevs, tag, false);
                            if (t > 0 && !okIn) s.inDev = isSystemDefaultType ? -1 : pickDefaultDevice(g_inDevs, tag, true);
                            if (tag == "ASIO") { s.bufHW = std::max(1024, s.bufSW * 4); }
                            syncOptionsFromSettings(s);
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) helpTip("Choose an available audio driver. Refresh the device list after connecting a soundcard.");
                    if (devType > 0 && !driverHasDevices[devType])
                        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "CAW found no %s output devices. Check the driver or choose All.", kDrivers[devType].name);
                }
                const std::string devTag = s.rtaudio == "portaudio" ? std::string(kDrivers[devType].tag) : std::string();
                const bool asioMode = s.rtaudio == "portaudio" && std::string(kDrivers[devType].tag) == "ASIO";
                row("Output device");
                if (devCombo("##out", s.outDev, g_outDevs, devTag)) syncOptionsFromSettings(s);
                if (ImGui::IsItemHovered()) helpTip("The device that plays the sound (-odac). System default or None are possible.");
                if (g_langCsound) {
                    row("Input device");
                    if (devCombo("##in", s.inDev, g_inDevs, devTag)) syncOptionsFromSettings(s);
                    if (ImGui::IsItemHovered()) helpTip("Csound recording input, used by instruments such as inch / in.");
                } else if (g_langPd) ImGui::TextDisabled("PD currently supports output only.");
                row("");
                if (ImGui::Button("Refresh device list")) refreshAudioDevices(g_languageChoiceMade && g_langCsound);
                row("Test sound");
                const bool soundTestBusy = g_testSound.running() || g_playing || g_pdEngineOn || g_pdPreview.active() || g_scEngineOn;
                if (soundTestBusy) ImGui::BeginDisabled();
                if (ImGui::Button("Play test sound")) {
                    int selectedId = s.outDev;
                    if (selectedId == -1 && plat::isWindows) selectedId = pickDefaultDevice(g_outDevs, "WASAPI", false);   // elsewhere: PortAudio default (empty label)
                    std::string selected;
                    if (selectedId >= 0) {
                        selected = "<selected device missing>";
                        for (const auto& device : g_outDevs)
                            if (device.id == selectedId) { selected = device.name; break; }
                    }
                    g_testSound.start(selected, s.outDev == -2, s.sr, s.nchnls, s.bufSW, s.bufHW);
                }
                if (soundTestBusy) ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) helpTip("Plays a quiet tone directly on the selected output. Stop other playback first if the device is busy.");
                ImGui::TextWrapped("Tests the selected output directly. Stop other playback first if the device is busy.");
                const std::string testMessage = g_testSound.message();
                if (!testMessage.empty()) ImGui::TextWrapped("%s", testMessage.c_str());

                ImGui::Spacing();
                ImGui::TextColored(head, "ENGINE");
                row("Sample rate");
                {
                    static const int rates[] = {44100, 48000, 88200, 96000, 192000};
                    if (caw_ui::beginCombo("##sr", (std::to_string(s.sr) + " Hz").c_str())) {
                        for (int r : rates) if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), s.sr == r)) s.sr = r;
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered()) helpTip("Use the sample rate of your sound card / ASIO driver (see its control panel). A different rate makes the system or the driver convert, which can crackle.");
                }
                row("Output channels");
                ImGui::SetNextItemWidth(S(160));
                if (caw_ui::beginCombo("##nchnls", (std::to_string(s.nchnls) + (s.nchnls == 1 ? " channel (mono)" : " channels")).c_str())) {
                    static const int chs[] = {1, 2, 3, 4, 5, 6, 7, 8};
                    for (int c : chs) if (ImGui::Selectable((std::to_string(c) + (c == 1 ? " channel (mono)" : " channels")).c_str(), s.nchnls == c)) s.nchnls = c;
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) helpTip("The project's output channels out1..outN - global for every engine, the master meter shows one bar each, and the export carries all of them.\nHearing follows the soundcard: a card with enough outputs plays each channel directly; a smaller card wraps them (out3 -> out1, out4 -> out2, ...), so you still hear everything as the card allows.\nApplies when the engines (re)start.");
                if (g_langCsound) {
                    row("Control period (ksmps)");
                    ImGui::InputInt("##ksmps", &s.ksmps, 0, 0);
                    s.ksmps = std::min(4096, std::max(1, s.ksmps));
                    ImGui::TextDisabled("kr = sr / ksmps = %.1f Hz", (double)s.sr / std::max(1, s.ksmps));
                } else if (g_langPd) ImGui::TextDisabled("PD: stereo output, 64-sample DSP ticks.");

                ImGui::Spacing();
                ImGui::TextColored(head, "BUFFER AND LATENCY");
                {
                    row("Audio buffer size");
                    {
                        static const int sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
                        const double srv = (double)std::max(1000, s.sr);
                        char cur[64];
                        std::snprintf(cur, sizeof cur, "%d samples (%.1f ms)", s.bufSW, 1000.0 * s.bufSW / srv);
                        if (caw_ui::beginCombo("##bufsize", cur)) {
                            for (int n : sizes) {
                                char item[64];
                                std::snprintf(item, sizeof item, "%d samples (%.1f ms)", n, 1000.0 * n / srv);
                                if (ImGui::Selectable(item, s.bufSW == n)) { s.bufSW = n; s.bufHW = std::max(1024, n * 4);   /* Csound hardware buffer: at least 4 periods, 2 periods crackles */ syncOptionsFromSettings(s); }
                            }
                            ImGui::EndCombo();
                        }
                        if (ImGui::IsItemHovered()) helpTip("Like the buffer size in Cabbage: ONE number. With ASIO choose the buffer size of your ASIO driver (its control panel); the hardware buffer is set to twice that.\nSmaller = less latency but more risk of crackling.");
                    }
                    row("Sound sync (ms)");
                    ImGui::SliderInt("##syncms", &s.syncMs, -100, 400, "%d ms");
                    if (ImGui::IsItemHovered()) helpTip("The timeline and the meters are moved by the output latency so that you hear a note when the playhead reaches it.\nASIO drivers often have more latency than the buffer size says: raise this until the sound meets the playhead. Also moves the meters.");
                    const double srv = (double)std::max(1000, s.sr);
                    const double outMs = 1000.0 * (double)s.bufHW / srv;
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.f), "Output latency about %.1f ms (+ %d ms sync)", outMs, s.syncMs);
                    {
                        char btn[96];
                        std::snprintf(btn, sizeof btn, "Apply %.0f ms latency to the sync slider", outMs);
                        if (ImGui::Button(btn)) s.syncMs = std::min(400, std::max(-100, (int)std::lround(outMs)));
                        if (ImGui::IsItemHovered()) helpTip("Copies the output latency shown above into the Sound sync slider.");
                    }
                    if (asioMode) ImGui::TextDisabled("ASIO: ONE buffer size. It must be the buffer of the ASIO driver itself (its control panel, or Voicemeeter's ASIO buffer setting). -B (hardware ring) is set to 4x that automatically.");
                    if (!asioMode && caw_ui::treeNode("Advanced buffers (-b / -B)")) {
                        row("Software buffer (-b)");
                        if (ImGui::InputInt("##bufsw", &s.bufSW, 64, 256)) { s.bufSW = std::min(16384, std::max(16, s.bufSW)); syncOptionsFromSettings(s); }
                        row("Hardware buffer (-B)");
                        if (ImGui::InputInt("##bufhw", &s.bufHW, 256, 1024)) { s.bufHW = std::min(65536, std::max(16, s.bufHW)); syncOptionsFromSettings(s); }
                        ImGui::TextDisabled("Input latency about %.1f ms | round trip %.1f ms", outMs, outMs * 2.0);
                        ImGui::TreePop();
                    }
                    if (s.bufHW < s.bufSW) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.f), "-B should be at least as large as -b");
                    else if (!asioMode && s.bufHW % std::max(1, s.bufSW) != 0) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "-B should be a multiple of -b (e.g. 2 times), otherwise the sound can glitch");
                    if (g_langCsound) {
                        if (s.bufSW < s.ksmps) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.f), "-b should be at least ksmps");
                        else if (s.bufSW % std::max(1, s.ksmps) != 0) ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "-b should be a multiple of ksmps");
                    }
                }
                if (g_langCsound) {
                    ImGui::Spacing();
                    ImGui::Checkbox("Send meter data from Csound (master, scope, spectrum, track meters)", &s.monitorFeed);
                    if (ImGui::IsItemHovered()) helpTip("Adds a small monitor instrument to the engine. Untick it if Csound reports an error about it, or to give a slow computer more room (less crackling). Applies on the next Run.");
                }
                ImGui::Spacing();
                ImGui::TextColored(head, "RENDER (LKD freeze + whole-project export)");
                row("Render mode");
                ImGui::SetNextItemWidth(S(240));
                if (caw_ui::beginCombo("##rendermode", s.renderMode == 0 ? "Offline (faster than real time)" : "Real-time capture")) {
                    if (ImGui::Selectable("Offline (faster than real time)", s.renderMode == 0)) s.renderMode = 0;
                    if (ImGui::Selectable("Real-time capture", s.renderMode == 1)) s.renderMode = 1;
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) helpTip(s.renderMode == 0
                    ? "Offline: every engine renders through its own offline pipeline (Csound host, PD C++ host, SC NRT, sample engine) at full CPU speed, silently and faster than the song. SuperCollider tracks need the NRT tools."
                    : "Real-time: the transport plays once (audibly) and the mix is captured - works for every engine, but takes as long as the song.");
                if (s.renderMode == 1) ImGui::TextDisabled("Real-time: LKD freezes need the track's engine running; the export plays the whole project once.");
                else ImGui::TextDisabled("Offline: no live engine needed (a Csound track needs Csound; SC needs the NRT tools).");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Programs
            if (ImGui::BeginTabItem("Programs", nullptr, g_settingsGoto == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "PROGRAMS");
                if (g_langCsound) {
                    static std::string csTestResult, csShownTyped, csShownExe;
                    ImGui::Spacing();
                    row("Csound");
                    if (ImGui::Button("Browse##csoundexe")) {
                        // The dialog opens in CAW's own folder: picking the csound.exe there
                        // (e.g. <CAW folder>\Csound\bin\csound.exe) is one click.
                        const auto path = plat::chooseFile(false, "Choose csound.exe", "Csound program", {"exe"}, plat::cawOwnFolder());
                        if (!path.empty()) { s.csoundExe = path; csTestResult.clear(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Test##csoundexe")) {
                        // The same check the engine runs before Play: load the Csound library beside the
                        // chosen exe and compile tiny abs / sqrt / pow probe orchestras.
                        const bool ok = ensureCsoundMathProbe(true);
                        csTestResult = ok ? std::string("Test passed: the meters use ") + (g_mathHasAbs ? "abs" : "sqrt(x*x)") + "."
                                          : "Test failed: this Csound cannot compile abs or sqrt" + (g_mathProbeDetail.empty() ? std::string(".") : ": " + g_mathProbeDetail);
                        logLine("[caw] Csound test: " + csTestResult);
                    }
                    if (csShownTyped != s.csoundExe) { csShownTyped = s.csoundExe; csShownExe = csoundExePath(); }
                    ImGui::TextDisabled("%s. ", plat::csoundProgramName());
                    ImGui::TextWrapped("CAW will use: %s", csShownExe.empty() ? "(no Csound found - Browse for a csound.exe)" : csShownExe.c_str());
                    if (!csTestResult.empty()) ImGui::TextWrapped("%s", csTestResult.c_str());
                }
                if (g_langSc) {
                    ImGui::Spacing();
                    row("SuperCollider language (sclang)");
                    if (g_scEngine.embedded()) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(S(330));
                    if (inputStr("##sc_sclang", g_scLangPath, 0)) saveScLangPath();
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##scsclang")) {
                        const auto path = plat::chooseFile(false, "Choose SuperCollider language", "SuperCollider language", plat::isWindows ? std::vector<std::string>{"exe"} : std::vector<std::string>{}, "");
                        if (!path.empty()) { g_scLangPath = path; saveScLangPath(); }
                    }
                    if (g_scEngine.embedded()) ImGui::EndDisabled();
                    ImGui::TextDisabled(plat::isWindows ? "Choose sclang.exe from the CAW SuperCollider build. CAW embeds libscsynth and shares its PortAudio output with Csound and PD." : "Choose sclang from CAW's SuperCollider build (~/.caw_deps/supercollider/runtime-x64/SuperCollider/sclang). A normal SuperCollider install does not work.");
                    row("SuperCollider synthesis library");
                    if (g_scEngine.embedded()) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(S(330));
                    if (inputStr("##sc_synthlib", g_scSynthPath, 0)) saveScSynthPath();
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##scsynthlib")) {
                        const char* extension = plat::isWindows ? "dll" : plat::isMac ? "dylib" : "so";
                        const auto path = plat::chooseFile(false, "Choose libscsynth", "SuperCollider synthesis library", {extension}, "");
                        if (!path.empty()) { g_scSynthPath = path; saveScSynthPath(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Auto##scsynthlib")) { g_scSynthPath.clear(); saveScSynthPath(); }
                    if (g_scEngine.embedded()) ImGui::EndDisabled();
                    ImGui::TextDisabled(plat::isWindows ? "Usually libscsynth.dll beside sclang.exe. Auto finds it in CAW's cached build/runtime folders." : "Usually libscsynth.so / .dylib beside sclang. Auto finds it in ~/.caw_deps/supercollider.");
                }
                if (g_langPd) {
                    static std::string pdTestResult;
                    ImGui::Spacing();
                    row("Pure Data library");
                    if (g_pdEngineOn || g_pdPreview.active()) ImGui::BeginDisabled();
                    ImGui::SetNextItemWidth(S(245));
                    if (inputStr("##pd_library", g_pdLibraryPath, 0)) { savePdLibraryPath(); pdTestResult.clear(); }
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##pdlib")) {
                        const auto path = plat::chooseFile(false, "Choose libpd library", "Library", plat::isWindows ? std::vector<std::string>{"dll"} : plat::isMac ? std::vector<std::string>{"dylib"} : std::vector<std::string>{"so"}, "");
                        if (!path.empty()) { g_pdLibraryPath = path; savePdLibraryPath(); pdTestResult.clear(); }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Test##pdlib")) {
                        std::string error;
                        pdTestResult = testPdLibrary(error) ? "Library test passed." : "Library test failed: " + error;
                        logLine("[caw] " + pdTestResult);
                    }
                    if (g_pdEngineOn || g_pdPreview.active()) ImGui::EndDisabled();
                    ImGui::TextDisabled("Select the libpd library. The test checks loading, symbols and a test patch.");
                    if (!pdTestResult.empty()) ImGui::TextWrapped("%s", pdTestResult.c_str());
                }
                row("MP3 encoder");
                ImGui::TextDisabled("Automatic: native encoder, bundled FFmpeg, or FFmpeg on PATH.");
#ifdef CAW_VST3
                ImGui::Spacing();
                ImGui::TextColored(head, "VST3 PLUGINS");
                {
                    const auto plugins = vstui::catalogSnapshot();
                    std::set<std::string> bundles;
                    for (const auto& info : plugins) bundles.insert(info.path);
                    const bool scanDone = vstui::catalog().done;
                    if (!scanDone && plugins.empty())
                        ImGui::TextDisabled("Scanning the VST3 folders...");
                    else
                        ImGui::TextDisabled("%d plugin%s in %d .vst3 file%s.", (int)plugins.size(), plugins.size() == 1 ? "" : "s",
                                            (int)bundles.size(), bundles.size() == 1 ? "" : "s");
                }
                if (ImGui::Button("Scan VST3 plugins##vstscan")) {
                    // A scan already done is restarted; the very first one runs from scratch.
                    if (vstui::catalog().done) vstui::startScan(true); else vstui::startScan();
                }
                ImGui::SameLine();
                if (ImGui::Button("Add folder...##vstaddfolder")) {
                    const auto dir = plat::chooseFolder("Choose a VST3 folder");
                    if (!dir.empty()) {
                        auto dirs = caw::vst3ExtraDirs();
                        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
                            dirs.push_back(dir);
                            caw::setVst3ExtraDirs(dirs);
                        }
                        if (vstui::catalog().done) vstui::startScan(true); else vstui::startScan();
                    }
                }
                ImGui::TextDisabled("The standard VST3 folders are always scanned. Add any folder that holds .vst3 files or bundles.");
                {
                    auto dirs = caw::vst3ExtraDirs();
                    int removeAt = -1;
                    for (size_t i = 0; i < dirs.size(); ++i) {
                        ImGui::PushID((int)i);
                        ImGui::SetNextItemWidth(S(330));
                        inputStr("##vstdir", dirs[i], ImGuiInputTextFlags_ReadOnly);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove##vstdir")) removeAt = (int)i;
                        ImGui::PopID();
                    }
                    if (removeAt >= 0) {
                        dirs.erase(dirs.begin() + removeAt);
                        caw::setVst3ExtraDirs(dirs);
                        if (vstui::catalog().done) vstui::startScan(true); else vstui::startScan();
                    }
                }
#endif
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Editor
            if (ImGui::BeginTabItem("Editor", nullptr, g_settingsGoto == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "PROGRAM INTERFACE");
                row("Program font size");
                ImGui::SliderFloat("##program_font_size", &g_proj.cs.programFontSize, 12.f, 26.f, "%.0f px");
                ImGui::TextColored(head, "CODE EDITOR");
                row("Code font size");
                ImGui::SliderFloat("##editor_font_size", &g_proj.cs.editorFontSize, 10.f, 30.f, "%.0f px");
                row("Font style");
                const char* editorStyles[] = {"Regular", "Bold", "Italic"};
                g_proj.cs.editorFontStyle = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
                if (caw_ui::beginCombo("##editor_font_style", editorStyles[g_proj.cs.editorFontStyle])) {
                    for (int i = 0; i < 3; ++i)
                        if (ImGui::Selectable(editorStyles[i], g_proj.cs.editorFontStyle == i)) g_proj.cs.editorFontStyle = i;
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("Source is syntax-highlighted live while you edit.");
                ImGui::TextColored(head, "PLUGIN WINDOWS");
                if (ImGui::Checkbox("Windows compatibility scaling (all VST editors)", &vstui::g_editorSystemScaling))
                    vstui::applyEditorSystemScaling(g_proj, vstui::g_editorSystemScaling);   // only the windows are recreated: patches and sound keep running
                if (ImGui::IsItemHovered()) helpTip("General for every VST / VSTi editor: let Windows scale the plugin window (bitmap scaling). Turn off if an editor is cropped, draws blurry or mouse clicks land on the wrong knob. Changing it reopens the visible editors.");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Advanced: the raw Csound options + header
            if (g_langCsound && ImGui::BeginTabItem("Advanced", nullptr, g_settingsGoto == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "CSOUND OPTIONS + HEADER (type here)");
                ImGui::TextDisabled("Everything on the Audio tab is also written here. Type any Csound flag (a line starting with -) or a header value.");
                static std::string text;
                static bool editing = false;
                if (!editing) text = engineSettingsToText(s);
                const ImVec2 boxSize(-FLT_MIN, S(230));
                if (inputMulti("##engine_text", text, boxSize, 0)) applyEngineText(s, text);
                editing = ImGui::IsItemActive();
                const std::string problems = engineTextProblems(text);
                if (!problems.empty()) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.f), "%s", problems.c_str());
                const std::string effective = s.options.empty() ? defaultOptions(s) : s.options;
                if (effective.find("-odac") == std::string::npos && effective.find("-o") == std::string::npos)
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.f), "No -odac in the options: Csound will not play to the sound card.");
                ImGui::TextDisabled(
                    "Options: any Csound flag, e.g.  -odac -+rtaudio=portaudio -b1024 -B4096 -d\n"
                    "sr / ksmps / kr   sample rate, samples per control period (kr = sr / ksmps)\n"
                    "nchnls / nchnls_i number of output / input channels\n"
                    "0dbfs             value of digital full scale (1 = floating point -1..1)\n"
                    "A4                tuning of A4 in Hz (440 default)\n"
                    "-b N / -B N       software / hardware buffer in sample frames; -odacN picks the output device");
                ImGui::EndTabItem();
            }

            // ---------------------------------------------------------- Files: project media references
            if (ImGui::BeginTabItem("Files", nullptr, g_settingsGoto == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextColored(head, "PROJECT FILES");
                ImGui::TextWrapped("File stores named references. CAW saves each path but does not copy the file.");
                ImGui::Spacing();
                ImGui::TextDisabled("File storage");
                {   // one combo, the "FX only" style the VSTe audio-out mode uses: where CAW saves the files it creates
                    static const char* storeModes[] = {"CAW folder (user directory)", "Inside the project folder"};
                    int storeMode = g_renderInProjectFolder ? 1 : 0;
                    if (caw_ui::combo("##file_storage", &storeMode, storeModes, 2))
                        g_renderInProjectFolder = storeMode == 1;
                    if (ImGui::IsItemHovered())
                        helpTip("Pick where CAW saves the files it creates (renders and other exports). Each kind gets its own folder inside exports.");
                }
                ImGui::Spacing();
                if (ImGui::Button("Open File pool")) g_audioPoolOpen = true;
                ImGui::SameLine();
                if (ImGui::Button("Open Includes")) g_includePoolOpen = true;
                ImGui::SameLine();
                ImGui::TextDisabled("%zu file(s), %zu include(s)", g_proj.audioPool.size(), g_proj.includes.size());
                ImGui::Separator();
                if (g_proj.audioPool.empty() && g_proj.includes.empty()) {
                    ImGui::TextDisabled("No files or includes in this project yet.");
                } else {
                    ImGui::BeginChild("##settings_files_list", ImVec2(0, 0), true);
                    for (const AudioAsset& asset : g_proj.audioPool) {
                        std::error_code ec;
                        const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                        ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.60f, 1.f) : ImVec4(0.95f, 0.55f, 0.35f, 1.f),
                                           "%s", exists ? "Available" : "Missing");
                        ImGui::SameLine();
                        ImGui::Text("%s", asset.name.c_str());
                        ImGui::Indent(S(18));
                        ImGui::TextWrapped("%s", asset.path.c_str());
                        if (!exists) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.f), "File not found on this computer");
                        ImGui::Unindent(S(18));
                    }
                    for (const IncludeAsset& asset : g_proj.includes) {
                        std::error_code ec;
                        const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                        ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.60f, 1.f) : ImVec4(0.95f, 0.55f, 0.35f, 1.f),
                                           "%s", exists ? "Available" : "Missing");
                        ImGui::SameLine();
                        ImGui::Text("%s  (%s)", asset.name.c_str(), asset.score ? "Score include" : "Orchestra include");
                        ImGui::Indent(S(18));
                        ImGui::TextWrapped("%s", asset.path.c_str());
                        if (!exists) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.f), "File not found on this computer");
                        ImGui::Unindent(S(18));
                    }
                    ImGui::EndChild();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            g_settingsGoto = -1;
        }
    }
    ImGui::End();
}
#include "app.h"
void drawUdoPool() {
    if (!g_udoPoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(920), S(600)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(220), S(100)), ImGuiCond_FirstUseEver);
    bool open = g_udoPoolOpen;
    winMaxPrepareT("UDO");
    if (ImGui::Begin("UDO", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("UDO");
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
        ImGui::TextDisabled("Project-wide Csound opcodes, available to every instrument.");
        if (ImGui::Button("Add UDO template")) {
            const auto entries = projectUdoEntries();
            std::set<std::string> used;
            for (const UdoPoolEntry& entry : entries) used.insert(entry.name);
            std::string name = "MyUDO";
            for (int n = 2; used.count(name); ++n) name = "MyUDO" + std::to_string(n);
            if (!g_proj.udoCode.empty() && g_proj.udoCode.back() != '\n') g_proj.udoCode.push_back('\n');
            g_proj.udoCode += "\nopcode " + name + ", a, a\naIn xin\nxout aIn\nendop\n";
            scanUserOpcodes(g_proj.udoCode);
            g_udoPoolSelected = (int)projectUdoEntries().size() - 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Select a name to edit only that opcode.");
        ImGui::Separator();

        const std::vector<UdoPoolEntry> entries = projectUdoEntries();
        if (entries.empty()) g_udoPoolSelected = -1;
        else if (g_udoPoolSelected < 0 || g_udoPoolSelected >= (int)entries.size()) g_udoPoolSelected = 0;
        ImGui::BeginChild("##udo_name_list", ImVec2(S(g_udoSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "UDO NAMES");
        ImGui::Separator();
        if (entries.empty()) {
            ImGui::TextWrapped("No UDOs found yet. Add a template to get started.");
        } else {
            for (size_t i = 0; i < entries.size(); ++i) {
                ImGui::PushID((int)i);
                if (ImGui::Selectable(entries[i].name.c_str(), g_udoPoolSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                    g_udoPoolSelected = (int)i;
                ImGui::TextDisabled("%s <- %s", entries[i].outputs.c_str(), entries[i].inputs.c_str());
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        poolSplitter("##udo_splitter", g_udoSidebarWidth);
        ImGui::BeginChild("##udo_source", ImVec2(0, 0), false);
        if (g_udoPoolSelected >= 0 && g_udoPoolSelected < (int)entries.size()) {
            const UdoPoolEntry& selected = entries[(size_t)g_udoPoolSelected];
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%s", selected.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s <- %s", selected.outputs.c_str(), selected.inputs.c_str());
            ImGui::Separator();
            std::string source = selected.source;
            const bool hasTrailingNewline = !source.empty() && source.back() == '\n';
            const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
            if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
            ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
            const int editorKey = -200001 - g_udoPoolSelected;
            if (codeEditorBody(g_codeEditors, editorKey, source, 0.f, 0, false)) {
                if (hasTrailingNewline && (source.empty() || source.back() != '\n')) source.push_back('\n');
                g_proj.udoCode.replace(selected.begin, selected.end - selected.begin, source);
                scanUserOpcodes(g_proj.udoCode);
            }
            ImGui::SetWindowFontScale(1.f);
            if (g_codeFonts[style]) ImGui::PopFont();
        } else {
            ImGui::TextDisabled("Add a UDO template to get started.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
    g_udoPoolOpen = open;
}
#include "app.h"
void drawInstrPool() {
    if (!g_instrPoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(920), S(600)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(220), S(100)), ImGuiCond_FirstUseEver);
    bool open = g_instrPoolOpen;
    winMaxPrepareT("Instrument pool");
    if (ImGui::Begin("Instrument pool", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Instrument pool");
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
        ImGui::TextDisabled("Project-wide instr definitions. Schedule them from clip code; use names distinct from clips.");
        if (ImGui::Button("Add instrument template")) {
            std::set<std::string> used;
            for (const std::string& source : g_proj.instrPool) used.insert(poolInstrName(source));
            for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) used.insert(c.name);
            std::string name = "PoolInstr";
            for (int n = 2; used.count(name); ++n) name = "PoolInstr" + std::to_string(n);
            g_proj.instrPool.push_back("instr " + name + "\n; Add Csound code here\nendin\n");
            g_instrPoolSelected = (int)g_proj.instrPool.size() - 1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Select an instrument to edit it.");
        ImGui::Separator();
        if (g_proj.instrPool.empty()) g_instrPoolSelected = -1;
        else if (g_instrPoolSelected < 0 || g_instrPoolSelected >= (int)g_proj.instrPool.size()) g_instrPoolSelected = 0;
        ImGui::BeginChild("##instr_name_list", ImVec2(S(g_instrSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "INSTRUMENTS");
        ImGui::Separator();
        for (size_t i = 0; i < g_proj.instrPool.size(); ++i) {
            ImGui::PushID((int)i);
            const std::string name = poolInstrName(g_proj.instrPool[i]);
            if (ImGui::Selectable(name.c_str(), g_instrPoolSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                g_instrPoolSelected = (int)i;
            ImGui::PopID();
        }
        ImGui::EndChild();
        poolSplitter("##instr_splitter", g_instrSidebarWidth);
        ImGui::BeginChild("##instr_source", ImVec2(0, 0), false);
        if (g_instrPoolSelected >= 0 && g_instrPoolSelected < (int)g_proj.instrPool.size()) {
            const int idx = g_instrPoolSelected;
            const std::string name = poolInstrName(g_proj.instrPool[(size_t)idx]);
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%s", name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Remove instrument")) {
                g_proj.instrPool.erase(g_proj.instrPool.begin() + idx);
                g_instrPoolSelected = -1;
                g_codeEditors.clear();
            } else {
                ImGui::Separator();
                const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
                if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
                ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
                (void)codeEditorBody(g_codeEditors, -300001 - idx, g_proj.instrPool[(size_t)idx], 0.f, 0, false);
                ImGui::SetWindowFontScale(1.f);
                if (g_codeFonts[style]) ImGui::PopFont();
            }
        } else ImGui::TextDisabled("Add an instrument template to get started.");
        ImGui::EndChild();
    }
    ImGui::End();
    g_instrPoolOpen = open;
}
#include "app.h"
void drawAudioPool() {
    if (!g_audioPoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(820), S(540)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(260), S(120)), ImGuiCond_FirstUseEver);
    bool open = g_audioPoolOpen;
    winMaxPrepareT("File pool");
    if (ImGui::Begin("File pool", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("File pool");
        if (ImGui::Button("Add file")) {
            const std::string picked = chooseAudioPath();
            if (!picked.empty()) {
                std::error_code ec;
                std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(picked), ec);
                if (ec) p = std::filesystem::path(picked);
                std::string name = p.filename().string();
                if (name.empty()) name = "File";
                const auto samePath = std::find_if(g_proj.audioPool.begin(), g_proj.audioPool.end(),
                    [&](const AudioAsset& a) { return a.path == p.string(); });
                if (samePath != g_proj.audioPool.end()) {
                    g_audioPoolSelected = (int)std::distance(g_proj.audioPool.begin(), samePath);
                } else {
                    g_proj.audioPool.push_back({name, p.string()});
                    g_audioPoolSelected = (int)g_proj.audioPool.size() - 1;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Files stay on disk. Refer to their names with cawfile in code.");
        ImGui::Separator();

        if (g_proj.audioPool.empty()) g_audioPoolSelected = -1;
        else if (g_audioPoolSelected < 0 || g_audioPoolSelected >= (int)g_proj.audioPool.size()) g_audioPoolSelected = 0;
        ImGui::BeginChild("##audio_list", ImVec2(S(g_audioSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "FILES");
        ImGui::Separator();
        if (g_proj.audioPool.empty()) {
            ImGui::TextWrapped("Add a file to see it here.");
        } else {
            for (size_t i = 0; i < g_proj.audioPool.size(); ++i) {
                const AudioAsset& asset = g_proj.audioPool[i];
                std::error_code ec;
                const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                ImGui::PushID((int)i);
                if (ImGui::Selectable(asset.name.c_str(), g_audioPoolSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                    g_audioPoolSelected = (int)i;
                ImGui::TextColored(exists ? ImVec4(0.55f, 0.85f, 0.60f, 1.f) : ImVec4(0.95f, 0.52f, 0.35f, 1.f),
                                   "%s", exists ? "Available" : "Missing");
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        poolSplitter("##audio_splitter", g_audioSidebarWidth);
        ImGui::BeginChild("##audio_details", ImVec2(0, 0), false);

        if (g_audioPoolSelected >= 0 && g_audioPoolSelected < (int)g_proj.audioPool.size()) {
            AudioAsset& asset = g_proj.audioPool[(size_t)g_audioPoolSelected];
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "SELECTED FILE");
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Name");
            ImGui::SameLine(S(80));
            ImGui::SetNextItemWidth(std::max(S(80), std::min(S(360), ImGui::GetContentRegionAvail().x - S(120))));
            inputStr("##audio_pool_name", asset.name, 0);
            if (ImGui::Button("Change file...")) {
                const std::string picked = chooseAudioPath();
                if (!picked.empty()) {
                    std::error_code ec;
                    const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(picked), ec);
                    asset.path = (ec ? std::filesystem::path(picked) : absolute).string();
                }
            }
            ImGui::TextDisabled("File path");
            ImGui::TextWrapped("%s", asset.path.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("Sfile cawfile \"%s\"", asset.name.c_str());
            if (ImGui::Button("Remove from list")) {
                g_proj.audioPool.erase(g_proj.audioPool.begin() + g_audioPoolSelected);
                if (g_proj.audioPool.empty()) g_audioPoolSelected = -1;
                else g_audioPoolSelected = std::min(g_audioPoolSelected, (int)g_proj.audioPool.size() - 1);
            }
        } else {
            ImGui::TextDisabled("Select a file on the left, or add one.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
    g_audioPoolOpen = open;
}
#include "app.h"
void drawOrchestraPool() {
    if (!g_orchestraOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(850), S(560)), ImGuiCond_FirstUseEver);
    bool open = g_orchestraOpen;
    winMaxPrepareT("Orchestra");
    if (ImGui::Begin("Orchestra", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Orchestra");
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) g_textEditorFocused = true;
        ImGui::TextDisabled("PROJECT ORCHESTRA");
        ImGui::TextDisabled("Global variables, tables and orchestra code shared by every clip.");
        ImGui::Separator();
        const int style = std::min(2, std::max(0, g_proj.cs.editorFontStyle));
        if (g_codeFonts[style]) ImGui::PushFont(g_codeFonts[style]);
        ImGui::SetWindowFontScale(g_proj.cs.editorFontSize / g_proj.cs.programFontSize);
        (void)codeEditorBody(g_codeEditors, -400001, g_proj.orchestraCode, 0.f, 0, false);
        ImGui::SetWindowFontScale(1.f);
        if (g_codeFonts[style]) ImGui::PopFont();
    }
    ImGui::End();
    g_orchestraOpen = open;
}
#include "app.h"
void drawIncludePool() {
    if (!g_includePoolOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(860), S(560)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(280), S(130)), ImGuiCond_FirstUseEver);
    bool open = g_includePoolOpen;
    winMaxPrepareT("Includes");
    if (ImGui::Begin("Includes", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Includes");
        if (ImGui::Button("Add include...")) {
            const std::string picked = chooseIncludePath();
            if (!picked.empty()) {
                std::error_code ec;
                std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(picked), ec);
                if (ec) p = std::filesystem::path(picked);
                const auto samePath = std::find_if(g_proj.includes.begin(), g_proj.includes.end(),
                    [&](const IncludeAsset& a) { return a.path == p.string(); });
                if (samePath != g_proj.includes.end()) {
                    g_includeSelected = (int)std::distance(g_proj.includes.begin(), samePath);
                } else {
                    std::string ext = p.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    g_proj.includes.push_back({p.filename().string(), p.string(), ext == ".sco"});
                    g_includeSelected = (int)g_proj.includes.size() - 1;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(".udo / .txt files default to Orchestra; .sco files default to Score.");
        ImGui::Separator();

        if (g_proj.includes.empty()) g_includeSelected = -1;
        else if (g_includeSelected < 0 || g_includeSelected >= (int)g_proj.includes.size()) g_includeSelected = 0;
        ImGui::BeginChild("##include_list", ImVec2(S(g_includeSidebarWidth), 0), true);
        ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "INCLUDED FILES");
        ImGui::Separator();
        if (g_proj.includes.empty()) {
            ImGui::TextWrapped("Add a .udo, .sco, or .txt file.");
        } else {
            for (size_t i = 0; i < g_proj.includes.size(); ++i) {
                const IncludeAsset& asset = g_proj.includes[i];
                std::error_code ec;
                const bool exists = !asset.path.empty() && std::filesystem::exists(std::filesystem::path(asset.path), ec);
                ImGui::PushID((int)i);
                if (ImGui::Selectable(asset.name.c_str(), g_includeSelected == (int)i, ImGuiSelectableFlags_SpanAllColumns))
                    g_includeSelected = (int)i;
                ImGui::TextDisabled("%s · %s", asset.score ? "Score" : "Orchestra", exists ? "Available" : "Missing");
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        poolSplitter("##include_splitter", g_includeSidebarWidth);
        ImGui::BeginChild("##include_details", ImVec2(0, 0), false);
        if (g_includeSelected >= 0 && g_includeSelected < (int)g_proj.includes.size()) {
            IncludeAsset& asset = g_proj.includes[(size_t)g_includeSelected];
            ImGui::TextColored(ImVec4(0.95f, 0.60f, 0.20f, 1.f), "%s", asset.name.c_str());
            ImGui::Separator();
            ImGui::TextDisabled("Full path");
            ImGui::TextWrapped("%s", asset.path.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("Csound section");
            ImGui::SameLine();
            int section = asset.score ? 1 : 0;
            if (comboFit(S(160), "##include_section", &section, "Orchestra\0Score\0")) asset.score = section == 1;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Orchestra includes are emitted inside <CsInstruments>; score includes are emitted inside <CsScore>.");
            if (asset.score) ImGui::TextDisabled("Score includes should omit an `e` terminator; CAW closes the generated score.");
            if (ImGui::Button("Change file...")) {
                const std::string picked = chooseIncludePath();
                if (!picked.empty()) {
                    std::error_code ec;
                    const std::filesystem::path p = std::filesystem::absolute(std::filesystem::path(picked), ec);
                    asset.path = (ec ? std::filesystem::path(picked) : p).string();
                    asset.name = std::filesystem::path(asset.path).filename().string();
                    std::string ext = std::filesystem::path(asset.path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    asset.score = ext == ".sco";
                }
            }
            ImGui::SameLine();
            const bool remove = ImGui::Button("Remove from list");
            Clip* clip = selectedClip();
            ImGui::Separator();
            ImGui::TextWrapped("Generated as: #include \"%s\"", std::filesystem::path(asset.path).generic_string().c_str());
            if (clip) ImGui::TextDisabled("Applies to the project, including clip %s.", clip->name.c_str());
            if (remove) {
                g_proj.includes.erase(g_proj.includes.begin() + g_includeSelected);
                if (g_proj.includes.empty()) g_includeSelected = -1;
                else g_includeSelected = std::min(g_includeSelected, (int)g_proj.includes.size() - 1);
            }
        } else {
            ImGui::TextDisabled("Select an include on the left, or add a file.");
        }
        ImGui::EndChild();
    }
    ImGui::End();
    g_includePoolOpen = open;
}
#include "app.h"
void drawMasterFxWindow();   // defined below the clip editors (called from the window batch)
#include "app.h"
void drawMasterFxWindow() {
    if (!g_masterFxOpen) return;
    ImGui::SetNextWindowSize(ImVec2(S(680), S(460)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(S(320), S(120)), ImGuiCond_FirstUseEver);
    bool open = g_masterFxOpen;
    winMaxPrepareT("Master FX###master_fx");
    if (ImGui::Begin("Master FX###master_fx", &open, ImGuiWindowFlags_NoCollapse)) {
        winMaxTitleButtonT("Master FX###master_fx");
        ImGui::TextDisabled("Up to 8 VST effects, processed in order on the master mix (after every engine, before the master gain). Runs on CAW's audio streams; a command-line Csound keeps its own output.");
        if (ImGui::IsItemHovered()) helpTip("Load effects top to bottom: the signal flows 1 -> 2 -> ... -> 8. On/off, the plugin's editor and remove work while the engine plays.");
        const auto snapshot = masterfx::rack().view();
#ifdef CAW_VST3
        vstui::startScan();   // the catalog is the same one the VSTe panel scans; without this the list would be empty
#endif
        const auto plugins = vstui::catalogSnapshot();
        const bool scanDone = vstui::catalog().done;
        for (int i = 0; i < masterfx::kSlots; ++i) {
            ImGui::PushID(i);
            const masterfx::SlotView& sv = snapshot[(size_t)i];
            if (i > 0) ImGui::Separator();
            if (ImGui::SmallButton(sv.on && sv.loaded ? "on" : "off")) masterfx::rack().setOn(i, !sv.on);   // toggle: off = bypassed, the chain keeps its order
            if (ImGui::IsItemHovered()) helpTip(sv.on ? "Bypass this effect (the chain order is kept)" : "Enable this effect");
            ImGui::SameLine();
            ImGui::TextDisabled("%d.", i + 1);
            ImGui::SameLine();
            const std::string current = sv.name.empty() ? "(empty) - choose an effect..." : sv.name;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(120));
            if (caw_ui::beginCombo("##mfx_pick", current.c_str())) {
                static char filter[128] = "";
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputTextWithHint("##mfx_filter", "search...", filter, sizeof filter)) {}
                std::string low = filter;
                for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
                if (!scanDone) ImGui::TextDisabled("Scanning plugin types...");
                int visible = 0;
                for (const caw::PluginInfo& info : plugins) {
                    if (info.kind == 1) continue;   // effects only: an instrument on the master would just drone
                    std::string low2 = info.name;
                    for (char& ch : low2) ch = (char)std::tolower((unsigned char)ch);
                    if (!low.empty() && low2.find(low) == std::string::npos) continue;
                    ++visible;
                    const std::string rowId = "##mfxrow" + info.path + "##" + info.classId;
                    if (ImGui::Selectable((info.name + rowId).c_str(), info.path == sv.path && info.classId == sv.classId)) {
                        const std::string err = masterfx::rack().load(i, info.path, info.classId, info.name, (double)g_proj.cs.sr, 4096);
                        if (!err.empty()) logLine("[caw] Master FX slot " + std::to_string(i + 1) + ": " + info.name + " failed: " + err);
                        else logLine("[caw] Master FX slot " + std::to_string(i + 1) + ": " + info.name);
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (scanDone && visible == 0) ImGui::TextDisabled("No effects match.");
                ImGui::EndCombo();
            }
            if (sv.loaded) {
                ImGui::SameLine();
                if (ImGui::SmallButton(sv.editorVisible ? "III*" : "III")) {   // open the effect's editor window (the * marks an editor that is already open)
                    std::string err;
                    if (!masterfx::rack().showEditor(i, sv.name + " (Master FX " + std::to_string(i + 1) + ")", err)) logLine("[caw] Master FX editor: " + err);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) masterfx::rack().clear(i);   // remove: the slot is freed, the chain order closes the gap
            if (ImGui::IsItemHovered()) helpTip("Remove this effect from the master chain.");
        }
    }
    ImGui::End();
    g_masterFxOpen = open;
}
#include "app.h"
float helpPanelHeight() { return g_helpOn ? S(66) : 0.f; }
#include "app.h"
void drawHelpPanel() {
    if (!g_helpOn) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.14f, 1.f));
    ImGui::BeginChild("##helppanel", ImVec2(0, helpPanelHeight()), false);
    ImGui::SetCursorPos(ImVec2(S(8), S(3)));
    ImGui::PushTextWrapPos(0.f);
    if (g_helpShown.empty()) ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.f), "Hover over a control for help.");
    else ImGui::TextUnformatted(g_helpShown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
#include "app.h"
