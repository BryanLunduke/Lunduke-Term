// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu_effects.hpp"
#include "sampling.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace lterm {

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
inline float step(float edge, float x) { return x >= edge ? 1.0f : 0.0f; }
inline float fractf(float x) { return x - ffloor(x); }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline float smoothstepf(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

CpuEffects::CpuEffects(const EffectParams& params, int threads)
    : p_(params), d_(derive(params)), pool_(new ThreadPool(threads)),
      weights_(bloomWeights(params.bloomRadiusHalfRes)) {}

void CpuEffects::resize(int width, int height) {
    w_ = width;
    h_ = height;
    hw_ = std::max(1, w_ / 2);
    hh_ = std::max(1, h_ / 2);
    const size_t n = static_cast<size_t>(w_) * h_ * 4;
    src_.assign(n, 0.0f);
    static_.assign(n, 0.0f);
    burn_.assign(n, 0.0f);
    burnTmp_.assign(n, 0.0f);
    bloomA_.assign(static_cast<size_t>(hw_) * hh_ * 4, 0.0f);
    bloomB_.assign(bloomA_.size(), 0.0f);
    haveBurn_ = false;
    buildLut();
}

void CpuEffects::buildLut() {
    warp_.assign(static_cast<size_t>(w_) * h_ * 4, 0.0f);
    pool_->parallelFor(h_, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float v = (y + 0.5f) / h_;
            for (int x = 0; x < w_; ++x) {
                float u = (x + 0.5f) / w_;
                float* o = &warp_[(static_cast<size_t>(y) * w_ + x) * 4];
                float su, sv;
                if (d_.curvature != 0.0f) {
                    barrel(u, v, d_.curvature, su, sv);
                } else {
                    su = u;
                    sv = v;
                }
                float cx = 0.5f - u, cy = 0.5f - v;
                o[0] = su;
                o[1] = sv;
                o[2] = std::sqrt(cx * cx + cy * cy);
                o[3] = std::min(step(0.0f, su) - step(1.0f, su), step(0.0f, sv) - step(1.0f, sv));
            }
        }
    });
}

void CpuEffects::setSource(const uint8_t* rgba, int stride, float timeMs) {
    // 0. bytes -> float
    pool_->parallelFor(h_, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = rgba + static_cast<size_t>(y) * stride;
            float* d = &src_[static_cast<size_t>(y) * w_ * 4];
            for (int i = 0; i < w_ * 4; ++i) d[i] = s[i] * (1.0f / 255.0f);
        }
    });

    // 1. bloom source: 2x2 box downsample, then separable Gaussian with transparent border.
    auto t0 = Clock::now();
    if (p_.bloom > 0) {
        const int R = p_.bloomRadiusHalfRes;
        pool_->parallelFor(hh_, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                int sy0 = clampi(2 * y, 0, h_ - 1), sy1 = clampi(2 * y + 1, 0, h_ - 1);
                for (int x = 0; x < hw_; ++x) {
                    int sx0 = clampi(2 * x, 0, w_ - 1), sx1 = clampi(2 * x + 1, 0, w_ - 1);
                    const float* a = &src_[(static_cast<size_t>(sy0) * w_ + sx0) * 4];
                    const float* b = &src_[(static_cast<size_t>(sy0) * w_ + sx1) * 4];
                    const float* c = &src_[(static_cast<size_t>(sy1) * w_ + sx0) * 4];
                    const float* d = &src_[(static_cast<size_t>(sy1) * w_ + sx1) * 4];
                    float* o = &bloomA_[(static_cast<size_t>(y) * hw_ + x) * 4];
                    for (int i = 0; i < 4; ++i) o[i] = 0.25f * (a[i] + b[i] + c[i] + d[i]);
                }
            }
        });
        // horizontal: A -> B
        pool_->parallelFor(hh_, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float* row = &bloomA_[static_cast<size_t>(y) * hw_ * 4];
                for (int x = 0; x < hw_; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    for (int k = -R; k <= R; ++k) {
                        int sx = x + k;
                        if (sx < 0 || sx >= hw_) continue;
                        float wk = weights_[k < 0 ? -k : k];
                        const float* s = row + static_cast<size_t>(sx) * 4;
                        for (int i = 0; i < 4; ++i) acc[i] += wk * s[i];
                    }
                    float* o = &bloomB_[(static_cast<size_t>(y) * hw_ + x) * 4];
                    for (int i = 0; i < 4; ++i) o[i] = acc[i];
                }
            }
        });
        // vertical: B -> A
        pool_->parallelFor(hh_, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* o = &bloomA_[static_cast<size_t>(y) * hw_ * 4];
                std::fill(o, o + hw_ * 4, 0.0f);
                for (int k = -R; k <= R; ++k) {
                    int sy = y + k;
                    if (sy < 0 || sy >= hh_) continue;
                    float wk = weights_[k < 0 ? -k : k];
                    const float* s = &bloomB_[static_cast<size_t>(sy) * hw_ * 4];
                    for (int i = 0; i < hw_ * 4; ++i) o[i] += wk * s[i];
                }
            }
        });
    }
    lastBloomMs = msSince(t0);

    // 2. burn-in feedback (source space, pointwise).
    t0 = Clock::now();
    if (p_.burnIn > 0) {
        if (!haveBurn_) {
            std::fill(burn_.begin(), burn_.end(), 0.0f);
            burnPrev_ = burnLast_ = timeMs;
            haveBurn_ = true;
        } else if (timeMs > burnLast_) {
            burnPrev_ = burnLast_;
            burnLast_ = timeMs;
        }
        const float decay0 = clamp01((burnLast_ - burnPrev_) * d_.burnInTime);
        pool_->parallelFor(h_, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * w_; i < static_cast<size_t>(y1) * w_; ++i) {
                float* acc = &burn_[i * 4];
                const float* t = &src_[i * 4];
                float decay = std::max(0.0f, decay0 - acc[3]);
                for (int c = 0; c < 3; ++c) acc[c] = clamp01(std::max(acc[c] - decay, t[c]));
                acc[3] = rgb2grey(t[0], t[1], t[2]);
            }
        });
    }
    lastBurnMs = msSince(t0);

    // 3. static pass.
    t0 = Clock::now();
    const Rgb fc = p_.fontColor, bg = p_.backgroundColor;
    const float chroma = p_.chromaColor;
    pool_->parallelFor(h_, [&](int y0, int y1) {
        float t[4], b[4];
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w_; ++x) {
                const size_t idx = static_cast<size_t>(y) * w_ + x;
                const float* lut = &warp_[idx * 4];
                const float cu = lut[0], cv = lut[1];
                sampleClamp<3>(src_.data(), w_, h_, cu, cv, t);
                t[0] += 0.0001f;
                t[1] += 0.0001f;
                t[2] += 0.0001f;
                const float g = rgb2grey(t[0], t[1], t[2]);
                float mask = (step(0.0f, cu) - step(1.0f, cu)) + (step(0.0f, cv) - step(1.0f, cv));
                mask = clamp01(mask);
                float fr, fg, fb;
                if (chroma != 0.0f) {
                    fr = mixf(fc.r, t[0] * fc.r / g, chroma);
                    fg = mixf(fc.g, t[1] * fc.g / g, chroma);
                    fb = mixf(fc.b, t[2] * fc.b / g, chroma);
                } else {
                    fr = fc.r;
                    fg = fc.g;
                    fb = fc.b;
                }
                float o0 = mixf(bg.r, fr, g * mask);
                float o1 = mixf(bg.g, fg, g * mask);
                float o2 = mixf(bg.b, fb, g * mask);
                if (p_.bloom > 0) {
                    sampleClamp<3>(bloomA_.data(), hw_, hh_, cu, cv, b);
                    const float bg2 = rgb2grey(b[0], b[1], b[2]);
                    float br = fc.r * mixf(bg2, b[0], chroma);
                    float bgc = fc.g * mixf(bg2, b[1], chroma);
                    float bb = fc.b * mixf(bg2, b[2], chroma);
                    o0 += std::min(std::max(br * d_.bloomStrength, 0.0f), 0.5f);
                    o1 += std::min(std::max(bgc * d_.bloomStrength, 0.0f), 0.5f);
                    o2 += std::min(std::max(bb * d_.bloomStrength, 0.0f), 0.5f);
                }
                float* o = &static_[idx * 4];
                o[0] = o0 * d_.screenBrightness;
                o[1] = o1 * d_.screenBrightness;
                o[2] = o2 * d_.screenBrightness;
                o[3] = 1.0f;
            }
        }
    });
    lastStaticMs = msSince(t0);
}

void CpuEffects::render(float timeMs, uint8_t* out) {
    const FrameUniforms fu = frameUniforms(p_, timeMs, burnLast_);
    const Rgb fc = p_.fontColor;
    const float chroma = p_.chromaColor;
    const float vrx = w_ / p_.scanlinePeriod, vry = h_ / p_.scanlinePeriod;
    (void)vrx;
    const float snx = (w_ * 0.75f) / kNoiseSize, sny = (h_ * 0.75f) / kNoiseSize;
    const float* noise = noiseTexture().data();
    const bool useNoise = p_.jitter != 0.0f || p_.staticNoise != 0.0f;
    float noiseAmount = p_.staticNoise;
    if (p_.horizontalSync != 0.0f && p_.staticNoise != 0.0f) noiseAmount += fu.hsyncScale * 7.0f;

    pool_->parallelFor(h_, [&](int y0, int y1) {
        float nt[4] = {0, 0, 0, 0}, t[4], b[4];
        for (int y = y0; y < y1; ++y) {
            const float v = (y + 0.5f) / h_;
            float dx = 0.0f;
            if (p_.horizontalSync != 0.0f) {
                dx = std::sin((v + timeMs * 0.001f) * fu.hsyncFreq) * fu.hsyncScale;
            }
            float glow = 0.0f;
            if (p_.glowingLine != 0.0f) {
                glow = fractf(smoothstepf(-120.0f, 0.0f,
                                          v * vry - (vry + 120.0f) * fractf(timeMs * 0.00015f))) *
                       d_.glowingLine;
            }
            uint8_t* orow = out + static_cast<size_t>(y) * w_ * 4;
            for (int x = 0; x < w_; ++x) {
                const size_t idx = static_cast<size_t>(y) * w_ + x;
                const float* lut = &warp_[idx * 4];
                const float su = lut[0], sv = lut[1], dist = lut[2], inScreen = lut[3];
                const float u = (x + 0.5f) / w_ + dx;
                if (useNoise) sampleRepeat(noise, kNoiseSize, kNoiseSize, snx * u + fu.noiseOffsetX,
                                           sny * v + fu.noiseOffsetY, nt);
                float tu = u, tv = v;
                if (p_.jitter != 0.0f) {
                    tu += (nt[2] - 0.5f) * d_.jitterX;
                    tv += (nt[3] - 0.5f) * d_.jitterY;
                }
                float color = 0.0001f;
                if (p_.staticNoise != 0.0f) color += nt[3] * noiseAmount * (1.0f - dist * 1.3f);
                color += glow;
                sampleClamp<3>(static_.data(), w_, h_, tu, tv, t);
                if (p_.burnIn != 0.0f) {
                    sampleClamp<3>(burn_.data(), w_, h_, su, sv, b);
                    float br = 0.65f * (b[0] - fu.burnInDecay);
                    float bgc = 0.65f * (b[1] - fu.burnInDecay);
                    float bb = 0.65f * (b[2] - fu.burnInDecay);
                    const float gg = rgb2grey(br, bgc, bb);
                    t[0] = std::max(t[0], fc.r * mixf(gg, br, chroma));
                    t[1] = std::max(t[1], fc.g * mixf(gg, bgc, chroma));
                    t[2] = std::max(t[2], fc.b * mixf(gg, bb, chroma));
                }
                t[0] += fc.r * color;
                t[1] += fc.g * color;
                t[2] += fc.b * color;
                if (p_.rasterization == 1) {
                    const float c = fractf(sv * vry) * 2.0f - 1.0f;
                    const float mask = 1.0f - std::fabs(c);
                    for (int i = 0; i < 3; ++i) {
                        const float hi = ((1.0f + 0.30f) - (0.2f * t[i])) * t[i];
                        const float lo = ((1.0f - 0.30f) + (0.1f * t[i])) * t[i];
                        t[i] = mixf(t[i], mixf(lo, hi, mask), p_.rasterizationIntensity);
                    }
                }
                const float amb = d_.ambient * (1.0f - dist) * (1.0f - dist);
                uint8_t* o = orow + x * 4;
                for (int i = 0; i < 3; ++i) {
                    float f = t[i] * fu.flickerBrightness + amb;
                    f = f * inScreen;  // outside the tube: black (stand-in for the bezel)
                    o[i] = static_cast<uint8_t>(clamp01(f) * 255.0f + 0.5f);
                }
                o[3] = 255;
            }
        }
    });
}

}  // namespace lterm
