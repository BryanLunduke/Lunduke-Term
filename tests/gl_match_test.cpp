// SPDX-License-Identifier: GPL-3.0-or-later
// CPU vs GL golden comparison at 640x400 (M1 half-res burn-in and the M0 full-res mode), then
// a resize loop that must not leak textures/FBOs and must still match afterwards.
// Skips (77) when no EGL/GL 3.3 context is available.
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "gl_pipeline.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"
#include "synthetic_frame.hpp"
#include "vterm_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
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
// M4 item 5: a frame of the Unicode sample (CJK, combining marks, emoji, RTL, invalid UTF-8) as
// the view paints it: VtermSession -> Grid -> GridPainter::paintRows, at the given scale.
std::vector<uint8_t> unicodeFrame(const std::string& text, int w, int h, int scale, int shift) {
    GridPainter painter("DejaVu Sans Mono", 14, scale);
    VtermSession s((h - 2 * kMargin * scale) / painter.cellHeight(), (w - 2 * kMargin * scale) / painter.cellWidth());
    s.feed(text.data(), text.size());
    for (int i = 0; i < shift; ++i) s.feed("x", 1);
    Grid g;
    s.fillGrid(g);
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(surf);
    painter.paintRows(cr, g, kMargin * scale, kMargin * scale, Color8{200, 200, 200}, Color8{0, 0, 0}, nullptr, w);
    cairo_destroy(cr);
    std::vector<uint8_t> out;
    surfaceToRgba(surf, out);
    cairo_surface_destroy(surf);
    return out;
}
}  // namespace

int main(int argc, char** argv) {
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
                                {"amber bezel+rgb", "amber", true, 0.3524f},
                                // M2 stage 3 profiles (vintage: scanlines + rgb shift + bezel)
                                {"vintage bezel", "vintage", true, -1.0f},
                                {"futuristic", "futuristic", false, -1.0f},
                                {"paper-white bezel", "paper-white", true, -1.0f}};
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

    // M4 item 5: wide (CJK, emoji), combining and RTL text through both back-ends. The text is
    // rasterised once on the CPU (the glyph cache, colour emoji included) and the effects run on
    // each back-end, so the wide glyphs cover the same 2 cells in both.
    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.empty()) {
            std::printf("FAIL: cannot read %s\n", argv[1]);
            ++fails;
        }
        for (int scale : {1, 2}) {
            const auto ua = unicodeFrame(text, scale * W, scale * H, scale, 0);
            const auto ub = unicodeFrame(text, scale * W, scale * H, scale, 3);
            for (const char* prof : {"amber", "green-scanlines", "vintage"}) {
                const Profile* pr = findProfile(prof);
                if (!pr) continue;
                for (EffectsQuality q : {EffectsQuality::Full, EffectsQuality::Static}) {
                    const EffectParams p = scaleEffectParams(applyEffectsQuality(pr->fx, q), scale);
                    GlEffects gq(p);
                    const double ps = compare(gq, p, ua, ub, &mx, scale * W, scale * H);
                    std::printf("unicode sample %s %s scale %d: PSNR %.2f dB, max diff %d\n", prof,
                                effectsQualityName(q), scale, ps, mx);
                    if (ps < 40.0) ++fails;
                }
            }
        }
    }

    // M5 item 1c: GlPipeline::setParams() (a governor probe switching quality) on one pipeline
    // must match the C++ back-end at every level, compile no program and leak no texture/FBO.
    {
        const Profile* pr = findProfile("amber");
        EffectParams bezel = pr ? pr->fx : m1;
        bezel.frame = true;
        GlEffects gs(applyEffectsQuality(bezel, EffectsQuality::Static));
        gs.resize(W, H);
        gs.renderNoReadback(0.0f);
        compare(gs, applyEffectsQuality(bezel, EffectsQuality::Static), a, b, &mx);  // readback: output FBO exists
        const long prog0 = GlPipeline::programsCompiled();
        const int t0 = gs.pipeline()->liveTextures(), f0 = gs.pipeline()->liveFramebuffers();
        double worst = 99.0;
        const EffectsQuality cycle[] = {EffectsQuality::Reduced, EffectsQuality::Static, EffectsQuality::Full,
                                        EffectsQuality::Static, EffectsQuality::Reduced, EffectsQuality::Static};
        for (EffectsQuality q : cycle) {
            const EffectParams p = applyEffectsQuality(bezel, q);
            gs.pipeline()->setParams(p);
            const double ps = compare(gs, p, a, b, &mx);
            std::printf("setParams -> amber bezel %s: PSNR %.2f dB, max diff %d\n", effectsQualityName(q), ps, mx);
            worst = std::min(worst, ps);
        }
        const long progs = GlPipeline::programsCompiled() - prog0;
        const int t1 = gs.pipeline()->liveTextures(), f1 = gs.pipeline()->liveFramebuffers();
        std::printf("setParams x6: programs compiled %ld, textures %d -> %d, fbos %d -> %d, worst PSNR %.2f dB\n",
                    progs, t0, t1, f0, f1, worst);
        if (progs != 0 || t1 != t0 || f1 != f0 || worst < 40.0) ++fails;
    }

    // Perf step 1: the RGBA8 output texture/FBO exist only once renderToMemory() reads back; a
    // window-style tick (no readback) must not allocate them, and a resize frees them again.
    {
        GlEffects gw(m1);
        gw.resize(W, H);
        gw.renderNoReadback(0.0f);
        const int t0 = gw.pipeline()->liveTextures(), f0 = gw.pipeline()->liveFramebuffers();
        const size_t b0 = gw.pipeline()->liveTextureBytes();
        gw.renderNoReadback(1.0f);
        const bool noneYet = gw.pipeline()->liveTextures() == t0 && gw.pipeline()->liveTextureBytes() == b0;
        std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
        gw.render(2.0f, out.data());
        const int t1 = gw.pipeline()->liveTextures(), f1 = gw.pipeline()->liveFramebuffers();
        const size_t b1 = gw.pipeline()->liveTextureBytes();
        gw.render(3.0f, out.data());  // the second readback reuses them
        const bool reused = gw.pipeline()->liveTextures() == t1 && gw.pipeline()->liveTextureBytes() == b1;
        gw.resize(W / 2, H / 2);
        gw.renderNoReadback(4.0f);
        const int t2 = gw.pipeline()->liveTextures();
        std::printf("lazy output texture: no readback %d textures / %d fbos / %zu B; after readback %d / %d / %zu B "
                    "(+%zu B); after resize without readback %d textures\n",
                    t0, f0, b0, t1, f1, b1, b1 - b0, t2);
        if (!noneYet || !reused || t1 != t0 + 1 || f1 != f0 + 1 || b1 - b0 != static_cast<size_t>(W) * H * 4 ||
            t2 != t0)
            ++fails;
    }

    // Resize loop: object counts must stay constant and the output must still match.
    EffectParams m1f = m1;  // with the bezel: its three extra textures must not leak either
    m1f.frame = true;
    m1f.rgbShift = 0.3f;
    const long look0 = GlPipeline::uniformLookups();
    GlEffects gl(m1f);
    const long lookCtor = GlPipeline::uniformLookups() - look0;
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
    // Perf step 1: uniform locations are looked up once per pipeline (constructor); 50 resizes
    // (bezel redrawn each time), content updates and ticks with readback add none.
    const long lookFrames = GlPipeline::uniformLookups() - look0 - lookCtor;
    std::printf("uniform lookups: %ld in the constructor, %ld during 50 resizes + frames + readback\n", lookCtor,
                lookFrames);
    if (lookCtor <= 0 || lookFrames != 0) ++fails;
    return fails ? 1 : 0;
}
