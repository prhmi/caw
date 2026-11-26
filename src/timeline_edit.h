#pragma once

// timeline_edit.h - timeline / clip editing helpers in one header.
// Merged (phase-1 cleanup) from: clip_automation_move, clip_timing, timeline_loop,
// timeline_zoom, midi_clip_ops, audio_channels.

// ---- (was clip_automation_move.h) --------------------------------------------------------------
#include "csd_builder.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

// A drag is reconstructed from its starting snapshot on every frame, so moving
// back and forth or crossing tracks never moves a point twice.
struct ClipAutomationMove {
    // clipId > 0: the dot belongs to a moved clip. clipId == -1: a selected dot that is not owned by a moved
    // clip - it follows the drag in time and stays on its own track. picked = it was selected (kept selected).
    struct TaggedPoint { AutoPoint point; int clipId = 0; bool picked = false; };
    std::map<int, std::map<std::string, std::vector<TaggedPoint>>> lanes;
    std::map<int, std::map<std::string, CawParam>> ranges;

    using DotSelection = std::map<std::pair<int, std::string>, std::set<int>>;
    void capture(const Project& project, const std::set<int>& selected, const DotSelection* dots = nullptr) {
        lanes.clear();
        ranges.clear();
        for (const Track& track : project.tracks) {
            ranges[track.id] = track.automationRanges;
            std::map<int, std::set<std::string>> channels;
            for (const Clip& clip : track.clips) if (selected.count(clip.id))
                for (const CawParam& p : parseClipForTrack(clip, track).params) channels[clip.id].insert(p.chn);
            auto& snapshot = lanes[track.id];
            for (const auto& lane : track.automation) {
                auto& saved = snapshot[lane.first];
                const std::set<int>* laneDots = nullptr;
                if (dots) { const auto d = dots->find({track.id, lane.first}); if (d != dots->end()) laneDots = &d->second; }
                for (int pointIndex = 0; pointIndex < (int)lane.second.size(); ++pointIndex) {
                    const AutoPoint& point = lane.second[(size_t)pointIndex];
                    const bool picked = laneDots && laneDots->count(pointIndex) > 0;
                    int owner = 0;
                    for (const Clip& clip : track.clips) {
                        if (selected.count(clip.id) && channels[clip.id].count(lane.first) &&
                            point.t >= clip.start - 1e-9 && point.t <= clip.start + clip.dur + 1e-9) {
                            owner = clip.id;
                            break;
                        }
                    }
                    if (!owner && picked) owner = -1;
                    saved.push_back({point, owner, picked});
                }
            }
        }
    }

    // selectedOut (optional): the new indices of the picked dots after the move (the lanes are re-sorted).
    void apply(Project& project, double dt, const std::map<int, double>* individualShifts = nullptr,
               DotSelection* selectedOut = nullptr) const {
        std::map<int, Track*> tracks;
        std::map<int, int> clipTracks;
        for (Track& track : project.tracks) {
            tracks[track.id] = &track;
            for (const Clip& clip : track.clips) clipTracks[clip.id] = track.id;
            track.automation.clear();
        }
        std::map<std::pair<int, std::string>, std::vector<std::pair<AutoPoint, bool>>> rebuilt;
        for (const auto& original : lanes) {
            if (!tracks.count(original.first)) continue;
            for (const auto& lane : original.second) {
                tracks[original.first]->automation[lane.first]; // retain empty lanes
                for (const TaggedPoint& tagged : lane.second) {
                    AutoPoint point = tagged.point;
                    int target = original.first;
                    if (tagged.clipId == -1) {   // a selected free dot: same track, shifted like the drag
                        point.t = std::max(0.0, point.t + (individualShifts ? 0.0 : dt));
                    } else if (tagged.clipId && clipTracks.count(tagged.clipId)) {
                        double shift = dt;
                        if (individualShifts) {
                            const auto it = individualShifts->find(tagged.clipId);
                            shift = it == individualShifts->end() ? 0.0 : it->second;
                        }
                        point.t = std::max(0.0, point.t + shift);
                        target = clipTracks[tagged.clipId];
                        const auto sourceRanges = ranges.find(original.first);
                        if (target != original.first && sourceRanges != ranges.end() && !tracks[target]->automationRanges.count(lane.first)) {
                            const auto range = sourceRanges->second.find(lane.first);
                            if (range != sourceRanges->second.end()) tracks[target]->automationRanges[lane.first] = range->second;
                        }
                    }
                    rebuilt[{target, lane.first}].push_back({point, tagged.picked});
                }
            }
        }
        if (selectedOut) selectedOut->clear();
        for (auto& [key, pts] : rebuilt) {
            std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first.t < b.first.t; });
            auto& dest = tracks[key.first]->automation[key.second];
            for (int k = 0; k < (int)pts.size(); ++k) {
                dest.push_back(pts[(size_t)k].first);
                if (selectedOut && pts[(size_t)k].second) (*selectedOut)[key].insert(k);
            }
        }
    }
};

// ---- (was clip_timing.h) --------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace clip_timing {
inline double nonnegative(double v, double fallback) {
    return std::isfinite(v) ? std::max(0.0, v) : fallback;
}
inline double duration(double v, double fallback, double minimum) {
    return std::isfinite(v) ? std::max(minimum, v) : fallback;
}
inline void moveCodeClip(Project& project, Clip& clip, double newStart) {
    newStart = nonnegative(newStart, clip.start);
    const double delta = newStart - clip.start;
    if (delta == 0.0) return;
    ClipAutomationMove move; move.capture(project, std::set<int>{clip.id});
    clip.start = newStart;
    move.apply(project, delta);
}
// Q: align all selected clip starts to the earliest one, preserving durations
// and relative MIDI note timing. Code/VST automation follows its owner.
inline bool sync(Project& project, const std::set<int>& clipIds, const std::set<int>& midiIds) {
    double first = std::numeric_limits<double>::infinity();
    size_t count = 0;
    for (const Track& track : project.tracks) {
        for (const Clip& clip : track.clips) if (clipIds.count(clip.id)) { first = std::min(first, clip.start); ++count; }
        for (const MidiClip& clip : track.midiClips) if (midiIds.count(clip.id)) { first = std::min(first, clip.start); ++count; }
    }
    if (count < 2 || !std::isfinite(first)) return false;
    ClipAutomationMove move;
    if (!clipIds.empty()) move.capture(project, clipIds);
    std::map<int, double> shifts;
    for (Track& track : project.tracks) {
        for (Clip& clip : track.clips) if (clipIds.count(clip.id)) {
            shifts[clip.id] = first - clip.start;
            clip.start = first;
        }
        for (MidiClip& clip : track.midiClips) if (midiIds.count(clip.id)) clip.start = first;
    }
    if (!clipIds.empty()) move.apply(project, 0.0, &shifts);
    return true;
}
}

// ---- (was timeline_loop.h) --------------------------------------------------------------
#include "csd_builder.h"
#include <algorithm>
#include <cmath>

namespace timeline_loop {
inline bool valid(const Project& p) {
    return p.loopEnabled && std::isfinite(p.loopStart) && std::isfinite(p.loopEnd) &&
           p.loopStart >= 0.0 && p.loopEnd - p.loopStart >= 0.01;
}
inline double wrap(const Project& p, double time) {
    if (!valid(p)) return time;
    if (time < p.loopStart) return p.loopStart;
    if (time < p.loopEnd) return time;
    return p.loopStart + std::fmod(time - p.loopStart, p.loopEnd - p.loopStart);
}
// Event times are relative to the playhead. Trim all starts/ends at the loop
// boundary; linear automation's endpoint must be interpolated when trimmed.
inline bool trim(TimelineEvent& e, double remaining) {
    if (e.start >= remaining || e.dur <= 0.0) return false;
    const double duration = std::min(e.dur, remaining - e.start);
    if (duration <= 0.0) return false;
    if (e.automation && duration < e.dur) e.b = e.a + (e.b - e.a) * (duration / e.dur);
    e.dur = duration;
    return true;
}
}

// ---- (was timeline_zoom.h) --------------------------------------------------------------
#include <algorithm>
#include <cmath>

// Centre the playhead when possible, but never expose negative timeline time.
inline float zoomAroundPlayhead(float newZoom, double playhead, float viewportWidth) {
    if (!(newZoom > 0.f) || !std::isfinite(newZoom) || !std::isfinite(playhead) || !(viewportWidth > 0.f) || !std::isfinite(viewportWidth)) return 0.f;
    return (float)std::max(0.0, playhead - (double)viewportWidth / (2.0 * newZoom));
}

// ---- (was midi_clip_ops.h) --------------------------------------------------------------
#include "model.h"
#include <algorithm>

namespace midi_clip_ops {
// Keep an existing region, or create one at its source clip's time and size.
inline bool ensureForClip(Project& project, size_t laneIndex, int clipId) {
    if (laneIndex >= project.tracks.size()) return false;
    Track& lane = project.tracks[laneIndex];
    const Clip* source = nullptr;
    for (const Clip& clip : lane.clips) if (clip.id == clipId) { source = &clip; break; }
    if (!source) return false;
    lane.midiHidden = false;
    for (const MidiClip& mc : lane.midiClips) if (mc.clipId == clipId) return true;
    int largest = 0;
    for (const Track& track : project.tracks)
        for (const MidiClip& mc : track.midiClips) largest = std::max(largest, mc.id);
    MidiClip mc;
    mc.id = largest + 1;
    mc.clipId = source->id;
    mc.name = source->name;
    mc.start = source->start;
    mc.dur = std::max(.01, source->dur);
    lane.midiClips.push_back(mc);
    return true;
}
}

// ---- (was audio_channels.h) --------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <vector>

// Export channel mapping: downmix mono by averaging, stereo by grouping even
// and odd inputs; mono expands to the front pair. Other layouts preserve
// channel order, discard excess channels and zero-fill missing ones.
template<class T>
inline void mapExportChannels(const T* input, size_t frames, int source, int target, std::vector<T>& output) {
    output.assign(frames * (size_t)target, T(0));
    if (!input || source < 1 || target < 1) return;
    for (size_t f = 0; f < frames; ++f) {
        const T* in = input + f * (size_t)source;
        T* out = output.data() + f * (size_t)target;
        if (target == 1) {
            double sum = 0.;
            for (int i = 0; i < source; ++i) sum += in[i];
            out[0] = (T)(sum / source);
        } else if (source == 1) {
            out[0] = out[1] = in[0];
        } else if (target == 2 && source > 2) {
            // The virtual soundcard fold: channel k plays on export channel k mod 2 - SUM the
            // pairs (ch1+ch3 -> left, ch2+ch4 -> right), exactly like the live master does.
            double left = 0, right = 0;
            for (int i = 0; i < source; ++i) {
                if (i % 2 == 0) left += in[i];
                else right += in[i];
            }
            out[0] = (T)(left); out[1] = (T)(right);
        } else {
            for (int i = 0; i < std::min(source, target); ++i) out[i] = in[i];
        }
    }
}

