#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Compact waveform and code-bracket app mark. Kept as geometry so GLFW,
// Windows/macOS icons and the startup screen can use the same visual.
namespace caw_brand {
struct Point { float x, y; };
inline const Point* points(int& count) {
    static const Point p[] = {
        {.25f,.35f}, {.13f,.50f}, {.25f,.65f},
        {.34f,.50f}, {.41f,.63f}, {.50f,.27f}, {.59f,.73f},
        {.66f,.50f}, {.75f,.65f}, {.87f,.50f}, {.75f,.35f}
    };
    count = (int)(sizeof(p) / sizeof(p[0]));
    return p;
}
inline std::vector<unsigned char> icon(int size) {
    std::vector<unsigned char> pixels((size_t)size * size * 4);
    int count = 0; const Point* path = points(count);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float px = (x + .5f) / size, py = (y + .5f) / size;
        const float cx = std::max(.10f, std::min(.90f, px)), cy = std::max(.10f, std::min(.90f, py));
        const float corner = std::sqrt((px-cx)*(px-cx)+(py-cy)*(py-cy));
        const bool inside = corner <= .10f;
        float distance = 2.f;
        for (int i = 1; i < count; ++i) {
            const float ax = path[i-1].x, ay = path[i-1].y, bx = path[i].x, by = path[i].y;
            const float vx = bx-ax, vy = by-ay;
            const float t = std::max(0.f, std::min(1.f, ((px-ax)*vx+(py-ay)*vy)/(vx*vx+vy*vy)));
            const float dx = px-(ax+t*vx), dy = py-(ay+t*vy);
            distance = std::min(distance, std::sqrt(dx*dx+dy*dy));
        }
        const bool ink = distance < .035f;
        const size_t n = ((size_t)y * size + x) * 4;
        pixels[n] = ink ? 247 : 25;
        pixels[n+1] = ink ? 151 : 28;
        pixels[n+2] = ink ? 51 : 34;
        pixels[n+3] = inside ? 255 : 0;
    }
    return pixels;
}
}
