#pragma once
// Runs the VST3 plugins next to Csound on the engine thread: once per Csound block it reads the channels the track code wrote
// (kP1..kP8, kNote/kVel, kMsg*, the audio bus), runs the plugin and adds the plugin sound to Csound's output block.
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "vst_job.h"
#include "monitor_capture.h"
#ifdef CAW_VST3
#include "plugin_host.h"
#endif

class VstRack {
public:
    using PtrFn = std::function<void*(const std::string&, int)>;     // channel name, flags -> memory (or nullptr)
    using TableFn = std::function<const void*(int)>;                 // table number -> memory (or nullptr)
    using LogFn = std::function<void(const std::string&)>;
#ifndef CAW_VST3
    bool init(const std::vector<VstJob>& jobs, int, int, int, int, PtrFn, TableFn, LogFn log, bool strict = false) { if (log) log("[caw] VST tracks are ignored: this build has no VST3 support.\n"); return !strict || jobs.empty(); }
    void process(void*, MonitorCapture* = nullptr, float = 1.f) {}
#else
    bool init(const std::vector<VstJob>& jobs, int sr, int block, int nchan, int sampleBytes, PtrFn ptr, TableFn table, LogFn log, bool strict = false) {
        sb_ = sampleBytes; block_ = block; nchan_ = nchan;
        for (const VstJob& j : jobs) {
            if (j.path.empty()) { log("[caw] VST track \"" + j.name + "\": no plugin loaded, skipped.\n"); if (strict) return false; continue; }
            Rt r; r.job = j; r.plug = caw::makeVst3Plugin(); std::string err;
            if (!r.plug->load(j.path, err)) { log("[caw] VST track \"" + j.name + "\": " + err + "\n"); if (strict) return false; continue; }
            for (const auto& parameter : r.plug->params()) {
                const auto saved = j.parameterState.find(parameter.id);
                if (!parameter.readOnly && saved != j.parameterState.end())
                    r.plug->setParam(parameter.index, saved->second);
            }
            r.plug->prepare(sr, block);
            r.ep = std::make_shared<caw::Endpoint>();
            const std::string base = "cawvst_" + std::to_string(j.id) + "_";
            for (size_t k = 0; k < 8 && k < j.pick.size(); ++k) r.p[k] = ptr(base + "p" + std::to_string(k + 1), 1 | 32);
            r.wr = ptr(base + "wr", 1 | 32);
            void* tabNo = ptr(base + "tab", 1 | 32);
            if (tabNo) r.tab = table((int)std::lround(rd(tabNo)));
            if (strict && (!r.wr || !r.tab)) { log("[caw] VST MIDI bridge unavailable.\n"); return false; }
            if (!j.instrument) {
                r.busL = ptr("cawbus_" + j.name + "_L", 2 | 16 | 32); r.busR = ptr("cawbus_" + j.name + "_R", 2 | 16 | 32);
                if (strict && (!r.busL || !r.busR)) { log("[caw] VST effect input bus unavailable.\n"); return false; }
            }
            if (!j.out.empty()) {
                r.outL = ptr("cawbus_" + j.out + "_L", 2 | 16 | 32); r.outR = ptr("cawbus_" + j.out + "_R", 2 | 16 | 32);
                if (!r.outL || !r.outR) {
                    if (strict) { log("[caw] VST track \"" + j.name + "\": output bus \"" + j.out + "\" unavailable.\n"); return false; }
                    r.outL = r.outR = nullptr; log("[caw] VST track \"" + j.name + "\": audio out bus \"" + j.out + "\" not found, using the master.\n");
                }
            }
            r.in.assign((size_t)block * 2, 0.f); r.out.assign((size_t)block * 2, 0.f);
            r.last.assign(8, -1.0);
            log("[caw] VST track \"" + j.name + "\" loaded (" + (j.instrument ? "instrument" : "effect") + ").\n");
            rts_.push_back(std::move(r));
        }
        // instruments first, then effects (an instrument can feed an effect in the same block; the other way round costs one block)
        std::stable_sort(rts_.begin(), rts_.end(), [](const Rt& a, const Rt& b) { return a.job.instrument && !b.job.instrument; });
        // buses nobody on the rack reads (Csound reads them with cawra): emptied at the start of every block, the senders add to it
        for (const Rt& r : rts_) {
            if (!r.outL) continue;
            bool readByEffect = false;
            for (const Rt& o : rts_) if (!o.job.instrument && o.busL == r.outL) readByEffect = true;
            if (!readByEffect) { bool known = false; for (void* p : ret_) if (p == r.outL) known = true; if (!known) { ret_.push_back(r.outL); ret_.push_back(r.outR); } }
        }
        return true;
    }
    void process(void* spout, MonitorCapture* capture = nullptr, float fullScale = 1.f) {
        if (!std::isfinite(fullScale) || fullScale <= 0.f) fullScale = 1.f;
        for (void* p : ret_) clearBus(p);
        for (Rt& r : rts_) {
            const size_t np = r.plug->params().size();
            // the 8 picked parameters: only a changed value is sent
            for (int k = 0; k < 8 && k < (int)r.job.pick.size(); ++k) {
                if (!r.p[k] || r.job.pick[k] < 0 || (size_t)r.job.pick[k] >= np) continue;
                const double v = std::min(1.0, std::max(0.0, rd(r.p[k])));
                if (std::fabs(v - r.last[k]) > 1e-6) { r.last[k] = v; caw::ParamMsg m; m.paramIndex = r.job.pick[k]; m.value = v; r.ep->paramIn.push(m); }
            }
            // MIDI written by the code (note, cc, msg) and by MIDI clips: a ring of 1024 messages in a Csound table
            if (r.wr && r.tab) {
                long w = std::lround(rd(r.wr));
                if (w - r.lastW > 1024) r.lastW = w - 1024;
                for (long n = r.lastW + 1; n <= w; ++n) {
                    const int idx = (int)(n % 1024) * 4;
                    caw::MidiMsg m;
                    const int st = (int)std::lround(rdAt(r.tab, idx));
                    if (st < 0x80 || st > 0xFF) continue;
                    m.status = (uint8_t)st; m.d1 = (uint8_t)std::min(127L, std::max(0L, std::lround(rdAt(r.tab, idx + 1))));
                    m.d2 = (uint8_t)std::min(127L, std::max(0L, std::lround(rdAt(r.tab, idx + 2))));
                    r.ep->midiIn.push(m);
                }
                if (w > r.lastW) r.lastW = w;
            }
            const float* in = nullptr;
            if (!r.job.instrument && r.busL && r.busR) {
                for (int i = 0; i < block_; ++i) { r.in[(size_t)i * 2] = (float)rdAt(r.busL, i) / fullScale; r.in[(size_t)i * 2 + 1] = (float)rdAt(r.busR, i) / fullScale; }
                clearBus(r.busL); clearBus(r.busR);          // the senders add (chnmix) again in the next block
                in = r.in.data();
            }
            r.plug->process(in, r.out.data(), block_, 2, *r.ep);
            if (capture && r.job.meterFirst >= 0) {
                float peakL = 0.f, peakR = 0.f;
                for (int i = 0; i < block_; ++i) {
                    const float l = r.out[(size_t)i * 2], rr = r.out[(size_t)i * 2 + 1];
                    if (std::isfinite(l)) peakL = std::max(peakL, std::fabs(l));
                    if (std::isfinite(rr)) peakR = std::max(peakR, std::fabs(rr));
                }
                capture->addMeter(r.job.meterFirst, peakL);
                capture->addMeter(r.job.meterFirst + 1, peakR);
            }
            if (r.outL) for (int i = 0; i < block_; ++i) { addAt(r.outL, (size_t)i, r.out[(size_t)i * 2] * fullScale); addAt(r.outR, (size_t)i, r.out[(size_t)i * 2 + 1] * fullScale); }
            if (!r.outL || r.job.outMaster)
                for (int i = 0; i < block_; ++i) for (int c = 0; c < nchan_ && c < 2; ++c) add(spout, (size_t)i * nchan_ + c, r.out[(size_t)i * 2 + c] * fullScale);
        }
    }
#endif
private:
#ifdef CAW_VST3
    struct Rt {
        VstJob job; std::unique_ptr<caw::IPlugin> plug; std::shared_ptr<caw::Endpoint> ep;
        void* p[8] = {}; void *wr = nullptr, *busL = nullptr, *busR = nullptr, *outL = nullptr, *outR = nullptr; const void* tab = nullptr; long lastW = 0;
        std::vector<double> last; std::vector<float> in, out;
    };
    double rd(const void* p) const { return sb_ == 8 ? *static_cast<const double*>(p) : (double)*static_cast<const float*>(p); }
    double rdAt(const void* p, int i) const { return sb_ == 8 ? static_cast<const double*>(p)[i] : (double)static_cast<const float*>(p)[i]; }
    void clearBus(void* p) const { if (sb_ == 8) std::fill_n(static_cast<double*>(p), block_, 0.0); else std::fill_n(static_cast<float*>(p), block_, 0.f); }
    void add(void* b, size_t i, float v) const { if (sb_ == 8) static_cast<double*>(b)[i] += v; else static_cast<float*>(b)[i] += v; }
    void addAt(void* b, size_t i, float v) const { add(b, i, v); }
    std::vector<void*> ret_;
    std::vector<Rt> rts_; int sb_ = 8, block_ = 0, nchan_ = 2;
#endif
};
