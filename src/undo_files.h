// undo/redo file safety: some edits live in FILES on disk, outside the project snapshot
// (a frozen track's rendered WAV, previously the sound editor's in-place save). The snapshot
// restore can bring the project back, but not a file that was deleted - so destructive file
// operations move the file aside into a named backup instead, and a restored snapshot that
// references a missing file pulls it back from there.
#pragma once
#include <string>
#include <map>
#include <filesystem>
#include <chrono>

// original path -> backup path
inline std::map<std::string, std::string>& cawUndoFileBackups() {
    static std::map<std::string, std::string> m;
    return m;
}

// Move a file aside instead of deleting it: undo may still need it.
inline void cawUndoKeepFile(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    const std::filesystem::path src = std::filesystem::u8path(path);
    if (!std::filesystem::exists(src, ec)) return;
    const long long stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::path backup = src;
    backup += ".cawundo-" + std::to_string(stamp);
    std::filesystem::rename(src, backup, ec);
    if (ec) return;
    cawUndoFileBackups()[path] = backup.u8string();
}

// A restored snapshot references a file that is not on disk: bring the backup back.
inline void cawUndoRestoreFile(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    const std::filesystem::path want = std::filesystem::u8path(path);
    if (std::filesystem::exists(want, ec)) { cawUndoFileBackups().erase(path); return; }
    auto it = cawUndoFileBackups().find(path);
    if (it == cawUndoFileBackups().end()) return;
    std::filesystem::rename(std::filesystem::u8path(it->second), want, ec);
    if (!ec) cawUndoFileBackups().erase(it);
}

// A fresh file now exists at the original path (a re-freeze, a re-render): an old backup of
// the same location is superseded - drop it.
inline void cawUndoDropBackup(const std::string& path) {
    if (path.empty()) return;
    auto it = cawUndoFileBackups().find(path);
    if (it == cawUndoFileBackups().end()) return;
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(it->second), ec);
    cawUndoFileBackups().erase(it);
}
