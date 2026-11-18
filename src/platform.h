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
// Windows: csound.exe of a Csound copy shipped inside CAW's own folder (<caw.exe folder>\Csound\...), else "".
std::string bundledCsound();
// CAW's own folder (where caw.exe sits), else "". The default start folder when picking a csound.exe.
std::string cawOwnFolder();
// When csoundExe lives inside CAW's own folder, point Csound's plugin search (OPCODE6DIR64 / OPCODE7DIR64)
// at the plugins folder next to it. Without this a copied (not installed) Csound finds no opcode plugins.
void prepareCsoundEnvironment(const std::string& csoundExe);
// Windows: the plugins folder of a Csound copy ("plugins*" beside csound.exe or one level up), "" when none.
std::string csoundPluginsDir(const std::string& csoundExe);
// Windows keeps ONE copy of every DLL name per process. Csound, SuperCollider and libpd each ship their own
// libsndfile-1.dll, libwinpthread-1.dll ...; whichever engine loads first decides what the others get, and an older
// copy makes the newer engine fail. This loads the NEWEST copy of every name that appears in more than one of the
// given folders, by full path, before any engine loads. Returns log lines (empty when nothing needed doing).
std::vector<std::string> preloadNewestSharedDlls(const std::vector<std::string>& engineDirs);
// Windows: load EVERY DLL of `dir` by full path (altered search path, so dependencies sitting beside the DLL are
// found) unless a DLL with that name is already in the process. Needed for a Csound copy inside CAW's own folder,
// which is not on PATH: Csound loads its opcode plugins with a plain LoadLibrary, whose dependencies can only come
// from DLLs already in the process. In Csound + PD mode libpd is loaded first and brings its own copies of several
// runtime DLL names, so Csound's plugins would bind to those (or not find their dependencies at all) and fail -
// while Csound alone keeps working. Returns log lines (empty when nothing needed doing).
std::vector<std::string> preloadAllDllsOf(const std::string& dir);
// Windows: lists DLLs of `dir` that this process already uses from another folder (the usual reason a bundled
// engine fails to load). Empty when there is no clash.
std::string dllConflictReport(const std::string& dir);
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
// Windows: existing local libpd build, then a DLL beside caw.exe as fallback.
std::string bundledLibpd();
// SuperCollider language executable from the CAW cache, a standard install, or PATH.
std::string superColliderSclang();
// Name of the system default playback / recording device (Windows only, others return "")
std::string defaultAudioEndpointName(bool capture);
// Folders that normally contain Csound (looked at before PATH), each with the relative exe paths to try
std::vector<std::string> csoundInstallRoots();
std::vector<std::string> csoundRelativeExePaths();
// Path of the UI / code fonts present on this OS (first existing file of each list), "" when none
std::string uiFontPath();
std::string codeFontPath(int style /*0 regular, 1 bold, 2 italic*/);

}  // namespace plat
