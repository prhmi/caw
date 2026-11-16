#pragma once
// Engine-neutral data layer. Csound today, SuperCollider later: both talk to plugins ONLY through this.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace caw {

struct MidiMsg { double t = 0; uint8_t status = 0, d1 = 0, d2 = 0; };           // t = seconds from block start (0 = now)
struct ParamMsg { int paramIndex = 0; double value = 0; };                       // normalized 0..1

// Single-producer / single-consumer lock-free ring (audio thread <-> engine thread).
template <class T, size_t N = 4096>
class Ring {
public:
    bool push(const T& v) { size_t w = w_.load(std::memory_order_relaxed), n = (w + 1) % N;
        if (n == r_.load(std::memory_order_acquire)) return false; buf_[w] = v; w_.store(n, std::memory_order_release); return true; }
    bool pop(T& v) { size_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false; v = buf_[r]; r_.store((r + 1) % N, std::memory_order_release); return true; }
private:
    T buf_[N]; std::atomic<size_t> w_{0}, r_{0};
};

// A named audio bus: unlimited sources can add into it (VSTe "empty space with unlimited inputs").
struct AudioBus {
    std::string name; int channels = 2; int frames = 0;
    std::vector<float> data;                     // interleaved
    void resize(int ch, int fr) { channels = ch; frames = fr; data.assign(size_t(ch) * fr, 0.f); }
    void clear() { std::fill(data.begin(), data.end(), 0.f); }
    void add(const float* in, int ch, int fr) { int c = std::min(ch, channels), f = std::min(fr, frames);
        for (int i = 0; i < f; ++i) for (int k = 0; k < c; ++k) data[size_t(i) * channels + k] += in[size_t(i) * ch + k]; }
};

// One endpoint per plugin target, addressed by NAME (same idea as free automation: the track name is the address).
struct Endpoint {
    Ring<MidiMsg> midiIn;      // engine code (cawsm) -> plugin
    Ring<MidiMsg> midiOut;     // plugin / device -> engine code (cawrm)
    Ring<ParamMsg> paramIn;    // cawsp -> plugin
    std::vector<std::atomic<float>> paramLast;   // last value of each parameter, read by cawgp
};

class Hub {
public:
    static Hub& get() { static Hub h; return h; }
    std::shared_ptr<Endpoint> endpoint(const std::string& name) {
        std::lock_guard<std::mutex> l(m_); auto& e = eps_[name]; if (!e) e = std::make_shared<Endpoint>(); return e; }
    std::shared_ptr<AudioBus> bus(const std::string& name) {
        std::lock_guard<std::mutex> l(m_); auto& b = buses_[name]; if (!b) { b = std::make_shared<AudioBus>(); b->name = name; } return b; }
private:
    std::mutex m_; std::map<std::string, std::shared_ptr<Endpoint>> eps_; std::map<std::string, std::shared_ptr<AudioBus>> buses_;
};

// Engine adapter: the only thing that differs between Csound and SuperCollider.
struct IEngineBridge {
    virtual ~IEngineBridge() = default;
    virtual const char* name() const = 0;                 // "csound" / "supercollider"
    virtual void emitHelpers(std::string& out) const = 0; // generated code for cawsm / cawrm / cawsp / cawgp / cawsa / cawra
};

} // namespace caw
