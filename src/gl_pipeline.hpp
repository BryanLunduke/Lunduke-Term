// SPDX-License-Identifier: GPL-3.0-or-later
// The GLSL effects pipeline on whatever OpenGL 3.3 core context is current: the GtkGLArea
// context in the terminal window, or the headless EGL context used by tests and benchmarks.
#pragma once

#include "effects.hpp"

#include <string>
#include <vector>

namespace lterm {

class GlPipeline {
public:
    // Requires a current GL 3.3 core context; all other calls need the same context current.
    explicit GlPipeline(const EffectParams& params);
    ~GlPipeline();  // does NOT touch GL: call release() with the context current first
    GlPipeline(const GlPipeline&) = delete;
    GlPipeline& operator=(const GlPipeline&) = delete;

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    std::string renderer() const;

    void resize(int width, int height);  // frees the previous size's textures and FBOs
    int width() const { return w_; }
    int height() const { return h_; }
    // Uploads rows [rowBegin, rowEnd) (rowEnd < 0: all) of an 8-bit source image and runs the
    // content passes (downsample, bloom, burn-in, static).
    void updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs, int rowBegin = 0,
                      int rowEnd = -1);
    // Effects tick into `fbo` (0 = default framebuffer) at (0,0,width,height). flipY for window
    // framebuffers (GtkGLArea); false for the internal FBO that readPixels() reads.
    void render(float timeMs, unsigned fbo, bool flipY);
    // Effects tick into the internal output FBO and read it back as RGBA8, row 0 = top.
    void renderToMemory(float timeMs, uint8_t* out);
    void release();  // deletes every GL object this pipeline owns

    // Leak accounting: textures / framebuffers currently owned by this pipeline.
    int liveTextures() const { return liveTex_; }
    int liveFramebuffers() const { return liveFbo_; }

private:
    unsigned compile(const char* fs);
    unsigned makeTex(int w, int h, unsigned internalFmt, bool linear, bool repeat);
    unsigned makeFbo(unsigned tex);
    void freeSized();
    void drawTo(unsigned fbo, int w, int h, unsigned prog);

    EffectParams p_;
    DerivedParams d_;
    bool ok_ = false;
    std::string error_;
    unsigned vao_ = 0;
    unsigned progDown_ = 0, progBlur_ = 0, progBurn_ = 0, progStatic_ = 0, progDyn_ = 0, progFrame_ = 0,
             progFrameComp_ = 0;
    unsigned texNoise_ = 0;
    // Size-dependent objects (recreated by resize()).
    unsigned texSrc_ = 0, texDown_ = 0, texBloomA_ = 0, texBloomB_ = 0, texBurn_[2] = {0, 0}, texStatic_ = 0,
             texOut_ = 0;
    unsigned fboDown_ = 0, fboBloomA_ = 0, fboBloomB_ = 0, fboBurn_[2] = {0, 0}, fboStatic_ = 0, fboOut_ = 0;
    // Bezel (p_.frame): static base + reflection lookup (rendered on resize), composite per content.
    unsigned texFrameBase_ = 0, texFrameRefl_ = 0, texFrame_ = 0;
    unsigned fboFrameBase_ = 0, fboFrameRefl_ = 0, fboFrame_ = 0;
    FrameGeometry geom_;
    int liveTex_ = 0, liveFbo_ = 0;
    int burnCur_ = 0;
    int w_ = 0, h_ = 0, hw_ = 0, hh_ = 0, bw_ = 0, bh_ = 0;
    float burnLast_ = 0, burnPrev_ = 0;
    bool haveBurn_ = false;
    std::vector<float> weights_;
};

// Number of texture names 1..maxName the current context reports as textures (glIsTexture).
// An independent leak check that does not rely on the pipeline's own accounting.
int countGlTextures(unsigned maxName = 4096);

}  // namespace lterm
