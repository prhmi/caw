#pragma once
#include "clip_automation_move.h"
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
