// Audio tracks (kind 7): decoded audio files played by the timeline inside CAW's shared audio
// callback. The engine renders voices scheduled by the timeline tick; each voice follows its
// segments (cut / join), the clip rate (basic time-stretch = speed and pitch together), reverse,
// the clip gain and the clip's volume-automation points.
//
// This header has no ImGui dependency: it decodes, caches, computes view data (wave columns,
// spectrogram cells, loudness) and mixes. The drawing happens in app.cpp.
#pragma once

#include "model.h"
#include "vst_script.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#define CAWAUDIO_GDI 1
#include <windows.h>
#else
#define CAWAUDIO_GDI 0
#endif

namespace cawaudio {

// ---------------------------------------------------------------- decoded audio
struct AudioData {
    int sr = 48000;
    int channels = 2;
    std::vector<float> samples;   // interleaved
    double duration() const { return channels > 0 && sr > 0 ? (double)(samples.size() / (size_t)channels) / (double)sr : 0.0; }
    float peak() const {
        float p = 0.f;
        for (float v : samples) p = std::max(p, std::fabs(v));
        return p;
    }
};

using AudioHandle = std::shared_ptr<const AudioData>;

namespace detail {

inline uint32_t readU32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t readU16(const unsigned char* p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

inline bool decodeWav(const std::string& path, AudioData& out, std::string& error) {
    FILE* f = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&f, std::wstring(path.begin(), path.end()).c_str(), L"rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) { error = "could not open the file"; return false; }
    auto fail = [&](const char* why) { if (f) std::fclose(f); error = why; return false; };
    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12) return fail("not a WAV file");
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return fail("not a WAV file");
    int fmt = 0, channels = 0, sr = 0, bits = 0;
    bool haveFmt = false, haveData = false;
    long dataPos = 0; size_t dataLen = 0;
    for (;;) {
        unsigned char ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        const uint32_t len = readU32(ch + 4);
        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<unsigned char> b((size_t)std::min<uint32_t>(len, 64));
            if (std::fread(b.data(), 1, b.size(), f) != b.size()) break;
            if (len > b.size()) std::fseek(f, (long)(len - b.size()), SEEK_CUR);
            fmt = readU16(b.data());
            channels = readU16(b.data() + 2);
            sr = (int)readU32(b.data() + 4);
            bits = readU16(b.data() + 14);
            if (fmt == 0xFFFE && b.size() >= 26) {   // WAVE_FORMAT_EXTENSIBLE: the real tag follows
                const uint16_t sub = readU16(b.data() + 24);
                if (sub == 1) fmt = 1;
                else if (sub == 3) fmt = 3;
            }
            haveFmt = fmt != 0 && channels > 0 && sr > 0 && bits > 0;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            dataPos = std::ftell(f);
            dataLen = len;
            haveData = true;
            if (fmt != 0) break;
            std::fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        } else {
            std::fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
        if (haveFmt && haveData) break;
    }
    if (!haveFmt || !haveData) return fail("the WAV file has no fmt/data chunk");
    if (fmt != 1 && fmt != 3) return fail("unsupported WAV encoding (PCM or IEEE float only)");
    if (channels < 1 || channels > 32) return fail("unsupported channel count");
    if (fmt == 3 && bits != 32 && bits != 64) return fail("unsupported float width");
    if (fmt == 1 && (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return fail("unsupported bit depth");
    if (std::fseek(f, dataPos, SEEK_SET) != 0) return fail("seek failed");
    const size_t frames = dataLen / ((size_t)channels * (size_t)(bits / 8));
    out.samples.resize(frames * (size_t)channels);
    out.channels = channels;
    out.sr = sr;
    const size_t bytesPerSample = (size_t)(bits / 8);
    std::vector<unsigned char> buf((size_t)channels * bytesPerSample);
    size_t w = 0;
    for (size_t i = 0; i < frames; ++i) {
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { out.samples.resize(w); break; }
        for (int chn = 0; chn < channels; ++chn) {
            const unsigned char* p = buf.data() + (size_t)chn * bytesPerSample;
            float v = 0.f;
            if (fmt == 3) {
                if (bits == 32) { float f32; std::memcpy(&f32, p, 4); v = f32; }
                else { double f64; std::memcpy(&f64, p, 8); v = (float)f64; }
            } else if (bits == 8) {
                v = ((int)p[0] - 128) / 128.f;
            } else if (bits == 16) {
                v = (float)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) / 32768.f;
            } else if (bits == 24) {
                const int32_t s = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;
                v = (float)s / 8388608.f;
            } else {
                int32_t s; std::memcpy(&s, p, 4);
                v = (float)s / 2147483648.f;
            }
            if (!std::isfinite(v)) v = 0.f;
            out.samples[w++] = v;
        }
    }
    std::fclose(f);
    if (out.samples.empty()) { error = "the WAV file has no samples"; return false; }
    return true;
}

#if CAWAUDIO_GDI
// Other formats (mp3, ogg, flac, aiff, wma...) go through FFmpeg when the user has set it up
// (Settings > Programs): "ffmpeg -i file -f f32le -acodec pcm_f32le -" piped stdout.
inline bool decodeWithFfmpeg(const std::string& ffmpeg, const std::string& path, int targetSr, AudioData& out, std::string& error) {
    if (ffmpeg.empty()) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    HANDLE myRead = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), rd, GetCurrentProcess(), &myRead, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
        CloseHandle(rd); CloseHandle(wr);
        return false;
    }
    std::string cmd = "\"" + ffmpeg + "\" -y -i \"" + path + "\" -f f32le -acodec pcm_f32le -ac 2" +
        (targetSr > 8000 ? " -ar " + std::to_string(targetSr) : std::string()) + " -";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    const bool ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); CloseHandle(myRead); return false; }
    out.samples.clear();
    out.channels = 2;
    out.sr = targetSr > 8000 ? targetSr : 48000;
    std::vector<float> chunk(65536);
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(myRead, chunk.data(), (DWORD)(chunk.size() * sizeof(float)), &got, nullptr) || got == 0) break;
        const float* p = reinterpret_cast<const float*>(chunk.data());
        const size_t n = got / sizeof(float);
        // the pipe holds whatever channel count ffmpeg chose: keep 2-channel stereo when it looks like pairs
        out.samples.insert(out.samples.end(), p, p + n);
        if (out.samples.size() > (size_t)48000 * 60 * 60) break;   // safety: ~1 hour
    }
    CloseHandle(rd); CloseHandle(myRead);
    WaitForSingleObject(pi.hProcess, 2000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    // stereo pairs without a header: assume the stream is stereo (ffmpeg -f f32le keeps the input channels;
    // most files are mono or stereo, interleaved pairs decode correctly for both)
    if (out.samples.empty()) { error = "FFmpeg produced no audio"; return false; }
    // odd-length stereo stream: drop the dangling sample
    if (out.samples.size() % 2 != 0) out.samples.pop_back();
    return code == 0;
}
#endif

}  // namespace detail

// ---------------------------------------------------------------- decode cache
struct DecodeEntry {
    AudioHandle data;
    long long mtime = 0;
    bool failed = false;
};

inline long long fileStamp(const std::string& path) {
#if CAWAUDIO_GDI
    WIN32_FILE_ATTRIBUTE_DATA fa;
    std::wstring w(path.begin(), path.end());
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
#else
    return 0;
#endif
}

inline AudioHandle getAudio(const std::string& path, int ffmpegSr, const std::string& ffmpegExe, std::string& error) {
    static std::mutex mu;
    static std::map<std::string, DecodeEntry> cache;
    const long long stamp = fileStamp(path);
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = cache.find(path);
        if (it != cache.end()) {
            if (it->second.mtime == stamp) {
                error = it->second.failed ? "the file failed to decode" : std::string();
                return it->second.data;
            }
        }
    }
    AudioHandle result;
    std::string why;
    AudioData d;
    if (detail::decodeWav(path, d, why)) {
        result = std::make_shared<AudioData>(std::move(d));
    } else {
#if CAWAUDIO_GDI
        AudioData f;
        if (detail::decodeWithFfmpeg(ffmpegExe, path, ffmpegSr, f, why)) result = std::make_shared<AudioData>(std::move(f));
        else why = "not a readable WAV file and no FFmpeg help: " + why;
#else
        why = "not a readable WAV file";
#endif
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        DecodeEntry& e = cache[path];
        e.data = result;
        e.mtime = stamp;
        e.failed = result == nullptr;
        if (result) error.clear(); else error = why;
        // keep the cache small
        while (cache.size() > 24) {
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->second.data.use_count() <= 1) { cache.erase(it); break; }
            if (cache.size() <= 24) break;
        }
    }
    return result;
}

inline std::string fileTitle(const std::string& path) {
    const size_t a = path.find_last_of("/\\");
    std::string name = a == std::string::npos ? path : path.substr(a + 1);
    const size_t d = name.find_last_of('.');
    if (d != std::string::npos) name = name.substr(0, d);
    return name;
}

// ---------------------------------------------------------------- mono view of the data
inline float monoAt(const AudioData& d, size_t frame) {
    const float* p = d.samples.data() + frame * (size_t)d.channels;
    if (d.channels == 1) return p[0];
    return 0.5f * (p[0] + p[1]);
}

// ---------------------------------------------------------------- spectrogram FFT
inline void fftMag(float* re, float* mag, int n) {   // in-place iterative radix-2; input real in re, output magnitudes 0..n/2
    std::vector<float> im((size_t)n, 0.f);
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -6.283185307179586f / (float)len;
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int k = 0; k < len / 2; ++k) {
                const float ur = re[i + k], ui = im[i + k];
                const float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
    for (int k = 0; k <= n / 2; ++k) mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
}

// ---------------------------------------------------------------- views (computed, cached per clip)
struct WaveColumn { float lo = 0.f, hi = 0.f; };   // min / max (-1..1)

struct LoudColumn { float rms = 0.f; };            // 0..1

struct ViewCache {
    long long key = -1;                            // geometry + params hash: rebuild when it changes
    std::vector<WaveColumn> wave;
    std::vector<LoudColumn> loud;
    int spCols = 0, spRows = 0;
    std::vector<float> spectro;                    // dB magnitude per cell, spCols * spRows
    std::vector<uint32_t> spectroRgba;             // cached final colors (0 = below the range: skip)
};

inline void hashMix(long long& h, long long v) { h ^= v + 0x9e3779b97f4a7c15LL + (h << 6) + (h >> 2); }
inline uint32_t spectroColor(float db, int cr, int cg, int cb);   // defined below (used by the view cache)

// A light scan of a sample clip's CAW code for the PLAIN CONSTANTS of rate / gain / reverse
// ("gain = 2"). Comment lines (# ... or // ...) never match, expressions like
// "gain = smooth(lfo(...))" are ignored - the engine evaluates those live per block.
inline bool sampleCodeConst(const std::string& line, const char* name, double& out) {
    const size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos) return false;
    if (line[p] == '#' || (line[p] == '/' && p + 1 < line.size() && line[p + 1] == '/')) return false;
    const size_t nlen = std::strlen(name);
    if (line.compare(p, nlen, name) != 0) return false;
    size_t q = p + nlen;
    while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
    if (q >= line.size() || line[q] != '=') return false;
    ++q;
    while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
    const size_t s = q;
    if (q < line.size() && (line[q] == '-' || line[q] == '+')) ++q;
    while (q < line.size() && ((line[q] >= '0' && line[q] <= '9') || line[q] == '.' ||
                               ((line[q] == 'e' || line[q] == 'E')) ||
                               ((line[q] == '-' || line[q] == '+') && (line[q - 1] == 'e' || line[q - 1] == 'E')))) ++q;
    if (q == s) return false;
    try { out = std::stod(line.substr(s, q - s)); } catch (...) { return false; }
    return std::isfinite(out);
}
inline void sampleCodeVisuals(const std::string& code, double& rate, double& gain, bool& reverse, bool& normalize) {
    rate = 1.0; gain = 1.0; reverse = false; normalize = false;
    size_t pos = 0;
    while (pos < code.size()) {
        size_t end = code.find('\n', pos);
        if (end == std::string::npos) end = code.size();
        double v = 0.0;
        if (sampleCodeConst(code.substr(pos, end - pos), "rate", v)) rate = v;
        else if (sampleCodeConst(code.substr(pos, end - pos), "gain", v)) gain = v;
        else if (sampleCodeConst(code.substr(pos, end - pos), "reverse", v)) reverse = std::fabs(v) > 0.5;
        else if (sampleCodeConst(code.substr(pos, end - pos), "normalize", v)) normalize = std::fabs(v) > 0.5;
        pos = end + 1;
    }
}

// Builds (or reuses) the view data of one audio clip. rectW/rectH are the DRAWN pixels of the
// waveform area; clipT maps into the source through the segments / rate / reverse.
inline ViewCache& viewFor(int clipId, long long key, const AudioHandle& data, const Clip& clip,
                          int rectW, int rectH, double pxPerSec, uint32_t clipRgb) {
    static std::map<int, ViewCache> caches;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    ViewCache& v = caches[clipId];
    if (v.key == key) return v;
    v.key = key;
    v.wave.clear();
    v.loud.clear();
    v.spectro.clear();
    if (!data || data->samples.empty() || rectW <= 0) return v;

    const int cols = std::min(4096, std::max(8, rectW));
    // The clip's CAW code is IN the picture: c.audioRate is the code's rate (the clip's length
    // scale, updated on Run: rate = 0.5 -> half as long, rate = 2 -> twice as long; the engine
    // plays at 1/rate), c.audioGain scales the drawn amplitude, c.audioReverse mirrors the shape.
    const double rate = std::max(0.05, (double)clip.audioRate);
    const double gain = std::min(4.0, std::max(0.0, (double)clip.audioGain));
    const double totalSrc = std::max(0.0, (double)clip.dur / rate);   // source seconds the clip spans
    (void)totalSrc;
    // source position at clip time t (respecting reverse): walk the segments
    auto segSpan = [&](double& startSrc, double& dir) {
        startSrc = 0.0;
        dir = clip.audioReverse ? -1.0 : 1.0;
        if (clip.audioReverse) {
            double total = 0.0;
            for (const AudioSeg& s : clip.audioSegs) total += s.dur;
            startSrc = total;
        }
    };
    double srcStart, dir;
    segSpan(srcStart, dir);
    auto sourceAt = [&](double clipT) -> double {
        // linear across all segments: cumulate source offset; reverse plays the whole source span backwards
        double acc = 0.0;
        double base = clip.audioReverse ? 0.0 : 0.0;
        (void)base;
        double covered = 0.0;
        if (clip.audioReverse) {
            for (const AudioSeg& s : clip.audioSegs) covered += s.dur;
            for (const AudioSeg& s : clip.audioSegs) {
                const double span = s.dur * rate;                     // clip seconds
                if (clipT < covered - span + 1e-9) { covered -= span; continue; }
                const double local = clipT - (covered - span);
                return s.srcOffset + s.dur - local / rate;
            }
            return clip.audioSegs.empty() ? 0.0 : clip.audioSegs.front().srcOffset;
        }
        for (const AudioSeg& s : clip.audioSegs) {
            const double span = s.dur * rate;
            if (clipT < acc + span || &s == &clip.audioSegs.back())
                return s.srcOffset + (clipT - acc) / rate;
            acc += span;
        }
        return 0.0;
    };
    (void)srcStart; (void)dir;

    const int ch = data->channels, sr = std::max(8000, data->sr);
    const size_t totalFrames = data->samples.size() / (size_t)ch;
    v.wave.resize((size_t)cols);
    v.loud.resize((size_t)cols);
    for (int col = 0; col < cols; ++col) {
        const double t0 = (double)col / cols * std::max(1e-9, clip.dur);
        const double t1 = (double)(col + 1) / cols * std::max(1e-9, clip.dur);
        const double s0 = sourceAt(t0), s1 = sourceAt(t1);
        long long f0 = (long long)std::floor(std::min(s0, s1) * sr);
        long long f1 = (long long)std::ceil(std::max(s0, s1) * sr);
        f0 = std::max(0LL, std::min((long long)totalFrames - 1, f0));
        f1 = std::max(f0 + 1, std::min((long long)totalFrames, f1));
        float lo = 1.f, hi = -1.f, sum = 0.f;
        for (long long f = f0; f < f1; ++f) {
            const float m = monoAt(*data, (size_t)f);
            lo = std::min(lo, m); hi = std::max(hi, m); sum += m * m;
        }
        if (lo > hi) { lo = hi = 0.f; }
        v.wave[(size_t)col] = {lo * (float)gain, hi * (float)gain};
        v.loud[(size_t)col] = {std::sqrt(sum / (float)std::max(1LL, f1 - f0)) * (float)gain};
    }

    // spectrogram: Audacity-style - Hann-windowed FFT on the MONO mixdown (one spectrogram
    // fills the whole clip), log-frequency rows, Audacity's default dB mapping (gain 20 dB,
    // range 80 dB) so the colors correspond to real levels. One cell per drawn PIXEL (x and y),
    // 2048-point overlapping Hann windows. The colors are cached as RGBA, so drawing a frame
    // stays a cheap quad batch even at pixel resolution. Cap the total cells so a full-screen
    // clip stays drawable per frame.
    {
        v.spRows = std::min(256, std::max(8, rectH));
        v.spCols = std::min(4096, std::max(8, rectW));
        if ((long long)v.spCols * (long long)v.spRows > 260000)
            v.spCols = std::max(8, (int)(260000 / v.spRows));
    }
    v.spectro.assign((size_t)v.spCols * (size_t)v.spRows, -140.f);
    v.spectroRgba.assign((size_t)v.spCols * (size_t)v.spRows, 0u);
    const int N = 2048;
    std::vector<float> buf((size_t)N, 0.f), mag((size_t)(N / 2 + 1), 0.f), hann((size_t)N);
    for (int k = 0; k < N; ++k) hann[(size_t)k] = 0.5f * (1.f - std::cos(6.283185307179586 * (double)k / (double)(N - 1)));
    const float norm = 4.f / (float)N;   // window gain: a full-scale sine reads about 0 dB
    for (int col = 0; col < v.spCols; ++col) {
        const double clipT = (double)(col + 0.5) / v.spCols * std::max(1e-9, clip.dur);
        const double src = sourceAt(clipT);
        const long long centre = (long long)std::llround(src * sr);
        for (int k = 0; k < N; ++k) {
            const long long f = centre + k - N / 2;
            buf[(size_t)k] = (f >= 0 && f < (long long)totalFrames) ? monoAt(*data, (size_t)f) * (float)gain * hann[(size_t)k] : 0.f;
        }
        fftMag(buf.data(), mag.data(), N);
        for (int r = 0; r < v.spRows; ++r) {
            const double fHi = std::min(20000.0, sr * 0.5), fLo = 20.0;
            const double f0 = fLo * std::pow(fHi / fLo, (double)r / v.spRows);
            const double f1 = fLo * std::pow(fHi / fLo, (double)(r + 1) / v.spRows);
            long long b0 = (long long)std::floor(f0 / sr * N), b1 = (long long)std::ceil(f1 / sr * N);
            b0 = std::max(1LL, std::min((long long)N / 2, b0));
            b1 = std::max(b0 + 1, std::min((long long)N / 2 + 1, b1));
            float best = 0.f;
            for (long long b = b0; b < b1; ++b) best = std::max(best, mag[(size_t)b]);
            const float db = 20.f * std::log10(std::max(1e-7f, best * norm));
            v.spectro[(size_t)r * (size_t)v.spCols + (size_t)col] = std::max(-140.f, db);
        }
    }
    // precompute the final colors once (the clip color is part of the cache key)
    {
        const int cr = (int)((clipRgb >> 16) & 0xFF), cg = (int)((clipRgb >> 8) & 0xFF), cb2 = (int)(clipRgb & 0xFF);
        for (size_t k = 0; k < v.spectro.size(); ++k) {
            const float db = v.spectro[k];
            v.spectroRgba[k] = db <= -99.f ? 0u : spectroColor(db, cr, cg, cb2);
        }
    }
    return v;
}

inline uint32_t spectroColor(float db, int cr, int cg, int cb) {   // Audacity dB mapping (gain 20 dB, range 80 dB) into a strong clip-color gradient
    float x = std::max(0.f, std::min(1.f, (db + 100.f) / 80.f));   // -100 dB -> 0, -20 dB -> 1
    const float fR = (float)cr / 255.f, fG = (float)cg / 255.f, fB = (float)cb / 255.f;
    float r, g, b;
    if (x < 0.45f) {          // black -> deep dark clip color: the noise floor stays dark
        float t = x / 0.45f; t *= t;
        r = fR * 0.30f * t; g = fG * 0.30f * t; b = fB * 0.30f * t;
    } else if (x < 0.80f) {   // dark -> full clip color
        float t = (x - 0.45f) / 0.35f; t = t * t * (3.f - 2.f * t);
        const float dR = fR * 0.30f, dG = fG * 0.30f, dBv = fB * 0.30f;
        r = dR + (fR - dR) * t; g = dG + (fG - dG) * t; b = dBv + (fB - dBv) * t;
    } else {                  // full color -> near-white crest
        float t = (x - 0.80f) / 0.20f; t *= t;
        r = fR + (1.f - fR) * t * 0.85f; g = fG + (1.f - fG) * t * 0.85f; b = fB + (1.f - fB) * t * 0.85f;
    }
    return 0xFF000000u | ((uint32_t)std::min(255, (int)(b * 255.f)) << 16) | ((uint32_t)std::min(255, (int)(g * 255.f)) << 8) | (uint32_t)std::min(255, (int)(r * 255.f));
}

// ---------------------------------------------------------------- engine
struct VoiceSpec {
    int trackId = 0, clipId = 0;
    int kind = 0;                       // 0 whole clip (managed by the timeline), 1 sampler note, 2 manual audition (unmanaged)
    double t0 = 0.0;                    // project time of the voice's first audio sample
    long long delay = 0;                // frames of silence before the audio (t0 ahead of the callback)
    double clipStart = 0.0;             // project time where clip time 0 sits (kind 0)
    double clipDur = 0.0;               // clip seconds the whole-clip voice spans (kind 0)
    std::vector<AudioSeg> segs;
    double rate = 1.0;                  // base rate (clip.audioRate); the CAW code can override per block
    bool reverse = false;               // base; the CAW code can override (read when the voice starts)
    double gain0 = 1.0;                 // base gain (clip.audioGain); the CAW code can override per block
    std::vector<std::pair<double, float>> gainPts;   // clip-time volume points (kind 0)
    double skip = 0.0;                  // source seconds the playhead is already into the clip (kind 0 mid-start)
    // kind 1 (sampler note):
    double srcOffset = 0.0, noteDur = 0.0;
    bool grain = false;                 // a code-scheduled grain: linen-style window (attack = release = half the duration)
    AudioHandle data;                   // decoded file this voice plays (resolved on the UI thread)
    std::shared_ptr<const vstscript::Program> script;   // the clip's CAW code (optional)
    std::array<int, 8> outSrc = {{0, -1, -1, -1, -1, -1, -1, -1}};   // per OUTPUT channel: the input channel (0-based) it plays; -1 = silent
    bool normalize = false;             // the code's normalize = 1: scale the file to full level before the gain
};

struct Voice {
    VoiceSpec spec;
    AudioHandle data;
    std::shared_ptr<const vstscript::Program> script;
    vstscript::Runtime rt;
    long long n = 0;                    // frames rendered since creation
    float fade[8] = {0.f};              // one-pole per channel to avoid clicks at boundaries
    // live code values (CAW code of the clip):
    double rate2 = 1.0, gain2 = 1.0, lpHz = 0.0, hpHz = 0.0, drive = 0.0;
    bool rev2 = false;
    bool hasGrains = false;             // the code schedules play: grains - this voice stays silent and only schedules
    int idxRate = -1, idxGain = -1, idxLp = -1, idxHp = -1, idxDrive = -1, idxRev = -1;
    bool scriptInited = false;          // one-time script setup ran (the old idxRate<0 guard re-ran it every block for granular clips: every fired per block and rand re-seeded - the frozen granular bug)
    int idxOut[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // out1..out8: custom routing from the code
    int liveOutSrc[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // live routing: output k plays input liveOutSrc[k] (0-based; -1 = silent)
    bool liveOutActive = false;                         // true once any out<n> var was found in the code
    float normGain = 1.f;               // the code's normalize = 1: 1 / file peak (computed once at the voice start)
    // position integration (the rate can change every block):
    double rel = 0.0;                   // audio seconds since the voice's first audio sample
    double srcPos = 0.0;                // local source position inside the current segment (seconds)
    int curSeg = 0;                     // segment being played
    float lpState[8] = {0.f}, hpState[8] = {0.f};
    int nIn = 0;                        // input channels this voice renders (capped 8)
};

class Engine {
public:
    void setSr(int sr) { sr_.store(std::max(8000, sr), std::memory_order_relaxed); }
    // Per-track audio handoff (VSTe inputs, engine 4): the app installs a push callback; render
    // accumulates every voice's post-track-gain output per track and calls it once per block.
    // Called with the render lock held: the callback must be lock-free (a ring push).
    void setFeed(std::function<void(int trackId, const float* interleaved, int frames, int channels)> fn) {
        std::lock_guard<std::mutex> lock(mu_);
        feedFn_ = std::move(fn);
    }
    int sr() const { return sr_.load(std::memory_order_relaxed); }
    // the audio frame clock: frames rendered by the shared callback since the engine was created
    double timeSeconds() const { return (double)frames_.load(std::memory_order_relaxed) / (double)std::max(8000, sr_.load(std::memory_order_relaxed)); }

    void render(float* out, int frames, int channels) noexcept {
        if (!out || frames <= 0 || channels <= 0) return;
        frames_.fetch_add(frames, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu_);
        const double invSr = 1.0 / (double)std::max(8000, sr_.load(std::memory_order_relaxed));
        const double dt = invSr * (double)frames;
        std::vector<VoiceSpec> grainSpawn;   // grains collected during the pass, spawned after it (push_back would reallocate voices_)
        const bool feedOn = (bool)feedFn_;
        if (feedOn)   // zero this block's per-track accumulators (kept allocated: no per-block allocation)
            for (auto& kv : feedAcc_) {
                const size_t need = (size_t)frames * 8;
                if (kv.second.size() < need) kv.second.resize(need, 0.f);
                std::fill(kv.second.begin(), kv.second.begin() + (long)need, 0.f);
            }
        for (size_t vi = 0; vi < voices_.size();) {
            Voice& v = voices_[vi];
            if (v.spec.delay < 0) v.spec.delay = 0;
            // ---- the clip's CAW code: evaluated once per block, then its values drive the voice
            if (v.script && v.script->ok) {
                if (!v.scriptInited) {   // ONE-TIME setup: variable indexes + the runtime (the old idxRate<0 guard re-ran this every block for granular clips: every fired per block and rand re-seeded - the frozen granular bug)
                v.scriptInited = true;
                    auto findIdx = [&](const char* name) {
                        for (size_t k = 0; k < v.script->varNames.size(); ++k)
                            if (v.script->varNames[k] == name) return (int)k;
                        return -1;
                    };
                    v.idxRate = findIdx("rate");
                    v.idxGain = findIdx("gain");
                    v.idxLp = findIdx("lp");
                    v.idxHp = findIdx("hp");
                    v.idxDrive = findIdx("drive");
                    v.idxRev = findIdx("reverse");
                    char outName[8] = "out0";
                    v.liveOutActive = false;
                    for (int k = 0; k < 8; ++k) {
                        outName[3] = (char)('1' + k);
                        v.idxOut[k] = findIdx(outName);
                        if (v.idxOut[k] >= 0) v.liveOutActive = true;   // the code routes: out<n> = in<m> (output n plays input m)
                    }
                    v.rt.init(*v.script, nullptr);
                    v.rate2 = v.spec.rate;
                    v.gain2 = v.spec.gain0;
                    v.rev2 = v.spec.reverse;
                    v.hasGrains = v.script->hasPlay;   // granular clip: play: grains replace the straight file
                    if (v.spec.normalize && v.data && !v.data->samples.empty()) {   // normalize: full level before the code's gain
                        float peak = 0.f;
                        for (float s : v.data->samples) { const float a = std::fabs(s); if (a > peak) peak = a; }
                        v.normGain = peak > 1e-6f ? 1.f / peak : 1.f;
                    }
                }
                vstscript::Env env;
                env.t = v.rel;             // seconds since the voice's audio started
                env.dur = v.spec.kind == 0 ? v.spec.clipDur : v.spec.noteDur;
                env.dt = dt;
                // "read": the clip's source position at the playhead (walks the segments at the current speed)
                {
                    double acc = 0.0;
                    env.read = 0.0;
                    if (!v.spec.segs.empty()) {
                        if (v.rev2) {
                            double covered = 0.0;
                            for (const AudioSeg& s : v.spec.segs) covered += s.dur;
                            for (const AudioSeg& s : v.spec.segs) {
                                const double span = s.dur / std::max(0.02, v.rate2);
                                if (v.rel < covered - span + 1e-9) { covered -= span; continue; }
                                env.read = s.srcOffset + s.dur - (v.rel - (covered - span)) * v.rate2;
                                break;
                            }
                        } else {
                            for (const AudioSeg& s : v.spec.segs) {
                                const double span = s.dur / std::max(0.02, v.rate2);
                                if (v.rel < acc + span || &s == &v.spec.segs.back()) { env.read = s.srcOffset + (v.rel - acc) * v.rate2; break; }
                                acc += span;
                            }
                        }
                    }
                }
                vstscript::Events ev;
                vstscript::tick(*v.script, env, v.rt, ev);
                auto read = [&](int idx, double fallback) {
                    return idx >= 0 && idx < (int)v.rt.vars.size() && std::isfinite(v.rt.vars[(size_t)idx]) ? v.rt.vars[(size_t)idx] : fallback;
                };
                // The code's "rate" is the clip's LENGTH scale (0.5 = half as long, double
                // speed): the engine's speed is the spec's base speed divided by the live value.
                if (v.idxRate >= 0) {
                    const double r = std::min(16.0, std::max(0.02, read(v.idxRate, 1.0)));
                    v.rate2 = std::min(16.0, std::max(0.02, v.spec.rate / r));
                } else {
                    v.rate2 = std::min(16.0, std::max(0.02, v.spec.rate));
                }
                v.gain2 = std::min(4.0, std::max(0.0, read(v.idxGain, v.spec.gain0) *
                    (v.idxGain >= 0 && v.spec.normalize ? (double)v.normGain : 1.0)));   // the code's gain (normalize scales it to full level first)
                v.lpHz = std::min(20000.0, std::max(0.0, read(v.idxLp, 0.0)));
                v.hpHz = std::min(20000.0, std::max(0.0, read(v.idxHp, 0.0)));
                v.drive = std::min(8.0, std::max(0.0, read(v.idxDrive, 0.0)));
                if ((long long)v.n < v.spec.delay && v.idxRev >= 0)   // reverse is read while the voice still waits (it would jump mid-play)
                    v.rev2 = std::fabs(read(v.idxRev, v.rev2 ? 1.0 : 0.0)) > 0.5;
                    // custom routing from the code: out<n> = in<m> - output n plays input m (1-based;
                    // a plain number is the input channel too). Outputs without a value stay silent.
                    if (v.liveOutActive)
                        for (int k = 0; k < 8; ++k)
                            v.liveOutSrc[k] = v.idxOut[k] >= 0 ? std::max(0, std::min(15, (int)std::lround(read(v.idxOut[k], (double)(k + 1)) - 1.0))) : -1;
                // granular: play: statements schedule one grain voice each (start, duration, speed; speed < 0 = reverse)
                for (const auto& gr : ev.grains) {
                    VoiceSpec g;
                    g.kind = 1;
                    g.grain = true;
                    g.trackId = v.spec.trackId;
                    g.clipId = v.spec.clipId;
                    g.data = v.data;
                    g.outSrc = v.spec.outSrc;
                    g.normalize = v.spec.normalize;
                    g.t0 = v.spec.t0 + gr.at;
                    const double now = v.spec.t0 + v.rel;
                    g.delay = (long long)std::llround(std::max(0.0, g.t0 - now) * (double)std::max(8000, sr_.load(std::memory_order_relaxed)));
                    g.srcOffset = gr.start;
                    const double speed = std::fabs(gr.speed);
                    g.rate = speed;   // the grain's speed is literal (1 = normal, 2 = octave up, negative = reverse)
                    g.reverse = gr.speed < 0.0;
                    const int fch = (v.data && !v.data->samples.empty()) ? v.data->channels : 1;
                    const double totalSrc = (v.data && !v.data->samples.empty()) ? (double)v.data->samples.size() / (double)std::max(1, fch) / (double)std::max(8000, v.data->sr) : 0.0;
                    const double span = speed * gr.dur;   // source seconds the grain reads
                    if (g.reverse) g.srcOffset = std::min(std::max(0.0, gr.start + span), std::max(0.0, totalSrc - 0.02));   // reversed: the grain covers [start, start + span], played backwards
                    g.noteDur = gr.dur;
                    g.gain0 = v.spec.gain0;   // the clip's gain (normalize folded) and the track volume
                    grainSpawn.push_back(std::move(g));
                }
            }
            bool alive = true;
            for (int i = 0; i < frames; ++i) {
                if ((long long)v.n < v.spec.delay) {          // scheduled ahead: wait silently
                    ++v.n;
                    continue;
                }
                if (v.rel == 0.0 && (long long)v.n == v.spec.delay) {   // first audio frame: set the start position
                    if (v.spec.kind == 1) { v.curSeg = 0; v.srcPos = 0.0; }   // notes: srcOffset carries the position
                    else if (v.spec.segs.empty()) { v.curSeg = 0; v.srcPos = 0.0; }
                    else if (v.rev2) {   // reversed: start at the END of the chain, play backwards (skip = into the chain from the end)
                        v.curSeg = (int)v.spec.segs.size() - 1;
                        v.srcPos = std::max(0.0, v.spec.skip);
                    } else {   // forward: walk the segments to the skip position (play from the playhead, not from zero)
                        double skip = std::max(0.0, v.spec.skip);
                        v.curSeg = 0;
                        while (v.curSeg + 1 < (int)v.spec.segs.size() && skip >= v.spec.segs[(size_t)v.curSeg].dur) {
                            skip -= v.spec.segs[(size_t)v.curSeg].dur;
                            ++v.curSeg;
                        }
                        v.srcPos = skip;
                    }
                }
                v.rel += invSr;
                // end conditions: the clip / note time is over, or the source ran out
                if (v.spec.kind == 0 && v.rel >= v.spec.clipDur) { alive = false; break; }
                if (v.spec.kind != 0 && v.rel >= v.spec.noteDur) { alive = false; break; }
                const AudioHandle& d = v.data;
                if (!d || d->samples.empty()) { alive = false; break; }
                if (v.hasGrains) { ++v.n; continue; }   // granular scheduler: the voice itself is silent, only the play: grains sound
                const int sr = std::max(8000, d->sr);
                const size_t total = d->samples.size() / (size_t)d->channels;
                // the file position: the current segment's offset plus the local position (backwards when reversed)
                double srcT = 0.0;
                if (v.spec.kind == 1) srcT = v.spec.srcOffset + (v.rev2 ? -v.srcPos : v.srcPos);
                else if (v.curSeg >= 0 && v.curSeg < (int)v.spec.segs.size())
                    srcT = v.spec.segs[(size_t)v.curSeg].srcOffset + (v.rev2 ? v.spec.segs[(size_t)v.curSeg].dur - v.srcPos : v.srcPos);
                const double srcFrames = srcT * (double)sr;
                if (srcFrames < 0.0) { alive = false; break; }
                double readFrames = srcFrames;
                const double maxFrame = (double)total - 2.0;
                if (readFrames > maxFrame) readFrames = maxFrame;   // reversed playback starts at the file's very end: read the last samples
                const size_t f0 = (size_t)readFrames;
                const double frac = readFrames - (double)f0;
                const size_t ch = (size_t)d->channels;
                const int nIn = std::min(8, std::max(1, v.nIn));
                // process every input channel once (effects, gain envelope, fade), then distribute
                // to the outputs through the routing: out<n> = in<m>
                float processed[8] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
                for (int inCh = 0; inCh < nIn; ++inCh) {
                    const size_t srcCh = std::min((size_t)inCh, ch - 1);
                    const float a = d->samples[f0 * ch + srcCh];
                    const float b = d->samples[(f0 + 1) * ch + srcCh];
                    float smp = a + (b - a) * (float)frac;
                    // simple effects from the code: drive (saturation), one-pole low / highpass
                    if (v.drive > 0.001) {
                        const double k = 1.0 + v.drive * 3.0;
                        smp = (float)(std::tanh((double)smp * k) / std::tanh(k));
                    }
                    if (v.lpHz > 20.0) {
                        const double al = std::exp(-6.283185307179586 * v.lpHz * invSr);
                        v.lpState[inCh] = (float)((double)v.lpState[inCh] * al + (double)smp * (1.0 - al));
                        smp = v.lpState[inCh];
                    }
                    if (v.hpHz > 20.0) {
                        const double ah = std::exp(-6.283185307179586 * v.hpHz * invSr);
                        const float lp = (float)((double)v.hpState[inCh] * ah + (double)smp * (1.0 - ah));
                        v.hpState[inCh] = lp;
                        smp -= lp;
                    }
                    // gain: the volume points (kind 0), the code gain, the base gain and the code's normalize
                    float gain = 1.f;
                    if (v.spec.kind == 0) gain = (float)envelope(v.spec, v.spec.t0 + v.rel - v.spec.clipStart, v.spec.clipDur);
                    else gain = (float)envelope(v.spec, v.rel, v.spec.noteDur);
                    const float target = smp * gain * (float)v.gain2;
                    v.fade[inCh] += (target - v.fade[inCh]) * 0.35f;   // light smoothing kills jump-cut clicks
                    processed[inCh] = v.fade[inCh] * trackGainFor(v.spec.trackId);   // the track's volume automation (live)
                }
                // the outputs this voice writes: the code's routing can use all 8, the saved map
                // says how many the default uses (as the source = one per file channel)
                int outCount = v.liveOutActive ? 8 : std::min(8, std::max(2, (int)v.spec.outSrc.size()));
                for (int outC = 0; outC < outCount; ++outC) {
                    const int src = v.liveOutActive   // the code's out<n> = in<m> routing wins over the saved map
                        ? v.liveOutSrc[outC]
                        : (outC < (int)v.spec.outSrc.size() ? v.spec.outSrc[(size_t)outC] : -1);
                    if (src < 0 || src >= nIn) continue;   // that output is silent (or its input does not exist)
                    const float w = processed[src];
                    if (outC < channels) out[(size_t)i * (size_t)channels + (size_t)outC] += w;
                    if (feedOn) {   // per-track handoff for VSTe inputs (post track gain, pre master)
                        auto& fb = feedAcc_[v.spec.trackId];
                        if (fb.size() >= (size_t)frames * 8) fb[(size_t)i * 8 + (size_t)outC] += w;
                    }
                    // per-track meter peak (per OUTPUT channel, after the track gain)
                    {
                        float& slot = trackPeaks_[v.spec.trackId][std::min(7, outC)];
                        const float a2 = std::fabs(w);
                        if (a2 > slot) slot = a2;
                    }
                }
                // advance the local position through the segments (rate can change every block);
                // kind 1 voices (sampler notes / grains) have no segment chain - the position just runs
                v.srcPos += v.rate2 * invSr;
                if (!v.spec.segs.empty()) {
                    if (v.rev2) {   // mirrored position: srcPos past this segment's dur means the previous segment is next
                        while (v.curSeg >= 0 && v.curSeg < (int)v.spec.segs.size() && v.srcPos > v.spec.segs[(size_t)v.curSeg].dur) {
                            if (v.curSeg > 0 && v.spec.kind != 1) {
                                v.srcPos -= v.spec.segs[(size_t)v.curSeg].dur;
                                --v.curSeg;
                            } else { alive = false; break; }
                        }
                    } else {
                        while (v.srcPos > v.spec.segs[(size_t)v.curSeg].dur) {
                            if (v.curSeg + 1 < (int)v.spec.segs.size()) {
                                v.srcPos -= v.spec.segs[(size_t)v.curSeg].dur;   // carry into the next segment (jump-cut, faded by the one-pole)
                                ++v.curSeg;
                            } else { alive = false; break; }
                        }
                    }
                }
                if (!alive) break;
                ++v.n;
            }
            if (!alive) {
                voices_.erase(voices_.begin() + (long)vi);
                continue;
            }
            ++vi;
        }
        // grains collected during the pass: spawn them now (push_back after the iteration is safe)
        for (const VoiceSpec& g : grainSpawn) {
            if (voices_.size() >= 256) break;   // runaway guard (e.g. every 0.001 with a huge grain count)
            voices_.push_back(makeVoice(g));
        }
        // per-track handoff: one callback per track that had a voice this block
        if (feedOn)
            for (const auto& kv : feedAcc_) {
                const size_t need = (size_t)frames * 8;
                if (kv.second.size() < need) continue;
                feedBuf_.resize((size_t)frames * (size_t)channels);
                for (int i = 0; i < frames; ++i)
                    for (int c = 0; c < channels && c < 8; ++c)
                        feedBuf_[(size_t)i * (size_t)channels + (size_t)c] = kv.second[(size_t)i * 8 + (size_t)c];
                feedFn_(kv.first, feedBuf_.data(), frames, channels);
            }
    }

    // ---- control (UI thread)
    void clearAll() { std::lock_guard<std::mutex> lock(mu_); voices_.clear(); trackGains_.clear(); }
    // live per-track gain multiplier (track volume automation, updated every tick without restarting voices)
    void setTrackGain(int trackId, float gain) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!std::isfinite(gain) || gain < 0.f) gain = 0.f;
        trackGains_[trackId] = std::min(4.f, gain);
    }
    void killClip(int clipId) {
        std::lock_guard<std::mutex> lock(mu_);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
            [&](const Voice& v) { return v.spec.clipId == clipId; }), voices_.end());
    }
    // only the timeline-managed voices of a clip (a manual audition keeps playing)
    void killManaged(int clipId) {
        std::lock_guard<std::mutex> lock(mu_);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
            [&](const Voice& v) { return v.spec.clipId == clipId && v.spec.kind == 0; }), voices_.end());
    }
    void killTrack(int trackId) {
        std::lock_guard<std::mutex> lock(mu_);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
            [&](const Voice& v) { return v.spec.trackId == trackId; }), voices_.end());
    }
    void startVoice(const VoiceSpec& spec) {
        std::lock_guard<std::mutex> lock(mu_);
        voices_.push_back(makeVoice(spec));
        if (voices_.size() > 128) voices_.erase(voices_.begin());   // runaway guard
    }
    // clip voices are keyed by clipId: one at a time (managed and manual audition voices)
    void setClipVoice(const VoiceSpec& spec) {
        std::lock_guard<std::mutex> lock(mu_);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) {
            return v.spec.clipId == spec.clipId && (v.spec.kind == 0 || v.spec.kind == 2);
        }), voices_.end());
        voices_.push_back(makeVoice(spec));
        if (voices_.size() > 128) voices_.erase(voices_.begin());
    }
    int clipVoiceCount() const { std::lock_guard<std::mutex> lock(mu_); int n = 0; for (const Voice& v : voices_) if (v.spec.kind == 0) ++n; return n; }
    // per-track meter peaks per output channel (max since the last take, one call per UI frame)
    std::map<int, std::array<float, 8>> takeTrackPeaks() {
        std::lock_guard<std::mutex> lock(mu_);
        return std::move(trackPeaks_);
    }

private:
    static Voice makeVoice(const VoiceSpec& spec) {
        Voice v;
        v.spec = spec;
        v.data = spec.data;
        v.script = spec.script;
        v.rt.init(spec.script ? *spec.script : vstscript::Program{}, nullptr);
        v.rate2 = spec.rate;
        v.gain2 = spec.gain0;
        v.rev2 = spec.reverse;
        v.nIn = v.data && !v.data->samples.empty() ? std::min(8, std::max(1, v.data->channels)) : 2;   // one voice channel per file channel
        return v;
    }
    static double envelope(const VoiceSpec& s, double t, double endT) {
        double g = 1.0;
        if (!s.gainPts.empty()) {
            const std::vector<std::pair<double, float>>& p = s.gainPts;
            if (t <= p.front().first) g = p.front().second;
            else if (t >= p.back().first) g = p.back().second;
            else {
                for (size_t k = 1; k < p.size(); ++k)
                    if (t <= p[k].first) {
                        const double a = (t - p[k - 1].first) / std::max(1e-9, p[k].first - p[k - 1].first);
                        g = p[k - 1].second + (p[k].second - p[k - 1].second) * a;
                        break;
                    }
            }
        }
        if (s.kind == 1) {   // note: short attack / release so notes do not click
            if (s.grain) {   // grain: symmetric window, attack = release = half the duration
                const double x = std::max(0.0, std::min(1.0, t / std::max(1e-9, endT)));
                g *= 0.5 - 0.5 * std::cos(6.283185307179586 * x);   // raised cosine: no click at either edge
            } else {
                const double atk = 0.004, rel = 0.02;
                g *= std::min(1.0, t / atk);
                g *= std::min(1.0, std::max(0.0, (endT - t) / rel));
            }
        }
        return std::max(0.0, g);
    }

    mutable std::mutex mu_;
    std::vector<Voice> voices_;
    std::map<int, float> trackGains_;
    std::map<int, std::array<float, 8>> trackPeaks_;
    std::function<void(int, const float*, int, int)> feedFn_;   // per-track handoff (VSTe inputs)
    std::map<int, std::vector<float>> feedAcc_;                 // per-track block accumulators (8-channel stride)
    std::vector<float> feedBuf_;                                // interleaved scratch for the feed callback
    std::atomic<int> sr_{48000};
    std::atomic<long long> frames_{0};

    float trackGainFor(int trackId) const {
        const auto it = trackGains_.find(trackId);
        return it == trackGains_.end() ? 1.f : it->second;
    }
};

inline Engine& engine() { static Engine e; return e; }

}  // namespace cawaudio
