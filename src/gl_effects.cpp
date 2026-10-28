// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_effects.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace lterm {

GlEffects::GlEffects(const EffectParams& params) {
    ok_ = initEgl();
    if (!ok_) return;
    pipe_.reset(new GlPipeline(params));
    if (!pipe_->ok()) {
        error_ = pipe_->error();
        ok_ = false;
    }
}

GlEffects::~GlEffects() {
    if (dpy_) {
        if (ctx_) {
            eglMakeCurrent(static_cast<EGLDisplay>(dpy_), static_cast<EGLSurface>(surf_),
                           static_cast<EGLSurface>(surf_), static_cast<EGLContext>(ctx_));
            if (pipe_) pipe_->release();
            if (scratchFbo_) glDeleteFramebuffers(1, &scratchFbo_);
            if (scratchTex_) glDeleteTextures(1, &scratchTex_);
        }
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

std::string GlEffects::renderer() const { return ok_ ? pipe_->renderer() : "(none)"; }

void GlEffects::resize(int width, int height) {
    if (ok_) pipe_->resize(width, height);
}

void GlEffects::updateSource(const uint8_t* px, int stride, PixelOrder order, float timeMs, int rowBegin,
                             int rowEnd) {
    if (ok_) pipe_->updateSource(px, stride, order, timeMs, rowBegin, rowEnd);
}

void GlEffects::render(float timeMs, uint8_t* out) {
    if (ok_) pipe_->renderToMemory(timeMs, out);
}

void GlEffects::renderNoReadback(float timeMs) {
    if (!ok_) return;
    // Stand-in for the window framebuffer: an RGBA8 FBO of the same size, drawn with flipY.
    if (scratchW_ != pipe_->width() || scratchH_ != pipe_->height()) {
        if (scratchFbo_) glDeleteFramebuffers(1, &scratchFbo_);
        if (scratchTex_) glDeleteTextures(1, &scratchTex_);
        scratchW_ = pipe_->width();
        scratchH_ = pipe_->height();
        glGenTextures(1, &scratchTex_);
        glBindTexture(GL_TEXTURE_2D, scratchTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, scratchW_, scratchH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &scratchFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, scratchFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchTex_, 0);
    }
    pipe_->render(timeMs, scratchFbo_, true);
    glFinish();
}

}  // namespace lterm
