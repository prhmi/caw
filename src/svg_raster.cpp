#include "svg_raster.h"
#include "logo_svg.h"

#include <cmath>
#include <cstring>
#include <string>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

bool rasterizeLogo(int px, std::vector<unsigned char>& rgba) {
    if (px < 8 || px > 2048) return false;
    std::string text(reinterpret_cast<const char*>(kLogoSvg), kLogoSvgSize);   // nsvgParse edits its input
    NSVGimage* image = nsvgParse(&text[0], "px", 96.f);
    if (!image || image->width <= 0.f || image->height <= 0.f) { if (image) nsvgDelete(image); return false; }
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (!rasterizer) { nsvgDelete(image); return false; }

    // Draw 3x larger, then average 3x3 blocks: much smoother edges than one pass of the rasterizer.
    constexpr int ss = 3;
    const int big = px * ss;
    std::vector<unsigned char> hi(static_cast<size_t>(big) * big * 4, 0);
    const float scale = static_cast<float>(big) / image->width;
    nsvgRasterize(rasterizer, image, 0.f, 0.f, scale, hi.data(), big, big, big * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);

    rgba.assign(static_cast<size_t>(px) * px * 4, 0);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            float r = 0, g = 0, b = 0, a = 0;   // average in premultiplied space, then un-premultiply
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const unsigned char* s = &hi[(static_cast<size_t>(y * ss + sy) * big + (x * ss + sx)) * 4];
                    const float alpha = s[3] / 255.f;
                    r += s[0] * alpha; g += s[1] * alpha; b += s[2] * alpha; a += alpha;
                }
            unsigned char* out = &rgba[(static_cast<size_t>(y) * px + x) * 4];
            if (a > 0.f) {
                out[0] = static_cast<unsigned char>(std::lround(r / a));
                out[1] = static_cast<unsigned char>(std::lround(g / a));
                out[2] = static_cast<unsigned char>(std::lround(b / a));
            }
            out[3] = static_cast<unsigned char>(std::lround(a / (ss * ss) * 255.f));
        }
    return true;
}
