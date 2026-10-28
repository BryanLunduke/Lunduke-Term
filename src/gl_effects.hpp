// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "effects.hpp"

#include <string>

namespace lterm {

// Headless EGL (surfaceless / pbuffer) + OpenGL 3.3 core back-end for the M0 comparison.
// Under LIBGL_ALWAYS_SOFTWARE=1 this runs on Mesa llvmpipe.
class GlEffects final : public EffectsPipeline {
public:
    explicit GlEffects(const EffectParams& params);
    ~GlEffects() override;
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    std::string renderer() const;
    void resize(int width, int height) override;
    void setSource(const uint8_t* rgba, int stride, float timeMs) override;
    void render(float timeMs, uint8_t* out) override;
    const char* name() const override { return "gl"; }

private:
    bool initEgl();
    unsigned compile(const char* fs);
    unsigned makeTex(int w, int h, unsigned internalFmt, bool linear, bool repeat);
    void drawTo(unsigned fbo, int w, int h, unsigned prog);

    EffectParams p_;
    DerivedParams d_;
    bool ok_ = false;
    std::string error_;
    void* dpy_ = nullptr;
    void* ctx_ = nullptr;
    void* surf_ = nullptr;
    unsigned vao_ = 0;
    unsigned progDown_ = 0, progBlur_ = 0, progBurn_ = 0, progStatic_ = 0, progDyn_ = 0;
    unsigned texSrc_ = 0, texBloomA_ = 0, texBloomB_ = 0, texBurn_[2] = {0, 0}, texStatic_ = 0,
             texNoise_ = 0, texOut_ = 0;
    unsigned fboBloomA_ = 0, fboBloomB_ = 0, fboBurn_[2] = {0, 0}, fboStatic_ = 0, fboOut_ = 0;
    int burnCur_ = 0;
    int w_ = 0, h_ = 0, hw_ = 0, hh_ = 0;
    std::vector<float> upload_;
    float burnLast_ = 0, burnPrev_ = 0;
    bool haveBurn_ = false;
};

}  // namespace lterm
