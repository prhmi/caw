#pragma once
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
