// SPDX-License-Identifier: GPL-3.0-or-later
#include "effects.hpp"
#include "sampling.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

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

float EffectParams::burnInFadeMs() const {
    if (burnIn <= 0) return 0.0f;
    return lint(160.0f, 1600.0f, burnIn);  // decay reaches 1 after 1/burnInTime ms
}

bool parseEffectsQuality(const std::string& s, EffectsQuality& out) {
    if (s == "auto") out = EffectsQuality::Auto;
    else if (s == "full") out = EffectsQuality::Full;
    else if (s == "reduced") out = EffectsQuality::Reduced;
    else if (s == "static") out = EffectsQuality::Static;
    else return false;
    return true;
}

const char* effectsQualityName(EffectsQuality q) {
    switch (q) {
    case EffectsQuality::Auto: return "auto";
    case EffectsQuality::Full: return "full";
    case EffectsQuality::Reduced: return "reduced";
    case EffectsQuality::Static: return "static";
    }
    return "?";
}

bool isSoftwareRendererName(const std::string& glRenderer) {
    std::string r;
    for (char c : glRenderer) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r.find("llvmpipe") != std::string::npos || r.find("softpipe") != std::string::npos ||
           r.find("swrast") != std::string::npos;
}

bool softwareRenderingForced() {
    const char* v = std::getenv("LIBGL_ALWAYS_SOFTWARE");
    if (!v || !*v) return false;
    const std::string s(v);
    return s != "0" && s != "false" && s != "FALSE";
}

EffectsQuality resolveEffectsQuality(EffectsQuality q, bool softwareRendering) {
    if (q != EffectsQuality::Auto) return q;
    return softwareRendering ? EffectsQuality::Reduced : EffectsQuality::Full;
}

EffectParams applyEffectsQuality(const EffectParams& p, EffectsQuality resolved) {
    EffectParams r = p;
    if (resolved == EffectsQuality::Reduced || resolved == EffectsQuality::Static) {
        r.staticNoise = 0.0f;
        r.jitter = 0.0f;
        r.bloomRadiusHalfRes = std::max(4, p.bloomRadiusHalfRes / 2);
        r.burnInHalfRes = true;
    }
    if (resolved == EffectsQuality::Static) {
        r.flickering = 0.0f;
        r.horizontalSync = 0.0f;
        r.glowingLine = 0.0f;
        r.burnIn = 0.0f;
    }
    return r;
}

bool sameEffectParams(const EffectParams& a, const EffectParams& b) {
    auto rgb = [](const Rgb& x, const Rgb& y) { return x.r == y.r && x.g == y.g && x.b == y.b; };
    return rgb(a.fontColor, b.fontColor) && rgb(a.backgroundColor, b.backgroundColor) && a.bloom == b.bloom &&
           a.burnIn == b.burnIn && a.chromaColor == b.chromaColor && a.brightness == b.brightness &&
           a.ambientLight == b.ambientLight && a.flickering == b.flickering && a.glowingLine == b.glowingLine &&
           a.horizontalSync == b.horizontalSync && a.jitter == b.jitter && a.screenCurvature == b.screenCurvature &&
           a.staticNoise == b.staticNoise && a.rasterization == b.rasterization &&
           a.scanlinePeriod == b.scanlinePeriod && a.rasterizationIntensity == b.rasterizationIntensity &&
           a.bloomRadiusHalfRes == b.bloomRadiusHalfRes && a.burnInHalfRes == b.burnInHalfRes;
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
