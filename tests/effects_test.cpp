// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu_effects.hpp"
#include "synthetic_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

int main() {
    // Barrel warp: identity at the centre, pulls corners inwards (samples further out).
    float u, v;
    barrel(0.5f, 0.5f, 0.12f, u, v);
    CHECK(std::fabs(u - 0.5f) < 1e-6f && std::fabs(v - 0.5f) < 1e-6f);
    barrel(0.0f, 0.0f, 0.12f, u, v);
    CHECK(u < 0.0f && v < 0.0f);
    barrel(0.3f, 0.7f, 0.0f, u, v);
    CHECK(u == 0.3f && v == 0.7f);

    // Derived values follow CRT 1.2.0.
    DerivedParams d = derive(EffectParams{});
    CHECK(std::fabs(d.curvature - 0.12f) < 1e-6f);
    CHECK(std::fabs(d.screenBrightness - 1.0f) < 1e-6f);

    // Bloom weights are normalised.
    auto wts = bloomWeights(20);
    float sum = wts[0];
    for (size_t i = 1; i < wts.size(); ++i) sum += 2 * wts[i];
    CHECK(std::fabs(sum - 1.0f) < 1e-5f);

    // Thread count must not change the output.
    const int W = 320, H = 200;
    auto frame = renderSyntheticFrame(W, H, 0, "DejaVu Sans Mono", 10);
    std::vector<uint8_t> a(W * H * 4), b(W * H * 4);
    {
        CpuEffects e1(EffectParams{}, 1), e4(EffectParams{}, 4);
        e1.resize(W, H);
        e4.resize(W, H);
        e1.setSource(frame.data(), W * 4, 100.0f);
        e4.setSource(frame.data(), W * 4, 100.0f);
        e1.render(140.0f, a.data());
        e4.render(140.0f, b.data());
        CHECK(a == b);
    }

    // Burn-in fades out: after a long time a black screen has no ghost left.
    {
        EffectParams p;
        p.staticNoise = 0;
        p.glowingLine = 0;
        p.ambientLight = 0;
        p.flickering = 0;
        p.jitter = 0;
        p.horizontalSync = 0;
        std::vector<uint8_t> black(W * H * 4, 0);
        for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
        CpuEffects e(p, 2);
        e.resize(W, H);
        e.setSource(frame.data(), W * 4, 0.0f);
        e.setSource(black.data(), W * 4, 10.0f);
        e.render(20.0f, a.data());
        int ghost = 0;
        for (size_t i = 0; i < a.size(); i += 4) ghost = std::max(ghost, int(a[i]));
        CHECK(ghost > 20);  // shortly after: visible ghost
        e.render(5000.0f, a.data());
        ghost = 0;
        for (size_t i = 0; i < a.size(); i += 4) ghost = std::max(ghost, int(a[i]));
        CHECK(ghost <= 1);  // long after: gone
    }

    if (failures == 0) std::printf("effects_test: OK\n");
    // M2 effects quality.
    {
        EffectsQuality q;
        CHECK(parseEffectsQuality("static", q) && q == EffectsQuality::Static);
        CHECK(parseEffectsQuality("auto", q) && q == EffectsQuality::Auto);
        CHECK(!parseEffectsQuality("ultra", q));
        CHECK(isSoftwareRendererName("llvmpipe (LLVM 15.0.6, 256 bits)"));
        CHECK(isSoftwareRendererName("Mesa softpipe"));
        CHECK(isSoftwareRendererName("Software Rasterizer (SWRAST)"));
        CHECK(!isSoftwareRendererName("AMD Radeon RX 6600 (radeonsi, navi23, LLVM 15.0.6)"));
        CHECK(!isSoftwareRendererName("Mesa Intel(R) UHD Graphics 620 (KBL GT2)"));
        setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
        CHECK(softwareRenderingForced());
        setenv("LIBGL_ALWAYS_SOFTWARE", "0", 1);
        CHECK(!softwareRenderingForced());
        unsetenv("LIBGL_ALWAYS_SOFTWARE");
        CHECK(!softwareRenderingForced());
        CHECK(resolveEffectsQuality(EffectsQuality::Auto, true) == EffectsQuality::Reduced);
        CHECK(resolveEffectsQuality(EffectsQuality::Auto, false) == EffectsQuality::Full);
        CHECK(resolveEffectsQuality(EffectsQuality::Full, true) == EffectsQuality::Full);
        CHECK(resolveEffectsQuality(EffectsQuality::Static, false) == EffectsQuality::Static);
        EffectParams p;  // animated, with burn-in
        CHECK(sameEffectParams(applyEffectsQuality(p, EffectsQuality::Full), p));  // full = unchanged
        const EffectParams r = applyEffectsQuality(p, EffectsQuality::Reduced);
        CHECK(r.staticNoise == 0 && r.jitter == 0 && r.flickering == p.flickering && r.burnIn == p.burnIn);
        CHECK(r.bloomRadiusHalfRes == p.bloomRadiusHalfRes / 2 && r.animated());
        const EffectParams st = applyEffectsQuality(p, EffectsQuality::Static);
        CHECK(!st.animated() && st.burnIn == 0 && st.burnInFadeMs() == 0 && st.bloom == p.bloom);
        // Static output does not depend on time: one render per content change is exact.
        const int W = 160, H = 100;
        auto src = renderSyntheticFrame(W, H, 0, "DejaVu Sans Mono", 12);
        CpuEffects e(st, 2);
        e.resize(W, H);
        e.setSource(src.data(), W * 4, 1000.0f);
        std::vector<uint8_t> o1(W * H * 4), o2(W * H * 4);
        e.render(1000.0f, o1.data());
        e.render(98765.0f, o2.data());
        CHECK(o1 == o2);
        CpuEffects full(p, 2);
        full.resize(W, H);
        full.setSource(src.data(), W * 4, 1000.0f);
        full.render(1000.0f, o1.data());
        full.render(1517.0f, o2.data());
        CHECK(o1 != o2);  // control: full animates
    }

    // M2 HiDPI: the scale-2 picture (pixel-sized parameters scaled) is the scale-1 picture at
    // twice the resolution: 2x2-averaged, it is close to the scale-1 output. Without the
    // parameter scaling the scanlines and the bloom would be half as wide (control).
    {
        const EffectParams base = applyEffectsQuality(EffectParams{}, EffectsQuality::Static);  // scanlines on
        const EffectParams p2 = scaleEffectParams(base, 2);
        CHECK(p2.scanlinePeriod == 2 * base.scanlinePeriod && p2.pixelScale == 2.0f);
        CHECK(p2.bloomRadiusHalfRes == std::min(64, 2 * base.bloomRadiusHalfRes));
        CHECK(scaleEffectParams(EffectParams{}, 4).bloomRadiusHalfRes == 64);
        const int w = 320, h = 200;
        auto s1 = renderSyntheticFrame(w, h, 0, "DejaVu Sans Mono", 10);
        auto s2 = renderSyntheticFrame(2 * w, 2 * h, 0, "DejaVu Sans Mono", 10, nullptr, 2);
        auto run = [](const EffectParams& p, const std::vector<uint8_t>& src, int ww, int hh) {
            CpuEffects e(p, 2);
            e.resize(ww, hh);
            e.setSource(src.data(), ww * 4, 1000.0f);
            std::vector<uint8_t> o(static_cast<size_t>(ww) * hh * 4);
            e.render(1000.0f, o.data());
            return o;
        };
        const auto o1 = run(base, s1, w, h);
        auto psnrHalf = [&](const std::vector<uint8_t>& o2) {
            double sq = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c) {
                        int v = 0;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx) v += o2[((2 * y + dy) * 2 * w + 2 * x + dx) * 4 + c];
                        const double d = v / 4.0 - o1[(y * w + x) * 4 + c];
                        sq += d * d;
                    }
            const double mse = sq / (w * h * 3.0);
            return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
        };
        const double scaled = psnrHalf(run(p2, s2, 2 * w, 2 * h)), unscaled = psnrHalf(run(base, s2, 2 * w, 2 * h));
        std::printf("scale 2 (2x2 average) vs scale 1, static + scanlines: PSNR %.2f dB (unscaled parameters %.2f dB)\n",
                    scaled, unscaled);
        CHECK(scaled > 24.0);
        CHECK(scaled > unscaled + 1.0);
    }

    // M2 item 5: RGB shift and bezel.
    {
        // RGB shift off: frame-off output is unchanged by the new code paths (geometry is zero).
        EffectParams p;
        CHECK(frameGeometry(p, 640, 400).inset == 0.0f);
        p.frame = true;
        p.frameSize = 20;
        const FrameGeometry g = frameGeometry(p, 640, 400);
        CHECK(g.inset == 20.0f && g.innerW == 600.0f && g.radius == 12.0f);
        CHECK(screenSdf(g, 0.5f, 0.5f) < -190.0f);
        CHECK(std::fabs(screenSdf(g, 0.0f, 0.5f)) < 1e-3f);
        CHECK(screenSdf(g, 0.0f, 0.0f) > 4.0f);  // rounded corner: outside the glass
        // Static layer: opaque frame at the window edge, empty in the middle of the glass,
        // a glass highlight (additive, alpha 0) top left.
        std::vector<float> base(640 * 400 * 4), refl(base.size());
        frameLayerRows(p, 640, 400, 0, 400, base.data(), refl.data());
        auto at = [&](std::vector<float>& v, int x, int y) { return &v[(static_cast<size_t>(y) * 640 + x) * 4]; };
        CHECK(at(base, 2, 200)[3] == 1.0f);
        CHECK(at(base, 320, 300)[3] == 0.0f && at(base, 320, 300)[0] == 0.0f);
        CHECK(at(base, 200, 90)[3] == 0.0f && at(base, 200, 90)[0] > 0.01f);
        CHECK(at(refl, 15, 200)[0] > 0.05f);  // reflection weight next to the glass
        CHECK(at(refl, 320, 200)[0] == 0.0f);
        // Reflection: a bright column just inside the glass shows up mirrored on the bezel.
        EffectParams q = p;
        q.staticNoise = q.jitter = q.flickering = q.horizontalSync = q.glowingLine = q.burnIn = 0.0f;
        q.screenCurvature = 0.0f;
        std::vector<uint8_t> dark(640 * 400 * 4, 0), lit = dark, o1(dark.size()), o2(dark.size());
        for (int y = 100; y < 300; ++y)
            for (int x = 22; x < 40; ++x)
                for (int c = 0; c < 3; ++c) lit[(y * 640 + x) * 4 + c] = 255;
        for (auto* src : {&dark, &lit}) {
            CpuEffects e(q, 2);
            e.resize(640, 400);
            e.setSource(src->data(), 640 * 4, 0.0f);
            e.render(0.0f, src == &dark ? o1.data() : o2.data());
        }
        const int px = (200 * 640 + 14) * 4;
        std::printf("bezel pixel next to a lit column: %d -> %d\n", o1[px], o2[px]);
        CHECK(o2[px] > o1[px] + 3);
        // RGB shift: a white vertical line gets a red fringe on its left and a blue one on its right
        // (CRT's weights: left sample feeds blue, right sample feeds red).
        EffectParams r;
        r.staticNoise = r.jitter = r.flickering = r.horizontalSync = r.glowingLine = r.burnIn = r.bloom = 0.0f;
        r.screenCurvature = 0.0f;
        r.chromaColor = 1.0f;
        r.fontColor = Rgb{1, 1, 1};
        r.rasterization = 0;
        r.ambientLight = 0.0f;
        r.rgbShift = 0.3524f;  // 3.17 px
        std::vector<uint8_t> line(64 * 32 * 4, 0), out(line.size());
        for (int y = 0; y < 32; ++y)
            for (int c = 0; c < 3; ++c) line[(y * 64 + 32) * 4 + c] = 255;
        CpuEffects e(r, 1);
        e.resize(64, 32);
        e.setSource(line.data(), 64 * 4, 0.0f);
        e.render(0.0f, out.data());
        const uint8_t* lft = &out[(16 * 64 + 29) * 4];
        const uint8_t* rgt = &out[(16 * 64 + 35) * 4];
        std::printf("rgb shift fringes: left %d,%d,%d right %d,%d,%d\n", lft[0], lft[1], lft[2], rgt[0], rgt[1], rgt[2]);
        CHECK(lft[0] > lft[2] && rgt[2] > rgt[0]);
    }
    return failures ? 1 : 0;
}
