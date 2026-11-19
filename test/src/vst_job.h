#pragma once
#include <string>
#include <vector>
#include <map>
#include <array>
#include <atomic>
#include <mutex>
#include <cstdint>
// One VST3 track as the Csound engine thread sees it.
struct VstJob {
    int id = 0;                    // Track::id: the Csound channels are cawvst_<id>_...
    int meterFirst = -1;           // stereo indices in the monitor snapshot, -1 disables metering
    std::string name;              // track name = the audio bus name used by cawsa "name", aL, aR
    std::string path;              // .vst3 file
    std::map<uint32_t, double> parameterState; // stable plugin parameter IDs
    bool instrument = true;        // true = VSTi (MIDI in), false = VSTe (audio in)
    std::vector<int> pick;         // plugin parameter index of kP1..kP8
    std::vector<double> init;      // start value of kP1..kP8 (normalized)
    // The 8 VSTe input slots, mirroring Track::vstInputs. lineStart / lineCount are the input lines
    // the source occupies (resolved at CSD build time, stable for the run). The rack copies every
    // ENABLED slot's lines into the plugin input packed from line 1, so switching an input on/off
    // live never leaves silent gaps in front of a later input.
    //   mode 0 = track, 1 = clips, 2 = master (spout copy).
    //   engine 0 = Csound lines (cawvstin_<id>_<line> channels, fed by the CSD routing);
    //             1 = the Pure Data mix (rendered by the PD bridge outside Csound);
    //             2 = the SuperCollider mix (rendered in CAW's audio callback);
    //             3 = a VSTi / VSTe source (srcId = its track id): the rack wires the source
    //                 plugin's own stereo output into the slot, exactly the channels the source
    //                 sends to the master - a VST track's audio never runs through Csound.
    //             PD / SC / VST sources cannot be split per clip: the whole source mix feeds the slot.
    struct Input { bool on = false; int mode = 0; std::string name; int lineStart = 0; int lineCount = 0; int engine = 0; int srcId = 0; };
    Input inputs[8];
    bool routedIn = false;         // true if any input is configured (the 8-line buffer is used instead of silence)
    int outMode = 0;               // 0 = master (replace source), 1 = mix (source + VSTe to master)
};
// Engine thread -> UI thread mirror of the parameter values the rack is actually using. The UI
// reflects them into the plugin editor GUI (so automation moves the knobs) without touching the
// audio path. Values are the normalized kP1..kP8 channel values per VstJob::id.
struct VstParamMirror {
    std::mutex mutex;
    std::map<int, std::array<double, 8>> values;
};
// One manual parameter override from the UI: the rack applies it on the SAME block (it bypasses
// the Csound channels completely), so a slider drag is always heard immediately - even while a
// clip overwrites the channel, while no clip runs at all, or while automation moves the lane.
struct VstParamOverride {
    int trackId = 0; int slot = 0; double value = 0;   // slot = 0..7 (kP1..kP8), value normalized 0..1
};
// PortAudio callback -> engine thread ring with the SuperCollider mix (interleaved stereo). The
// callback produces at device rate, the engine consumes one Csound block per block; the ring
// absorbs the clock drift (oldest frames are dropped on overrun, underruns read as silence).
struct CawScRing {
    static constexpr size_t CAP = 1u << 16;            // frames of stereo
    std::array<float, CAP * 2> buf{};
    std::atomic<size_t> r{0}, w{0};                    // frame positions, wrapping
    void push(const float* interleaved, size_t frames) {   // callback thread: device-scale samples
        for (size_t i = 0; i < frames; ++i) {
            const size_t wpos = w.load(std::memory_order_relaxed);
            if ((wpos + 1) % CAP == r.load(std::memory_order_acquire))
                r.store((r.load(std::memory_order_relaxed) + 1) % CAP, std::memory_order_release);   // full: drop the oldest
            buf[(size_t)wpos * 2] = interleaved[i * 2];
            buf[(size_t)wpos * 2 + 1] = (i * 2 + 1 < frames * 2) ? interleaved[i * 2 + 1] : 0.f;
            w.store((wpos + 1) % CAP, std::memory_order_release);
        }
    }
    void pop(float* out, size_t frames) {                  // engine thread: fills exactly frames*2 floats
        for (size_t i = 0; i < frames; ++i) {
            const size_t rpos = r.load(std::memory_order_relaxed);
            if (rpos == w.load(std::memory_order_acquire)) { out[i * 2] = out[i * 2 + 1] = 0.f; continue; }   // underrun: silence
            out[i * 2] = buf[(size_t)rpos * 2]; out[i * 2 + 1] = buf[(size_t)rpos * 2 + 1];
            r.store((rpos + 1) % CAP, std::memory_order_release);
        }
    }
};
// Handoff of engine-side realtime mixes to the VST rack (set once before the rack starts).
struct VstEngineAudio {
    const std::atomic<const float*>* pdMix = nullptr;  // the latest Pure Data render (interleaved, pdMixChannels)
    int pdMixChannels = 2;
    CawScRing* scRing = nullptr;                       // the SuperCollider mix (stereo, device scale)
};
