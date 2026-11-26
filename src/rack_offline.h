// Pure C++ OFFLINE renderer for a VSTe rack lock (LKD). No Csound anywhere: the crash that
// killed the app during the offline bounce happened inside csound7_64.dll while the rack ran
// hosted in the offline Csound worker. This renderer is the same pure C++ path the live 4th
// engine uses (VstRack with no Csound pointers) - the configuration that provably never crashes.
//
// What it renders, block by block at full CPU speed:
//   - the rack's plugins (FX-only: only the effect's output lands in the WAV),
//   - frozen source tracks through their rendered WAVs, decoded streaming from disk
//     (a multi-gigabyte freeze never loads whole) and pushed into the same per-track
//     sample rings the live engine uses (engine 4),
//   - unfrozen VSTi sources natively (the rack wires their output into the effect, engine 3),
//   - the MIDI clips of the rack and of unfrozen VSTi sources, scheduled on the block clock.
// The output is the same ".full.wav" the Csound offline host produced, so the existing
// freeze finalizer (slicing the frozen range into the freeze WAV) works unchanged.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "vst_rack.h"
#include "vst_job.h"
#include "vst_solo.h"   // VstScript + sampleVstAuto: the offline rack renders clip-code / lane parameter changes too
#include "wav_writer.h"
#include "pd_timeline.h"
#include "pd_host_mixer.h"

namespace cawcpp {

// ---- minimal streaming WAV reader (the frozen WAVs are CawWavWriter output: pcm16/24/32 or float32) ----
struct WavInfo { unsigned fmt = 0, chans = 0, bits = 0; int rate = 0; long long dataPos = -1; unsigned long long dataSize = 0; };

inline bool probeWavCpp(const std::string& path, WavInfo& p, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "the file could not be opened."; return false; }
    char head[12];
    in.read(head, 12);
    if (in.gcount() < 12 || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) {
        error = "not a WAV file."; return false;
    }
    bool haveFmt = false;
    for (;;) {
        char id[4] = {}; uint32_t sz = 0;
        in.read(id, 4);
        in.read(reinterpret_cast<char*>(&sz), 4);
        if (in.gcount() < 4) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            char fmt[40] = {};
            const uint32_t take = sz < 40 ? sz : 40;
            in.read(fmt, take);
            in.ignore(static_cast<std::streamsize>(sz - take));
            std::memcpy(&p.fmt, fmt, 2);
            std::memcpy(&p.chans, fmt + 2, 2);
            std::memcpy(&p.rate, fmt + 4, 4);
            std::memcpy(&p.bits, fmt + 14, 2);
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            p.dataPos = static_cast<long long>(in.tellg());
            p.dataSize = sz;
            break;   // data is last in CAW's own writer; stop there
        } else {
            in.ignore(static_cast<std::streamsize>(sz));
        }
        if (!in || in.eof()) break;
    }
    if (!haveFmt || p.dataPos < 0 || p.chans == 0 || p.bits == 0 || (p.bits % 8) != 0) {
        error = "unsupported WAV layout."; return false;
    }
    return true;
}

inline void decodeWavBlock(const unsigned char* raw, size_t bytes, unsigned fmt, unsigned chans, unsigned bits, std::vector<float>& out) {
    const size_t frameBytes = (size_t)chans * (size_t)(bits / 8);
    const size_t frames = bytes / frameBytes;
    out.resize(frames * (size_t)chans, 0.f);
    for (size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < chans; ++c) {
            const size_t o = (i * (size_t)chans + c) * (size_t)(bits / 8);
            float v = 0.f;
            if (fmt == 3 && bits == 32) {
                if (o + 4 <= bytes) std::memcpy(&v, raw + o, 4);
            } else if (bits == 16) {
                if (o + 2 <= bytes) v = (float)(short)((unsigned short)raw[o] | ((unsigned short)raw[o + 1] << 8)) / 32768.f;
            } else if (bits == 24) {
                if (o + 3 <= bytes) v = (float)((int)(((unsigned)raw[o] << 16) | ((unsigned)raw[o + 1] << 8) | (unsigned)raw[o + 2]) << 8) / 2147483648.f;
            } else if (bits == 32) {
                if (o + 4 <= bytes) v = (float)((int)((unsigned)raw[o] | ((unsigned)raw[o + 1] << 8) | ((unsigned)raw[o + 2] << 16) | ((unsigned)raw[o + 3] << 24))) / 2147483648.f;
            }
            out[i * (size_t)chans + c] = v;
        }
}

// One open frozen WAV, decoded per requested window (sequential use: one disk read per block).
struct WavReader {
    std::ifstream in;
    WavInfo p;
    unsigned long long totalFrames = 0;
    unsigned long long winStart = 0;
    std::vector<float> win;
    bool open(const std::string& path, std::string& error) {
        if (!probeWavCpp(path, p, error)) return false;
        totalFrames = p.dataSize / ((unsigned long long)p.chans * (unsigned long long)(p.bits / 8));
        in.open(path, std::ios::binary);
        if (!in) { error = "the file could not be opened."; return false; }
        return true;
    }
    void read(unsigned long long frame, int count, std::vector<float>& out) {
        out.assign((size_t)count * (size_t)p.chans, 0.f);
        if (!in || frame >= totalFrames) return;
        const unsigned long long end = frame + (unsigned long long)count < totalFrames ? frame + (unsigned long long)count : totalFrames;
        const unsigned frameBytes = p.chans * (p.bits / 8);
        if (!(winStart <= frame && end <= winStart + (win.size() / std::max(1u, p.chans)))) {
            std::vector<unsigned char> raw((size_t)(end - frame) * frameBytes);
            in.clear();
            in.seekg(p.dataPos + (long long)(frame * frameBytes));
            in.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size());
            const size_t got = (size_t)in.gcount() / frameBytes * frameBytes;
            decodeWavBlock(raw.data(), got, p.fmt, p.chans, p.bits, win);
            winStart = frame;
        }
        const unsigned long long winFrames = win.size() / std::max(1u, p.chans);
        for (unsigned long long i = frame; i < end; ++i) {
            const unsigned long long w = i - winStart;
            if (w >= winFrames) break;
            for (unsigned c = 0; c < p.chans; ++c)
                out[(size_t)(i - frame) * p.chans + c] = win[w * p.chans + c];
        }
    }
};

// ---- the offline render ----
struct RackCppRender {
    struct SourceStream { std::string path; double start = 0.0; float gain = 1.f; };
    struct Note { double start = 0.0, dur = 0.0; int trackId = 0, pitch = 60, vel = 100; };

    std::atomic<bool> done{true};
    std::atomic<bool> stop{false};
    std::atomic<int> code{-1};

    void start(std::vector<VstJob> jobs, std::map<int, SourceStream> streams, std::vector<Note> notes,
               int rate, std::string wavPath, double maxSeconds, std::vector<VstScript> scripts = {}) {
        startCommon(std::move(jobs), std::move(streams), std::move(notes), rate, std::move(wavPath),
                    maxSeconds, std::string(), std::vector<pd_timeline::Event>(), std::move(scripts));
    }
    // The same render with live Pure Data clips: the C++ PD host mixer renders the PD sources
    // (the rack reads them through ea.pdMix, engine 1) - PD never needs a freeze or the Csound
    // offline host. An empty job set with a plan = a PD-only render (a PD track's own LKD).
    void startPd(std::vector<VstJob> jobs, std::map<int, SourceStream> streams, std::vector<Note> notes,
                 int rate, std::string wavPath, double maxSeconds,
                 std::string pdLibrary, std::vector<pd_timeline::Event> pdPlan, std::vector<VstScript> scripts = {}) {
        startCommon(std::move(jobs), std::move(streams), std::move(notes), rate, std::move(wavPath),
                    maxSeconds, std::move(pdLibrary), std::move(pdPlan), std::move(scripts));
    }
    void startCommon(std::vector<VstJob> jobs, std::map<int, SourceStream> streams, std::vector<Note> notes,
               int rate, std::string wavPath, double maxSeconds,
               std::string pdLibrary, std::vector<pd_timeline::Event> pdPlan, std::vector<VstScript> scripts) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        jobs_ = std::move(jobs);
        streams_ = std::move(streams);
        notes_ = std::move(notes);
        scripts_ = std::move(scripts);   // clip code + automation lanes drive P1..P8 during the render
        rate_ = rate;
        wavPath_ = std::move(wavPath);
        maxSeconds_ = maxSeconds;
        pdLibrary_ = std::move(pdLibrary);
        pdPlan_ = std::move(pdPlan);
        stop.store(false, std::memory_order_release);
        code.store(-1, std::memory_order_release);
        done.store(false, std::memory_order_release);
        { std::lock_guard<std::mutex> l(logMutex_); log_.clear(); }
        error_.clear();
        std::thread([this] { run(); }).detach();   // freezeTick polls done/code; freezeCancel sets stop
    }
    std::vector<std::string> takeLog() {
        std::lock_guard<std::mutex> l(logMutex_);
        std::vector<std::string> out;
        out.swap(log_);
        return out;
    }
    std::string takeError() {
        std::lock_guard<std::mutex> l(logMutex_);
        std::string e;
        e.swap(error_);
        return e;
    }

private:
    void say(const std::string& s) {
        std::lock_guard<std::mutex> l(logMutex_);
        if (log_.size() < 512) log_.push_back(s);
    }
    void fail(const std::string& why) {
        std::lock_guard<std::mutex> l(logMutex_);
        error_ = why;
    }
    void run() {
        std::vector<std::string> initLog;
        auto logFn = [&](const std::string& s) {
            std::lock_guard<std::mutex> l(logMutex_);
            if (log_.size() < 512) log_.push_back(s);
        };
        const int block = 512;
        const int nchan = 2;
        std::map<int, CawScRing> rings;
        for (const auto& kv : streams_) rings[kv.first];
        std::map<int, WavReader> readers;
        for (auto& kv : streams_) {
            WavReader r;
            std::string err;
            if (r.open(kv.second.path, err) && r.totalFrames > 0) {
                say("[caw] Offline render: locked source for track " + std::to_string(kv.first) + " = " + kv.second.path
                  + " (" + std::to_string((double)r.totalFrames / (double)std::max(1, r.p.rate)) + " s, "
                  + std::to_string(r.p.chans) + " ch, " + std::to_string(r.p.bits) + " bit).");
                readers[kv.first] = std::move(r);
            } else {
                say("[caw] Offline render: the locked WAV of track " + std::to_string(kv.first) + " could not be read ("
                  + err + ") - that input stays silent.");
            }
        }
        VstEngineAudio ea;
        ea.sampleRing = [&rings](int trackId) -> CawScRing* {
            auto it = rings.find(trackId);
            return it == rings.end() ? nullptr : &it->second;
        };
        // Live Pure Data sources: the C++ PD host mixer renders the plan block by block; the rack
        // reads the mix through ea.pdMix (engine 1) - the same handoff the live engine uses.
        std::unique_ptr<PdHostMixer> pd;
        std::vector<float> pdBuf;
        std::atomic<const float*> pdMixPtr{nullptr};
        bool pdActive = false;
        if (!pdLibrary_.empty() && !pdPlan_.empty()) {
            pd = std::make_unique<PdHostMixer>();
            std::string pdErr;
            if (pd->prepare(pdLibrary_, pdPlan_, rate_, nchan, (unsigned)block, pdErr)) {
                pd->transport(true, 0.0, 0.0, 0.0, false, rate_, 1);   // the PD clock rolls from project start (no ack handshake offline)
                pdBuf.assign((size_t)block * (size_t)nchan, 0.f);
                ea.pdMix = &pdMixPtr;
                ea.pdMixChannels = nchan;
                pdActive = true;
                say("[caw] Offline render: Pure Data clips render in the C++ PD host mixer.");
            } else {
                say("[caw] Offline render: the PD host mixer failed to prepare (" + pdErr + ") - PD sources stay silent.");
            }
        }
        VstRack rack;
        const bool rackNeeded = !jobs_.empty();
        rack.setSoloMode(true);   // the 4th-engine mode: no Csound pointers anywhere
        std::atomic<bool> okInit{false};
        if (rackNeeded) {
            try {
                okInit.store(rack.init(jobs_, rate_, block, nchan, 4,
                    [](const std::string&, int) -> void* { return nullptr; },   // NO Csound channels: the pure C++ path
                    [](int) -> const void* { return nullptr; },
                    logFn, false, nullptr, &ea));
            } catch (const std::exception& e) { say(std::string("[caw] The VST rack failed to initialize: ") + e.what()); }
              catch (...) { say("[caw] The VST rack failed to initialize."); }
        }
        size_t loaded = rack.loaded();
        if (rackNeeded) {
            say(std::string("[caw] Offline render: ") + (okInit.load() ? "the rack initialized" : "THE RACK FAILED TO INITIALIZE") + ", "
              + std::to_string(loaded) + " of " + std::to_string(jobs_.size()) + " plugin(s) loaded.");
            if (!okInit.load() || loaded == 0) { finish(-1); return; }
        } else {
            say("[caw] Offline render: no plugins to host (PD-only render).");
        }

        CawWavWriter wav;
        std::string werr;
        if (!wav.open(wavPath_, rate_, 2, 2, werr)) { fail("the render WAV could not be written: " + werr); say("[caw] Offline render: " + werr); finish(-1); return; }

        // Sorted note events (on + off): the rack's own MIDI clips and the unfrozen VSTi sources'.
        struct Ev { double t = 0.0; bool on = false; int trackId = 0, pitch = 60, vel = 100; };
        std::vector<Ev> evs;
        evs.reserve(notes_.size() * 2);
        for (const Note& n : notes_) {
            const double dur = n.dur > 0.002 ? n.dur : 0.002;
            evs.push_back({n.start, true, n.trackId, n.pitch, n.vel});
            evs.push_back({n.start + dur, false, n.trackId, n.pitch, 0});
        }
        std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
        size_t evIdx = 0;

        // Clip-code programs compiled once: one interpreter tick per block, exactly like the
        // live VST engine (P1..P8 writes and automation lanes become parameter overrides).
        struct ScriptState {
            VstScript s;
            vstscript::Program prog;
            vstscript::Runtime rt;
            bool started = false;
            double lastP[8] = {};
            bool lastPValid[8] = {};
        };
        std::vector<ScriptState> scripts;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            scripts.reserve(scripts_.size());
            for (const VstScript& s : scripts_) {   // constructed in place (Runtime is not assignable)
                ScriptState st;
                st.s = s;
                scripts.push_back(std::move(st));
            }
        }
        int scriptParams = 0;
        for (ScriptState& st : scripts) {
            try { st.prog = vstscript::Compiler().compile(st.s.code); }
            catch (const std::exception& e) { say("[caw] Offline render: a clip-code program failed to compile: " + std::string(e.what())); st.s.code.clear(); st.prog = vstscript::Program(); }
            catch (...) { say("[caw] Offline render: a clip-code program failed to compile."); st.s.code.clear(); st.prog = vstscript::Program(); }
            st.rt.init(st.prog, st.s.params);
            for (int k = 0; k < 8; ++k) if (st.prog.writesP[k] || !st.s.autoPts[k].empty()) ++scriptParams;
        }
        if (scriptParams > 0) say("[caw] Offline render: " + std::to_string(scriptParams) + " parameter slot(s) ride clip code / automation lanes.");
        for (const ScriptState& st : scripts) {
            std::string slots;
            for (int k = 0; k < 8; ++k) {
                if (st.prog.writesP[k]) slots += " P" + std::to_string(k + 1) + "=code";
                else if (!st.s.autoPts[k].empty()) slots += " P" + std::to_string(k + 1) + "=lane(" + std::to_string(st.s.autoPts[k].size()) + " pts)";
            }
            if (st.s.code.empty()) slots = " (the code did not compile)";
            say("[caw] Offline render: script track " + std::to_string(st.s.trackId) + " clip " + std::to_string(st.s.clipId) +
                " at " + std::to_string(st.s.start) + " s for " + std::to_string(st.s.dur) + " s:" + slots);
        }

        std::vector<float> spout((size_t)block * nchan, 0.f);
        std::vector<float> blk((size_t)block * 2, 0.f);
        std::vector<float> slice;
        std::set<int> activeIds;
        for (const VstJob& j : jobs_) activeIds.insert(j.id);
        struct ScriptNoteOn { int trackId; int pitch; double end; };   // a clip-code note waiting for its note-off (the live engine's active_ list)
        std::vector<ScriptNoteOn> scriptOn;
        unsigned long long frames = 0;
        const unsigned long long capFrames =
            maxSeconds_ > 0.0 ? (unsigned long long)(maxSeconds_ * (double)rate_) : 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const double blockStart = (double)frames / (double)rate_;
            if (capFrames && frames >= capFrames) break;   // the frozen range (+ tail) is fully rendered
            // The locked sources' WAV voices: decode this block's window and push into the rings.
            for (auto& kv : streams_) {
                const auto rit = readers.find(kv.first);
                if (rit == readers.end() || rit->second.totalFrames == 0) continue;
                const int fileRate = rit->second.p.rate > 0 ? rit->second.p.rate : rate_;
                const int fileCh = std::max(1u, rit->second.p.chans);
                const double first = (blockStart - kv.second.start) * (double)fileRate;
                const double last = (blockStart - kv.second.start + (double)block / (double)rate_) * (double)fileRate;
                std::fill(blk.begin(), blk.end(), 0.f);
                if (last > 0.0) {
                    const unsigned long long f0 = first > 0.0 ? (unsigned long long)first : 0;
                    const unsigned long long f1 = (unsigned long long)std::max(first, last) + 1;
                    rit->second.read(f0, (int)std::min<unsigned long long>(f1 - f0, (unsigned long long)1 << 20), slice);
                    for (int n = 0; n < block; ++n) {
                        const double srcFrame = (blockStart - kv.second.start + (double)n / (double)rate_) * (double)fileRate;
                        if (srcFrame < 0.0) continue;
                        const unsigned long long frame = (unsigned long long)srcFrame;
                        if (frame >= rit->second.totalFrames) break;
                        const unsigned long long w = frame - f0;
                        if ((size_t)w * fileCh >= slice.size()) break;
                        blk[(size_t)n * 2] += slice[w * fileCh] * kv.second.gain;
                        blk[(size_t)n * 2 + 1] += slice[w * fileCh + (fileCh > 1 ? 1 : 0)] * kv.second.gain;
                    }
                }
                rings[kv.first].push(blk.data(), (size_t)block);
            }
            // Live Pure Data sources: render this block in the C++ PD host mixer; the rack reads
            // it through ea.pdMix (engine 1) on this very block.
            if (pdActive) {
                const float* pdAudio = nullptr;
                std::string pdErr;
                bool rendered = false;
                try { rendered = pd->render((unsigned)block, pdAudio, pdErr); }
                catch (const std::exception& e) { pdErr = e.what(); }
                catch (...) { pdErr = "unexpected exception"; }
                if (!rendered || !pdAudio) {
                    say("[caw] Offline render: the PD host mixer failed (" + pdErr + ") - PD sources stay silent for this block.");
                    pdMixPtr.store(nullptr, std::memory_order_release);
                } else {
                    std::memcpy(pdBuf.data(), pdAudio, (size_t)block * (size_t)nchan * sizeof(float));
                    pdMixPtr.store(pdBuf.data(), std::memory_order_release);
                }
            }
            // The rack's and the unfrozen VSTi sources' MIDI clips, on the block clock.
            const double blockEnd = blockStart + (double)block / (double)rate_;
            while (evIdx < evs.size() && evs[evIdx].t < blockEnd) {
                const Ev& ev = evs[evIdx];
                caw::MidiMsg m;
                m.t = ev.t > blockStart ? ev.t - blockStart : 0.0;
                m.status = (uint8_t)(ev.on ? 0x90 : 0x80);
                m.d1 = (uint8_t)ev.pitch;
                m.d2 = (uint8_t)ev.vel;
                rack.pushMidi(ev.trackId, m);
                ++evIdx;
            }
            // Clip code + automation lanes drive P1..P8 during the render (the live engine's path):
            // LFOs in the clip code, drawn param ramps and script notes all reach the plugin.
            if (!scripts.empty()) {
                {   // note-offs of the clip-code notes that end inside this block. The live engine sends one
                    // 0x80 per script note after its length; without it every script note rang on forever
                    // in the render (different notes, wrong sustain).
                    const double blockEnd = blockStart + (double)block / (double)rate_;
                    for (size_t a = 0; a < scriptOn.size();) {
                        if (scriptOn[a].end <= blockEnd) {
                            caw::MidiMsg off;
                            off.t = 0.0; off.status = 0x80; off.d1 = (uint8_t)scriptOn[a].pitch; off.d2 = 0;
                            rack.pushMidi(scriptOn[a].trackId, off);
                            scriptOn[a] = scriptOn.back(); scriptOn.pop_back();
                        } else ++a;
                    }
                }
                for (ScriptState& st : scripts) {
                    const VstScript& s = st.s;
                    if (s.code.empty()) continue;
                    const bool inside = blockStart + 1e-9 >= s.start && blockStart < s.start + s.dur - 1e-9;
                    if (!inside) { if (st.started) st.started = false; continue; }
                    bool firstTick = false;
                    if (!st.started) {
                        st.started = true; firstTick = true;
                        st.rt.init(st.prog, s.params);
                        for (int k = 0; k < 8; ++k) st.lastPValid[k] = false;
                    }
                    vstscript::Env env;
                    env.t = std::max(0.0, blockStart - s.start);
                    env.dur = s.dur;
                    env.dt = firstTick ? 0.0 : (double)block / (double)rate_;   // like the live engine: the first tick of a clip does not advance time
                    for (int k = 0; k < 8; ++k) env.p[k] = st.rt.p[k];
                    vstscript::Events ev;
                    vstscript::tick(st.prog, env, st.rt, ev);
                    for (int k = 0; k < 8; ++k) st.rt.p[k] = env.p[k];
                    for (const vstscript::Events::Note& n : ev.notes) {
                        caw::MidiMsg m;
                        m.t = 0.0;
                        m.status = 0x90; m.d1 = (uint8_t)std::min(127, std::max(0, n.pitch)); m.d2 = (uint8_t)std::min(127, std::max(0, n.vel));
                        rack.pushMidi(s.trackId, m);
                        if (scriptOn.size() < 256) scriptOn.push_back({s.trackId, std::min(127, std::max(0, n.pitch)), blockStart + std::max(0.005, n.len)});
                    }
                    for (const vstscript::Events::Midi& mv : ev.midi) {
                        caw::MidiMsg m;
                        m.t = 0.0; m.status = (uint8_t)mv.status; m.d1 = (uint8_t)mv.d1; m.d2 = (uint8_t)mv.d2;
                        rack.pushMidi(s.trackId, m);
                    }
                    for (int k = 0; k < 8; ++k) {
                        double v = -1.0;
                        if (st.prog.writesP[k]) v = std::min(1.0, std::max(0.0, env.p[k]));
                        else if (!s.autoPts[k].empty()) {
                            const double av = sampleVstAuto(s.autoPts[k], blockStart);
                            if (av >= 0.0) v = std::min(1.0, std::max(0.0, av));
                        }
                        if (v < 0.0) continue;
                        if (!st.lastPValid[k] || std::fabs(v - st.lastP[k]) > 1e-4) {
                            st.lastP[k] = v; st.lastPValid[k] = true;
                            rack.pushParamOverride(s.trackId, k, v);
                        }
                    }
                }
            }
            std::fill(spout.begin(), spout.end(), 0.f);
            if (rackNeeded) {
                rack.process(spout.data(), nullptr, 1.f, &activeIds, true, nullptr);
            } else if (pdActive) {
                const float* pm = pdMixPtr.load(std::memory_order_acquire);
                if (pm) std::memcpy(spout.data(), pm, (size_t)block * (size_t)nchan * sizeof(float));   // PD-only render: the PD mix IS the master
            }
            if (!wav.write(spout.data(), (size_t)block, 1.0, 1.f, werr)) {
                fail("the render WAV could not be written: " + werr);
                say("[caw] Offline render: " + werr);
                finish(-1);
                return;
            }
            frames += (unsigned long long)block;
        }
        if (stop.load(std::memory_order_relaxed)) {
            wav.close();   // a discarded pass: the sizes stay unwritten
            say("[caw] Offline render: cancelled at " + std::to_string((double)frames / (double)rate_) + " s.");
            finish(-1);
            return;
        }
        // finish() (not close()): only it writes the RIFF / data chunk lengths - a close()d file
        // keeps dataLen = 0 and reads back as "the WAV file has no samples".
        std::string ferr;
        if (!wav.finish(ferr)) {
            fail("the render WAV could not be finished: " + ferr);
            say("[caw] Offline render: " + ferr);
            finish(-1);
            return;
        }
        say("[caw] Offline render: finished normally, " + std::to_string((double)frames / (double)rate_) + " s rendered.");
        finish(0);
    }
    void finish(int c) {
        code.store(c, std::memory_order_release);
        done.store(true, std::memory_order_release);
    }

    std::vector<VstJob> jobs_;
    std::map<int, SourceStream> streams_;
    std::vector<Note> notes_;
    std::vector<VstScript> scripts_;   // clip code + lane automation: the params change during the render
    int rate_ = 48000;
    std::string wavPath_;
    double maxSeconds_ = 0.0;
    std::string pdLibrary_;
    std::vector<pd_timeline::Event> pdPlan_;
    std::mutex stateMutex_;
    std::mutex logMutex_;
    std::vector<std::string> log_;
    std::string error_;
};

}   // namespace cawcpp
