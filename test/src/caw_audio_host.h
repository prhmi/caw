#pragma once
#include <portaudio.h>
#include <mutex>
#include <string>

// One PortAudio host for the whole app. Pa_Initialize / Pa_Terminate are process
// global: if one engine's stream closes while another engine's stream is still
// running (PD stopped while SC plays, a device scan while a stream is open ...),
// that terminate pulls the card away from the other engine. Every PortAudio user
// in CAW takes a reference here instead: the host initializes once and terminates
// only when the last user is gone, and the init / terminate calls are serialized.
namespace caw_audio_host {
inline std::mutex& mutex() { static std::mutex m; return m; }
inline int& refs() { static int r = 0; return r; }

inline bool begin(std::string* error = nullptr) {
    std::lock_guard<std::mutex> lock(mutex());
    if (refs() == 0) {
        const PaError e = Pa_Initialize();
        if (e != paNoError) {
            if (error) *error = std::string("PortAudio initialization: ") + Pa_GetErrorText(e);
            return false;
        }
    }
    ++refs();
    return true;
}
inline void end() {
    std::lock_guard<std::mutex> lock(mutex());
    if (refs() > 0 && --refs() == 0) Pa_Terminate();
}
// RAII form for short operations (a device scan).
struct Ref {
    bool ok = false;
    explicit Ref(std::string* error = nullptr) { ok = begin(error); }
    ~Ref() { if (ok) end(); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
};
}
