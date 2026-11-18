#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

inline bool cawMp3RateSupported(int hz) {
    for (int rate : {8000,11025,12000,16000,22050,24000,32000,44100,48000})
        if (rate == hz) return true;
    return false;
}
inline bool cawMp3BitrateSupported(int hz, int kbps) {
    if (!cawMp3RateSupported(hz)) return false;
    static const int mpeg1[] = {32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const int mpeg2[] = {8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    if (hz >= 32000) {
        for (int value : mpeg1) if (value == kbps) return true;
    } else {
        for (int value : mpeg2) if (value == kbps && (hz >= 16000 || value <= 64)) return true;
    }
    return false;
}

// Native FLAC/MP3 encoder using the libsndfile library that often ships with
// Csound. Encoding runs off the UI thread. The output is always a temporary
// file and callers must publish it only after success.
class CawCodecRunner {
    struct Info { int64_t frames = 0; int samplerate = 0, channels = 0, format = 0, sections = 0, seekable = 0; };
#ifdef _WIN32
    using Library = HMODULE;
    static Library openLib(const std::string& s) { return LoadLibraryA(s.c_str()); }
    static void* sym(Library l, const char* n) { return reinterpret_cast<void*>(GetProcAddress(l, n)); }
    static void closeLib(Library l) { if (l) FreeLibrary(l); }
#else
    using Library = void*;
    static Library openLib(const std::string& s) { return dlopen(s.c_str(), RTLD_NOW | RTLD_LOCAL); }
    static void* sym(Library l, const char* n) { return dlsym(l, n); }
    static void closeLib(Library l) { if (l) dlclose(l); }
#endif
    std::thread worker_;
    std::atomic<bool> done_{true}, stop_{false}, success_{false};
    std::string error_;
    void run(std::string input, std::string output, std::string program, int format, int depth, int bitrate) {
        Library lib = nullptr;
#ifdef _WIN32
        const char* names[] = {"libsndfile-1.dll", "sndfile.dll", "libsndfile.dll"};
#elif defined(__APPLE__)
        const char* names[] = {"libsndfile.1.dylib", "libsndfile.dylib"};
#else
        const char* names[] = {"libsndfile.so.1", "libsndfile.so"};
#endif
        std::vector<std::string> paths;
        const auto parent = std::filesystem::path(program).parent_path();
        if (!parent.empty()) for (const char* n : names) {
            paths.push_back((parent / n).string());
            paths.push_back((parent / ".." / "lib" / n).string());
        }
        for (const char* n : names) paths.emplace_back(n);
        for (const auto& p : paths) if ((lib = openLib(p))) break;
        if (!lib) { error_ = "Native audio encoder is unavailable."; done_ = true; return; }
        using Open = void*(*)(const char*, int, Info*);
        using Close = int(*)(void*);
        using Read = int64_t(*)(void*, float*, int64_t);
        using Write = int64_t(*)(void*, const float*, int64_t);
        using Command = int(*)(void*, int, void*, int);
        using Check = int(*)(const Info*);
        using Error = const char*(*)(void*);
        using ErrorCode = int(*)(void*);
        auto open = reinterpret_cast<Open>(sym(lib, "sf_open"));
        auto close = reinterpret_cast<Close>(sym(lib, "sf_close"));
        auto read = reinterpret_cast<Read>(sym(lib, "sf_readf_float"));
        auto write = reinterpret_cast<Write>(sym(lib, "sf_writef_float"));
        auto cmd = reinterpret_cast<Command>(sym(lib, "sf_command"));
        auto check = reinterpret_cast<Check>(sym(lib, "sf_format_check"));
        auto why = reinterpret_cast<Error>(sym(lib, "sf_strerror"));
        auto errcode = reinterpret_cast<ErrorCode>(sym(lib, "sf_error"));
        void *src = nullptr, *dst = nullptr;
        bool ok = open && close && read && write && cmd && check && why && errcode;
        if (!ok) error_ = "Audio encoder library is incomplete.";
        Info in;
        if (ok && !(src = open(input.c_str(), 0x10, &in))) { error_ = "Could not read rendered audio."; ok = false; }
        Info out;
        if (ok) {
            out.samplerate = in.samplerate; out.channels = in.channels;
            out.format = format == 3 ? (0x170000 | (depth == 16 ? 2 : 3)) : (0x230000 | 0x82);
            if (!check(&out)) { error_ = "Native encoder does not support this format, sample rate or channel count."; ok = false; }
        }
        if (ok && !(dst = open(output.c_str(), 0x20, &out))) {
            error_ = std::string("Could not create encoded audio: ") + why(nullptr); ok = false;
        }
        if (ok && format == 4) {
            const int hi = in.samplerate >= 32000 ? 320 : in.samplerate >= 16000 ? 160 : 64;
            const int lo = in.samplerate >= 32000 ? 32 : 8;
            if (!cawMp3BitrateSupported(in.samplerate, bitrate) || bitrate < lo || bitrate > hi) { error_ = "MP3 bitrate is not supported for the project sample rate."; ok = false; }
            else {
                double compression = std::max(0., std::min(1., (double)(hi - bitrate) / (hi - lo) - 1e-8));
                int mode = 0; // constant bitrate
                if (cmd(dst, 0x1301, &compression, sizeof(compression)) != 1 ||
                    cmd(dst, 0x1305, &mode, sizeof(mode)) != 1) {
                    error_ = "Native encoder could not set the selected MP3 bitrate."; ok = false;
                }
            }
        }
        std::vector<float> block;
        if (ok) block.resize((size_t)4096 * (size_t)in.channels);
        int64_t total = 0;
        while (ok && !stop_.load()) {
            const int64_t n = read(src, block.data(), 4096);
            if (n < 0) { error_ = "Could not read rendered audio."; ok = false; break; }
            if (!n) break;
            if (write(dst, block.data(), n) != n) { error_ = std::string("Encoding failed: ") + why(dst); ok = false; break; }
            total += n;
        }
        if (ok && src && errcode(src) != 0) { error_ = std::string("Could not read rendered audio: ") + why(src); ok = false; }
        if (ok && !stop_.load() && total == 0) { error_ = "No audio frames to export."; ok = false; }
        if (src && close(src) != 0) { error_ = "Could not finish reading rendered audio."; ok = false; }
        if (dst && close(dst) != 0) { error_ = "Could not finish encoded audio."; ok = false; }
        closeLib(lib);
        success_ = ok && !stop_.load();
        done_ = true;
    }
public:
    ~CawCodecRunner() { stop(); }
    bool start(const std::string& input, const std::string& output, const std::string& program, int format, int depth, int bitrate) {
        if (worker_.joinable() || (format != 3 && format != 4)) return false;
        error_.clear(); stop_ = false; success_ = false; done_ = false;
        try { worker_ = std::thread([=] {
            try { run(input, output, program, format, depth, bitrate); }
            catch (...) { error_ = "Unexpected audio encoding failure."; success_ = false; done_ = true; }
        }); }
        catch (...) { done_ = true; error_ = "Could not start the audio encoder."; return false; }
        return true;
    }
    void stop() { stop_ = true; if (worker_.joinable()) worker_.join(); }
    bool active() const { return worker_.joinable(); }
    bool finished() const { return active() && done_.load(); }
    bool success() const { return success_.load(); }
    const std::string& error() const { return error_; } // after finished or stop only
};
