#include "platform.h"

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
    // Prefer the already built library requested by this installation.
    const fs::path existing = fs::path(L"C:\\Users\\Parham\\CAW-libpd\\build-J5ruj3Sp\\libpd.dll");
    std::error_code ec;
    if (fs::is_regular_file(existing, ec) && !ec) return existing.u8string();
    std::vector<wchar_t> name(32768);
    const DWORD size = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!size || size >= name.size()) return {};
    const fs::path library = fs::path(std::wstring(name.data(), size)).parent_path() / L"libpd.dll";
    ec.clear();
    return fs::is_regular_file(library, ec) ? library.u8string() : std::string();
#else
    return {};
#endif
}

std::string superColliderSclang() {
    const std::string overridePath = getEnv("CAW_SCLANG");
    std::error_code ec;
    if (!overridePath.empty() && fs::is_regular_file(fs::u8path(overridePath), ec) && !ec) return overridePath;
#ifdef _WIN32
    std::vector<fs::path> roots;
    const std::string user = getEnv("USERPROFILE");
    if (!user.empty()) roots.push_back(fs::u8path(user) / L".caw_deps" / L"supercollider" / L"runtime-x64");
    for (const char* var : {"ProgramFiles", "ProgramW6432", "LOCALAPPDATA"}) {
        const std::string root = getEnv(var);
        if (root.empty()) continue;
        const fs::path base = fs::u8path(root);
        roots.push_back(base / L"SuperCollider");
        if (std::string(var) == "LOCALAPPDATA") roots.push_back(base / L"Programs" / L"SuperCollider");
    }
    std::vector<wchar_t> module(32768);
    const DWORD size = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (size && size < module.size()) roots.push_back(fs::path(std::wstring(module.data(), size)).parent_path());
    for (const fs::path& root : roots) {
        for (const fs::path& rel : {fs::path(L"sclang.exe"), fs::path(L"SuperCollider") / L"sclang.exe", fs::path(L"bin") / L"sclang.exe"}) {
            const fs::path candidate = root / rel;
            ec.clear();
            if (fs::is_regular_file(candidate, ec) && !ec) return candidate.u8string();
        }
        ec.clear();
        if (!fs::is_directory(root, ec) || ec) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 5) { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file(ec) && !ec && it->path().filename() == L"sclang.exe") return it->path().u8string();
        }
    }
    return findOnPath("sclang.exe");
#elif defined(__APPLE__)
    const fs::path app = "/Applications/SuperCollider.app/Contents/MacOS/sclang";
    ec.clear();
    if (fs::is_regular_file(app, ec) && !ec) return app.string();
    return findOnPath("sclang");
#else
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
