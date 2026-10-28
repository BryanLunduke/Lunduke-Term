// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m1-bench: M1 measurements (glyph cache repaint, effects tick, CPU/GL PSNR, resize leak).
//
//   lterm-m1-bench --paint                     direct Pango vs glyph cache, full vs one row
//   lterm-m1-bench --tick [--threads 1,4,8]    CPU fallback tick + content pass per thread count
//   lterm-m1-bench --tick-gl                   GL tick (no readback); set LP_NUM_THREADS outside
//   lterm-m1-bench --compare                   CPU vs GL PSNR (golden sequence) at --size
//   lterm-m1-bench --resize-loop N             N GL resizes, texture count + RSS, then PSNR
//   common: --size WxH (1280x800) --profile ID (amber) --runs N
#include "cpu_effects.hpp"
#include "gl_effects.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"
#include "synthetic_frame.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lterm;
using Clock = std::chrono::steady_clock;

namespace {

int W = 1280, H = 800;

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
    return -1;
}

double psnr(const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, int* maxd) {
    double sq = 0;
    int mx = 0;
    for (size_t i = 0; i < x.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            int d = std::abs(int(x[i + c]) - int(y[i + c]));
            sq += double(d) * d;
            mx = std::max(mx, d);
        }
    if (maxd) *maxd = mx;
    const double mse = sq / (x.size() / 4 * 3.0);
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

std::vector<uint8_t> golden(EffectsPipeline& p, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
    p.resize(W, H);
    p.setSource(a.data(), W * 4, 1000.0f);
    p.setSource(b.data(), W * 4, 1100.0f);
    p.render(1180.0f, out.data());
    return out;
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
    bool doPaint = false, doTick = false, doTickGl = false, doCompare = false;
    int resizeLoop = 0, runs = 30;
    std::vector<int> threads = {1, 4, 8};
    std::string profileId = "amber";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--paint") doPaint = true;
        else if (a == "--tick") doTick = true;
        else if (a == "--tick-gl") doTickGl = true;
        else if (a == "--compare") doCompare = true;
        else if (a == "--resize-loop") resizeLoop = std::atoi(next());
        else if (a == "--threads") threads = parseList(next());
        else if (a == "--runs") runs = std::atoi(next());
        else if (a == "--profile") profileId = next();
        else if (a == "--size") std::sscanf(next(), "%dx%d", &W, &H);
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    const Profile* prof = findProfile(profileId);
    if (!prof) {
        std::fprintf(stderr, "unknown profile %s\n", profileId.c_str());
        return 2;
    }
    const EffectParams fx = prof->fx;
    const FontPick pick = profileFont(*prof);
    const std::string family = pick.family;
    const int px = pick.px;

    if (doPaint) {
        GridPainter painter(family, px);
        Grid g;
        g.resize(std::max(1, (H - 2 * kMargin) / painter.cellHeight()), std::max(1, (W - 2 * kMargin) / painter.cellWidth()));
        fillSyntheticGrid(g, 0);
        // Fill every row so a full repaint really paints a full screen of text.
        for (int r = 0; r < g.rows; ++r)
            if (g.at(r, 0).text.empty() || g.at(r, 0).text == " ")
                for (int c = 0; c < g.cols; ++c) {
                    g.at(r, c).text = std::string(1, char('!' + (r * 7 + c) % 90));
                }
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
        cairo_t* cr = cairo_create(s);
        const Color8 cur{255, 255, 255}, bg{0, 0, 0};
        std::vector<double> direct, cold, warm, one;
        for (int i = 0; i < std::max(3, runs / 5); ++i) {
            auto t0 = Clock::now();
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_paint(cr);
            painter.paint(cr, g, kMargin, kMargin, cur);
            cairo_surface_flush(s);
            direct.push_back(ms(t0, Clock::now()));
            painter.clearCache();
            t0 = Clock::now();
            painter.paintRows(cr, g, kMargin, kMargin, cur, bg, nullptr, W);
            cairo_surface_flush(s);
            cold.push_back(ms(t0, Clock::now()));
        }
        std::vector<char> rows(g.rows, 0);
        rows[g.rows / 2] = 1;
        for (int i = 0; i < runs; ++i) {
            auto t0 = Clock::now();
            painter.paintRows(cr, g, kMargin, kMargin, cur, bg, nullptr, W);
            cairo_surface_flush(s);
            warm.push_back(ms(t0, Clock::now()));
            t0 = Clock::now();
            painter.paintRows(cr, g, kMargin, kMargin, cur, bg, &rows, W);
            cairo_surface_flush(s);
            one.push_back(ms(t0, Clock::now()));
        }
        std::printf("paint %dx%d grid %dx%d font '%s' %dpx: direct Pango %.2f ms, glyph cache cold %.2f ms, "
                    "warm full %.2f ms, one changed row %.3f ms (%zu glyphs cached)\n",
                    W, H, g.cols, g.rows, painter.resolvedFamily().c_str(), px, median(direct), median(cold),
                    median(warm), median(one), painter.cachedGlyphs());
        cairo_destroy(cr);
        cairo_surface_destroy(s);
    }

    const auto fa = renderSyntheticFrame(W, H, 0, family, px);
    const auto fb = renderSyntheticFrame(W, H, 3, family, px);

    if (doTick) {
        std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4);
        for (int th : threads) {
            CpuEffects cpu(fx, th);
            cpu.resize(W, H);
            cpu.setSource(fa.data(), W * 4, 0.0f);
            cpu.render(0.0f, out.data());
            std::vector<double> content, tick;
            float t = 50;
            for (int i = 0; i < runs; ++i, t += 50) {
                auto t0 = Clock::now();
                cpu.setSource((i & 1 ? fa : fb).data(), W * 4, t);
                content.push_back(ms(t0, Clock::now()));
                t0 = Clock::now();
                cpu.render(t + 10, out.data());
                tick.push_back(ms(t0, Clock::now()));
            }
            std::printf("cpu %s %dx%d threads=%d: content pass %.2f ms, tick %.2f ms\n", profileId.c_str(), W, H, th,
                        median(content), median(tick));
        }
    }

    if (doTickGl) {
        GlEffects gl(fx);
        if (!gl.ok()) {
            std::printf("GL unavailable: %s\n", gl.error().c_str());
            return 1;
        }
        gl.resize(W, H);
        gl.setSource(fa.data(), W * 4, 0.0f);
        gl.renderNoReadback(0.0f);
        std::vector<double> content, tick;
        float t = 50;
        for (int i = 0; i < runs; ++i, t += 50) {
            auto t0 = Clock::now();
            gl.setSource((i & 1 ? fa : fb).data(), W * 4, t);
            glFinish();
            content.push_back(ms(t0, Clock::now()));
            t0 = Clock::now();
            gl.renderNoReadback(t + 10);
            tick.push_back(ms(t0, Clock::now()));
        }
        const char* lp = std::getenv("LP_NUM_THREADS");
        std::printf("gl %s %dx%d LP_NUM_THREADS=%s (%s): content pass %.2f ms, tick %.2f ms\n", profileId.c_str(), W, H,
                    lp ? lp : "default", gl.renderer().c_str(), median(content), median(tick));
    }

    if (doCompare || resizeLoop > 0) {
        GlEffects gl(fx);
        if (!gl.ok()) {
            std::printf("GL unavailable: %s\n", gl.error().c_str());
            return 1;
        }
        CpuEffects cpu(fx, 4);
        const auto c = golden(cpu, fa, fb);
        int mx = 0;
        if (doCompare) {
            const auto g = golden(gl, fa, fb);
            const double p = psnr(c, g, &mx);
            std::printf("compare %s %dx%d (%s): CPU vs GL PSNR %.2f dB, max diff %d\n", profileId.c_str(), W, H,
                        gl.renderer().c_str(), p, mx);
        }
        if (resizeLoop > 0) {
            gl.resize(W, H);
            gl.renderNoReadback(0.0f);  // creates GlEffects' scratch target, so it is in the baseline
            const int tex0 = gl.pipeline()->liveTextures(), gltex0 = countGlTextures(), fbo0 = gl.pipeline()->liveFramebuffers();
            const long rss0 = rssKb();
            const int sizes[][2] = {{640, 400}, {1920, 1080}, {800, 600}, {1281, 799}, {1024, 768}, {1280, 800}};
            long rssCycle1 = rss0;
            for (int i = 0; i < resizeLoop; ++i) {
                const auto& sz = sizes[i % 6];
                gl.resize(sz[0], sz[1]);
                gl.renderNoReadback(float(i));
                if (i == 5) rssCycle1 = rssKb();
            }
            const auto g = golden(gl, fa, fb);  // ends at W x H
            const double p = psnr(c, g, &mx);
            std::printf("resize-loop %d (%s): pipeline textures %d -> %d, fbos %d -> %d, glIsTexture count %d -> %d, "
                        "RSS %ld -> %ld (after first 6-size cycle) -> %ld kB; then CPU vs GL PSNR %.2f dB, max diff %d\n",
                        resizeLoop, profileId.c_str(), tex0, gl.pipeline()->liveTextures(), fbo0,
                        gl.pipeline()->liveFramebuffers(), gltex0, countGlTextures(), rss0, rssCycle1, rssKb(), p, mx);
        }
    }
    return 0;
}
