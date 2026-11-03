// SPDX-License-Identifier: GPL-3.0-or-later
// M6 item 6: hash the C++ effects output for several parameter sets (bit-exactness check).
#include "cpu_effects.hpp"
#include "synthetic_frame.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
using namespace lterm;
static uint64_t fnv(const std::vector<uint8_t>& v) { uint64_t h = 1469598103934665603ull; for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; } return h; }
int main() {
    const int W = 640, H = 400;
    auto f0 = renderSyntheticFrame(W, H, 0, "DejaVu Sans Mono", 12);
    auto f1 = renderSyntheticFrame(W, H, 3, "DejaVu Sans Mono", 12);
    for (int k = 0; k < 6; ++k) {
        EffectParams p;
        if (k == 1) p.frame = true;
        if (k == 2) p.rgbShift = 0.5f;
        if (k == 3) p.burnInHalfRes = false;
        if (k == 4) { p.frame = true; p.rgbShift = 0.3f; p.burnInHalfRes = false; p.chromaColor = 0.5f; }
        if (k == 5) { p.bloom = 0; p.burnIn = 0; }
        CpuEffects e(p, 2);
        e.resize(W, H);
        std::vector<uint8_t> out(W * H * 4);
        uint64_t h = 0;
        for (int t = 0; t < 4; ++t) {
            e.setSource((t & 1 ? f1 : f0).data(), W * 4, 100.0f + 40 * t);
            e.render(120.0f + 40 * t, out.data());
            h = h * 31 + fnv(out);
        }
        std::printf("set %d: %016llx\n", k, (unsigned long long)h);
    }
}
