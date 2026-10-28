// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_effects.hpp"
#include "gl_shaders.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace lterm {

GlEffects::GlEffects(const EffectParams& params) : p_(params), d_(derive(params)) {
    ok_ = initEgl();
    if (!ok_) return;
    glGenVertexArrays(1, &vao_);
    progDown_ = compile(glsl::kDownsample);
    progBlur_ = compile(glsl::kBlur);
    progBurn_ = compile(glsl::kBurnIn);
    progStatic_ = compile(glsl::kStatic);
    progDyn_ = compile(glsl::kDynamic);
    if (!progDown_ || !progBlur_ || !progBurn_ || !progStatic_ || !progDyn_) {
        ok_ = false;
        return;
    }
    texNoise_ = makeTex(kNoiseSize, kNoiseSize, GL_RGBA32F, true, true);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kNoiseSize, kNoiseSize, GL_RGBA, GL_FLOAT,
                    noiseTexture().data());
}

GlEffects::~GlEffects() {
    if (dpy_) {
        eglMakeCurrent(static_cast<EGLDisplay>(dpy_), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx_) eglDestroyContext(static_cast<EGLDisplay>(dpy_), static_cast<EGLContext>(ctx_));
        if (surf_) eglDestroySurface(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(surf_));
        eglTerminate(static_cast<EGLDisplay>(dpy_));
    }
}

bool GlEffects::initEgl() {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    bool surfaceless = false;
    if (epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless") &&
        epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_platform_base")) {
        dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        surfaceless = dpy != EGL_NO_DISPLAY;
    }
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        error_ = "eglInitialize failed";
        return false;
    }
    dpy_ = dpy;
    eglBindAPI(EGL_OPENGL_API);
    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, surfaceless ? 0 : EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1) {
        error_ = "eglChooseConfig found no OpenGL config";
        return false;
    }
    const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                              EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) {
        error_ = "eglCreateContext (GL 3.3 core) failed";
        return false;
    }
    ctx_ = ctx;
    EGLSurface surf = EGL_NO_SURFACE;
    if (!surfaceless) {
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surf = eglCreatePbufferSurface(dpy, cfg, pb);
        surf_ = surf;
    }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        error_ = "eglMakeCurrent failed";
        return false;
    }
    return true;
}

std::string GlEffects::renderer() const {
    if (!ok_) return "(none)";
    return std::string(reinterpret_cast<const char*>(glGetString(GL_RENDERER))) + " / " +
           reinterpret_cast<const char*>(glGetString(GL_VERSION));
}

unsigned GlEffects::compile(const char* fsBody) {
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
    if (!vs || !fs) return 0;
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
        return 0;
    }
    return p;
}

unsigned GlEffects::makeTex(int w, int h, unsigned fmt, bool linear, bool repeat) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GL_RGBA, fmt == GL_RGBA8 ? GL_UNSIGNED_BYTE : GL_FLOAT,
                 nullptr);
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

static GLuint makeFbo(GLuint tex) {
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return f;
}

void GlEffects::resize(int width, int height) {
    if (!ok_) return;
    w_ = width;
    h_ = height;
    hw_ = std::max(1, w_ / 2);
    hh_ = std::max(1, h_ / 2);
    // (M0: textures are recreated without freeing the old ones; fine for a one-shot tool.)
    texSrc_ = makeTex(w_, h_, GL_RGBA32F, true, false);
    texBloomA_ = makeTex(hw_, hh_, GL_RGBA32F, true, false);
    texBloomB_ = makeTex(hw_, hh_, GL_RGBA32F, true, false);
    for (int i = 0; i < 2; ++i) texBurn_[i] = makeTex(w_, h_, GL_RGBA32F, true, false);
    texStatic_ = makeTex(w_, h_, GL_RGBA32F, true, false);
    texOut_ = makeTex(w_, h_, GL_RGBA8, false, false);
    fboBloomA_ = makeFbo(texBloomA_);
    fboBloomB_ = makeFbo(texBloomB_);
    for (int i = 0; i < 2; ++i) fboBurn_[i] = makeFbo(texBurn_[i]);
    fboStatic_ = makeFbo(texStatic_);
    fboOut_ = makeFbo(texOut_);
    upload_.assign(static_cast<size_t>(w_) * h_ * 4, 0.0f);
    haveBurn_ = false;
}

void GlEffects::drawTo(unsigned fbo, int w, int h, unsigned prog) {
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

void GlEffects::setSource(const uint8_t* rgba, int stride, float timeMs) {
    if (!ok_) return;
    for (int y = 0; y < h_; ++y) {
        const uint8_t* s = rgba + static_cast<size_t>(y) * stride;
        float* d = &upload_[static_cast<size_t>(y) * w_ * 4];
        for (int i = 0; i < w_ * 4; ++i) d[i] = s[i] * (1.0f / 255.0f);
    }
    glBindTexture(GL_TEXTURE_2D, texSrc_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_FLOAT, upload_.data());

    if (p_.bloom > 0) {
        glUseProgram(progDown_);
        bindTex(progDown_, "src", 0, texSrc_);
        glUniform2i(glGetUniformLocation(progDown_, "srcSize"), w_, h_);
        drawTo(fboBloomA_, hw_, hh_, progDown_);
        std::vector<float> wts = bloomWeights(p_.bloomRadiusHalfRes);
        wts.resize(65, 0.0f);
        glUseProgram(progBlur_);
        glUniform1fv(glGetUniformLocation(progBlur_, "weights"), 65, wts.data());
        glUniform1i(glGetUniformLocation(progBlur_, "radius"), std::min(64, p_.bloomRadiusHalfRes));
        glUniform2i(glGetUniformLocation(progBlur_, "size"), hw_, hh_);
        bindTex(progBlur_, "src", 0, texBloomA_);
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
        float decay0 = std::clamp((burnLast_ - burnPrev_) * d_.burnInTime, 0.0f, 1.0f);
        int next = 1 - burnCur_;
        glUseProgram(progBurn_);
        bindTex(progBurn_, "src", 0, texSrc_);
        bindTex(progBurn_, "prev", 1, texBurn_[burnCur_]);
        glUniform1f(glGetUniformLocation(progBurn_, "decay0"), decay0);
        drawTo(fboBurn_[next], w_, h_, progBurn_);
        burnCur_ = next;
    }

    GLuint p = progStatic_;
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
    drawTo(fboStatic_, w_, h_, p);
}

void GlEffects::render(float timeMs, uint8_t* out) {
    if (!ok_) return;
    const FrameUniforms fu = frameUniforms(p_, timeMs, burnLast_);
    GLuint p = progDyn_;
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
    glUniform2f(glGetUniformLocation(p, "noiseScale"), (w_ * 0.75f) / kNoiseSize, (h_ * 0.75f) / kNoiseSize);
    glUniform2f(glGetUniformLocation(p, "noiseOffset"), fu.noiseOffsetX, fu.noiseOffsetY);
    u1("glowingLine", d_.glowingLine);
    u1("burnIn", p_.burnIn);
    u1("burnInDecay", fu.burnInDecay);
    glUniform1i(glGetUniformLocation(p, "rasterization"), p_.rasterization);
    u1("vrY", h_ / p_.scanlinePeriod);
    u1("rasterIntensity", p_.rasterizationIntensity);
    u1("flicker", fu.flickerBrightness);
    u1("ambient", d_.ambient);
    drawTo(fboOut_, w_, h_, p);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

}  // namespace lterm
