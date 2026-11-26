#pragma once
// The track / master volume boxes have no practical upper limit (whole dB). 200 dB keeps the linear gain finite
// in a float (10^10); everything that used to clamp the fader at 2.0 (+6 dB) uses kMaxVolumeGain now.
constexpr int kMaxVolumeDb = 200;
constexpr float kMaxVolumeGain = 1e10f;
