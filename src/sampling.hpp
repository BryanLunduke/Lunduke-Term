// SPDX-License-Identifier: GPL-3.0-or-later
// Bilinear sampling helpers that reproduce GL_LINEAR with CLAMP_TO_EDGE / REPEAT.
#pragma once

#include <cmath>

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
