// SPDX-License-Identifier: GPL-3.0-or-later
// Pins the C++ effects output (perf step 3, PR #11 review N5). Renders a few odd sizes with every
// effect on (bezel, RGB shift, scanlines, burn-in at full and half resolution, curvature, bloom,
// noise, jitter, sync, flicker, glowing line) from a font-free synthetic source: a full update, a
// partial BGRA row update, then two ticks (RGBA and BGRA). Each case's 64-bit FNV-1a over all
// output bytes must match the hash recorded from master 6283674's C++ output (before the perf
// step 3 buffer changes), so any drift from that output fails.
// The hashes hold for x86-64 builds without FMA contraction; elsewhere the test prints the hashes
// and reports a skip (77). `cpu_hash_test --print` prints the table for re-recording.
#include "cpu_effects.hpp"
#include "effects.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace lterm;

namespace {

uint64_t fnv(uint64_t h, const std::vector<uint8_t>& v) {
    for (uint8_t b : v) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

std::vector<uint8_t> pattern(int w, int h, int seed) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            // Text-like: mostly black, 6x10 cells of which about a third are lit with a glyph-ish
            // sub-pattern, so bloom and the burn-in ghost both show.
            const unsigned cell = (static_cast<unsigned>(x / 6) * 73856093u) ^ (static_cast<unsigned>(y / 10) * 19349663u) ^
                                  (static_cast<unsigned>(seed) * 83492791u);
            const bool lit = cell % 3 == 0 && (x % 6) != 5 && (y % 10) < 8 && ((x + y + seed) % 3 != 0);
            for (int c = 0; c < 3; ++c) p[c] = lit ? static_cast<uint8_t>(200 + ((cell >> (c * 3)) & 55)) : static_cast<uint8_t>(c == 2 ? 8 : 0);
            p[3] = 255;
        }
    return px;
}

EffectParams allOn(bool halfResBurn) {
    EffectParams p;  // the amber defaults: every effect non-zero
    p.rasterization = 1;
    p.rgbShift = 0.35f;
    p.frame = true;
    p.frameSize = 6.0f;  // keeps some glass even at 7x9
    p.burnIn = 0.6f;
    p.burnInHalfRes = halfResBurn;
    return p;
}

uint64_t runCase(int w, int h, bool halfResBurn) {
    CpuEffects fx(allOn(halfResBurn), 3);
    fx.resize(w, h);
    uint64_t hash = 1469598103934665603ull;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    const std::vector<uint8_t> a = pattern(w, h, 1), b = pattern(w, h, 2);
    fx.updateSource(a.data(), w * 4, PixelOrder::RGBA, 1000.0f);
    fx.renderTo(1050.0f, out.data(), w * 4, PixelOrder::RGBA);
    hash = fnv(hash, out);
    // Rows [h/3, 2h/3] change, given in BGRA order.
    fx.updateSource(b.data(), w * 4, PixelOrder::BGRA, 1100.0f, h / 3, h / 3 + (h + 2) / 3);
    fx.renderTo(1180.0f, out.data(), w * 4, PixelOrder::RGBA);
    hash = fnv(hash, out);
    fx.renderTo(2500.0f, out.data(), w * 4, PixelOrder::BGRA);  // late tick: burn-in decay
    return fnv(hash, out);
}

struct Case {
    int w, h;
    bool halfResBurn;
    uint64_t expect;
};

// Recorded from master 6283674 (C++ path, release, x86-64 without FMA).
Case kCases[] = {
    {1, 1, true, 0x05fc69acbeadd64cull},
    {1, 1, false, 0x05fc69acbeadd64cull},
    {2, 3, true, 0xce6f45f7b93ad495ull},
    {2, 3, false, 0xce6f45f7b93ad495ull},
    {7, 9, true, 0x4788a2da13c5f471ull},
    {7, 9, false, 0x4788a2da13c5f471ull},
    {33, 17, true, 0xb884ee107a1c0ba4ull},
    {33, 17, false, 0x9bf855cdcb91e681ull},
    {641, 399, true, 0x5690a5f0cb5e868aull},
    {641, 399, false, 0x507480fb494ac744ull},
    {1279, 801, true, 0x0997b17c7ca1e591ull},
    {1279, 801, false, 0x5dc6f445c4b43453ull},
};

}  // namespace

int main(int argc, char** argv) {
    const bool print = argc > 1 && std::strcmp(argv[1], "--print") == 0;
#if defined(__x86_64__) && !defined(__FMA__)
    const bool pinned = true;
#else
    const bool pinned = false;
#endif
    int fails = 0;
    for (Case& c : kCases) {
        const uint64_t h = runCase(c.w, c.h, c.halfResBurn);
        if (print) {
            std::printf("    {%d, %d, %s, 0x%016" PRIx64 "ull},\n", c.w, c.h, c.halfResBurn ? "true" : "false", h);
            continue;
        }
        const bool ok = h == c.expect;
        std::printf("%s %dx%d burn-in %s: %016" PRIx64 "%s\n", ok ? "ok  " : "FAIL", c.w, c.h,
                    c.halfResBurn ? "half-res" : "full-res", h, ok ? "" : " (master differs)");
        if (!ok) ++fails;
    }
    if (print) return 0;
    if (!pinned) {
        std::printf("hashes are pinned for x86-64 without FMA only: skipped\n");
        return 77;
    }
    if (fails) std::printf("%d case(s) differ from master's C++ output\n", fails);
    return fails ? 1 : 0;
}
