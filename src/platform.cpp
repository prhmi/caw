#include "platform.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <mmdeviceapi.h>
#include <winver.h>
#include <cwctype>
#include <map>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace plat {

const char* osName() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

const char* csoundProgramName() { return isWindows ? "csound.exe" : "csound"; }

std::string bundledFfmpeg() {
#ifdef _WIN32
    std::vector<char> name(32768);
    const DWORD size = GetModuleFileNameA(nullptr, name.data(), (DWORD)name.size());
    if (!size || size >= name.size()) return {};
    const fs::path executable = fs::path(std::string(name.data(), size)).parent_path() / "ffmpeg.exe";
    std::error_code ec;
    return fs::is_regular_file(executable, ec) ? executable.string() : std::string();
#else
    return {};
#endif
}

std::string bundledLibpd() {
#ifdef _WIN32
    // Installed layout first: <caw.exe folder>\PureData\libpd.dll (made by the installer, together with
    // libwinpthread-1.dll), then libpd.dll directly beside caw.exe.
    std::vector<wchar_t> name(32768);
    const DWORD size = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!size || size >= name.size()) return {};
    const fs::path exeDir = fs::path(std::wstring(name.data(), size)).parent_path();
    std::error_code ec;
    for (const fs::path& library : {exeDir / L"PureData" / L"libpd.dll", exeDir / L"libpd.dll"}) {
        ec.clear();
        if (fs::is_regular_file(library, ec) && !ec) return library.u8string();
    }
    return std::string();
#else
    return {};
#endif
}

#ifdef _WIN32
static fs::path cawExeDir() {
    std::vector<wchar_t> name(32768);
    const DWORD size = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!size || size >= name.size()) return {};
    return fs::path(std::wstring(name.data(), size)).parent_path();
}
#endif

std::string bundledCsound() {
#ifdef _WIN32
    const fs::path dir = cawExeDir();
    if (dir.empty()) return {};
    std::error_code ec;
    for (const fs::path& rel : {fs::path(L"Csound") / L"bin" / L"csound.exe", fs::path(L"Csound") / L"csound.exe"}) {
        const fs::path candidate = dir / rel;
        ec.clear();
        if (fs::is_regular_file(candidate, ec) && !ec) return candidate.u8string();
    }
#endif
    return {};
}

std::string cawOwnFolder() {
#ifdef _WIN32
    const fs::path dir = cawExeDir();
    if (dir.empty()) return {};
    return dir.u8string();
#elif defined(__APPLE__)
    char buf[4096] = {};
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(fs::u8path(buf), ec);
    return (ec ? fs::u8path(buf) : exe).parent_path().u8string();
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return exe.parent_path().u8string();
#endif
}

#ifdef _WIN32
// plugins live in a "plugins*" folder beside csound.exe or one level up (Csound\bin\csound.exe -> Csound\plugins64).
// Sorted by name so the choice does not depend on directory order; plain "plugins64" wins over "plugins64-7.0".
fs::path csoundPluginsDirOf(const fs::path& exe) {
    std::error_code ec;
    for (const fs::path& base : {exe.parent_path(), exe.parent_path().parent_path()}) {
        ec.clear();
        if (!fs::is_directory(base, ec) || ec) continue;
        std::vector<fs::path> found;
        for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code dirEc;
            const std::wstring name = it->path().filename().wstring();
            if (name.rfind(L"plugins", 0) != 0 || !it->is_directory(dirEc) || dirEc) continue;
            found.push_back(it->path());
        }
        if (!found.empty()) {
            std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
                const std::wstring& an = a.filename().wstring();
                const std::wstring& bn = b.filename().wstring();
                for (size_t i = 0; i < an.size() && i < bn.size(); ++i)
                    if (towlower(an[i]) != towlower(bn[i])) return towlower(an[i]) < towlower(bn[i]);
                return an.size() < bn.size();
            });
            return found.front();
        }
    }
    return {};
}
#endif

void prepareCsoundEnvironment(const std::string& csoundExe) {
#ifdef _WIN32
    const fs::path appDir = cawExeDir();
    if (appDir.empty() || csoundExe.empty()) return;
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(fs::u8path(csoundExe), ec);
    const fs::path app = fs::weakly_canonical(appDir, ec);
    if (exe.empty() || app.empty() || exe.native().rfind(app.native(), 0) != 0) return;   // only for the copy inside CAW
    const fs::path plugins = csoundPluginsDirOf(exe);
    if (!plugins.empty()) {
        SetEnvironmentVariableW(L"OPCODE6DIR64", plugins.wstring().c_str());
        SetEnvironmentVariableW(L"OPCODE7DIR64", plugins.wstring().c_str());
    }
#else
    (void)csoundExe;
#endif
}

std::string csoundPluginsDir(const std::string& csoundExe) {
#ifdef _WIN32
    if (csoundExe.empty()) return {};
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(fs::u8path(csoundExe), ec);
    const fs::path plugins = exe.empty() ? fs::path() : csoundPluginsDirOf(exe);
    return plugins.empty() ? std::string() : plugins.u8string();
#else
    (void)csoundExe;
    return {};
#endif
}

#ifdef _WIN32
namespace {
unsigned long long dllFileVersion(const fs::path& file) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size) return 0;
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return 0;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return 0;
    return (static_cast<unsigned long long>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
}
std::wstring lowerFileName(const fs::path& file) {
    std::wstring name = file.filename().wstring();
    for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
    return name;
}
bool isDll(const fs::path& file) {
    std::wstring ext = file.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".dll";
}
fs::path loadedModulePath(HMODULE module) {
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    return n && n < buffer.size() ? fs::path(std::wstring(buffer.data(), n)) : fs::path();
}
bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return !a.empty() && !b.empty() && fs::equivalent(a, b, ec) && !ec;
}
}  // namespace
#endif

std::vector<std::string> preloadNewestSharedDlls(const std::vector<std::string>& engineDirs) {
    std::vector<std::string> log;
#ifdef _WIN32
    struct Copy { fs::path file; unsigned long long version; };
    std::map<std::wstring, std::vector<Copy>> byName;
    std::vector<fs::path> dirs;
    for (const std::string& text : engineDirs) {
        if (text.empty()) continue;
        std::error_code ec;
        const fs::path dir = fs::u8path(text);
        if (!fs::is_directory(dir, ec) || ec) continue;
        bool seen = false;
        for (const fs::path& known : dirs) seen = seen || sameFile(known, dir);
        if (seen) continue;
        dirs.push_back(dir);
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || !isDll(it->path())) continue;
            byName[lowerFileName(it->path())].push_back({it->path(), dllFileVersion(it->path())});
        }
    }
    for (auto& entry : byName) {
        const std::wstring& name = entry.first;
        std::vector<Copy>& copies = entry.second;
        if (copies.size() < 2) continue;
        // the engines' own entry libraries are never shared
        if (name.rfind(L"libscsynth", 0) == 0 || name.rfind(L"csound", 0) == 0 || name == L"libpd.dll") continue;
        const Copy* best = &copies[0];
        for (const Copy& c : copies) if (c.version > best->version) best = &c;
        const std::string shown = best->file.u8string();
        if (HMODULE already = GetModuleHandleW(name.c_str())) {
            if (!sameFile(loadedModulePath(already), best->file))
                log.push_back("[caw] " + fs::path(name).u8string() + " was already loaded from " +
                              loadedModulePath(already).u8string() + " (not the newest copy: " + shown +
                              "). Restart CAW if an engine fails to load.");
            continue;
        }
        if (LoadLibraryExW(best->file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            log.push_back("[caw] " + fs::path(name).u8string() + " is shipped by several engines: using the newest copy, " + shown);
        else
            log.push_back("[caw] Could not preload " + shown + " (Windows error " + std::to_string(GetLastError()) + ").");
    }
#else
    (void)engineDirs;
#endif
    return log;
}

std::vector<std::string> preloadAllDllsOf(const std::string& dirText) {
    std::vector<std::string> log;
#ifdef _WIN32
    std::error_code ec;
    const fs::path dir = fs::u8path(dirText);
    if (dirText.empty() || !fs::is_directory(dir, ec) || ec) return log;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc || !isDll(it->path())) continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return lowerFileName(a) < lowerFileName(b); });
    int loaded = 0, skipped = 0, failed = 0;
    for (const fs::path& file : files) {
        // a DLL with this name is already in the process (the shared-copy pass or another engine chose it)
        if (GetModuleHandleW(file.filename().wstring().c_str())) { skipped++; continue; }
        if (LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) loaded++;
        else failed++;
    }
    if (loaded || failed)
        log.push_back("[caw] " + dir.u8string() + ": preloaded " + std::to_string(loaded) + " DLL(s)" +
                      (failed ? ", " + std::to_string(failed) + " failed to load" : std::string()) +
                      (skipped ? ", " + std::to_string(skipped) + " already loaded" : std::string()) + ".");
#else
    (void)dirText;
#endif
    return log;
}

std::string dllConflictReport(const std::string& dirText) {
    std::string report;
#ifdef _WIN32
    std::error_code ec;
    const fs::path dir = fs::u8path(dirText);
    if (!fs::is_directory(dir, ec) || ec) return report;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !isDll(it->path())) continue;
        const std::wstring name = it->path().filename().wstring();
        HMODULE loaded = GetModuleHandleW(name.c_str());
        if (!loaded) continue;
        const fs::path used = loadedModulePath(loaded);
        if (!sameFile(used, it->path()))
            report += "\n  " + it->path().filename().u8string() + " is already loaded from " + used.parent_path().u8string();
    }
#else
    (void)dirText;
#endif
    return report;
}

std::string superColliderSclang() {
    const std::string overridePath = getEnv("CAW_SCLANG");
    std::error_code ec;
    if (!overridePath.empty() && fs::is_regular_file(fs::u8path(overridePath), ec) && !ec) return overridePath;
#ifdef _WIN32
    // CAW uses ONLY its own SuperCollider runtime (sclang.exe + libscsynth.dll shipped with CAW).
    // A SuperCollider installed on the PC (Program Files, AppData, PATH) is deliberately ignored,
    // because its libscsynth lacks CAW's host-driven pump and its user config/extensions break the bridge.
    std::vector<fs::path> roots;
    std::vector<wchar_t> module(32768);
    const DWORD size = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (size && size < module.size()) roots.push_back(fs::path(std::wstring(module.data(), size)).parent_path() / L"SuperCollider");
    const std::string user = getEnv("USERPROFILE");
    if (!user.empty()) roots.push_back(fs::u8path(user) / L".caw_deps" / L"supercollider" / L"runtime-x64");
    for (const fs::path& root : roots) {
        ec.clear();
        if (!fs::is_directory(root, ec) || ec) continue;
        for (const fs::path& rel : {fs::path(L"sclang.exe"), fs::path(L"SuperCollider") / L"sclang.exe", fs::path(L"bin") / L"sclang.exe"}) {
            const fs::path candidate = root / rel;
            ec.clear();
            if (fs::is_regular_file(candidate, ec) && !ec) return candidate.u8string();
        }
        ec.clear();
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file(ec) && !ec && it->path().filename() == L"sclang.exe") return it->path().u8string();
        }
    }
    return std::string();
#else
    // macOS / Linux: CAW's own SuperCollider runtime first (tools/build-libscsynth.sh installs it in
    // ~/.caw_deps/supercollider/runtime-x64, or a "SuperCollider" folder next to caw). A normal
    // SuperCollider install has no CAW host pump (caw_sc_world_new...), so it is only the last resort.
    std::vector<fs::path> roots;
    const std::string own = cawOwnFolder();
    if (!own.empty()) roots.push_back(fs::u8path(own) / "SuperCollider");
    const std::string home = getEnv("HOME");
    if (!home.empty()) roots.push_back(fs::u8path(home) / ".caw_deps" / "supercollider" / "runtime-x64");
    for (const fs::path& root : roots) {
        ec.clear();
        if (!fs::is_directory(root, ec) || ec) continue;
        for (const fs::path& rel : {fs::path("sclang"), fs::path("bin") / "sclang"}) {
            const fs::path candidate = root / rel;
            ec.clear();
            if (fs::is_regular_file(candidate, ec) && !ec) return candidate.u8string();
        }
        ec.clear();
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file(ec) && !ec && it->path().filename() == "sclang") return it->path().u8string();
        }
    }
#if defined(__APPLE__)
    const fs::path app = "/Applications/SuperCollider.app/Contents/MacOS/sclang";
    ec.clear();
    if (fs::is_regular_file(app, ec) && !ec) return app.string();
#endif
    return findOnPath("sclang");
#endif
}

std::string getEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

#ifndef _WIN32
static bool isExecutableFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
}

static std::string shellQuote(const std::string& s) {   // 'text' with ' escaped
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

static std::string runAndRead(const std::string& command, int* exitCode = nullptr) {
    std::string out;
    FILE* p = popen(command.c_str(), "r");
    if (!p) { if (exitCode) *exitCode = -1; return out; }
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    const int status = pclose(p);
    if (exitCode) *exitCode = status;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static bool commandExists(const char* name) { return !findOnPath(name).empty(); }
#endif

std::string findOnPath(const std::string& name) {
#ifdef _WIN32
    char found[MAX_PATH * 2] = {};
    if (SearchPathA(nullptr, name.c_str(), ".exe", (DWORD)sizeof(found), found, nullptr) > 0) return found;
    return std::string();
#else
    std::vector<std::string> dirs;
    const std::string path = getEnv("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        if (end > start) dirs.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    // GUI programs started from the Dock / a launcher often have a short PATH: add the usual places
    for (const char* extra : {"/usr/local/bin", "/opt/homebrew/bin", "/opt/local/bin", "/usr/bin", "/snap/bin", "/Applications/Csound/bin"})
        dirs.push_back(extra);
    for (const std::string& d : dirs) {
        const fs::path candidate = fs::path(d) / name;
        if (isExecutableFile(candidate)) return candidate.string();
    }
    return std::string();
#endif
}

std::vector<std::string> csoundInstallRoots() {
    std::vector<std::string> roots;
#ifdef _WIN32
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "LOCALAPPDATA", "APPDATA"}) {
        const std::string v = getEnv(var);
        if (v.empty()) continue;
        roots.push_back(std::string(var) == "LOCALAPPDATA" ? v + "\\Programs" : v);
    }
    roots.push_back("C:\\Program Files");
    roots.push_back("C:\\Program Files (x86)");
    roots.push_back("C:\\");
#elif defined(__APPLE__)
    roots = {"/Applications", "/usr/local/opt", "/opt/homebrew/opt", "/Library/Frameworks"};
#else
    roots = {"/opt", "/usr/local"};
#endif
    return roots;
}

std::vector<std::string> csoundRelativeExePaths() {
#ifdef _WIN32
    return {"bin\\csound.exe", "csound.exe", "bin\\csound7.exe"};
#else
    return {"bin/csound", "csound", "Contents/MacOS/csound", "bin/csound7"};
#endif
}

void openWithDefaultProgram(const std::string& path) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    const std::string cmd = "open " + shellQuote(path) + " >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#else
    const std::string cmd = "xdg-open " + shellQuote(path) + " >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

std::string chooseFile(bool save, const std::string& title, const std::string& filterName,
                       const std::vector<std::string>& extensions, const std::string& suggestedPath) {
#ifdef _WIN32
    char path[MAX_PATH * 4] = {};
    std::snprintf(path, sizeof(path), "%s", suggestedPath.c_str());
    std::string filter;
    if (!extensions.empty()) {
        std::string pat;
        for (const std::string& e : extensions) { if (!pat.empty()) pat += ";"; pat += "*." + e; }
        std::string name = filterName.empty() ? "Files" : filterName;
        filter += name + " (" + pat + ")";
        filter.push_back('\0'); filter += pat; filter.push_back('\0');
    }
    filter += "All files (*.*)"; filter.push_back('\0'); filter += "*.*"; filter.push_back('\0'); filter.push_back('\0');
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof(path);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrTitle = title.empty() ? nullptr : title.c_str();
    dialog.lpstrDefExt = extensions.empty() ? nullptr : extensions.front().c_str();
    dialog.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog);
    return ok ? std::string(path) : std::string();
#elif defined(__APPLE__)
    // AppleScript "choose file" / "choose file name"; the result is a POSIX path
    std::string types;
    for (const std::string& e : extensions) { if (!types.empty()) types += ", "; types += "\"" + e + "\""; }
    auto esc = [](const std::string& s) { std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o; };
    std::string script;
    if (save) {
        script = "POSIX path of (choose file name with prompt \"" + esc(title.empty() ? "Save" : title) + "\"";
        const std::string name = suggestedPath.empty() ? std::string() : fs::path(suggestedPath).filename().string();
        if (!name.empty()) script += " default name \"" + esc(name) + "\"";
        script += ")";
    } else {
        script = "POSIX path of (choose file with prompt \"" + esc(title.empty() ? "Open" : title) + "\"";
        if (!types.empty()) script += " of type {" + types + "}";
        script += ")";
    }
    int code = 0;
    const std::string out = runAndRead("osascript -e " + shellQuote(script) + " 2>/dev/null", &code);
    return code == 0 ? out : std::string();
#else
    int code = 0;
    std::string out;
    if (commandExists("zenity")) {
        std::string cmd = "zenity --file-selection";
        if (save) cmd += " --save --confirm-overwrite";
        if (!title.empty()) cmd += " --title=" + shellQuote(title);
        if (!suggestedPath.empty()) cmd += " --filename=" + shellQuote(suggestedPath);
        if (!extensions.empty()) {
            std::string pats;
            for (const std::string& e : extensions) pats += " *." + e;
            cmd += " " + shellQuote("--file-filter=" + (filterName.empty() ? std::string("Files") : filterName) + " |" + pats);
            cmd += " " + shellQuote("--file-filter=All files | *");
        }
        out = runAndRead(cmd + " 2>/dev/null", &code);
        return code == 0 ? out : std::string();
    }
    if (commandExists("kdialog")) {
        std::string start = suggestedPath.empty() ? getEnv("HOME") : suggestedPath;
        std::string cmd = std::string("kdialog ") + (save ? "--getsavefilename " : "--getopenfilename ") + shellQuote(start);
        if (!extensions.empty()) {
            std::string pats;
            for (const std::string& e : extensions) { if (!pats.empty()) pats += " "; pats += "*." + e; }
            cmd += " " + shellQuote(pats + "|" + (filterName.empty() ? std::string("Files") : filterName));
        }
        out = runAndRead(cmd + " 2>/dev/null", &code);
        return code == 0 ? out : std::string();
    }
    return std::string();   // no dialog program installed: the caller tells the user to install zenity
#endif
}

std::string chooseFolder(const std::string& title) {
#ifdef _WIN32
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOA info = {};
    info.hwndOwner = GetActiveWindow();
    info.lpszTitle = title.c_str();
    info.ulFlags = BIF_RETURNONLYFSDIRS | (SUCCEEDED(com) ? BIF_NEWDIALOGSTYLE : 0);
    PIDLIST_ABSOLUTE item = SHBrowseForFolderA(&info);
    if (!item) { if (SUCCEEDED(com)) CoUninitialize(); return {}; }
    char path[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListA(item, path) != 0;
    CoTaskMemFree(item);
    if (SUCCEEDED(com)) CoUninitialize();
    return ok ? std::string(path) : std::string();
#elif defined(__APPLE__)
    std::string safe;
    for (char c : title) { if (c == '"' || c == '\\') safe += '\\'; safe += c; }
    int code = 0;
    const std::string script = "POSIX path of (choose folder with prompt \"" + safe + "\")";
    const std::string out = runAndRead("osascript -e " + shellQuote(script) + " 2>/dev/null", &code);
    return code == 0 ? out : std::string();
#else
    int code = 0;
    if (commandExists("zenity"))
        return runAndRead("zenity --file-selection --directory --title=" + shellQuote(title) + " 2>/dev/null", &code);
    if (commandExists("kdialog"))
        return runAndRead("kdialog --getexistingdirectory " + shellQuote(getEnv("HOME")) + " --title " + shellQuote(title) + " 2>/dev/null", &code);
    return {};
#endif
}

std::string defaultAudioEndpointName(bool capture) {
#ifdef _WIN32
    std::string result;
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) && en) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &dev)) && dev) {
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                const PROPERTYKEY friendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};   // PKEY_Device_FriendlyName
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(friendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                    const int n = WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 1) { result.assign((size_t)n - 1, '\0'); WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, &result[0], n, nullptr, nullptr); }
                }
                PropVariantClear(&pv);
                ps->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(initHr)) CoUninitialize();
    return result;
#else
    (void)capture;
    return std::string();   // PortAudio's own default device is used
#endif
}

static std::string firstExisting(const std::vector<std::string>& list) {
    for (const std::string& p : list) if (fileExists(p)) return p;
    return std::string();
}

std::string uiFontPath() {
#ifdef _WIN32
    return firstExisting({"C:\\Windows\\Fonts\\segoeui.ttf"});
#elif defined(__APPLE__)
    return firstExisting({"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
                          "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/SFNS.ttf"});
#else
    return firstExisting({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                          "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"});
#endif
}

std::string codeFontPath(int style) {
    const int s = style < 0 || style > 2 ? 0 : style;
#ifdef _WIN32
    static const char* f[] = {"C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\consolab.ttf", "C:\\Windows\\Fonts\\consolai.ttf"};
    return firstExisting({f[s]});
#elif defined(__APPLE__)
    static const char* f[] = {"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Menlo.ttc"};
    if (s == 0) return firstExisting({f[0], "/System/Library/Fonts/Monaco.ttf", "/Library/Fonts/Courier New.ttf"});
    return std::string();   // .ttc collections hold the bold / italic cut in another index: use the regular font for them
#else
    static const char* dejavu[] = {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-Oblique.ttf"};
    static const char* liber[] = {"LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf", "LiberationMono-Italic.ttf"};
    std::vector<std::string> c;
    for (const char* dir : {"/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu/", "/usr/share/fonts/TTF/"}) c.push_back(std::string(dir) + dejavu[s]);
    for (const char* dir : {"/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/liberation/"}) c.push_back(std::string(dir) + liber[s]);
    return firstExisting(c);
#endif
}

}  // namespace plat
