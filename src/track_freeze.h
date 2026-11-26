#pragma once
// TRACK FREEZE (Cubase-style lock) - included from engine_dispatch.cpp after app_shared.h.
//
// Freezing a track renders it ONCE into a WAV file on disk and locks it: the live engines stop
// processing the track (its clips are no longer scheduled, its plugin instances are dropped, its
// automation stops streaming) and the frozen WAV plays through the sample engine like one long
// clip instead - a tiny fixed CPU cost no matter how heavy the track was. The track draws dimmed
// and cannot be edited until it is unfrozen (unfreeze deletes the WAV, the live sound returns).
//
// The render pass: the transport plays once from the track's earliest clip to its last clip end
// (+ a 2 s tail) while ONLY this track is audible (every other track is temporarily muted, the
// target is soloed, the master FX chain and the VSTe routing are bypassed - the WAV is the raw
// track sound). The audio callback copies the final mix BEFORE the master chain into a lock-free
// sink (FreezeCaptureSink in audio_output.h); a writer thread outside the callback writes the
// WAV. A modal progress dialog blocks edits while the pass runs.
//
// The track volume slider is NOT baked: during the capture it is forced to unity and the frozen
// clip carries the slider value at playback time, so the fader keeps working on frozen tracks.
// The "volume" automation lane IS baked (it rides on top of unity during the capture), like every
// other automation of the track. The WAV keeps the few milliseconds of pre-roll it captures before
// the transport goes live (one or two device buffers) - the same buffer-granular alignment the
// live playback has, no trimming, so quiet content at the very start of the range is never cut.
//
// NOTE: only the STATE lives in this header. The functions are plain definitions at the end of
// engine_dispatch.cpp (the public API is declared in app_internal.h) - an inline function defined
// in a header that only one TU includes is DISCARDED by MSVC when that TU never calls it, which
// broke the link with LNK2019.
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "wav_writer.h"
#include "audio_output.h"

// The timeline clip id of a frozen track's WAV voice (negative: real clip ids are positive).
inline int freezeClipId(int trackId) { return -1000000 - trackId; }

struct FreezeRender {
    // Reset the state for a new pass (atomics cannot be assigned: every field is reset here;
    // the caller must have joined any previous writer thread and released the sink before).
    void resetForNewPass() {
        phase = 0;
        vsteBounce = false;
        trackIdx = -1;
        trackId = 0;
        trackName.clear();
        t0 = t1 = 0.0;
        savedMuteSolo.clear();
        savedLoopEnabled = false;
        savedLoopStart = savedLoopEnd = 0.0;
        savedMasterChain = false;
        savedVstInputs.clear();
        savedVolume = 1.f;
        tmpPath.clear();
        finalPath.clear();
        offline = false;
        cppOffline = false;
        smpOffline = false;
        scOffline = false;
        csdPath.clear();
        offlineWavPath.clear();
        rate = 48000;
        sink.quiesce();
        sink.reset();
        stopWriter.store(false, std::memory_order_release);
        writeFailed.store(0, std::memory_order_release);
        settle = 0;
        error.clear();
        exportPass = false;
    }
    int phase = 0;                    // 0 idle, 1 capturing (transport rolling), 2 finishing (draining + writing)
    bool vsteBounce = false;          // this pass renders a VSTe rack: its (still live) sources feed it during the capture
    int trackIdx = -1;
    int trackId = 0;
    std::string trackName;
    double t0 = 0.0, t1 = 0.0;        // timeline extent being rendered (t1 includes the tail)
    std::vector<std::array<bool, 2>> savedMuteSolo;   // parallel to g_proj.tracks: {muted, solo}
    bool savedLoopEnabled = false;
    double savedLoopStart = 0.0, savedLoopEnd = 0.0;
    bool savedMasterChain = false;
    std::vector<std::array<bool, 8>> savedVstInputs;  // per VSTe track: the 8 input on-flags
    float savedVolume = 1.f;          // the target track's volume slider (bypassed during the capture)
    std::string tmpPath, finalPath;
    // offline pass (Csound / VSTi / sample tracks): the render runs through the offline Csound
    // host at full CPU speed, silently - no transport, no device audio. The rendered project WAV
    // is then sliced into the freeze WAV. SuperCollider tracks render with the NRT pipeline
    // (sclang virtual-time capture + scsynth -N, sc_nrt.h) and PD with CAW's C++ PD host.
    bool offline = false;
    bool cppOffline = false;          // the offline pass is the pure C++ VST host (rack_offline.h), not the Csound offline host
    bool smpOffline = false;          // the offline pass is the pure C++ sample engine (the sample track's own freeze)
    bool scOffline = false;           // the offline pass is the SuperCollider NRT pipeline (sc_nrt.h)
    std::string csdPath, offlineWavPath;
    std::chrono::steady_clock::time_point startedAt;
    int rate = 48000;
    CawWavWriter wav;                 // touched by the writer thread only
    FreezeCaptureSink sink;           // stable address: an audio callback may still hold it across stop
    std::thread writer;
    std::atomic<bool> stopWriter{false};
    std::atomic<int> writeFailed{0};  // the writer thread could not write samples (disk full, file gone)
    int settle = 0;                   // frames waited for the offline worker's final atomic stores
    std::string error;                // last failure (shown in the console and the dialog)
    bool exportPass = false;          // this pass renders ONE PART of the whole-project offline export (no lock at the end)
};
inline FreezeRender g_freeze;

// Internal helpers, defined (with the public API) at the end of engine_dispatch.cpp.
void freezeRestoreStates();                     // put back every mixer state the capture pass touched
void freezeStopWriter(bool finishWav);          // stop + join the writer thread, close the WAV
void freezeWavPaths(const Track& t);            // fill tmpPath / finalPath from the project folder
