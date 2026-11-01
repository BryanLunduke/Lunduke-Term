// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m5-flood-bench: review item 1 (output flood). One TerminalView runs a flooding child
// (`yes`, or `seq 1 N` and then idle) under a real Glib::MainLoop. A 10 ms timer at default
// priority stands in for key events: the bench reports how late it ran (the longest main-loop
// stall), how many content frames were painted, and, for `yes`, how long Ctrl+C (sent from that
// timer at 2 s, as a key handler would) takes to stop the child. Uses only API the M4 code has,
// so the same file measures before and after. Needs $DISPLAY.
//
//   lterm-m5-flood-bench yes|seq [profile] [seconds]
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lterm;

static double nowMs() { return g_get_monotonic_time() / 1000.0; }

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "yes";
    const std::string profile = argc > 2 ? argv[2] : "plain";
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 10;
    auto app = Gtk::Main(argc, argv);
    ViewOptions o;
    o.profile = profile;
    o.command = mode == "yes" ? std::vector<std::string>{"/bin/sh", "-c", "exec yes"}
                              : std::vector<std::string>{"/bin/sh", "-c", "seq 1 3000000; exec sleep 600"};
    Gtk::Window win;
    auto* view = new TerminalView(o);
    win.add(*view);
    win.set_default_size(1024, 768);
    win.show_all();
    auto loop = Glib::MainLoop::create();
    bool exited = false;
    double exitAt = -1;
    view->signalExitStatus.connect([&](int) {  // emitted on hold too (SIGINT counts as an error)
        exited = true;
        exitAt = nowMs();
    });
    const double t0 = nowMs();
    if (!view->start()) return 1;
    std::vector<double> gaps;
    double last = nowMs(), ctrlcSent = -1, firstSeqDone = -1;
    const long frames0 = view->stats().contentFrames;
    Glib::signal_timeout().connect(
        [&] {
            const double t = nowMs();
            gaps.push_back(t - last);
            last = t;
            if (mode == "yes" && ctrlcSent < 0 && t - t0 >= 2000) {
                ctrlcSent = t;
                view->session().sendChar('c', VTERM_MOD_CTRL);
                view->session().flushWrites();
            }
            if (t - t0 >= seconds * 1000.0 || (exited && t - exitAt > 200)) {
                loop->quit();
                return false;
            }
            return true;
        },
        10);
    loop->run();
    const double wall = nowMs() - t0;
    std::sort(gaps.begin(), gaps.end());
    const double maxGap = gaps.empty() ? 0 : gaps.back();
    const double p99 = gaps.empty() ? 0 : gaps[gaps.size() * 99 / 100];
    std::printf("mode %s, profile %s (%s), %.1f s wall\n", mode.c_str(), profile.c_str(), view->backendName().c_str(),
                wall / 1000.0);
    std::printf("10 ms timer: %zu runs, longest gap %.0f ms, p99 gap %.0f ms\n", gaps.size(), maxGap, p99);
    std::printf("content frames: %ld (%.1f per s)\n", view->stats().contentFrames - frames0,
                (view->stats().contentFrames - frames0) * 1000.0 / wall);
    if (mode == "yes") {
        if (ctrlcSent < 0) std::printf("ctrl+c: never sent (the timer did not run after 2 s)\n");
        else if (exited)
            std::printf("ctrl+c: sent at %.0f ms (due 2000), child exited %.0f ms later\n", ctrlcSent - t0,
                        exitAt - ctrlcSent);
        else std::printf("ctrl+c: sent at %.0f ms (due 2000), child still running at the end\n", ctrlcSent - t0);
    }
    (void)firstSeqDone;
    win.remove();
    delete view;
    return 0;
}
