// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m5-probe-bench: M5 item 1c. What one failed governor probe costs in process CPU on the
// real code path (TerminalView + GtkGLArea or the C++ renderer), not just the frame the governor
// measures. A test hook adds 3900 ms to every measured effects frame (like one GL frame in the
// TCG VM), so auto demotes to static and every probe is aborted after its first frame; the
// probe delay is fixed at 2 s so a run has many probes. For each probe it reports the process
// CPU from the probe's start until 400 ms after the following demotion, minus the idle CPU rate
// measured between probes. Uses only API that 27a9d46 (M4) already had, so the same file
// measures the old and the new code. Needs $DISPLAY.
//
//   lterm-m5-probe-bench [auto|cpu] [seconds] [focused|unfocused]   (default auto 30 focused)
//
// "unfocused": the window is reported inactive (pauseWhenUnfocused on, the default), the case
// where nobody watches the animation.
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace lterm;

static double cpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static double wallMs() { return g_get_monotonic_time() / 1000.0; }

static void pump(int ms) {
    const gint64 end = g_get_monotonic_time() + ms * 1000;
    auto ctx = Glib::MainContext::get_default();
    while (g_get_monotonic_time() < end) {
        while (ctx->pending()) ctx->iteration(false);
        g_usleep(1000);
    }
}

int main(int argc, char** argv) {
    const std::string renderer = argc > 1 ? argv[1] : "auto";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
    const bool unfocused = argc > 3 && std::strcmp(argv[3], "unfocused") == 0;
    auto app = Gtk::Main(argc, argv);
    ViewOptions o;
    o.profile = "amber";
    o.renderer = renderer;
    o.pauseWhenUnfocused = true;
    o.command = {"/bin/sh", "-c", "printf '$ '; exec sleep 600"};  // a prompt, then idle
    Gtk::Window win;
    auto* view = new TerminalView(o);
    win.add(*view);
    win.set_default_size(1024, 768);
    win.show_all();
    pump(300);
    if (!view->start()) {
        std::printf("spawn failed\n");
        return 1;
    }
    view->setWindowVisible(true);
    view->setWindowActive(true);
    GovernorConfig c = view->governor().config();
    c.firstProbeMs = 2000;
    c.maxProbeMs = 2000;
    view->governor().configure(c);
    pump(1500);
    std::printf("renderer %s, %s, window 1024x768, software=%d\n", view->backendName().c_str(),
                unfocused ? "unfocused" : "focused", view->softwareRendering() ? 1 : 0);
    view->setRenderCostForTest(3900);
    const double d0 = wallMs();
    while (!view->qualityDemoted() && wallMs() - d0 < 20000) pump(5);
    if (!view->qualityDemoted()) {
        std::printf("never demoted (animating %d, tick %.0f ms, last frame %.1f ms, load %.2f)\n",
                    view->animating() ? 1 : 0, view->tickIntervalMs(), view->governor().lastFrameMs(),
                    view->governor().lastLoad());
        return 1;
    }
    pump(500);
    if (unfocused) view->setWindowActive(false);
    // Probe cycles: Demoted -> (probe) -> Demoted again.
    std::vector<double> probeCpu, probeWall;
    double idleCpu = 0, idleWall = 0;
    const double t0 = wallMs(), c0 = cpuMs();
    bool inProbe = false;
    double pc0 = 0, pw0 = 0, ic0 = cpuMs(), iw0 = wallMs();
    int probes = 0;
    while (wallMs() - t0 < seconds * 1000.0) {
        pump(2);
        const bool demoted = view->qualityDemoted();
        if (!inProbe && !demoted) {  // a probe started
            idleCpu += cpuMs() - ic0;
            idleWall += wallMs() - iw0;
            inProbe = true;
            ++probes;
            pc0 = cpuMs();
            pw0 = wallMs();
        } else if (inProbe && demoted) {  // aborted: let the static frame land, then count it
            pump(400);
            probeCpu.push_back(cpuMs() - pc0);
            probeWall.push_back(wallMs() - pw0);
            inProbe = false;
            ic0 = cpuMs();
            iw0 = wallMs();
        }
    }
    if (!inProbe) {
        idleCpu += cpuMs() - ic0;
        idleWall += wallMs() - iw0;
    }
    const double totalCpu = cpuMs() - c0, totalWall = wallMs() - t0;
    const double idleRate = idleWall > 0 ? idleCpu / idleWall : 0;  // CPU ms per wall ms
    std::vector<double> net;
    for (size_t i = 0; i < probeCpu.size(); ++i) net.push_back(probeCpu[i] - idleRate * probeWall[i]);
    std::sort(net.begin(), net.end());
    std::printf("run %.1f s: %d probe(s) started, %zu completed; process CPU %.0f ms total (%.1f %% of a core)\n",
                totalWall / 1000.0, probes, probeCpu.size(), totalCpu, 100.0 * totalCpu / totalWall);
    std::printf("idle between probes: %.2f ms CPU per s\n", idleRate * 1000.0);
    if (!net.empty()) {
        double sum = 0;
        for (double v : net) sum += v;
        std::printf("per probe (net of idle): median %.1f ms, min %.1f, max %.1f, mean %.1f (n=%zu)\n",
                    net[net.size() / 2], net.front(), net.back(), sum / net.size(), net.size());
    }
    win.remove();
    delete view;
    return 0;
}
