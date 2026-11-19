#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// An isolated, audio-frame scheduling building block. This does not open
// libpd or a device: a host supplies one independently prepared PD instance
// per event through Sink. No UI thread or wall clock participates.
namespace pd_frame_scheduler {
struct Event {
    int id = 0;
    std::int64_t start = 0;  // inclusive audio frame
    std::int64_t end = 0;    // exclusive audio frame
    bool muted = false;      // loaded, but not started / ticked (live M / S)
};
struct Control { std::int64_t frame=0; int id=0; int kind=0; int a=0; int b=0; };

struct Sink {
    virtual ~Sink() = default;
    // A new or sought-into event. At present a seek restarts patch state.
    virtual bool start(int id) = 0;
    // Render exactly 64 interleaved frames into out. One isolated patch
    // instance is required per simultaneously active event.
    virtual bool tick(int id, float* out, int channels) = 0;
    virtual void stop(int id) = 0;
    virtual bool control(const Control&) { return true; }
    virtual void setFrame(int, std::int64_t) {}
};

class Scheduler {
    struct Voice {
        Event event;
        std::vector<float> tick;
        int cursor = 64;
        bool started = false;
    };
    std::vector<Voice> voices_;
    std::vector<Control> controls_;
    std::vector<Event> plannedEvents_;
    std::vector<Control> plannedControls_;
    std::size_t controlCursor_ = 0;
    std::int64_t frame_ = 0;
    int channels_ = 0;
public:
    // Call only while stopped. All buffers are allocated here, not in render.
    bool prepare(const std::vector<Event>& events, int channels, std::string& error, std::vector<Control> controls={}) {
        if (channels < 1 || channels > 8) { error = "Unsupported channel count."; return false; }
        std::vector<Voice> next;
        next.reserve(events.size());
        for (const auto& e : events) {
            if (e.id <= 0 || e.start < 0 || e.end <= e.start) {
                error = "Invalid clip event."; return false;
            }
            for (const auto& v : next) if (v.event.id == e.id) {
                error = "Clip event IDs must be unique."; return false;
            }
            Voice v; v.event = e; v.tick.resize(static_cast<std::size_t>(64 * channels)); next.push_back(std::move(v));
        }
        for(const auto& c:controls){
            bool found=false;
            for(const auto& v:next)if(v.event.id==c.id && c.frame>=v.event.start && c.frame<v.event.end){found=true;break;}
            if(!found){error="Control falls outside its clip.";return false;}
        }
        std::stable_sort(controls.begin(),controls.end(),[](const auto& a,const auto& b){return a.frame<b.frame;});
        plannedEvents_=events;plannedControls_=controls;
        voices_ = std::move(next);controls_=std::move(controls);controlCursor_=0;
        channels_ = channels; frame_ = 0; error.clear(); return true;
    }
    std::int64_t frame() const { return frame_; }
    // Replace future timing without stopping already sounding voices. A moved
    // active clip keeps its old end and controls, like Csound's live replan.
    // A new start overlapping that same active instance is deferred until
    // the next seek/loop: one isolated PD instance cannot overlap itself.
    bool replan(const std::vector<Event>& events,std::vector<Control> controls,
                bool keepActive,std::string& error) {
        Scheduler validation;
        if(!validation.prepare(events,channels_,error,controls))return false;
        std::vector<Voice> next;
        std::vector<Control> nextControls;
        next.reserve(events.size()*2);
        for(const auto& e:events) {
            // The voice already loaded for this clip (started or waiting); keep exactly one so a
            // muted clip does not accumulate voices on every refresh.
            const Voice* current=nullptr;
            if(keepActive)for(const auto& old:voices_)
                if(old.event.id==e.id){current=&old;break;}
            const bool started=current && current->started;
            const bool moved=started && (current->event.start!=e.start || current->event.end!=e.end);
            if(current) {
                next.push_back(*current);
                // Live M / S: the instance stays loaded; a muted voice is not ticked, an unmuted
                // one that covers the playhead starts right away (cursor reset, render starts it).
                next.back().event=e;
                if(!started)next.back().cursor=64;
            }
            // A voice is created only for a clip that has none yet. Moving / trimming an already
            // loaded clip updates its window in place: no second voice, no restart of the instance.
            const bool scheduleNew=!current;
            if(scheduleNew) {
                Voice v;v.event=e;v.tick.resize(static_cast<std::size_t>(64*channels_));
                next.push_back(std::move(v));
            }
            if(moved) {
                for(const auto& c:controls_)
                    if(c.id==e.id && c.frame>=frame_ && c.frame<current->event.end)nextControls.push_back(c);
            }
            if(!moved || scheduleNew)
                for(const auto& c:controls)
                    if(c.id==e.id && c.frame>=frame_ && (started || scheduleNew))nextControls.push_back(c);
        }
        std::stable_sort(nextControls.begin(),nextControls.end(),
            [](const auto& a,const auto& b){return a.frame<b.frame;});
        plannedEvents_=events;plannedControls_=std::move(validation.plannedControls_);
        voices_=std::move(next);controls_=std::move(nextControls);controlCursor_=0;
        error.clear();return true;
    }
    void seek(std::int64_t frame, Sink& sink) {
        for (auto& v : voices_) {
            if (v.started) sink.stop(v.event.id);
            v.started = false; v.cursor = 64;
        }
        voices_.clear();
        voices_.reserve(plannedEvents_.size());
        for(const auto& e:plannedEvents_) {
            Voice v;v.event=e;v.tick.resize(static_cast<std::size_t>(64*channels_));
            voices_.push_back(std::move(v));
        }
        controls_=plannedControls_;
        frame_ = std::max<std::int64_t>(0, frame);
        controlCursor_=static_cast<std::size_t>(std::lower_bound(controls_.begin(),controls_.end(),frame_,
            [](const Control& c,std::int64_t f){return c.frame<f;})-controls_.begin());
    }
    bool mutedClip(int id) const {
        for (const auto& v : voices_) if (v.event.id == id) return v.event.muted;
        return false;
    }
    // Render contiguous frames. Caller supplies a correctly sized buffer.
    // Clip boundaries are gated on exact sample frames, even within a PD
    // 64-frame DSP tick. Host/device and queue latency remain separate.
    bool render(float* out, std::size_t frames, Sink& sink) {
        if (!out || channels_ <= 0 || frames > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - frame_))
            return false;
        std::fill(out, out + frames * static_cast<std::size_t>(channels_), 0.f);
        // Muting is live: a clip that was just muted releases its instance (caw-gate 0)
        // and is not ticked again until it is unmuted.
        for (auto& v : voices_)
            if (v.event.muted && v.started) { sink.stop(v.event.id); v.started = false; v.cursor = 64; }
        for (std::size_t n = 0; n < frames; ++n) {
            const std::int64_t at = frame_ + static_cast<std::int64_t>(n);
            for (auto& v : voices_) {
                if (v.event.muted) continue;
                if (v.started && at >= v.event.end) { sink.stop(v.event.id); v.started = false; v.cursor = 64; }
                if (at < v.event.start || at >= v.event.end) continue;
                if (!v.started) {
                    if (!sink.start(v.event.id)) { frame_ = at; return false; }
                    v.started = true; v.cursor = 64;
                }
            }
            while(controlCursor_<controls_.size() && controls_[controlCursor_].frame<=at) {
                const Control ctrl=controls_[controlCursor_];
                ++controlCursor_;
                if(mutedClip(ctrl.id))continue;   // a muted clip receives no runs / notes
                if(!sink.control(ctrl)){frame_=at;return false;}
            }
            for (auto& v : voices_) {
                if (v.event.muted) continue;
                if (at < v.event.start || at >= v.event.end) continue;
                if (v.cursor == 64) {
                    sink.setFrame(v.event.id,at);
                    if (!sink.tick(v.event.id, v.tick.data(), channels_)) { frame_ = at; return false; }
                    v.cursor = 0;
                }
                for (int ch = 0; ch < channels_; ++ch) {
                    const float value = v.tick[static_cast<std::size_t>(v.cursor * channels_ + ch)];
                    out[n * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(ch)] += std::isfinite(value) ? value : 0.f;
                }
                ++v.cursor;
            }
        }
        frame_ += static_cast<std::int64_t>(frames);
        // Close events that end exactly at a buffer edge.
        for (auto& v : voices_) {
            if (v.started && frame_ >= v.event.end) { sink.stop(v.event.id); v.started = false; v.cursor = 64; }
        }
        return true;
    }
};
}
