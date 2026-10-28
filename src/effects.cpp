// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.hpp"
#include "sampling.hpp"

#include <algorithm>
#include <cmath>

namespace lterm {

static inline float lint(float a, float b, float t) { return (1.0f - t) * a + t * b; }

DerivedParams derive(const EffectParams& p) {
    DerivedParams d{};
    d.curvature = p.screenCurvature * 0.4f;
    d.bloomStrength = p.bloom * 2.5f;
    d.screenBrightness = lint(0.5f, 1.5f, p.brightness);
    d.ambient = p.ambientLight * 0.2f;
    d.glowingLine = p.glowingLine * 0.2f;
    d.hsyncStrength = lint(0.05f, 0.35f, p.horizontalSync);
    d.jitterX = 0.007f * p.jitter;
    d.jitterY = 0.002f * p.jitter;
    d.burnInTime = p.burnIn > 0 ? 1.0f / lint(160.0f, 1600.0f, p.burnIn) : 0.0f;
    return d;
}

const std::vector<float>& noiseTexture() {
    static const std::vector<float> tex = [] {
        std::vector<float> t(static_cast<size_t>(kNoiseSize) * kNoiseSize * 4);
        uint32_t s = 0x4c544d30u;  // "LTM0"
        for (auto& v : t) {
            // xorshift32, quantised to 8 bits like a PNG texture would be.
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            v = static_cast<float>(s >> 24) / 255.0f;
        }
        return t;
    }();
    return tex;
}

static inline float fractf(float x) { return x - ffloor(x); }

FrameUniforms frameUniforms(const EffectParams& p, float timeMs, float burnInLastUpdateMs) {
    const DerivedParams d = derive(p);
    FrameUniforms u{};
    u.timeMs = timeMs;
    float n[4];
    sampleRepeat(noiseTexture().data(), kNoiseSize, kNoiseSize, fractf(timeMs / 2048.0f),
                 fractf(timeMs / 1048576.0f), n);
    u.flickerBrightness = 1.0f + (n[1] - 0.5f) * p.flickering;
    if (p.horizontalSync > 0) {
        float randval = d.hsyncStrength - n[0];
        u.hsyncScale = (randval >= 0.0f ? 1.0f : 0.0f) * randval * d.hsyncStrength;
        u.hsyncFreq = lint(4.0f, 40.0f, n[1]);
    }
    u.noiseOffsetX = fractf(timeMs / 51.0f);
    u.noiseOffsetY = fractf(timeMs / 237.0f);
    u.burnInDecay = std::clamp((timeMs - burnInLastUpdateMs) * d.burnInTime, 0.0f, 1.0f);
    return u;
}

std::vector<float> bloomWeights(int radius) {
    std::vector<float> w(static_cast<size_t>(radius) + 1);
    const float sigma = std::max(1.0f, radius / 2.0f);
    float sum = 0;
    for (int i = 0; i <= radius; ++i) {
        w[i] = std::exp(-0.5f * (i * i) / (sigma * sigma));
        sum += (i == 0 ? 1.0f : 2.0f) * w[i];
    }
    for (auto& v : w) v /= sum;
    return w;
}

}  // namespace lterm
