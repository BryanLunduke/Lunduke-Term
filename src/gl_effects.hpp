// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "effects.hpp"
#include "gl_pipeline.hpp"

#include <memory>
#include <string>

namespace lterm {

// Headless EGL (surfaceless, or a pbuffer) + OpenGL 3.3 core context running a GlPipeline, for
// tests, benchmarks and the golden comparison. Under LIBGL_ALWAYS_SOFTWARE=1 this is Mesa
// llvmpipe. The terminal window uses GlPipeline directly on its GtkGLArea context.
class GlEffects final : public EffectsPipeline {
public:
    explicit GlEffects(const EffectParams& params);
    ~GlEffects() override;
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    std::string renderer() const;
    void resize(int width, int height) override;
    void updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs, int rowBegin = 0,
                      int rowEnd = -1) override;
    void render(float timeMs, uint8_t* out) override;
    const char* name() const override { return "gl"; }
    // Tick without readback (what the window does), followed by glFinish so it can be timed.
    void renderNoReadback(float timeMs);
    GlPipeline* pipeline() { return pipe_.get(); }

private:
    bool initEgl();
    bool ok_ = false;
    std::string error_;
    void* dpy_ = nullptr;
    void* ctx_ = nullptr;
    void* surf_ = nullptr;
    std::unique_ptr<GlPipeline> pipe_;
    unsigned scratchTex_ = 0, scratchFbo_ = 0;
    int scratchW_ = 0, scratchH_ = 0;
};

}  // namespace lterm
