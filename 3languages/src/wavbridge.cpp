#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "wavbridge.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace wavbridge {
namespace {

struct Wav {
    int sr = 44100;
    long long frames = 0;
    std::vector<float> lr;   // interleaved stereo
};

struct Voice {
    std::shared_ptr<const Wav> w;
    std::string tag;
    long long startFrame = 0;   // output clock frame where the voice begins
    long long skipFrames = 0;   // file frames skipped at the start
    long long endFrame = 0;     // output clock frame where it stops
    long long fadeFrom = -1;    // >= 0: fading out, silent at endFrame
};

constexpr int kBufFrames = 512;
constexpr int kBufCount = 4;

std::mutex g_mx;
std::vector<Voice> g_voices;
std::map<std::string, std::pair<long long, std::shared_ptr<const Wav>>> g_cache;   // path -> (stamp, wav)
HWAVEOUT g_hwo = nullptr;
WAVEHDR g_hdr[kBufCount];
std::vector<int16_t> g_buf[kBufCount];
std::thread g_thread;
std::atomic<bool> g_run{false};
int g_sr = 44100;
long long g_clock = 0;   // output frames already handed to the sound card

uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::shared_ptr<const Wav> loadWav(const std::string& path, std::string& error) {
    std::error_code ec;
    const auto wt = std::filesystem::last_write_time(path, ec);
    if (ec) { error = "wav file not found: " + path; return nullptr; }
    const long long stamp = (long long)wt.time_since_epoch().count() ^ (long long)std::filesystem::file_size(path, ec);
    auto it = g_cache.find(path);
    if (it != g_cache.end() && it->second.first == stamp) return it->second.second;

    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open " + path; return nullptr; }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) { error = "not a wav file: " + path; return nullptr; }
    int fmtTag = 0, ch = 2, bits = 16, sr = 44100;
    size_t dataPos = 0, dataLen = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const uint32_t len = rd32(&d[pos + 4]);
        if (std::memcmp(&d[pos], "fmt ", 4) == 0 && pos + 8 + 16 <= d.size()) {
            fmtTag = rd16(&d[pos + 8]);
            ch = rd16(&d[pos + 10]);
            sr = (int)rd32(&d[pos + 12]);
            bits = rd16(&d[pos + 22]);
            if (fmtTag == 0xFFFE && len >= 26 && pos + 8 + 26 <= d.size()) fmtTag = rd16(&d[pos + 8 + 24]);   // extensible: first 2 bytes of the sub format
        } else if (std::memcmp(&d[pos], "data", 4) == 0) {
            dataPos = pos + 8;
            dataLen = std::min<size_t>(len, d.size() - dataPos);   // a file that is still being written / has a 0 length header: use what is there
            if (len == 0 || len == 0xFFFFFFFFu) dataLen = d.size() - dataPos;
            break;
        }
        pos += 8 + (size_t)len + (len & 1);
    }
    if (!dataPos || ch < 1 || sr < 1000) { error = "unsupported wav: " + path; return nullptr; }
    const int bytes = bits / 8;
    if (!((fmtTag == 1 && (bits == 16 || bits == 24 || bits == 32 || bits == 8)) || (fmtTag == 3 && (bits == 32 || bits == 64)))) { error = "unsupported wav format (" + std::to_string(bits) + " bit): " + path; return nullptr; }
    const long long frames = (long long)(dataLen / ((size_t)bytes * (size_t)ch));
    auto w = std::make_shared<Wav>();
    w->sr = sr;
    w->frames = frames;
    w->lr.resize((size_t)frames * 2);
    auto sample = [&](size_t idx) -> float {
        const unsigned char* p = &d[dataPos + idx * (size_t)bytes];
        if (fmtTag == 3) { if (bits == 32) { float v; std::memcpy(&v, p, 4); return v; } double v; std::memcpy(&v, p, 8); return (float)v; }
        if (bits == 8) return ((int)p[0] - 128) / 128.f;
        if (bits == 16) return (int16_t)rd16(p) / 32768.f;
        if (bits == 24) { int32_t v = (p[0] << 8) | (p[1] << 16) | ((int32_t)p[2] << 24); return (float)(v >> 8) / 8388608.f; }
        return (int32_t)rd32(p) / 2147483648.f;
    };
    for (long long i = 0; i < frames; ++i) {
        const size_t base = (size_t)i * (size_t)ch;
        const float l = sample(base);
        const float r = ch > 1 ? sample(base + 1) : l;
        w->lr[(size_t)i * 2] = std::isfinite(l) ? l : 0.f;
        w->lr[(size_t)i * 2 + 1] = std::isfinite(r) ? r : 0.f;
    }
    g_cache[path] = {stamp, w};
    return w;
}

void fillBuffer(int16_t* out) {
    std::vector<float> mix((size_t)kBufFrames * 2, 0.f);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (Voice& v : g_voices) {
            const double ratio = (double)v.w->sr / (double)g_sr;
            for (int i = 0; i < kBufFrames; ++i) {
                const long long t = g_clock + i;
                if (t < v.startFrame || t >= v.endFrame) continue;
                const double fp = (double)v.skipFrames + (double)(t - v.startFrame) * ratio;
                const long long i0 = (long long)fp;
                if (i0 >= v.w->frames) continue;
                const long long i1 = std::min(i0 + 1, v.w->frames - 1);
                const float fr = (float)(fp - (double)i0);
                float gain = 1.f;
                if (v.fadeFrom >= 0 && v.endFrame > v.fadeFrom) gain = std::max(0.f, (float)(v.endFrame - t) / (float)(v.endFrame - v.fadeFrom));
                const float* a = &v.w->lr[(size_t)i0 * 2];
                const float* b = &v.w->lr[(size_t)i1 * 2];
                mix[(size_t)i * 2] += (a[0] + (b[0] - a[0]) * fr) * gain;
                mix[(size_t)i * 2 + 1] += (a[1] + (b[1] - a[1]) * fr) * gain;
            }
        }
        g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [](const Voice& v) { return v.endFrame <= g_clock + kBufFrames; }), g_voices.end());
        g_clock += kBufFrames;
    }
    for (size_t i = 0; i < mix.size(); ++i) {
        const float s = std::max(-1.f, std::min(1.f, mix[i]));
        out[i] = (int16_t)std::lrintf(s * 32767.f);
    }
}

void mixerThread() {
    while (g_run) {
        bool any = false;
        for (int b = 0; b < kBufCount; ++b) {
            if (!(g_hdr[b].dwFlags & WHDR_DONE)) continue;
            fillBuffer(g_buf[b].data());
            waveOutWrite(g_hwo, &g_hdr[b], sizeof(WAVEHDR));
            any = true;
        }
        if (!any) Sleep(4);
    }
}

}  // namespace

bool running() { return g_run; }
double latencyMs() { return 1000.0 * (double)(kBufFrames * (kBufCount / 2 + 1)) / (double)std::max(8000, g_sr); }

bool start(int sampleRate, std::string& error) {
    if (g_run) return true;
    g_sr = std::max(8000, sampleRate);
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)g_sr;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_hwo = nullptr; error = "could not open the sound card (default output device)"; return false; }
    { std::lock_guard<std::mutex> lk(g_mx); g_voices.clear(); g_clock = 0; }
    for (int b = 0; b < kBufCount; ++b) {
        g_buf[b].assign((size_t)kBufFrames * 2, 0);
        g_hdr[b] = {};
        g_hdr[b].lpData = reinterpret_cast<LPSTR>(g_buf[b].data());
        g_hdr[b].dwBufferLength = (DWORD)(g_buf[b].size() * sizeof(int16_t));
        waveOutPrepareHeader(g_hwo, &g_hdr[b], sizeof(WAVEHDR));
        g_hdr[b].dwFlags |= WHDR_DONE;   // free: the mixer thread fills and queues it
    }
    g_run = true;
    g_thread = std::thread(mixerThread);
    return true;
}

void stop() {
    if (!g_run && !g_hwo) return;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    if (g_hwo) {
        waveOutReset(g_hwo);
        for (int b = 0; b < kBufCount; ++b) waveOutUnprepareHeader(g_hwo, &g_hdr[b], sizeof(WAVEHDR));
        waveOutClose(g_hwo);
        g_hwo = nullptr;
    }
    std::lock_guard<std::mutex> lk(g_mx);
    g_voices.clear();
}

bool play(const std::string& path, const std::string& tag, double delay, double dur, double offset, std::string& error) {
    if (!g_run) { error = "the Pd / SC player is not running"; return false; }
    std::lock_guard<std::mutex> lk(g_mx);
    std::shared_ptr<const Wav> w = loadWav(path, error);
    if (!w) return false;
    Voice v;
    v.w = w;
    v.tag = tag;
    v.startFrame = g_clock + (long long)(std::max(0.0, delay) * g_sr) + kBufFrames;   // one buffer of head room: the timeline thread and the mixer are not in lockstep
    v.skipFrames = (long long)(std::max(0.0, offset) * w->sr);
    const double fileLeft = (double)(w->frames - v.skipFrames) / (double)w->sr;
    const double len = std::min(dur > 0.0 ? dur : fileLeft, fileLeft);
    if (len <= 0.0) return true;
    v.endFrame = v.startFrame + (long long)(len * g_sr);
    g_voices.push_back(std::move(v));
    return true;
}

void off(const std::string& tag) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (Voice& v : g_voices) {
        if (!tag.empty() && v.tag != tag) continue;
        if (v.startFrame > g_clock) { v.endFrame = v.startFrame; continue; }   // not started yet: gone
        const long long fadeEnd = std::min(v.endFrame, g_clock + kBufFrames + 441);
        v.fadeFrom = g_clock + kBufFrames;
        v.endFrame = fadeEnd;
    }
}

}  // namespace wavbridge
