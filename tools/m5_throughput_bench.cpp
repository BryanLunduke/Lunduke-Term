// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m5-throughput-bench: PR #6 review N1 (flood throughput). One TerminalView in a
// 1280x800 window runs `seq 1 N` or `cat FILE`, then prints a sentinel and idles. Reports the
// wall time until the sentinel is on screen (and MB/s for cat), content frames, and the longest
// gap of a 10 ms default-priority timer (a stand-in for key events). With "ctrlc", Ctrl+C is sent
// from that timer at 2 s and the time until the child is gone is reported instead. Uses only
// API the M4 code has, so the same file measures master and the branch. Needs $DISPLAY.
//
//   lterm-m5-throughput-bench seq|cat [profile] [N|FILE] [ctrlc]
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace lterm;

static double nowMs() { return g_get_monotonic_time() / 1000.0; }

static bool gridHas(const Grid& g, const std::string& s) {
    for (int r = 0; r < g.rows; ++r) {
        std::string t;
        for (int c = 0; c < g.cols; ++c) t += g.at(r, c).text.empty() ? " " : g.at(r, c).text;
        if (t.find(s) != std::string::npos) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "seq";
    const std::string profile = argc > 2 ? argv[2] : "amber";
    const std::string arg = argc > 3 ? argv[3] : (mode == "seq" ? "3000000" : "");
    const bool ctrlc = argc > 4 && std::string(argv[4]) == "ctrlc";
    auto app = Gtk::Main(argc, argv);
    ViewOptions o;
    o.profile = profile;
    const std::string body = mode == "seq" ? "seq 1 " + arg : "cat '" + arg + "'";
    o.command = {"/bin/sh", "-c", body + "; printf 'LTERM%sDONE\\n' -; exec sleep 600"};
    double bytes = 0;
    if (mode == "cat") {
        struct stat st {};
        if (stat(arg.c_str(), &st) != 0) return 2;
        bytes = static_cast<double>(st.st_size);
    }
    Gtk::Window win;
    auto* view = new TerminalView(o);
    win.add(*view);
    win.set_default_size(1280, 800);
    win.show_all();
    auto loop = Glib::MainLoop::create();
    bool exited = false;
    double exitAt = -1;
    view->signalExitStatus.connect([&](int) {
        exited = true;
        exitAt = nowMs();
    });
    const double t0 = nowMs();
    if (!view->start()) return 1;
    std::vector<double> gaps;
    double last = nowMs(), doneAt = -1, ctrlcSent = -1;
    const long frames0 = view->stats().contentFrames;
    Glib::signal_timeout().connect(
        [&] {
            const double t = nowMs();
            gaps.push_back(t - last);
            last = t;
            if (ctrlc && ctrlcSent < 0 && t - t0 >= 2000) {
                ctrlcSent = t;
                view->session().sendChar('c', VTERM_MOD_CTRL);
                view->session().flushWrites();
            }
            if (!ctrlc && doneAt < 0 && gridHas(view->shownGrid(), "LTERM-DONE")) doneAt = t;
            if (doneAt >= 0 || (exited && t - exitAt > 200) || t - t0 > 120000) {
                loop->quit();
                return false;
            }
            return true;
        },
        10);
    loop->run();
    const double wall = nowMs() - t0;
    std::sort(gaps.begin(), gaps.end());
    std::printf("%s %s, profile %s (%s), 1280x800\n", mode.c_str(), arg.c_str(), profile.c_str(),
                view->backendName().c_str());
    if (!ctrlc) {
        if (doneAt < 0) std::printf("sentinel not seen within %.1f s\n", wall / 1000.0);
        else if (mode == "cat")
            std::printf("on screen after %.2f s, %.2f MB/s\n", (doneAt - t0) / 1000.0,
                        bytes / 1e6 / ((doneAt - t0) / 1000.0));
        else std::printf("on screen after %.2f s\n", (doneAt - t0) / 1000.0);
    } else if (exited) {
        std::printf("ctrl+c: sent at %.0f ms, child gone %.0f ms later\n", ctrlcSent - t0, exitAt - ctrlcSent);
    } else {
        std::printf("ctrl+c: sent at %.0f ms, child still running after %.1f s\n", ctrlcSent - t0, wall / 1000.0);
    }
    std::printf("10 ms timer: %zu runs, longest gap %.0f ms, p99 %.0f ms; content frames %ld (%.1f/s)\n",
                gaps.size(), gaps.empty() ? 0 : gaps.back(), gaps.empty() ? 0 : gaps[gaps.size() * 99 / 100],
                view->stats().contentFrames - frames0, (view->stats().contentFrames - frames0) * 1000.0 / wall);
    win.remove();
    delete view;
    return 0;
}
