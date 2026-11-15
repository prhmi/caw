#pragma once
#include "model.h"
#include "csd_builder.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <cctype>
#include <set>
#include <sstream>

namespace code_export {
inline std::string safeName(const std::string& text) {
    std::string out;
    for (unsigned char c : text)
        out += (std::isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
    if (out.empty()) out = "clip";
    if (out.size() > 80) out.resize(80);
    return out;
}
enum class Mode { SeparateRaw, CombinedText, CsoundRaw, CsoundInstruments, CsoundProject };
enum class Filter { All, Csound, Vst };
inline bool accepts(const Track& track, Filter filter) {
    return filter == Filter::All || (filter == Filter::Csound && track.kind == 0) ||
           (filter == Filter::Vst && (track.kind == 3 || track.kind == 4));
}
inline const char* language(const Track& track) {
    if (track.kind == 0) return "Csound";
    if (track.kind == 3 || track.kind == 4) return "VST CAW code";
    if (track.kind == 5) return "SuperCollider placeholder";
    if (track.kind == 6) return "Pure Data placeholder";
    return "Other";
}
// Each invocation creates a fresh directory; no existing export is overwritten.
inline bool write(const Project& project, const std::filesystem::path& parent,
                  std::filesystem::path& directory, int& written, std::string& error,
                  Mode mode = Mode::SeparateRaw, Filter filter = Filter::All) {
    written = 0;
    size_t count = 0;
    const bool onlyCsound = mode == Mode::CsoundRaw || mode == Mode::CsoundInstruments;
    for (const Track& track : project.tracks)
        if (onlyCsound ? track.kind == 0 : accepts(track, filter)) count += track.clips.size();
    if (!count && mode != Mode::CsoundProject) { error = "There are no matching code clips to export."; return false; }
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) { error = "Export folder is unavailable."; return false; }
    for (int n = 1; n < 100000; ++n) {
        directory = parent / (n == 1 ? "CAW code export" : "CAW code export " + std::to_string(n));
        if (std::filesystem::create_directory(directory, ec)) break;
        if (ec) { error = "Could not create the export folder: " + ec.message(); return false; }
        directory.clear();
    }
    if (directory.empty()) { error = "Could not find an unused export folder name."; return false; }
    auto save = [&](const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        if (!out || !(out << text) || !(out.flush())) {
            error = "Could not write " + path.string();
            return false;
        }
        ++written;
        return true;
    };
    if (mode == Mode::CsoundProject) {
        Project copy = project;
        copy.score.analyze = false; // Export code, not CAW's score-analysis probe.
        copy.tracks.erase(std::remove_if(copy.tracks.begin(), copy.tracks.end(),
                          [](const Track& track) { return track.kind == 5 || track.kind == 6; }), copy.tracks.end());
        return save(directory / "project.csd", buildCsd(copy, "export", 0.0));
    }
    if (mode != Mode::SeparateRaw) {
        std::ostringstream text;
        std::set<std::string> emitted;
        for (const Track& track : project.tracks) for (const Clip& clip : track.clips) {
            if (onlyCsound ? track.kind != 0 : !accepts(track, filter)) continue;
            if (mode == Mode::CombinedText)
                text << "===== " << language(track) << " | " << track.name << " | " << clip.name << " =====\n" << clip.code << "\n\n";
            else if (mode == Mode::CsoundRaw)
                text << "; Track: " << track.name << " | Clip: " << clip.name << "\n" << clip.code << "\n\n";
            else if (mode == Mode::CsoundInstruments && emitted.insert(clip.name).second) {
                const ParsedTrack parsed = parseClipForTrack(clip, track, false);
                text << "; Track: " << track.name << " | Clip: " << clip.name << "\n";
                for (const auto& param : parsed.params)
                    text << "chn_k \"" << channelName(clip.name, param.chn) << "\", 3\n";
                text << "instr " << clip.name << "\n" << parsed.body << "\nendin\n\n";
            }
        }
        const char* filename = mode == Mode::CombinedText ? "all_clip_code.txt" :
                               mode == Mode::CsoundRaw ? "csound_clip_code.orc" : "csound_instruments.orc";
        return save(directory / filename, text.str());
    }
    for (const Track& track : project.tracks) for (const Clip& clip : track.clips) {
        if (!accepts(track, filter)) continue;
        const std::string ext = track.kind == 0 ? ".orc" : (track.kind == 3 || track.kind == 4) ? ".cawcode" : ".txt";
        const std::filesystem::path path = directory / (std::to_string(written + 1) + "_" + safeName(clip.name) + ext);
        if (!save(path, clip.code)) return false;
    }
    return true;
}
}
