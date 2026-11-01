// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m5-idle-bench: review items 5 and 7. Reports the terminal process's own CPU time
// (CLOCK_PROCESS_CPUTIME_ID: every thread, llvmpipe's too; not the children) over a window.
// Uses only API the M4 code has, so the same file measures before and after. Needs $DISPLAY.
//
//   lterm-m5-idle-bench blink PROFILE RENDERER QUALITY [seconds]   one focused view, cursor blink
//                                                                  on, idle shell prompt
//   lterm-m5-idle-bench tabs PROFILE shown|hidden [seconds [quality]]        a tab printing 20 numbered lines
//                                                                  every 20 ms, shown or behind an idle tab
//   lterm-m5-idle-bench rss PROFILE RENDERER TABS                  review item 3: 1280x800, TABS tabs, each
//                                                                  shown for 1.5 s, then tab 1 for 6 s;
//                                                                  VmRSS, threads, texture bytes
#include "terminal_window.hpp"

#include <gtkmm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <ctime>
#include <fstream>
#include <string>

using namespace lterm;

static double cpuMs() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static long statusKb(const char* key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, std::strlen(key), key) == 0) return std::atol(line.c_str() + std::strlen(key));
    return -1;
}

static void run(int ms) {
    auto loop = Glib::MainLoop::create();
    Glib::signal_timeout().connect_once([&] { loop->quit(); }, static_cast<unsigned>(ms));
    loop->run();
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: see the source\n");
        return 2;
    }
    const std::string mode = argv[1];
    setenv("SHELL", "/bin/sh", 1);
    auto app = Gtk::Main(argc, argv);
    ViewOptions o;
    o.profile = argv[2];
    o.onExit = "hold";
    if (mode == "rss") {
        o.renderer = argv[3];
        parseEffectsQuality("static", o.effectsQuality);
        o.command = {"/bin/sh", "-c", "printf '$ '; exec sleep 600"};
        const int tabs = std::max(1, std::atoi(argv[4]));
        TerminalWindow w(o);
        w.settingsFile = "/dev/null";
        w.set_default_size(1280, 800);
        w.show();
        std::vector<TerminalView*> views;
        for (int i = 0; i < tabs; ++i) views.push_back(w.newTab(i == 0));
        for (int i = 0; i < tabs; ++i) {
            w.setCurrent(i);
            run(1500);
        }
        w.setCurrent(0);
        run(6000);  // past any free-when-hidden delay
        size_t tex = 0;
        for (TerminalView* v : views) tex += v->memory().textureBytes;
        std::printf("rss %s %s %d tabs: %s; VmRSS %ld kB, VmHWM %ld kB, threads %ld, our GL textures %.1f MB (all tabs)\n",
                    argv[2], argv[3], tabs, views[0]->backendName().c_str(), statusKb("VmRSS:"), statusKb("VmHWM:"),
                    statusKb("Threads:"), tex / 1048576.0);
        return 0;
    }
    if (mode == "blink") {
        o.renderer = argv[3];
        o.cursorBlink = true;
        o.pauseWhenUnfocused = false;
        if (argc > 4) parseEffectsQuality(argv[4], o.effectsQuality);
        o.command = {"/bin/sh", "-c", "printf '$ '; exec sleep 600"};
    } else {
        if (argc > 5) parseEffectsQuality(argv[5], o.effectsQuality);
        o.command = {"/bin/sh", "-c", "n=0; while :; do i=0; while [ $i -lt 20 ]; do echo line $i of burst $n of a busy tab; i=$((i+1)); done; n=$((n+1)); sleep 0.02; done"};
    }
    const int seconds = mode == "blink" ? (argc > 5 ? std::atoi(argv[5]) : 30) : (argc > 4 ? std::atoi(argv[4]) : 20);
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.set_default_size(1024, 640);
    w.show();
    TerminalView* v = w.newTab(true);
    if (mode == "tabs") w.newTab();
    run(1500);
    if (mode == "tabs") w.setCurrent(std::string(argv[3]) == "hidden" ? 1 : 0);
    if (mode == "blink") v->setWindowActive(true);  // no window manager on Xvfb: say "focused"
    run(1500);
    std::printf("%s %s %s%s%s: %s, window visible %d, active %d\n", mode.c_str(), argv[2], argv[3],
                mode == "blink" && argc > 4 ? " " : "", mode == "blink" && argc > 4 ? argv[4] : "",
                v->backendName().c_str(), v->windowVisible() ? 1 : 0, v->terminalHasFocus() ? 1 : 0);
    // Windows of 10 s: the blink fix stops blinking after gtk-cursor-blink-timeout (10 s).
    double total = 0;
    for (int t = 0; t < seconds; t += 10) {
        const int len = std::min(10, seconds - t);
        const long f0 = v->stats().contentFrames;
        const double c0 = cpuMs();
        run(len * 1000);
        const double c = cpuMs() - c0;
        total += c;
        std::printf("  %2d-%2d s: %7.0f ms CPU = %5.1f %% of one core, %ld content frames\n", t, t + len, c,
                    c / (len * 10.0), v->stats().contentFrames - f0);
    }
    std::printf("  total %.0f ms CPU over %d s = %.1f %%\n", total, seconds, total / (seconds * 10.0));
    return 0;
}
