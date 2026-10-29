// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m3-close-bench: M3 item 1c. Opens N tabs per kind of child, closes them one by one and
// reports how long each closeTab() blocks the main thread, how long until every child is gone,
// and whether any is left as a zombie. Uses only API that d402d31 already had, so the same file
// measures the M2 code. Needs $DISPLAY.
//
//   lterm-m3-close-bench [N]   (default 20)
#include "terminal_window.hpp"

#include <gtkmm.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace lterm;

static void pump(int ms) {
    const gint64 end = g_get_monotonic_time() + ms * 1000;
    auto ctx = Glib::MainContext::get_default();
    while (g_get_monotonic_time() < end) {
        while (ctx->pending()) ctx->iteration(false);
        g_usleep(2000);
    }
}

static char procState(pid_t pid) {  // 0: gone
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    char buf[512] = {};
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* p = std::strrchr(buf, ')');
    return p && p[1] && p[2] ? p[2] : '?';
}

static void run(const char* label, const std::vector<std::string>& cmd, int n) {
    ViewOptions o;
    o.command = cmd;
    o.profile = "plain";
    TerminalWindow win(o);
    win.settingsFile = "/dev/null";
    win.set_default_size(640, 400);
    win.show_all();
    std::vector<pid_t> pids;
    for (int i = 0; i < n; ++i) {
        TerminalView* v = win.newTab(true);
        if (v) pids.push_back(v->childPid());
        pump(10);
    }
    pump(500);  // the children are up (traps installed)
    std::vector<double> ms;
    // Keep one tab so the window stays; close the others, newest first.
    while (win.tabCount() > 1) {
        TerminalView* v = win.view(win.tabCount() - 1);
        const auto t0 = std::chrono::steady_clock::now();
        win.closeTab(v);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        pump(5);
    }
    const auto c0 = std::chrono::steady_clock::now();
    const std::vector<pid_t> closed(pids.begin() + 1, pids.end());  // tab 0 stays open
    double goneMs = -1;
    for (int t = 0; t < 6000; t += 20) {
        bool all = true;
        for (pid_t p : closed)
            if (p > 0 && procState(p) != 0) all = false;
        if (all) {
            goneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            break;
        }
        pump(20);
    }
    int alive = 0, zombies = 0;
    for (pid_t p : closed) {
        const char s = p > 0 ? procState(p) : 0;
        if (s == 'Z') ++zombies;
        else if (s) ++alive;
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double total = 0;
    for (double x : ms) total += x;
    std::printf("%-34s %2zu closes: per close median %6.2f ms, max %6.2f ms, total %7.2f ms; ", label, ms.size(),
                sorted[sorted.size() / 2], sorted.back(), total);
    if (goneMs >= 0) std::printf("all children gone %.0f ms after the last close\n", goneMs);
    else std::printf("after 6 s: %d still running, %d zombies\n", alive, zombies);
}

int main(int argc, char** argv) {
    if (!getenv("DISPLAY")) return 77;
    const int n = argc > 1 ? std::atoi(argv[1]) : 20;
    int fakeArgc = 1;
    auto app = Gtk::Application::create(fakeArgc, argv, "org.lunduke.CloseBench", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    run("sh (exits on SIGHUP)", {"/bin/sh", "-c", "sleep 30"}, n);
    run("bash -i (exits on SIGHUP)", {"/bin/bash", "--norc", "-i"}, n);
    run("ignores SIGHUP (trap '' HUP)", {"/bin/sh", "-c", "trap '' HUP; sleep 30"}, n);
    return 0;
}
