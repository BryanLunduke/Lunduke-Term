// SPDX-License-Identifier: GPL-3.0-or-later
// CPU vs GL golden comparison at 640x400 (M1 half-res burn-in and the M0 full-res mode), then
// a resize loop that must not leak textures/FBOs and must still match afterwards.
// Skips (77) when no EGL/GL 3.3 context is available.
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "profiles.hpp"
#include "synthetic_frame.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lterm;

namespace {
const int W = 640, H = 400;

double compare(GlEffects& gl, const EffectParams& p, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               int* maxd, int w = W, int h = H) {
    CpuEffects cpu(p, 2);
    std::vector<uint8_t> oc(w * h * 4), og(w * h * 4);
    for (EffectsPipeline* e : {static_cast<EffectsPipeline*>(&cpu), static_cast<EffectsPipeline*>(&gl)}) {
        e->resize(w, h);
        e->setSource(a.data(), w * 4, 1000.0f);
        e->setSource(b.data(), w * 4, 1100.0f);
        e->render(1180.0f, e == &cpu ? oc.data() : og.data());
    }
    double sq = 0;
    int mx = 0;
    for (size_t i = 0; i < oc.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            int d = std::abs(int(oc[i + c]) - int(og[i + c]));
            sq += double(d) * d;
            mx = std::max(mx, d);
        }
    *maxd = mx;
    const double mse = sq / (w * h * 3.0);
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}
}  // namespace

int main() {
    auto a = renderSyntheticFrame(W, H, 0, "DejaVu Sans Mono", 14);
    auto b = renderSyntheticFrame(W, H, 3, "DejaVu Sans Mono", 14);
    int fails = 0, mx = 0;
    EffectParams m1;
    EffectParams m0;
    m0.burnInHalfRes = false;
    for (const EffectParams* p : {&m1, &m0}) {
        GlEffects gl(*p);
        if (!gl.ok()) {
            std::printf("SKIP: %s\n", gl.error().c_str());
            return 77;
        }
        const double ps = compare(gl, *p, a, b, &mx);
        std::printf("%s burn-in, renderer %s: PSNR %.2f dB, max diff %d\n", p->burnInHalfRes ? "half-res" : "full-res",
                    gl.renderer().c_str(), ps, mx);
        if (ps < 40.0) ++fails;
    }

    // M2: every effects-quality level of the amber profile (and of the default parameters,
    // which have scanlines) must match between the GL and C++ back-ends too.
    for (const char* prof : {"amber", "green-scanlines"}) {
        const Profile* pr = findProfile(prof);
        if (!pr) continue;
        for (EffectsQuality q : {EffectsQuality::Full, EffectsQuality::Reduced, EffectsQuality::Static}) {
            const EffectParams p = applyEffectsQuality(pr->fx, q);
            GlEffects gq(p);
            const double ps = compare(gq, p, a, b, &mx);
            std::printf("%s %s: PSNR %.2f dB, max diff %d\n", prof, effectsQualityName(q), ps, mx);
            if (ps < 40.0) ++fails;
        }
    }

    // M2 HiDPI: scale 2 (1280x800 device pixels, the 640x400 logical frame with the font
    // rasterised at 28 px) with the pixel-sized parameters scaled, for every quality level.
    for (const char* prof : {"amber", "green-scanlines"}) {
        const Profile* pr = findProfile(prof);
        if (!pr) continue;
        const auto a2 = renderSyntheticFrame(2 * W, 2 * H, 0, "DejaVu Sans Mono", 14, nullptr, 2);
        const auto b2 = renderSyntheticFrame(2 * W, 2 * H, 3, "DejaVu Sans Mono", 14, nullptr, 2);
        for (EffectsQuality q : {EffectsQuality::Full, EffectsQuality::Reduced, EffectsQuality::Static}) {
            const EffectParams p = scaleEffectParams(applyEffectsQuality(pr->fx, q), 2);
            GlEffects gq(p);
            const double ps = compare(gq, p, a2, b2, &mx, 2 * W, 2 * H);
            std::printf("%s %s scale 2: PSNR %.2f dB, max diff %d\n", prof, effectsQualityName(q), ps, mx);
            if (ps < 40.0) ++fails;
        }
    }

    // M2 item 5: RGB shift and the bezel (static layer, reflection, composite) at both scales and
    // every quality level. "amber+bezel+rgb" has both at once.
    struct Variant { const char* name; const char* prof; bool frame; float rgb; };
    const Variant variants[] = {{"ibm-dos rgb", "ibm-dos", false, -1.0f},
                                {"amber bezel", "amber", true, -1.0f},
                                {"green-scanlines bezel", "green-scanlines", true, -1.0f},
                                {"amber bezel+rgb", "amber", true, 0.3524f}};
    for (int scale : {1, 2}) {
        const auto a2 = renderSyntheticFrame(scale * W, scale * H, 0, "DejaVu Sans Mono", 14, nullptr, scale);
        const auto b2 = renderSyntheticFrame(scale * W, scale * H, 3, "DejaVu Sans Mono", 14, nullptr, scale);
        for (const Variant& v : variants) {
            const Profile* pr = findProfile(v.prof);
            if (!pr) continue;
            EffectParams base = pr->fx;
            base.frame = v.frame;
            if (v.rgb >= 0) base.rgbShift = v.rgb;
            for (EffectsQuality q : {EffectsQuality::Full, EffectsQuality::Reduced, EffectsQuality::Static}) {
                const EffectParams p = scaleEffectParams(applyEffectsQuality(base, q), scale);
                GlEffects gq(p);
                const double ps = compare(gq, p, a2, b2, &mx, scale * W, scale * H);
                std::printf("%s %s scale %d: PSNR %.2f dB, max diff %d\n", v.name, effectsQualityName(q), scale, ps, mx);
                if (ps < 40.0) ++fails;
            }
        }
    }

    // Resize loop: object counts must stay constant and the output must still match.
    EffectParams m1f = m1;  // with the bezel: its three extra textures must not leak either
    m1f.frame = true;
    m1f.rgbShift = 0.3f;
    GlEffects gl(m1f);
    gl.resize(W, H);
    gl.renderNoReadback(0.0f);
    const int tex0 = gl.pipeline()->liveTextures(), fbo0 = gl.pipeline()->liveFramebuffers();
    const int gltex0 = countGlTextures();
    const int sizes[][2] = {{320, 200}, {1024, 640}, {641, 401}, {800, 600}, {640, 400}};
    for (int i = 0; i < 50; ++i) {
        gl.resize(sizes[i % 5][0], sizes[i % 5][1]);
        gl.renderNoReadback(float(i));
    }
    const int tex1 = gl.pipeline()->liveTextures(), fbo1 = gl.pipeline()->liveFramebuffers();
    const int gltex1 = countGlTextures();
    const double ps = compare(gl, m1f, a, b, &mx);
    std::printf("after 50 resizes (bezel on): textures %d -> %d, fbos %d -> %d, glIsTexture %d -> %d, PSNR %.2f dB, max diff %d\n",
                tex0, tex1, fbo0, fbo1, gltex0, gltex1, ps, mx);
    if (tex1 != tex0 || fbo1 != fbo0 || gltex1 != gltex0 || ps < 40.0) ++fails;
    return fails ? 1 : 0;
}
