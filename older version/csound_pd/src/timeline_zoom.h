#pragma once
#include <algorithm>
#include <cmath>

// Centre the playhead when possible, but never expose negative timeline time.
inline float zoomAroundPlayhead(float newZoom, double playhead, float viewportWidth) {
    if (!(newZoom > 0.f) || !std::isfinite(newZoom) || !std::isfinite(playhead) || !(viewportWidth > 0.f) || !std::isfinite(viewportWidth)) return 0.f;
    return (float)std::max(0.0, playhead - (double)viewportWidth / (2.0 * newZoom));
}
