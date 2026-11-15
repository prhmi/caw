#pragma once
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
