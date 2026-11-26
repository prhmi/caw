#pragma once
// CAW AUDIO BUSES: named audio streams that run BETWEEN the engines (Csound, Pure Data, SuperCollider,
// sample tracks). The number hub (caw_hub.h: caws / cawr) carries control values; this carries
// SIGNALS. Every name is one stereo bus. Any engine may WRITE into a bus (several writers add up)
// and any engine may READ it.
//
//   Csound           cawbsend "name", aL, aR          aL, aR cawbrecv "name"
//   Pure Data        [s~ $0-cawsend-name-L] / -R      [r~ $0-cawrecv-name-L] / -R   (adc~ is NOT used)
//                    (the patch declares the bus by name; CAW copies the audio in and out around each 64-frame tick)
//   SuperCollider    ~cawbsend.("name", sig)           ~cawbrecv.("name")    (stereo In.ar / Out.ar on a reserved bus pair)
//   Sample tracks    every sample track also writes its stereo mix into the bus "smp_<track name>"
//
// Timing: each engine runs in its own callback, so a bus is a ONE-BLOCK delay between two engines
// (a ring of a few ms). Inside one engine (two Csound clips) it is sample-aligned like chnmix/chnget.
// A reader that starts while nobody writes hears silence; nothing ever blocks or allocates on the
// audio path after the first block that names a bus.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cawbus {
// One bus: a lock-free accumulate-then-publish double buffer. Writers ADD into the open block of the
// current cycle; the engine that "owns" the cycle publishes it (swaps) once per block, readers read the
// last published block. Because the engines run at different block sizes, the stream is kept in a ring
// of frames: writers add at their own write head, readers read behind it. That gives a stable
// one-block delay with no clicks when the block sizes differ (Csound ksmps 32, PD 64, SC 64, device 256).
struct Bus {
    static constexpr size_t CAP = 1u << 14;   // 16384 frames ring (about 340 ms at 48 kHz)
    static constexpr int CH = 2;
    static constexpr int kWriters = 16;
    std::array<float, CAP * CH> buf{};
    std::mutex m;                              // only taken when a port REGISTERS (first use), never per block
    std::array<std::atomic<std::uint64_t>, kWriters> wpos{};   // each writer's position (0 = free slot); readers stay behind the SLOWEST live writer
    std::atomic<std::uint64_t> head{0};        // furthest frame any writer has reached
    std::atomic<std::uint64_t> cleared{0};     // frames below this are zeroed for the next lap
    // The frame every live writer has passed: readers never read beyond it (a burst-rendering writer
    // runs ahead; a steady one is behind - reading past the slow one would drop its audio).
    std::uint64_t safe() const {
        const std::uint64_t h = head.load(std::memory_order_acquire);
        std::uint64_t lo = h;
        for (const auto& w : wpos) {
            const std::uint64_t p = w.load(std::memory_order_acquire);
            if (p == 0) continue;
            if (p + CAP / 4 < h) continue;     // a writer that stopped (its engine is off): ignored
            lo = std::min(lo, p);
        }
        return lo;
    }
};
inline std::mutex& registryMutex() { static std::mutex m; return m; }
inline std::map<std::string, std::shared_ptr<Bus>>& registry() { static auto* r = new std::map<std::string, std::shared_ptr<Bus>>(); return *r; }
inline std::shared_ptr<Bus> bus(const std::string& name) {
    if (name.empty() || name.size() > 100) return {};
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& b = registry()[name];
    if (!b) b = std::make_shared<Bus>();
    return b;
}
inline std::vector<std::string> names() {
    std::lock_guard<std::mutex> lock(registryMutex());
    std::vector<std::string> out; for (const auto& kv : registry()) out.push_back(kv.first); return out;
}

struct Port {
    std::string busName;
    std::shared_ptr<Bus> b;
    std::uint64_t pos = 0;
    int slot = -1;           // writer slot in Bus::wpos
    bool registered = false;
    void rename(const std::string& n) { if (n != busName) { release(); busName = n; b.reset(); registered = false; } }
    void release() {
        if (b && slot >= 0) b->wpos[(size_t)slot].store(0, std::memory_order_release);
        slot = -1;
    }
    ~Port() { release(); }
    bool ensure(bool writer) {
        if (!b) b = bus(busName);
        if (!b) return false;
        if (!registered) {
            std::lock_guard<std::mutex> lock(b->m);
            const std::uint64_t h = b->head.load(std::memory_order_acquire);
            if (writer) {
                pos = std::max<std::uint64_t>(h, 1);
                for (int k = 0; k < Bus::kWriters; ++k) {
                    std::uint64_t expected = 0;
                    if (b->wpos[(size_t)k].compare_exchange_strong(expected, pos, std::memory_order_acq_rel)) { slot = k; break; }
                }
            } else {
                const std::uint64_t s = b->safe();
                pos = s > 256 ? s - 256 : 0;
            }
            registered = true;
        }
        return true;
    }
};
// ADD `frames` stereo frames (interleaved L R) into the bus at this writer's position.
inline void write(Port& p, const float* lr, size_t frames) {
    if (!lr || frames == 0 || !p.ensure(true)) return;
    Bus& b = *p.b;
    for (size_t i = 0; i < frames; ++i) {
        const size_t slot = (size_t)((p.pos + i) % Bus::CAP) * Bus::CH;
        b.buf[slot] += lr[i * 2];
        b.buf[slot + 1] += lr[i * 2 + 1];
    }
    p.pos += frames;
    if (p.slot >= 0) b.wpos[(size_t)p.slot].store(p.pos, std::memory_order_release);
    std::uint64_t h = b.head.load(std::memory_order_relaxed);
    while (h < p.pos && !b.head.compare_exchange_weak(h, p.pos, std::memory_order_release, std::memory_order_relaxed)) {}
}
// READ `frames` stereo frames into lr, `lag` frames behind the slowest live writer (never a frame
// that is still being added to). A reader that drifted too far (paused engine) resyncs.
inline void read(Port& p, float* lr, size_t frames, size_t lag = 256) {
    if (!lr || frames == 0) return;
    std::memset(lr, 0, frames * 2 * sizeof(float));
    if (!p.ensure(false)) return;
    Bus& b = *p.b;
    const std::uint64_t safe = b.safe();
    const std::uint64_t want = safe > lag + frames ? safe - lag - frames : 0;
    if (p.pos + Bus::CAP / 4 < want || p.pos > want + 4 * lag + frames) p.pos = want;
    for (size_t i = 0; i < frames; ++i) {
        const std::uint64_t at = p.pos + i;
        if (at >= safe) break;   // not complete yet: silence
        const size_t slot = (size_t)(at % Bus::CAP) * Bus::CH;
        lr[i * 2] = b.buf[slot];
        lr[i * 2 + 1] = b.buf[slot + 1];
    }
    p.pos += frames;
}
// Zero the ring lap that is CAP/2 behind the head, so a writer always adds into silence on the next
// lap. Every writer calls it after writing; it only clears what nobody cleared yet (an atomic mark).
inline void consumeOld(Port& p) {
    if (!p.b) return;
    Bus& b = *p.b;
    const std::uint64_t h = b.head.load(std::memory_order_acquire);
    if (h < Bus::CAP / 2) return;
    const std::uint64_t target = std::min(h - Bus::CAP / 2, b.safe() > Bus::CAP / 4 ? b.safe() - Bus::CAP / 4 : 0);
    std::uint64_t from = b.cleared.load(std::memory_order_acquire);
    if (from >= target) return;
    if (!b.cleared.compare_exchange_strong(from, target, std::memory_order_acq_rel)) return;   // another writer clears it
    if (target - from > Bus::CAP / 2) from = target - Bus::CAP / 2;
    for (std::uint64_t at = from; at < target; ++at) {
        const size_t slot = (size_t)(at % Bus::CAP) * Bus::CH;
        b.buf[slot] = 0.f; b.buf[slot + 1] = 0.f;
    }
}
// write() + consumeOld() in one call: the normal writer entry point.
inline void send(Port& p, const float* lr, size_t frames) { write(p, lr, frames); consumeOld(p); }

// Names of the per-track automatic buses (UI thread publishes, audio threads read with try_lock):
// every SuperCollider track writes "sc_<track name>", every sample track "smp_<track name>",
// every Pure Data clip "pd_<track name>" (the raw track mix, before the fader).
struct NameTable {
    std::mutex m;
    std::map<int, std::string> byKey;   // SC: track slot; sample: track id
};
inline NameTable& scSlotNames() { static NameTable t; return t; }
inline NameTable& sampleTrackNames() { static NameTable t; return t; }
inline std::string safeName(const std::string& n) {
    std::string o;
    for (char c : n) o += (std::isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    return o;
}
}  // namespace cawbus
