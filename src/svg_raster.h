#pragma once
#include <vector>

// Renders the built-in vector logo (logo_svg.h, from res/logo.svg) at exactly px x px pixels.
// Output: straight-alpha RGBA, px*px*4 bytes. Returns false if the SVG cannot be parsed.
bool rasterizeLogo(int px, std::vector<unsigned char>& rgba);
