#pragma once
// Small platform layer: everything that differs between Windows, macOS and Linux lives behind these functions.
#include <string>
#include <vector>

namespace plat {

constexpr bool isWindows =
#ifdef _WIN32
    true;
#else
    false;
#endif
constexpr bool isMac =
#ifdef __APPLE__
    true;
#else
    false;
#endif

// "Windows", "macOS" or "Linux" (used in messages)
const char* osName();
// File name of the Csound program: "csound.exe" on Windows, "csound" elsewhere
const char* csoundProgramName();
// Native open / save file dialog. patterns like {"caw"} or {"udo","sco","txt"}; empty = all files. Returns "" when cancelled.
// Windows: the system dialog. macOS: osascript. Linux: zenity, else kdialog.
std::string chooseFile(bool save, const std::string& title, const std::string& filterName,
                       const std::vector<std::string>& extensions, const std::string& suggestedPath);
// Native directory picker; empty means cancelled or unavailable.
std::string chooseFolder(const std::string& title);
// Opens a file with the default program (browser for .html, ...)
void openWithDefaultProgram(const std::string& path);
// Environment variable, "" when not set
std::string getEnv(const char* name);
// First executable called `name` on PATH (plus the usual install folders on macOS / Linux), "" when not found
std::string findOnPath(const std::string& name);
// Windows: FFmpeg bundled next to the application, regardless of working directory.
std::string bundledFfmpeg();
// Name of the system default playback / recording device (Windows only, others return "")
std::string defaultAudioEndpointName(bool capture);
// Folders that normally contain Csound (looked at before PATH), each with the relative exe paths to try
std::vector<std::string> csoundInstallRoots();
std::vector<std::string> csoundRelativeExePaths();
// Path of the UI / code fonts present on this OS (first existing file of each list), "" when none
std::string uiFontPath();
std::string codeFontPath(int style /*0 regular, 1 bold, 2 italic*/);

}  // namespace plat
