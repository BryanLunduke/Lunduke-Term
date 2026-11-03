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
    // Static pass output, RGB float (perf step 3: was RGBA, 16 B per pixel, now 12; nothing reads the
    // alpha). 16-bit fixed point (6 B) was measured and cost up to 0.2 dB of GL-vs-CPU PSNR, taking
    // the worst configuration below the bar (docs/bench/perf3), so the values stay floats.
    std::vector<float> static_;
    // Perf step 3: the screen-local coordinates before the barrel warp (separable), per column / row.
    std::vector<float> colLu_, rowLv_;
    // Row y of the warp, mirroring screenCoords() (barrel(), then the bezel inset; same operations in
    // the same order): source uv (su, sv) per pixel always; when non-null, the distance from the
    // centre of the unwarped glass and the in-screen mask of the warped screen-local uv. Plain loops
    // over locals so the compiler can vectorise them (Ted's row prepass, PR #11 review).
    void warpRow(int y, float* __restrict su, float* __restrict sv, float* __restrict dist,
                 float* __restrict inScreen) const;
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
