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
    bw_ = p_.burnInHalfRes ? hw_ : w_;
    bh_ = p_.burnInHalfRes ? hh_ : h_;
    const size_t n = static_cast<size_t>(w_) * h_ * 4;
    src_.assign(n, 0.0f);
    static_.assign(n, 0.0f);
    burn_.assign(static_cast<size_t>(bw_) * bh_ * 4, 0.0f);
    down_.assign(static_cast<size_t>(hw_) * hh_ * 4, 0.0f);
    bloomA_.assign(down_.size(), 0.0f);
    bloomB_.assign(down_.size(), 0.0f);
    haveBurn_ = false;
    buildLut();
    buildFrame();
}

void CpuEffects::buildFrame() {
    frame_.clear();
    refl_.clear();
    spans_.clear();
    if (!p_.frame) return;
    frame_.assign(static_cast<size_t>(w_) * h_ * 4, 0.0f);
    std::vector<float> refl(frame_.size());
    pool_->parallelFor(h_, [&](int y0, int y1) {
        frameLayerRows(p_, w_, h_, y0, y1, &frame_[static_cast<size_t>(y0) * w_ * 4],
                       &refl[static_cast<size_t>(y0) * w_ * 4]);
    });
    spans_.resize(h_);
    for (int y = 0; y < h_; ++y) {
        int start = -1;
        for (int x = 0; x <= w_; ++x) {
            bool on = false;
            if (x < w_) {
                const float* f = &frame_[(static_cast<size_t>(y) * w_ + x) * 4];
                on = f[0] != 0.0f || f[1] != 0.0f || f[2] != 0.0f || f[3] != 0.0f;
            }
            if (on && start < 0) start = x;
            if (!on && start >= 0) {
                spans_[y].emplace_back(start, x);
                start = -1;
            }
        }
    }
    if (p_.bloom > 0 && p_.frameShininess > 0) {
        for (size_t i = 0; i < static_cast<size_t>(w_) * h_; ++i)
            if (refl[i * 4] > 0.0f) refl_.push_back(Refl{static_cast<uint32_t>(i), refl[i * 4], refl[i * 4 + 1], refl[i * 4 + 2]});
    }
    base_ = frame_;
}

void CpuEffects::buildLut() {
    geom_ = frameGeometry(p_, w_, h_);
    warp_.assign(static_cast<size_t>(w_) * h_ * 4, 0.0f);
    pool_->parallelFor(h_, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float v = (y + 0.5f) / h_;
            for (int x = 0; x < w_; ++x) {
                float u = (x + 0.5f) / w_;
                float* o = &warp_[(static_cast<size_t>(y) * w_ + x) * 4];
                float su, sv, lu, lv;
                screenCoords(u, v, geom_, d_.curvature, su, sv, lu, lv);
                // Distance from the centre of the (unwarped) glass: noise and ambient light.
                float cx = 0.5f - (u - geom_.ix) / (1.0f - 2.0f * geom_.ix);
                float cy = 0.5f - (v - geom_.iy) / (1.0f - 2.0f * geom_.iy);
                o[0] = su;
                o[1] = sv;
                o[2] = std::sqrt(cx * cx + cy * cy);
                o[3] = std::min(step(0.0f, lu) - step(1.0f, lu), step(0.0f, lv) - step(1.0f, lv));
            }
        }
    });
}

void CpuEffects::updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs, int rowBegin,
                              int rowEnd) {
    // 0. bytes -> float, only the rows that changed.
    if (rowEnd < 0 || rowEnd > h_) rowEnd = h_;
    rowBegin = std::max(0, rowBegin);
    const bool bgra = order == PixelOrder::BGRA;
    if (rowEnd > rowBegin) {
        pool_->parallelFor(rowEnd - rowBegin, [&](int y0, int y1) {
            for (int y = rowBegin + y0; y < rowBegin + y1; ++y) {
                const uint8_t* s = px + static_cast<size_t>(y) * stride;
                float* d = &src_[static_cast<size_t>(y) * w_ * 4];
                for (int x = 0; x < w_; ++x) {
                    d[x * 4 + 0] = s[x * 4 + (bgra ? 2 : 0)] * (1.0f / 255.0f);
                    d[x * 4 + 1] = s[x * 4 + 1] * (1.0f / 255.0f);
                    d[x * 4 + 2] = s[x * 4 + (bgra ? 0 : 2)] * (1.0f / 255.0f);
                    d[x * 4 + 3] = 1.0f;
                }
            }
        });
    }

    // 1. half-resolution 2x2 box downsample (input of the bloom blur and of the half-res burn-in).
    auto t0 = Clock::now();
    const bool halfBurn = p_.burnIn > 0 && p_.burnInHalfRes;
    if (p_.bloom > 0 || halfBurn) {
        pool_->parallelFor(hh_, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                int sy0 = clampi(2 * y, 0, h_ - 1), sy1 = clampi(2 * y + 1, 0, h_ - 1);
                for (int x = 0; x < hw_; ++x) {
                    int sx0 = clampi(2 * x, 0, w_ - 1), sx1 = clampi(2 * x + 1, 0, w_ - 1);
                    const float* a = &src_[(static_cast<size_t>(sy0) * w_ + sx0) * 4];
                    const float* b = &src_[(static_cast<size_t>(sy0) * w_ + sx1) * 4];
                    const float* c = &src_[(static_cast<size_t>(sy1) * w_ + sx0) * 4];
                    const float* d = &src_[(static_cast<size_t>(sy1) * w_ + sx1) * 4];
                    float* o = &down_[(static_cast<size_t>(y) * hw_ + x) * 4];
                    for (int i = 0; i < 4; ++i) o[i] = 0.25f * (a[i] + b[i] + c[i] + d[i]);
                }
            }
        });
    }
    // Bloom: separable Gaussian with transparent border, down_ -> B -> A.
    if (p_.bloom > 0) {
        const int R = p_.bloomRadiusHalfRes;
        // horizontal: A -> B
        pool_->parallelFor(hh_, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float* row = &down_[static_cast<size_t>(y) * hw_ * 4];
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
        const float* srcBuf = p_.burnInHalfRes ? down_.data() : src_.data();
        pool_->parallelFor(bh_, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * bw_; i < static_cast<size_t>(y1) * bw_; ++i) {
                float* acc = &burn_[i * 4];
                const float* t = srcBuf + i * 4;
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
    const float shift = rgbShiftPx(p_) / std::max(1, w_);  // uv
    pool_->parallelFor(h_, [&](int y0, int y1) {
        float t[4], b[4], rc[4], lc[4];
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w_; ++x) {
                const size_t idx = static_cast<size_t>(y) * w_ + x;
                const float* lut = &warp_[idx * 4];
                const float cu = lut[0], cv = lut[1];
                sampleClamp<3>(src_.data(), w_, h_, cu, cv, t);
                if (shift != 0.0f) {  // CRT rbgShift
                    sampleClamp<3>(src_.data(), w_, h_, cu + shift, cv, rc);
                    sampleClamp<3>(src_.data(), w_, h_, cu - shift, cv, lc);
                    t[0] = lc[0] * 0.10f + rc[0] * 0.30f + t[0] * 0.60f;
                    t[1] = lc[1] * 0.20f + rc[1] * 0.20f + t[1] * 0.60f;
                    t[2] = lc[2] * 0.30f + rc[2] * 0.10f + t[2] * 0.60f;
                }
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
    // 4. bezel reflection: the glow mirrored at the glass edge, only on the frame's pixels.
    if (!refl_.empty()) {
        const float bs = d_.bloomStrength, sb = d_.screenBrightness;
        pool_->parallelFor(static_cast<int>(refl_.size()), [&](int i0, int i1) {
            float b[4];
            for (int i = i0; i < i1; ++i) {
                const Refl& r = refl_[i];
                sampleClamp<3>(bloomA_.data(), hw_, hh_, r.u, r.v, b);
                const float g2 = rgb2grey(b[0], b[1], b[2]);
                const float* s0 = &base_[static_cast<size_t>(r.idx) * 4];
                float* o = &frame_[static_cast<size_t>(r.idx) * 4];
                o[0] = s0[0] + r.w * std::min(std::max(fc.r * mixf(g2, b[0], chroma) * bs, 0.0f), 0.5f) * sb;
                o[1] = s0[1] + r.w * std::min(std::max(fc.g * mixf(g2, b[1], chroma) * bs, 0.0f), 0.5f) * sb;
                o[2] = s0[2] + r.w * std::min(std::max(fc.b * mixf(g2, b[2], chroma) * bs, 0.0f), 0.5f) * sb;
            }
        });
    }
    lastStaticMs = msSince(t0);
}

void CpuEffects::renderTo(float timeMs, uint8_t* out, int stride, PixelOrder order) {
    const int ri = order == PixelOrder::BGRA ? 2 : 0, bi = 2 - ri;
    const FrameUniforms fu = frameUniforms(p_, timeMs, burnLast_);
    const Rgb fc = p_.fontColor;
    const float chroma = p_.chromaColor;
    const float vrx = w_ / p_.scanlinePeriod, vry = h_ / p_.scanlinePeriod;
    (void)vrx;
    const float snx = (w_ / p_.pixelScale * 0.75f) / kNoiseSize, sny = (h_ / p_.pixelScale * 0.75f) / kNoiseSize;
    const float* noise = noiseTexture().data();
    const bool useNoise = p_.jitter != 0.0f || p_.staticNoise != 0.0f;
    float noiseAmount = p_.staticNoise;
    if (p_.horizontalSync != 0.0f && p_.staticNoise != 0.0f) noiseAmount += fu.hsyncScale * 7.0f;

    const bool frame = !spans_.empty();
    pool_->parallelFor(h_, [&](int y0, int y1) {
        float nt[4] = {0, 0, 0, 0}, t[4], b[4];
        std::vector<float> rowf(frame ? static_cast<size_t>(w_) * 3 : 0);  // unquantised, for the bezel
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
            uint8_t* orow = out + static_cast<size_t>(y) * stride;
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
                    sampleClamp<3>(burn_.data(), bw_, bh_, su, sv, b);
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
                    f = clamp01(f * inScreen);  // outside the tube: black (or the bezel, below)
                    if (frame) rowf[x * 3 + i] = f;
                    o[i == 0 ? ri : (i == 2 ? bi : 1)] = static_cast<uint8_t>(f * 255.0f + 0.5f);
                }
                o[3] = 255;
            }
            if (frame) {  // bezel over the tube (premultiplied), only where the layer is not empty
                for (const auto& sp : spans_[y]) {
                    for (int x = sp.first; x < sp.second; ++x) {
                        const float* f = &frame_[(static_cast<size_t>(y) * w_ + x) * 4];
                        uint8_t* o = orow + x * 4;
                        const float k = 1.0f - f[3];
                        const float c0 = rowf[x * 3 + 0] * k + f[0];
                        const float c1 = rowf[x * 3 + 1] * k + f[1];
                        const float c2 = rowf[x * 3 + 2] * k + f[2];
                        o[ri] = static_cast<uint8_t>(clamp01(c0) * 255.0f + 0.5f);
                        o[1] = static_cast<uint8_t>(clamp01(c1) * 255.0f + 0.5f);
                        o[bi] = static_cast<uint8_t>(clamp01(c2) * 255.0f + 0.5f);
                    }
                }
            }
        }
    });
}

}  // namespace lterm
