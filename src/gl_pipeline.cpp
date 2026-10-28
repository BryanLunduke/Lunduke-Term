// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_pipeline.hpp"
#include "gl_shaders.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <string>

namespace lterm {

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
    texNoise_ = makeTex(kNoiseSize, kNoiseSize, GL_RGBA32F, true, true);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kNoiseSize, kNoiseSize, GL_RGBA, GL_FLOAT, noiseTexture().data());
}

GlPipeline::~GlPipeline() = default;

void GlPipeline::release() {
    freeSized();
    if (texNoise_) {
        glDeleteTextures(1, &texNoise_);
        texNoise_ = 0;
        --liveTex_;
    }
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
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

unsigned GlPipeline::makeFbo(unsigned tex) {
    GLuint f;
    glGenFramebuffers(1, &f);
    ++liveFbo_;
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return f;
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
        if (*t) {
            glDeleteTextures(1, t);
            --liveTex_;
        }
        *t = 0;
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
    texSrc_ = makeTex(w_, h_, GL_RGBA32F, true, false);
    texDown_ = makeTex(hw_, hh_, GL_RGBA32F, true, false);
    texBloomA_ = makeTex(hw_, hh_, GL_RGBA32F, true, false);
    texBloomB_ = makeTex(hw_, hh_, GL_RGBA32F, true, false);
    for (int i = 0; i < 2; ++i) texBurn_[i] = makeTex(bw_, bh_, GL_RGBA32F, true, false);
    texStatic_ = makeTex(w_, h_, GL_RGBA32F, true, false);
    texOut_ = makeTex(w_, h_, GL_RGBA8, false, false);
    fboDown_ = makeFbo(texDown_);
    fboBloomA_ = makeFbo(texBloomA_);
    fboBloomB_ = makeFbo(texBloomB_);
    for (int i = 0; i < 2; ++i) fboBurn_[i] = makeFbo(texBurn_[i]);
    fboStatic_ = makeFbo(texStatic_);
    fboOut_ = makeFbo(texOut_);
    haveBurn_ = false;
    geom_ = frameGeometry(p_, w_, h_);
    if (p_.frame) {
        texFrameBase_ = makeTex(w_, h_, GL_RGBA32F, false, false);
        texFrameRefl_ = makeTex(w_, h_, GL_RGBA32F, false, false);
        texFrame_ = makeTex(w_, h_, GL_RGBA32F, false, false);
        fboFrameBase_ = makeFbo(texFrameBase_);
        fboFrameRefl_ = makeFbo(texFrameRefl_);
        fboFrame_ = makeFbo(texFrame_);
        // The static layer: once per size (the parameters are fixed for a pipeline).
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glDisable(GL_BLEND);
        const GLuint p = progFrame_;
        glUseProgram(p);
        glUniform2f(glGetUniformLocation(p, "size"), static_cast<float>(w_), static_cast<float>(h_));
        glUniform2f(glGetUniformLocation(p, "inset"), geom_.ix, geom_.iy);
        glUniform1f(glGetUniformLocation(p, "insetPx"), geom_.inset);
        glUniform2f(glGetUniformLocation(p, "inner"), geom_.innerW, geom_.innerH);
        glUniform1f(glGetUniformLocation(p, "radius"), geom_.radius);
        glUniform1f(glGetUniformLocation(p, "curvature"), d_.curvature);
        glUniform3f(glGetUniformLocation(p, "frameColor"), p_.frameColor.r, p_.frameColor.g, p_.frameColor.b);
        glUniform1f(glGetUniformLocation(p, "shininess"), p_.frameShininess);
        glUniform1f(glGetUniformLocation(p, "pixelScale"), p_.pixelScale);
        glUniform1i(glGetUniformLocation(p, "mode"), 0);
        drawTo(fboFrameBase_, w_, h_, p);
        glUniform1i(glGetUniformLocation(p, "mode"), 1);
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

static void bindTex(GLuint prog, const char* name, int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(prog, name), unit);
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
        // 8-bit rows straight into the float texture; GL normalises (c / 255) on upload.
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
        bindTex(progDown_, "src", 0, texSrc_);
        glUniform2i(glGetUniformLocation(progDown_, "srcSize"), w_, h_);
        drawTo(fboDown_, hw_, hh_, progDown_);
    }
    if (p_.bloom > 0) {
        glUseProgram(progBlur_);
        glUniform1fv(glGetUniformLocation(progBlur_, "weights"), 65, weights_.data());
        glUniform1i(glGetUniformLocation(progBlur_, "radius"), std::min(64, p_.bloomRadiusHalfRes));
        glUniform2i(glGetUniformLocation(progBlur_, "size"), hw_, hh_);
        bindTex(progBlur_, "src", 0, texDown_);
        glUniform2i(glGetUniformLocation(progBlur_, "dir"), 1, 0);
        drawTo(fboBloomB_, hw_, hh_, progBlur_);
        bindTex(progBlur_, "src", 0, texBloomB_);
        glUniform2i(glGetUniformLocation(progBlur_, "dir"), 0, 1);
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
        bindTex(progBurn_, "src", 0, p_.burnInHalfRes ? texDown_ : texSrc_);
        bindTex(progBurn_, "prev", 1, texBurn_[burnCur_]);
        glUniform1f(glGetUniformLocation(progBurn_, "decay0"), decay0);
        drawTo(fboBurn_[next], bw_, bh_, progBurn_);
        burnCur_ = next;
    }
    const GLuint p = progStatic_;
    glUseProgram(p);
    bindTex(p, "src", 0, texSrc_);
    bindTex(p, "bloomSrc", 1, texBloomA_);
    glUniform2f(glGetUniformLocation(p, "size"), static_cast<float>(w_), static_cast<float>(h_));
    glUniform1f(glGetUniformLocation(p, "curvature"), d_.curvature);
    glUniform3f(glGetUniformLocation(p, "fontColor"), p_.fontColor.r, p_.fontColor.g, p_.fontColor.b);
    glUniform3f(glGetUniformLocation(p, "backgroundColor"), p_.backgroundColor.r, p_.backgroundColor.g,
                p_.backgroundColor.b);
    glUniform1f(glGetUniformLocation(p, "chroma"), p_.chromaColor);
    glUniform1f(glGetUniformLocation(p, "bloomStrength"), d_.bloomStrength);
    glUniform1f(glGetUniformLocation(p, "screenBrightness"), d_.screenBrightness);
    glUniform1i(glGetUniformLocation(p, "useBloom"), p_.bloom > 0);
    glUniform2f(glGetUniformLocation(p, "inset"), geom_.ix, geom_.iy);
    glUniform1f(glGetUniformLocation(p, "rgbShift"), rgbShiftPx(p_) / static_cast<float>(w_));
    drawTo(fboStatic_, w_, h_, p);
    if (p_.frame && p_.bloom > 0 && p_.frameShininess > 0) {
        const GLuint c = progFrameComp_;
        glUseProgram(c);
        bindTex(c, "baseTex", 0, texFrameBase_);
        bindTex(c, "reflTex", 1, texFrameRefl_);
        bindTex(c, "bloomSrc", 2, texBloomA_);
        glUniform3f(glGetUniformLocation(c, "fontColor"), p_.fontColor.r, p_.fontColor.g, p_.fontColor.b);
        glUniform1f(glGetUniformLocation(c, "chroma"), p_.chromaColor);
        glUniform1f(glGetUniformLocation(c, "bloomStrength"), d_.bloomStrength);
        glUniform1f(glGetUniformLocation(c, "screenBrightness"), d_.screenBrightness);
        glUniform1i(glGetUniformLocation(c, "useRefl"), 1);
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
    bindTex(p, "staticTex", 0, texStatic_);
    bindTex(p, "burnTex", 1, texBurn_[burnCur_]);
    bindTex(p, "noiseTex", 2, texNoise_);
    auto u1 = [&](const char* n, float v) { glUniform1f(glGetUniformLocation(p, n), v); };
    glUniform2f(glGetUniformLocation(p, "size"), static_cast<float>(w_), static_cast<float>(h_));
    u1("curvature", d_.curvature);
    glUniform3f(glGetUniformLocation(p, "fontColor"), p_.fontColor.r, p_.fontColor.g, p_.fontColor.b);
    u1("chroma", p_.chromaColor);
    u1("time", timeMs);
    u1("hsync", p_.horizontalSync);
    u1("hsyncScale", fu.hsyncScale);
    u1("hsyncFreq", fu.hsyncFreq);
    u1("jitter", p_.jitter);
    glUniform2f(glGetUniformLocation(p, "jitterDisp"), d_.jitterX, d_.jitterY);
    u1("staticNoise", p_.staticNoise);
    float noiseAmount = p_.staticNoise;
    if (p_.horizontalSync != 0.0f && p_.staticNoise != 0.0f) noiseAmount += fu.hsyncScale * 7.0f;
    u1("noiseAmount", noiseAmount);
    glUniform2f(glGetUniformLocation(p, "noiseScale"), (w_ / p_.pixelScale * 0.75f) / kNoiseSize,
                (h_ / p_.pixelScale * 0.75f) / kNoiseSize);
    glUniform2f(glGetUniformLocation(p, "noiseOffset"), fu.noiseOffsetX, fu.noiseOffsetY);
    u1("glowingLine", d_.glowingLine);
    u1("burnIn", p_.burnIn);
    u1("burnInDecay", fu.burnInDecay);
    glUniform1i(glGetUniformLocation(p, "rasterization"), p_.rasterization);
    u1("vrY", h_ / p_.scanlinePeriod);
    u1("rasterIntensity", p_.rasterizationIntensity);
    u1("flicker", fu.flickerBrightness);
    u1("ambient", d_.ambient);
    glUniform1i(glGetUniformLocation(p, "flipY"), flipY ? 1 : 0);
    glUniform2f(glGetUniformLocation(p, "inset"), geom_.ix, geom_.iy);
    glUniform1i(glGetUniformLocation(p, "useFrame"), p_.frame ? 1 : 0);
    bindTex(p, "frameTex", 3, p_.frame ? texFrame_ : texStatic_);
    drawTo(fbo, w_, h_, p);
}

void GlPipeline::renderToMemory(float timeMs, uint8_t* out) {
    if (!ok_ || !texSrc_) return;
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
