// SPDX-License-Identifier: GPL-3.0-or-later
// Bilinear sampling helpers that reproduce GL_LINEAR with CLAMP_TO_EDGE / REPEAT.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace lterm {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// floor() without a libm call (x86-64 baseline has no SSE4.1 roundss). Exact for |x| < 2^31.
inline int ifloor(float x) {
    int i = static_cast<int>(x);
    return i - (x < static_cast<float>(i) ? 1 : 0);
}
inline float ffloor(float x) { return static_cast<float>(ifloor(x)); }

// tex: RGBA float. Writes the first N channels (N <= 4).
template <int N = 4>
inline void sampleClamp(const float* tex, int w, int h, float u, float v, float out[4]) {
    float x = u * w - 0.5f, y = v * h - 0.5f;
    int x0 = ifloor(x), y0 = ifloor(y);
    float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    int x1 = clampi(x0 + 1, 0, w - 1), y1 = clampi(y0 + 1, 0, h - 1);
    x0 = clampi(x0, 0, w - 1);
    y0 = clampi(y0, 0, h - 1);
    const float* a = tex + (static_cast<size_t>(y0) * w + x0) * 4;
    const float* b = tex + (static_cast<size_t>(y0) * w + x1) * 4;
    const float* c = tex + (static_cast<size_t>(y1) * w + x0) * 4;
    const float* d = tex + (static_cast<size_t>(y1) * w + x1) * 4;
    for (int i = 0; i < N; ++i) {
        float top = a[i] + (b[i] - a[i]) * fx;
        float bot = c[i] + (d[i] - c[i]) * fx;
        out[i] = top + (bot - top) * fy;
    }
}

// tex: RGBA bytes, each tap read as k * (1/255), the value the float version would hold. N <= 3
// here: the byte alpha is not a float alpha.
inline float byteToFloat(uint8_t k) { return k * (1.0f / 255.0f); }
template <int N = 3>
inline void sampleClamp(const uint8_t* tex, int w, int h, float u, float v, float out[4]) {
    static_assert(N <= 3, "byte textures carry no float alpha");
    float x = u * w - 0.5f, y = v * h - 0.5f;
    int x0 = ifloor(x), y0 = ifloor(y);
    float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    int x1 = clampi(x0 + 1, 0, w - 1), y1 = clampi(y0 + 1, 0, h - 1);
    x0 = clampi(x0, 0, w - 1);
    y0 = clampi(y0, 0, h - 1);
    const uint8_t* a = tex + (static_cast<size_t>(y0) * w + x0) * 4;
    const uint8_t* b = tex + (static_cast<size_t>(y0) * w + x1) * 4;
    const uint8_t* c = tex + (static_cast<size_t>(y1) * w + x0) * 4;
    const uint8_t* d = tex + (static_cast<size_t>(y1) * w + x1) * 4;
    for (int i = 0; i < N; ++i) {
        const float fa = byteToFloat(a[i]), fb = byteToFloat(b[i]), fc = byteToFloat(c[i]), fd = byteToFloat(d[i]);
        float top = fa + (fb - fa) * fx;
        float bot = fc + (fd - fc) * fx;
        out[i] = top + (bot - top) * fy;
    }
}

inline int wrapi(int v, int n) {
    int r = v % n;
    return r < 0 ? r + n : r;
}

// REPEAT wrap for power-of-two textures (the noise texture).
inline void sampleRepeat(const float* tex, int w, int h, float u, float v, float out[4]) {
    float x = u * w - 0.5f, y = v * h - 0.5f;
    int x0 = ifloor(x), y0 = ifloor(y);
    float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    int x1 = (x0 + 1) & (w - 1), y1 = (y0 + 1) & (h - 1);
    x0 &= (w - 1);
    y0 &= (h - 1);
    const float* a = tex + (static_cast<size_t>(y0) * w + x0) * 4;
    const float* b = tex + (static_cast<size_t>(y0) * w + x1) * 4;
    const float* c = tex + (static_cast<size_t>(y1) * w + x0) * 4;
    const float* d = tex + (static_cast<size_t>(y1) * w + x1) * 4;
    for (int i = 0; i < 4; ++i) {
        float top = a[i] + (b[i] - a[i]) * fx;
        float bot = c[i] + (d[i] - c[i]) * fx;
        out[i] = top + (bot - top) * fy;
    }
}

}  // namespace lterm
