// SPDX-License-Identifier: GPL-3.0-or-later
// M5 review fixes (docs/M5-NOTES.md, numbering from the 2026-11-01 code review). Needs an X
// display; skipped without one.
//   1  an output flood does not freeze the main loop, and Ctrl+C reaches the child
//   14 frame-time samples only with --stats
//   12 the retro look (amber) is the default profile; plain stays selectable
#include "sanitizer.hpp"
#include "terminal_view.hpp"
#include "terminal_window.hpp"

#include <gtkmm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

// Runs the default main context for ms. Unlike `while (pending()) iteration()`, this ends on
// time even when an idle source is always ready (a flood being read in slices).
static void pump(int ms) {
    const gint64 end = g_get_monotonic_time() + ms * 1000;
    auto ctx = Glib::MainContext::get_default();
    while (g_get_monotonic_time() < end) {
        if (!ctx->iteration(false)) g_usleep(1000);
    }
}

static bool waitFor(const std::function<bool()>& f, int ms) {
    for (int t = 0; t < ms && !f(); t += 20) pump(20);
    return f();
}

static GdkEventKey keyEv(guint keyval, guint state) {
    GdkEventKey e{};
    e.type = GDK_KEY_PRESS;
    e.keyval = keyval;
    e.state = state;
    return e;
}

struct Win {
    Gtk::Window win;
    TerminalView* view;
    explicit Win(const ViewOptions& o, int w = 640, int h = 400) {
        view = new TerminalView(o);
        win.add(*view);
        win.set_default_size(w, h);
        win.show_all();
        pump(50);
    }
    ~Win() {
        win.remove();
        delete view;
    }
};

static std::string screenText(const Grid& g) {
    std::string t;
    for (int r = 0; r < g.rows; ++r) {
        for (int c = 0; c < g.cols; ++c) t += g.at(r, c).text.empty() ? " " : g.at(r, c).text;
        t += '\n';
    }
    return t;
}

// ---- item 1: output flood ----
static void testFlood(const char* profile) {
    ViewOptions o;
    o.profile = profile;
    o.onExit = "hold";
    o.command = {"/bin/sh", "-c", "exec yes"};
    Win w(o);
    int status = -1;
    w.view->signalExitStatus.connect([&](int s) { status = s; });
    CHECK(w.view->start());
    // A 10 ms timer at default priority, like key events: its longest gap is the longest freeze.
    double last = g_get_monotonic_time() / 1000.0, maxGap = 0;
    auto timer = Glib::signal_timeout().connect(
        [&] {
            const double t = g_get_monotonic_time() / 1000.0;
            maxGap = std::max(maxGap, t - last);
            last = t;
            return true;
        },
        10);
    pump(1500);
    const long frames = w.view->stats().contentFrames;
    CHECK(w.view->stats().ptySlicesCut > 0);
    CHECK(status == -1);
    GdkEventKey e = keyEv(GDK_KEY_c, GDK_CONTROL_MASK);  // Ctrl+C through the key handler
    w.view->testKey(&e);
    const gint64 t0 = g_get_monotonic_time();
    CHECK(waitFor([&] { return status != -1; }, ltermWaitMs(2000, 8000)));
    const double exitMs = (g_get_monotonic_time() - t0) / 1000.0;
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
    timer.disconnect();
    // M4 code: the timer never ran again once `yes` started (gap = the whole run).
    CHECK(maxGap < ltermTimingLimitMs(500, 2000));
    std::printf("flood %s (%s): longest main-loop gap %.0f ms, %ld slices cut, %ld content frames, "
                "Ctrl+C -> exit %.0f ms\n",
                profile, ltermBuildLabel(), maxGap, w.view->stats().ptySlicesCut, frames, exitMs);
}

// A finite flood: everything is read, the screen shows the end, and reading goes back to the
// fd watch (the next output after a pause is still seen).
static void testFloodEnds() {
    ViewOptions o;
    o.profile = "plain";
    o.onExit = "hold";
    o.command = {"/bin/sh", "-c", "seq 1 200000; sleep 1; echo after-pause; exec sleep 30"};
    Win w(o);
    CHECK(w.view->start());
    auto screenHas = [&](const char* s) {
        return screenText(w.view->shownGrid()).find(s) != std::string::npos;
    };
    CHECK(waitFor([&] { return screenHas("200000"); }, ltermWaitMs(10000, 40000)));
    CHECK(w.view->stats().ptySlicesCut > 0);
    CHECK(waitFor([&] { return screenHas("after-pause"); }, ltermWaitMs(5000, 15000)));
}

// ---- item 14: no frame statistics unless --stats ----
static void testStatsOff(bool stats) {
    ViewOptions o;
    o.profile = "amber";
    o.stats = stats;
    o.command = {"/bin/sh", "-c", "i=0; while [ $i -lt 30 ]; do echo $i; sleep 0.05; i=$((i+1)); done; exec sleep 30"};
    Win w(o);
    CHECK(w.view->start());
    CHECK(waitFor([&] { return w.view->stats().contentFrames > 5; }, ltermWaitMs(5000, 15000)));
    pump(300);
    const ViewStats& st = w.view->stats();
    const size_t samples = st.paintMs.size() + st.effectsSourceMs.size() + st.tickMs.size();
    CHECK(w.view->memory().statSamples == samples);
    if (stats) CHECK(st.paintMs.size() > 5);
    else CHECK(samples == 0);
    std::printf("stats %s: %ld content frames, %zu samples kept\n", stats ? "on" : "off", st.contentFrames, samples);
}

// ---- item 12: the retro look is the default profile ----
static void testDefaultProfile() {
    CHECK(ViewOptions{}.profile == "amber");
    ViewOptions o;
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.show();
    TerminalView* v = w.newTab(true);
    pump(300);
    CHECK(v && v->profile().id == "amber" && v->effectsOn());
    GdkEventKey e = keyEv(GDK_KEY_S, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    CHECK(w.handleKey(&e));
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d && d->profile.get_active_id() == "amber");
    if (!d) return;
    CHECK(d->profile.get_active_text().find("(default)") != Glib::ustring::npos);
    d->profile.set_active_id("plain");  // plain stays selectable
    pump(100);
    CHECK(d->profile.get_active_text().find("(default)") == Glib::ustring::npos);
    CHECK(v->profile().id == "plain" && !v->effectsOn());
    d->response(Gtk::RESPONSE_CLOSE);
}

int main(int argc, char** argv) {
    if (!getenv("DISPLAY") || !*getenv("DISPLAY")) {
        std::printf("review test skipped: no DISPLAY\n");
        return 77;
    }
    signal(SIGPIPE, SIG_IGN);
    setenv("SHELL", "/bin/sh", 1);
    auto app = Gtk::Application::create(argc, argv, "org.lunduke.TerminalReviewTest", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    testFlood("plain");
    testFlood("amber");
    testFloodEnds();
    testDefaultProfile();
    testStatsOff(false);
    testStatsOff(true);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("review tests passed\n");
    return 0;
}
