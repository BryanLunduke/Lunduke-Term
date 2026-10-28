// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m0-bench: headless benchmark + CPU/GL golden comparison for the M0 effects pass.
//
//   lterm-m0-bench --bench [--threads 1,4,8] [--runs 5] [--frames 20] [--size 1280x800]
//   lterm-m0-bench --compare --out DIR      (writes source.png, cpu.png, gl.png, diff stats)
//   lterm-m0-bench --bench-gl               (llvmpipe timing; set LIBGL_ALWAYS_SOFTWARE=1)
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "grid_painter.hpp"
#include "synthetic_frame.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace lterm;
using Clock = std::chrono::steady_clock;

namespace {

int W = 1280, H = 800;
std::string fontFamily = "Terminus (TTF), Terminus, DejaVu Sans Mono, Monospace";
int fontPx = 28;

double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void writePng(const std::string& path, const std::vector<uint8_t>& rgba) {
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    rgbaToSurface(rgba.data(), W, H, s);
    cairo_surface_write_to_png(s, path.c_str());
    cairo_surface_destroy(s);
}

// The fixed golden sequence: frame A at t=1000 ms, frame B (scrolled by 3 lines) at t=1100 ms,
// effects tick at t=1180 ms. Burn-in therefore shows a fading copy of frame A.
std::vector<uint8_t> runGolden(EffectsPipeline& p, const std::vector<uint8_t>& a,
                               const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
    p.resize(W, H);
    p.setSource(a.data(), W * 4, 1000.0f);
    p.setSource(b.data(), W * 4, 1100.0f);
    p.render(1180.0f, out.data());
    return out;
}

struct Timing {
    double full, tick, bloom, burn, stat;
};

Timing benchOnce(EffectsPipeline& p, CpuEffects* cpu, const std::vector<uint8_t>& a,
                 const std::vector<uint8_t>& b, int frames) {
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
    p.resize(W, H);
    p.setSource(a.data(), W * 4, 0.0f);
    p.render(0.0f, out.data());  // warm-up (also GL shader JIT)
    std::vector<double> full, tick, bl, bu, st;
    float t = 50.0f;
    for (int i = 0; i < frames; ++i, t += 50.0f) {
        auto t0 = Clock::now();
        p.setSource((i & 1 ? a : b).data(), W * 4, t);
        p.render(t, out.data());
        auto t1 = Clock::now();
        full.push_back(ms(t0, t1));
        if (cpu) {
            bl.push_back(cpu->lastBloomMs);
            bu.push_back(cpu->lastBurnMs);
            st.push_back(cpu->lastStaticMs);
        }
    }
    for (int i = 0; i < frames; ++i, t += 50.0f) {
        auto t0 = Clock::now();
        p.render(t, out.data());
        tick.push_back(ms(t0, Clock::now()));
    }
    Timing r{median(full), median(tick), 0, 0, 0};
    if (cpu) {
        r.bloom = median(bl);
        r.burn = median(bu);
        r.stat = median(st);
    }
    return r;
}

struct Diff {
    double mae, psnr;
    int maxd;
    double over2pct;
};

Diff diff(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, std::vector<uint8_t>* vis) {
    double sum = 0, sq = 0;
    int mx = 0;
    size_t over = 0, n = 0;
    if (vis) vis->assign(x.size(), 0);
    for (size_t i = 0; i < x.size(); i += 4) {
        int pm = 0;
        for (int c = 0; c < 3; ++c) {
            int d = std::abs(int(x[i + c]) - int(y[i + c]));
            sum += d;
            sq += double(d) * d;
            mx = std::max(mx, d);
            pm = std::max(pm, d);
            ++n;
        }
        if (pm > 2) ++over;
        if (vis) {
            uint8_t v = static_cast<uint8_t>(std::min(255, pm * 32));
            (*vis)[i] = v;
            (*vis)[i + 1] = v;
            (*vis)[i + 2] = v;
            (*vis)[i + 3] = 255;
        }
    }
    double mse = sq / n;
    return {sum / n, mse == 0 ? INFINITY : 10.0 * std::log10(255.0 * 255.0 / mse), mx,
            100.0 * over / (x.size() / 4)};
}

std::vector<int> parseList(const char* s) {
    std::vector<int> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) v.push_back(std::atoi(item.c_str()));
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    bool bench = false, compare = false, benchGl = false, noEffectsDefaults = false;
    std::vector<int> threads = {1, 4, 8};
    int runs = 5, frames = 20;
    std::string outDir = ".";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--bench") bench = true;
        else if (a == "--bench-gl") benchGl = true;
        else if (a == "--compare") compare = true;
        else if (a == "--threads") threads = parseList(next());
        else if (a == "--runs") runs = std::atoi(next());
        else if (a == "--frames") frames = std::atoi(next());
        else if (a == "--out") outDir = next();
        else if (a == "--font") fontFamily = next();
        else if (a == "--font-px") fontPx = std::atoi(next());
        else if (a == "--size") std::sscanf(next(), "%dx%d", &W, &H);
        else if (a == "--no-raster") noEffectsDefaults = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    EffectParams params;
    if (noEffectsDefaults) params.rasterization = 0;

    std::string resolved;
    auto t0 = Clock::now();
    const auto frameA = renderSyntheticFrame(W, H, 0, fontFamily, fontPx, &resolved);
    const double srcMs = ms(t0, Clock::now());
    const auto frameB = renderSyntheticFrame(W, H, 3, fontFamily, fontPx);
    std::printf("size %dx%d font '%s' (%d px), synthetic Pango/Cairo frame render %.2f ms\n", W, H,
                resolved.c_str(), fontPx, srcMs);

    if (bench) {
        std::printf("threads\trun\tfull_ms\ttick_ms\tbloom_ms\tburn_ms\tstatic_ms\n");
        for (int th : threads) {
            std::vector<double> full, tick, bl, bu, st;
            for (int r = 0; r < runs; ++r) {
                CpuEffects cpu(params, th);
                Timing t = benchOnce(cpu, &cpu, frameA, frameB, frames);
                std::printf("%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\n", th, r, t.full, t.tick, t.bloom, t.burn,
                            t.stat);
                full.push_back(t.full);
                tick.push_back(t.tick);
                bl.push_back(t.bloom);
                bu.push_back(t.burn);
                st.push_back(t.stat);
            }
            const double f = median(full), k = median(tick);
            std::printf("MEDIAN threads=%d full=%.2f ms (%.1f fps) tick=%.2f ms (%.1f fps) bloom=%.2f "
                        "burn=%.2f static=%.2f\n",
                        th, f, 1000.0 / f, k, 1000.0 / k, median(bl), median(bu), median(st));
        }
    }

    if (benchGl) {
        GlEffects gl(params);
        if (!gl.ok()) {
            std::printf("GL unavailable: %s\n", gl.error().c_str());
            return 1;
        }
        std::printf("GL renderer: %s\n", gl.renderer().c_str());
        std::vector<double> full, tick;
        for (int r = 0; r < runs; ++r) {
            Timing t = benchOnce(gl, nullptr, frameA, frameB, frames);
            std::printf("gl\t%d\t%.2f\t%.2f\n", r, t.full, t.tick);
            full.push_back(t.full);
            tick.push_back(t.tick);
        }
        const double f = median(full), k = median(tick);
        std::printf("MEDIAN gl full=%.2f ms (%.1f fps) tick=%.2f ms (%.1f fps) (incl. upload + glReadPixels)\n",
                    f, 1000.0 / f, k, 1000.0 / k);
    }

    if (compare) {
        writePng(outDir + "/source.png", frameA);
        writePng(outDir + "/source-b.png", frameB);
        CpuEffects cpu1(params, 1), cpu4(params, 4);
        auto c1 = runGolden(cpu1, frameA, frameB);
        auto c4 = runGolden(cpu4, frameA, frameB);
        writePng(outDir + "/cpu.png", c1);
        Diff dt = diff(c1, c4, nullptr);
        std::printf("cpu 1 thread vs 4 threads: max diff %d (expect 0)\n", dt.maxd);
        GlEffects gl(params);
        if (!gl.ok()) {
            std::printf("GL unavailable: %s\n", gl.error().c_str());
            return 1;
        }
        std::printf("GL renderer: %s\n", gl.renderer().c_str());
        auto g = runGolden(gl, frameA, frameB);
        writePng(outDir + "/gl.png", g);
        std::vector<uint8_t> vis;
        Diff d = diff(c1, g, &vis);
        writePng(outDir + "/diff-x32.png", vis);
        std::printf("cpu vs gl: mean abs diff %.6f (0-255 scale), max diff %d, PSNR %.2f dB, pixels with diff>2: %.4f%%\n",
                    d.mae, d.maxd, d.psnr, d.over2pct);
    }
    return 0;
}
