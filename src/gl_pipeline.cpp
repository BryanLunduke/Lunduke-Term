// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_pipeline.hpp"
#include "gl_shaders.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <string>

namespace lterm {

// Review item 3: the float textures (down/bloom, burn-in, static and the noise) are half floats (8 B per
// texel, was 16). Every value is a colour in 0..~4 (bloom sums) or a burn-in level; the gl-match test
// checks the result against the C++ reference (PSNR). The source and output textures are RGBA8
// (M6 item 6: the source comes from an 8-bit Cairo image).
static constexpr GLenum kSizedFmt = GL_RGBA16F;

GlPipeline::GlPipeline(const EffectParams& params)
    : p_(params), d_(derive(params)), weights_(bloomWeights(params.bloomRadiusHalfRes)) {
    weights_.resize(65, 0.0f);
    glGenVertexArrays(1, &vao_);
    progDown_ = compile(glsl::kDownsample);
    progBlur_ = compile(glsl::kBlur);
    progBurn_ = compile(glsl::kBurnIn);
    progStatic_ = compile(glsl::kStatic);
    progDyn_ = compile(glsl::kDynamic);
    progFrame_ = compile(glsl::kFrame);
    progFrameComp_ = compile(glsl::kFrameComposite);
    ok_ = progDown_ && progBlur_ && progBurn_ && progStatic_ && progDyn_ && progFrame_ && progFrameComp_;
    if (!ok_) return;
    lookUpUniforms();
    texNoise_ = makeTex(kNoiseSize, kNoiseSize, kSizedFmt, true, true);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kNoiseSize, kNoiseSize, GL_RGBA, GL_FLOAT, noiseTexture().data());
}

GlPipeline::~GlPipeline() = default;

namespace {
long gProgramsCompiled = 0;
}

long GlPipeline::programsCompiled() { return gProgramsCompiled; }

namespace {
long gUniformLookups = 0;
GLint uniformLocation(GLuint prog, const char* name) {
    ++gUniformLookups;
    return glGetUniformLocation(prog, name);
}
}  // namespace

long GlPipeline::uniformLookups() { return gUniformLookups; }

void GlPipeline::lookUpUniforms() {
#define LTERM_LOC_GET(n) loc_.down.n = uniformLocation(progDown_, #n);
    LTERM_GL_UNIFORMS_DOWN(LTERM_LOC_GET)
#undef LTERM_LOC_GET
#define LTERM_LOC_GET(n) loc_.blur.n = uniformLocation(progBlur_, #n);
    LTERM_GL_UNIFORMS_BLUR(LTERM_LOC_GET)
#undef LTERM_LOC_GET
#define LTERM_LOC_GET(n) loc_.burn.n = uniformLocation(progBurn_, #n);
    LTERM_GL_UNIFORMS_BURN(LTERM_LOC_GET)
#undef LTERM_LOC_GET
#define LTERM_LOC_GET(n) loc_.stat.n = uniformLocation(progStatic_, #n);
    LTERM_GL_UNIFORMS_STAT(LTERM_LOC_GET)
#undef LTERM_LOC_GET
#define LTERM_LOC_GET(n) loc_.dyn.n = uniformLocation(progDyn_, #n);
    LTERM_GL_UNIFORMS_DYN(LTERM_LOC_GET)
#undef LTERM_LOC_GET
#define LTERM_LOC_GET(n) loc_.frame.n = uniformLocation(progFrame_, #n);
    LTERM_GL_UNIFORMS_FRAME(LTERM_LOC_GET)
#undef LTERM_LOC_GET
#define LTERM_LOC_GET(n) loc_.comp.n = uniformLocation(progFrameComp_, #n);
    LTERM_GL_UNIFORMS_COMP(LTERM_LOC_GET)
#undef LTERM_LOC_GET
}

void GlPipeline::setParams(const EffectParams& params) {
    p_ = params;
    d_ = derive(params);
    weights_ = bloomWeights(params.bloomRadiusHalfRes);
    weights_.resize(65, 0.0f);
    freeSized();
    w_ = h_ = 0;  // the next resize() allocates for the new parameters (burn-in size, bezel)
    haveBurn_ = false;
}

void GlPipeline::release() {
    freeSized();
    if (texNoise_) deleteTex(&texNoise_);
    for (unsigned* p : {&progDown_, &progBlur_, &progBurn_, &progStatic_, &progDyn_, &progFrame_, &progFrameComp_}) {
        if (*p) glDeleteProgram(*p);
        *p = 0;
    }
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
    ok_ = false;
}

std::string GlPipeline::renderer() const {
    const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return std::string(r ? r : "?") + " / " + (v ? v : "?");
}

unsigned GlPipeline::compile(const char* fsBody) {
    auto shader = [&](GLenum type, const std::string& src) -> GLuint {
        GLuint s = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        GLint okc = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &okc);
        if (!okc) {
            char log[4096];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            error_ += std::string("shader compile: ") + log;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = shader(GL_VERTEX_SHADER, glsl::kVertex);
    GLuint fs = shader(GL_FRAGMENT_SHADER, std::string(glsl::kCommon) + fsBody);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    ++gProgramsCompiled;
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint okl = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &okl);
    if (!okl) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        error_ += std::string("program link: ") + log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

unsigned GlPipeline::makeTex(int w, int h, unsigned fmt, bool linear, bool repeat) {
    GLuint t;
    glGenTextures(1, &t);
    ++liveTex_;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, fmt == GL_RGBA8 ? GL_UNSIGNED_BYTE : GL_FLOAT, nullptr);
    const size_t texel = fmt == GL_RGBA8 ? 4 : fmt == GL_RGBA16F ? 8 : 16;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * texel;
    texBytes_[t] = bytes;
    liveTexBytes_ += bytes;
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

void GlPipeline::deleteTex(unsigned* t) {
    glDeleteTextures(1, t);
    --liveTex_;
    if (auto it = texBytes_.find(*t); it != texBytes_.end()) {
        liveTexBytes_ -= it->second;
        texBytes_.erase(it);
    }
    *t = 0;
}

unsigned GlPipeline::makeFbo(unsigned tex) {
    GLuint f;
    glGenFramebuffers(1, &f);
    ++liveFbo_;
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return f;
}

void GlPipeline::releaseSized() {
    freeSized();
    w_ = h_ = 0;
}

void GlPipeline::freeSized() {
    for (unsigned* f : {&fboDown_, &fboBloomA_, &fboBloomB_, &fboBurn_[0], &fboBurn_[1], &fboStatic_, &fboOut_,
                        &fboFrameBase_, &fboFrameRefl_, &fboFrame_}) {
        if (*f) {
            glDeleteFramebuffers(1, f);
            --liveFbo_;
        }
        *f = 0;
    }
    for (unsigned* t : {&texSrc_, &texDown_, &texBloomA_, &texBloomB_, &texBurn_[0], &texBurn_[1], &texStatic_,
                        &texOut_, &texFrameBase_, &texFrameRefl_, &texFrame_}) {
        if (*t) deleteTex(t);
    }
}

void GlPipeline::resize(int width, int height) {
    if (!ok_) return;
    if (width == w_ && height == h_ && texSrc_) return;
    freeSized();  // M0 leaked these on every resize
    w_ = std::max(1, width);
    h_ = std::max(1, height);
    hw_ = std::max(1, w_ / 2);
    hh_ = std::max(1, h_ / 2);
    bw_ = p_.burnInHalfRes ? hw_ : w_;
    bh_ = p_.burnInHalfRes ? hh_ : h_;
    // M6 item 6: the source is uploaded from an 8-bit Cairo image (glTexSubImage2D, GL_UNSIGNED_BYTE), so
    // an 8-bit texture holds it exactly; a half-float one cost 4 more bytes per pixel (4 MB at 1280x800).
    texSrc_ = makeTex(w_, h_, GL_RGBA8, true, false);
    texDown_ = makeTex(hw_, hh_, kSizedFmt, true, false);
    texBloomA_ = makeTex(hw_, hh_, kSizedFmt, true, false);
    texBloomB_ = makeTex(hw_, hh_, kSizedFmt, true, false);
    for (int i = 0; i < 2; ++i) texBurn_[i] = makeTex(bw_, bh_, kSizedFmt, true, false);
    texStatic_ = makeTex(w_, h_, kSizedFmt, true, false);
    // texOut_/fboOut_ (RGBA8, w x h) are only read back by renderToMemory() (headless tests and
    // benchmarks); the window renders to the GtkGLArea framebuffer, so they are made on first use.
    fboDown_ = makeFbo(texDown_);
    fboBloomA_ = makeFbo(texBloomA_);
    fboBloomB_ = makeFbo(texBloomB_);
    for (int i = 0; i < 2; ++i) fboBurn_[i] = makeFbo(texBurn_[i]);
    fboStatic_ = makeFbo(texStatic_);
    haveBurn_ = false;
    geom_ = frameGeometry(p_, w_, h_);
    if (p_.frame) {
        texFrameBase_ = makeTex(w_, h_, kSizedFmt, false, false);
        texFrameRefl_ = makeTex(w_, h_, kSizedFmt, false, false);
        texFrame_ = makeTex(w_, h_, kSizedFmt, false, false);
        fboFrameBase_ = makeFbo(texFrameBase_);
        fboFrameRefl_ = makeFbo(texFrameRefl_);
        fboFrame_ = makeFbo(texFrame_);
        // The static layer: once per size (the parameters are fixed for a pipeline).
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glDisable(GL_BLEND);
        const GLuint p = progFrame_;
        glUseProgram(p);
        glUniform2f(loc_.frame.size, static_cast<float>(w_), static_cast<float>(h_));
        glUniform2f(loc_.frame.inset, geom_.ix, geom_.iy);
        glUniform1f(loc_.frame.insetPx, geom_.inset);
        glUniform2f(loc_.frame.inner, geom_.innerW, geom_.innerH);
        glUniform1f(loc_.frame.radius, geom_.radius);
        glUniform1f(loc_.frame.curvature, d_.curvature);
        glUniform3f(loc_.frame.frameColor, p_.frameColor.r, p_.frameColor.g, p_.frameColor.b);
        glUniform1f(loc_.frame.shininess, p_.frameShininess);
        glUniform1f(loc_.frame.pixelScale, p_.pixelScale);
        glUniform1i(loc_.frame.mode, 0);
        drawTo(fboFrameBase_, w_, h_, p);
        glUniform1i(loc_.frame.mode, 1);
        drawTo(fboFrameRefl_, w_, h_, p);
        // Until the first content frame the composite is the base.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fboFrameBase_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboFrame_);
        glBlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    }
}

void GlPipeline::drawTo(unsigned fbo, int w, int h, unsigned prog) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glUseProgram(prog);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void bindTex(GLint loc, int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(loc, unit);
}

void GlPipeline::updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs, int rowBegin,
                              int rowEnd) {
    if (!ok_ || !texSrc_) return;
    if (rowEnd < 0 || rowEnd > h_) rowEnd = h_;
    rowBegin = std::max(0, rowBegin);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    if (rowEnd > rowBegin) {
        // 8-bit rows straight into the RGBA8 source texture; shaders sample it as c / 255.
        glBindTexture(GL_TEXTURE_2D, texSrc_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, rowBegin, w_, rowEnd - rowBegin,
                        order == PixelOrder::BGRA ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE,
                        px + static_cast<size_t>(rowBegin) * stride);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    const bool halfBurn = p_.burnIn > 0 && p_.burnInHalfRes;
    if (p_.bloom > 0 || halfBurn) {
        glUseProgram(progDown_);
        bindTex(loc_.down.src, 0, texSrc_);
        glUniform2i(loc_.down.srcSize, w_, h_);
        drawTo(fboDown_, hw_, hh_, progDown_);
    }
    if (p_.bloom > 0) {
        glUseProgram(progBlur_);
        glUniform1fv(loc_.blur.weights, 65, weights_.data());
        glUniform1i(loc_.blur.radius, std::min(64, p_.bloomRadiusHalfRes));
        glUniform2i(loc_.blur.size, hw_, hh_);
        bindTex(loc_.blur.src, 0, texDown_);
        glUniform2i(loc_.blur.dir, 1, 0);
        drawTo(fboBloomB_, hw_, hh_, progBlur_);
        bindTex(loc_.blur.src, 0, texBloomB_);
        glUniform2i(loc_.blur.dir, 0, 1);
        drawTo(fboBloomA_, hw_, hh_, progBlur_);
    }
    if (p_.burnIn > 0) {
        if (!haveBurn_) {
            for (int i = 0; i < 2; ++i) {
                glBindFramebuffer(GL_FRAMEBUFFER, fboBurn_[i]);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            burnPrev_ = burnLast_ = timeMs;
            haveBurn_ = true;
        } else if (timeMs > burnLast_) {
            burnPrev_ = burnLast_;
            burnLast_ = timeMs;
        }
        const float decay0 = std::clamp((burnLast_ - burnPrev_) * d_.burnInTime, 0.0f, 1.0f);
        const int next = 1 - burnCur_;
        glUseProgram(progBurn_);
        bindTex(loc_.burn.src, 0, p_.burnInHalfRes ? texDown_ : texSrc_);
        bindTex(loc_.burn.prev, 1, texBurn_[burnCur_]);
        glUniform1f(loc_.burn.decay0, decay0);
        drawTo(fboBurn_[next], bw_, bh_, progBurn_);
        burnCur_ = next;
    }
    const GLuint p = progStatic_;
    glUseProgram(p);
    bindTex(loc_.stat.src, 0, texSrc_);
    bindTex(loc_.stat.bloomSrc, 1, texBloomA_);
    glUniform2f(loc_.stat.size, static_cast<float>(w_), static_cast<float>(h_));
    glUniform1f(loc_.stat.curvature, d_.curvature);
    glUniform3f(loc_.stat.fontColor, p_.fontColor.r, p_.fontColor.g, p_.fontColor.b);
    glUniform3f(loc_.stat.backgroundColor, p_.backgroundColor.r, p_.backgroundColor.g,
                p_.backgroundColor.b);
    glUniform1f(loc_.stat.chroma, p_.chromaColor);
    glUniform1f(loc_.stat.bloomStrength, d_.bloomStrength);
    glUniform1f(loc_.stat.screenBrightness, d_.screenBrightness);
    glUniform1i(loc_.stat.useBloom, p_.bloom > 0);
    glUniform2f(loc_.stat.inset, geom_.ix, geom_.iy);
    glUniform1f(loc_.stat.rgbShift, rgbShiftPx(p_) / static_cast<float>(w_));
    drawTo(fboStatic_, w_, h_, p);
    if (p_.frame && p_.bloom > 0 && p_.frameShininess > 0) {
        const GLuint c = progFrameComp_;
        glUseProgram(c);
        bindTex(loc_.comp.baseTex, 0, texFrameBase_);
        bindTex(loc_.comp.reflTex, 1, texFrameRefl_);
        bindTex(loc_.comp.bloomSrc, 2, texBloomA_);
        glUniform3f(loc_.comp.fontColor, p_.fontColor.r, p_.fontColor.g, p_.fontColor.b);
        glUniform1f(loc_.comp.chroma, p_.chromaColor);
        glUniform1f(loc_.comp.bloomStrength, d_.bloomStrength);
        glUniform1f(loc_.comp.screenBrightness, d_.screenBrightness);
        glUniform1i(loc_.comp.useRefl, 1);
        drawTo(fboFrame_, w_, h_, c);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void GlPipeline::render(float timeMs, unsigned fbo, bool flipY) {
    if (!ok_ || !texSrc_) return;
    const FrameUniforms fu = frameUniforms(p_, timeMs, burnLast_);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    const GLuint p = progDyn_;
    glUseProgram(p);
    bindTex(loc_.dyn.staticTex, 0, texStatic_);
    bindTex(loc_.dyn.burnTex, 1, texBurn_[burnCur_]);
    bindTex(loc_.dyn.noiseTex, 2, texNoise_);
    auto u1 = [](GLint l, float v) { glUniform1f(l, v); };
    glUniform2f(loc_.dyn.size, static_cast<float>(w_), static_cast<float>(h_));
    u1(loc_.dyn.curvature, d_.curvature);
    glUniform3f(loc_.dyn.fontColor, p_.fontColor.r, p_.fontColor.g, p_.fontColor.b);
    u1(loc_.dyn.chroma, p_.chromaColor);
    u1(loc_.dyn.time, timeMs);
    u1(loc_.dyn.hsync, p_.horizontalSync);
    u1(loc_.dyn.hsyncScale, fu.hsyncScale);
    u1(loc_.dyn.hsyncFreq, fu.hsyncFreq);
    u1(loc_.dyn.jitter, p_.jitter);
    glUniform2f(loc_.dyn.jitterDisp, d_.jitterX, d_.jitterY);
    u1(loc_.dyn.staticNoise, p_.staticNoise);
    float noiseAmount = p_.staticNoise;
    if (p_.horizontalSync != 0.0f && p_.staticNoise != 0.0f) noiseAmount += fu.hsyncScale * 7.0f;
    u1(loc_.dyn.noiseAmount, noiseAmount);
    glUniform2f(loc_.dyn.noiseScale, (w_ / p_.pixelScale * 0.75f) / kNoiseSize,
                (h_ / p_.pixelScale * 0.75f) / kNoiseSize);
    glUniform2f(loc_.dyn.noiseOffset, fu.noiseOffsetX, fu.noiseOffsetY);
    u1(loc_.dyn.glowingLine, d_.glowingLine);
    u1(loc_.dyn.burnIn, p_.burnIn);
    u1(loc_.dyn.burnInDecay, fu.burnInDecay);
    glUniform1i(loc_.dyn.rasterization, p_.rasterization);
    u1(loc_.dyn.vrY, h_ / p_.scanlinePeriod);
    u1(loc_.dyn.rasterIntensity, p_.rasterizationIntensity);
    u1(loc_.dyn.flicker, fu.flickerBrightness);
    u1(loc_.dyn.ambient, d_.ambient);
    glUniform1i(loc_.dyn.flipY, flipY ? 1 : 0);
    glUniform2f(loc_.dyn.inset, geom_.ix, geom_.iy);
    glUniform1i(loc_.dyn.useFrame, p_.frame ? 1 : 0);
    bindTex(loc_.dyn.frameTex, 3, p_.frame ? texFrame_ : texStatic_);
    drawTo(fbo, w_, h_, p);
}

void GlPipeline::renderToMemory(float timeMs, uint8_t* out) {
    if (!ok_ || !texSrc_) return;
    if (!texOut_) {  // freed with the other sized objects by resize()/setParams()/releaseSized()
        texOut_ = makeTex(w_, h_, GL_RGBA8, false, false);
        fboOut_ = makeFbo(texOut_);
    }
    render(timeMs, fboOut_, false);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

int countGlTextures(unsigned maxName) {
    int n = 0;
    for (GLuint i = 1; i <= maxName; ++i)
        if (glIsTexture(i)) ++n;
    return n;
}

}  // namespace lterm
