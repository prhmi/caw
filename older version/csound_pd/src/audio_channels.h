#pragma once
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
            double left = 0, right = 0;
            int nl = 0, nr = 0;
            for (int i = 0; i < source; ++i) {
                if (i % 2 == 0) { left += in[i]; ++nl; }
                else { right += in[i]; ++nr; }
            }
            out[0] = (T)(left / nl); out[1] = (T)(right / nr);
        } else {
            for (int i = 0; i < std::min(source, target); ++i) out[i] = in[i];
        }
    }
}
