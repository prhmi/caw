#pragma once
// Tiny GPU-texture helper for the sample editor's per-pixel spectrogram: an RGBA buffer is
// uploaded ONCE per view change and handed back as an ImTextureID the draw list can draw as ONE
// quad - instead of re-emitting hundreds of thousands of tiny quads every frame (which is what
// capped the old spectrogram's quality). Windows uses the D3D11 device the app already created
// (main.cpp hands it over with setD3DDevice). Other platforms have no texture backend here yet:
// createRgba returns nullptr and the editor keeps its draw-list rendering, so this header is
// safe to include everywhere.

#include "imgui.h"
#include <cstdint>

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace uitex {

#if defined(_WIN32)

inline ID3D11Device* g_uitex_dev = nullptr;   // the app's swap-chain device (main.cpp)

inline void setD3DDevice(void* dev) { g_uitex_dev = (ID3D11Device*)dev; }

// Uploads w*h RGBA pixels (straight alpha, row-major). Returns nullptr when the device is gone
// or the upload fails; the caller then falls back to draw-list rendering.
inline ImTextureID createRgba(int w, int h, const uint32_t* rgba) {
    if (!g_uitex_dev || w <= 0 || h <= 0 || !rgba) return nullptr;
    D3D11_TEXTURE2D_DESC td;
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_IMMUTABLE;          // uploaded once, drawn many frames
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0;
    D3D11_SUBRESOURCE_DATA sd;
    sd.pSysMem = rgba; sd.SysMemPitch = (UINT)w * 4u; sd.SysMemSlicePitch = 0;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_uitex_dev->CreateTexture2D(&td, &sd, &tex)) || !tex) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_uitex_dev->CreateShaderResourceView(tex, nullptr, &srv)) || !srv) { tex->Release(); return nullptr; }
    tex->Release();
    return (ImTextureID)(void*)srv;
}

inline void destroy(ImTextureID tex) { if (tex) ((ID3D11ShaderResourceView*)tex)->Release(); }

#elif defined(CAW_GL_TEXTURES)

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

inline void setD3DDevice(void*) {}

inline ImTextureID createRgba(int w, int h, const uint32_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba) return nullptr;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return nullptr;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return (ImTextureID)(void*)(intptr_t)tex;
}

inline void destroy(ImTextureID tex) {
    if (!tex) return;
    const GLuint t = (GLuint)(intptr_t)tex;
    glDeleteTextures(1, &t);
}

#else

inline void setD3DDevice(void*) {}
inline ImTextureID createRgba(int, int, const uint32_t*) { return nullptr; }
inline void destroy(ImTextureID) {}

#endif

}   // namespace uitex
