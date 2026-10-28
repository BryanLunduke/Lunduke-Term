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
    return failures ? 1 : 0;
}
