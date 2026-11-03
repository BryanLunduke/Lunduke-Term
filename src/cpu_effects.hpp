// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "effects.hpp"
#include "thread_pool.hpp"

#include <memory>

namespace lterm {

// Plain C++ software implementation, row-parallel over a small thread pool.
class CpuEffects final : public EffectsPipeline {
public:
    CpuEffects(const EffectParams& params, int threads);
    void resize(int width, int height) override;
    void updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs, int rowBegin = 0,
                      int rowEnd = -1) override;
    void render(float timeMs, uint8_t* out) override { renderTo(timeMs, out, w_ * 4, PixelOrder::RGBA); }
    // Effects tick straight into a caller buffer (e.g. a Cairo RGB24 surface with BGRA order).
    void renderTo(float timeMs, uint8_t* out, int stride, PixelOrder order);
    int width() const { return w_; }
    int height() const { return h_; }
    const char* name() const override { return "cpu"; }

    // Stage timings of the last setSource() in ms (bloom, burn-in, static).
    double lastBloomMs = 0, lastBurnMs = 0, lastStaticMs = 0;

private:
    void buildLut();
    EffectParams p_;
    DerivedParams d_;
    std::unique_ptr<ThreadPool> pool_;
    int w_ = 0, h_ = 0, hw_ = 0, hh_ = 0;
    std::vector<uint8_t> src_;    // RGBA bytes (R,G,B,255), source; read as k/255 (M6: was float, 16 B/px)
    // Half res RGB (perf step 3: no alpha, nothing reads one): the 2x2 box downsample of src_, then
    // (after the burn-in has read it) the bloom; bloomB_ is the blur's horizontal pass.
    std::vector<float> down_;
    std::vector<float> bloomB_;
    std::vector<float> burn_;             // RGBA float (rgb + prev mask), full or half res
    int bw_ = 0, bh_ = 0;                 // burn-in buffer size
    std::vector<float> static_;   // RGBA float, static pass output
    // Perf step 3: the screen-local coordinates before the barrel warp (separable), per column / row.
    std::vector<float> colLu_, rowLv_;
    // Source uv (su, sv) and warped screen-local uv (lu, lv) of pixel (x, y); mirrors screenCoords().
    void warpAt(int x, int y, float& su, float& sv, float& lu, float& lv) const {
        lu = colLu_[static_cast<size_t>(x)];
        lv = rowLv_[static_cast<size_t>(y)];
        if (d_.curvature != 0.0f) barrel(lu, lv, d_.curvature, lu, lv);
        su = geom_.ix + lu * (1.0f - 2.0f * geom_.ix);
        sv = geom_.iy + lv * (1.0f - 2.0f * geom_.iy);
    }
    std::vector<float> weights_;
    // Bezel (p_.frame): the composited premultiplied layer (static base + content reflection),
    // the pixels that carry a reflection, and per row the x spans where the layer is not empty.
    FrameGeometry geom_;
    // M6: the static base of a reflecting pixel lives in its Refl (was a full copy of frame_).
    std::vector<float> frame_;
    struct Refl { uint32_t idx; float w, u, v; float base[3]; };
    std::vector<Refl> refl_;
    std::vector<std::vector<std::pair<int, int>>> spans_;
    void buildFrame();
    float burnLast_ = 0, burnPrev_ = 0;
    bool haveBurn_ = false;
};

}  // namespace lterm
