// SPDX-License-Identifier: GPL-3.0-or-later
// CPU vs GL golden comparison at 640x400 (M1 half-res burn-in and the M0 full-res mode), then
// a resize loop that must not leak textures/FBOs and must still match afterwards.
// Skips (77) when no EGL/GL 3.3 context is available.
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "synthetic_frame.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lterm;

namespace {
const int W = 640, H = 400;

double compare(GlEffects& gl, const EffectParams& p, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               int* maxd) {
    CpuEffects cpu(p, 2);
    std::vector<uint8_t> oc(W * H * 4), og(W * H * 4);
    for (EffectsPipeline* e : {static_cast<EffectsPipeline*>(&cpu), static_cast<EffectsPipeline*>(&gl)}) {
        e->resize(W, H);
        e->setSource(a.data(), W * 4, 1000.0f);
        e->setSource(b.data(), W * 4, 1100.0f);
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
    const double mse = sq / (W * H * 3.0);
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

    // Resize loop: object counts must stay constant and the output must still match.
    GlEffects gl(m1);
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
    const double ps = compare(gl, m1, a, b, &mx);
    std::printf("after 50 resizes: textures %d -> %d, fbos %d -> %d, glIsTexture %d -> %d, PSNR %.2f dB, max diff %d\n",
                tex0, tex1, fbo0, fbo1, gltex0, gltex1, ps, mx);
    if (tex1 != tex0 || fbo1 != fbo0 || gltex1 != gltex0 || ps < 40.0) ++fails;
    return fails ? 1 : 0;
}
