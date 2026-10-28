// SPDX-License-Identifier: GPL-3.0-or-later
// CPU vs GL golden comparison at 640x400. Skips (77) when no EGL/GL 3.3 context is available.
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "synthetic_frame.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lterm;

int main() {
    const int W = 640, H = 400;
    auto a = renderSyntheticFrame(W, H, 0, "DejaVu Sans Mono", 14);
    auto b = renderSyntheticFrame(W, H, 3, "DejaVu Sans Mono", 14);
    GlEffects gl(EffectParams{});
    if (!gl.ok()) {
        std::printf("SKIP: %s\n", gl.error().c_str());
        return 77;
    }
    CpuEffects cpu(EffectParams{}, 2);
    std::vector<uint8_t> oc(W * H * 4), og(W * H * 4);
    for (EffectsPipeline* p : {static_cast<EffectsPipeline*>(&cpu), static_cast<EffectsPipeline*>(&gl)}) {
        p->resize(W, H);
        p->setSource(a.data(), W * 4, 1000.0f);
        p->setSource(b.data(), W * 4, 1100.0f);
        p->render(1180.0f, p == &cpu ? oc.data() : og.data());
    }
    double sq = 0;
    int mx = 0;
    for (size_t i = 0; i < oc.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            int d = std::abs(int(oc[i + c]) - int(og[i + c]));
            sq += double(d) * d;
            mx = std::max(mx, d);
        }
    const double mse = sq / (W * H * 3.0);
    const double psnr = mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
    std::printf("renderer %s: PSNR %.2f dB, max diff %d\n", gl.renderer().c_str(), psnr, mx);
    return psnr >= 40.0 ? 0 : 1;
}
