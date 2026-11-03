// perf3 scratch: render the gl-match configurations on CPU and GL, write raw RGBA per config.
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "profiles.hpp"
#include "synthetic_frame.hpp"
#include <cstdio>
#include <string>
#include <vector>
using namespace lterm;
static void dump(const std::string& f, const std::vector<uint8_t>& v) { FILE* o = std::fopen(f.c_str(), "wb"); std::fwrite(v.data(), 1, v.size(), o); std::fclose(o); }
int main(int argc, char** argv) {
    std::string out = argv[1];
    struct V { const char* name; const char* prof; bool frame; float rgb; };
    const V vs[] = {{"amber", "amber", false, -1}, {"green-scanlines", "green-scanlines", false, -1},
                    {"ibm-dos-rgb", "ibm-dos", false, -1}, {"amber-bezel", "amber", true, -1},
                    {"amber-bezel-rgb", "amber", true, 0.3524f}, {"vintage-bezel", "vintage", true, -1},
                    {"futuristic", "futuristic", false, -1}, {"paper-white-bezel", "paper-white", true, -1}};
    for (int scale : {1, 2}) {
        const int W = 640 * scale, H = 400 * scale;
        auto a = renderSyntheticFrame(W, H, 0, "DejaVu Sans Mono", 14, nullptr, scale);
        auto b = renderSyntheticFrame(W, H, 3, "DejaVu Sans Mono", 14, nullptr, scale);
        if (scale == 2) dump(out + "/source-1280x800.rgba", b);
        for (const V& v : vs) {
            const Profile* pr = findProfile(v.prof);
            if (!pr) continue;
            EffectParams base = pr->fx; base.frame = v.frame; if (v.rgb >= 0) base.rgbShift = v.rgb;
            for (EffectsQuality q : {EffectsQuality::Full, EffectsQuality::Reduced, EffectsQuality::Static}) {
                const EffectParams p = scaleEffectParams(applyEffectsQuality(base, q), scale);
                CpuEffects cpu(p, 2); GlEffects gl(p);
                if (!gl.ok()) { std::printf("no gl\n"); return 1; }
                std::vector<uint8_t> oc(W * H * 4), og(W * H * 4);
                for (EffectsPipeline* e : {static_cast<EffectsPipeline*>(&cpu), static_cast<EffectsPipeline*>(&gl)}) {
                    e->resize(W, H);
                    e->setSource(a.data(), W * 4, 1000.0f);
                    e->setSource(b.data(), W * 4, 1100.0f);
                    e->render(1180.0f, e == &cpu ? oc.data() : og.data());
                }
                std::string n = out + "/" + v.name + "-" + effectsQualityName(q) + "-s" + std::to_string(scale) + "-" + std::to_string(W) + "x" + std::to_string(H);
                dump(n + "-cpu.rgba", oc); dump(n + "-gl.rgba", og);
            }
        }
    }
    std::printf("ok\n");
}
