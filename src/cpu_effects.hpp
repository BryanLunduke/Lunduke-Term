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
    void setSource(const uint8_t* rgba, int stride, float timeMs) override;
    void render(float timeMs, uint8_t* out) override;
    const char* name() const override { return "cpu"; }

    // Stage timings of the last setSource() in ms (bloom, burn-in, static).
    double lastBloomMs = 0, lastBurnMs = 0, lastStaticMs = 0;

private:
    void buildLut();
    EffectParams p_;
    DerivedParams d_;
    std::unique_ptr<ThreadPool> pool_;
    int w_ = 0, h_ = 0, hw_ = 0, hh_ = 0;
    std::vector<float> src_;      // RGBA float, source
    std::vector<float> bloomA_, bloomB_;  // half res RGBA
    std::vector<float> burn_, burnTmp_;   // RGBA float (rgb + prev mask)
    std::vector<float> static_;   // RGBA float, static pass output
    std::vector<float> warp_;     // per pixel: staticCoords.u, .v, distance, inScreen
    std::vector<float> weights_;
    float burnLast_ = 0, burnPrev_ = 0;
    bool haveBurn_ = false;
};

}  // namespace lterm
