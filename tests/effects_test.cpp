// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu_effects.hpp"
#include "synthetic_frame.hpp"

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

    return failures ? 1 : 0;
}
