// SPDX-License-Identifier: GPL-3.0-or-later
// M5 review fixes (docs/M5-NOTES.md, numbering from the 2026-11-01 code review). Needs an X
// display; skipped without one.
//   1  an output flood does not freeze the main loop, and Ctrl+C reaches the child
//   5  cursor blink stops after the blink timeout, restarts on a key
//   7  a hidden tab does not paint; showing it paints once
//   9  closing a tab/window with a foreground job asks first
//   10 lowering the scrollback drops history only when new output arrives
//   4  scrollback maximum 100,000; Settings shows the memory it can take
//   2  input method: dead key and Compose sequences reach the PTY as UTF-8 (gtk-im-context-simple)
//   3  a hidden tab frees its effects buffers after a delay and rebuilds them on show
//   8  output compares only the on-screen part of a selection with its snapshot
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

// ---- item 8: the selection is not rebuilt on every read ----
static bool findXY(TerminalView& v, int row, int col, double& x, double& y) {
    for (int yy = 0; yy < 2000; ++yy)
        for (int xx = 0; xx < 2000; xx += 2) {
            int r, c;
            if (v.cellAtLogical(xx, yy, r, c) && r == row && c == col) {
                x = xx + 1;
                y = yy + 1;
                return true;
            }
            if (r > row) return false;
        }
    return false;
}

static void testSelectionCompare() {
    ViewOptions o;
    o.profile = "plain";
    o.scrollbackLines = 5000;
    o.command = {"/bin/sh", "-c",
                 "seq 1 3000; sleep 3; i=0; while [ $i -lt 5 ]; do echo tick $i; sleep 0.05; i=$((i+1)); done;"
                 "sleep 1; printf '\\033[H\\033[2J'; exec sleep 30"};
    Win w(o);
    TerminalView& v = *w.view;
    CHECK(v.start());
    auto screen = [&] {  // the live screen (the view may be scrolled back)
        std::string t;
        VtermSession& s = v.session();
        for (long l = s.topLine(); l < s.endLine(); ++l) {
            for (int c = 0; c < s.cols(); ++c) t += s.cellText(l, c).empty() ? " " : s.cellText(l, c);
            t += '\n';
        }
        return t;
    };
    CHECK(waitFor([&] { return screen().find("3000") != std::string::npos; }, ltermWaitMs(5000, 20000)));
    const int rows = v.shownGrid().rows;
    double x0, y0, x1, y1;
    CHECK(findXY(v, rows - 2, 0, x0, y0) && findXY(v, 0, 0, x1, y1));
    // Press on the last line of output, drag above the top: every motion scrolls one line back.
    v.testButton(true, 1, x0, y0);
    for (int i = 0; i < 1000; ++i) v.testMotion(x1, 0, GDK_BUTTON1_MASK);
    v.testMotion(x1, y1, GDK_BUTTON1_MASK);
    v.testButton(false, 1, x1, y1);
    CHECK(v.selection().active());
    const long selLines = v.selection().end().line - v.selection().start().line + 1;
    CHECK(selLines > 1000);
    const long cmp0 = v.stats().selLinesCompared;
    CHECK(waitFor([&] { return screen().find("tick 4") != std::string::npos; }, ltermWaitMs(8000, 20000)));
    const long compared = v.stats().selLinesCompared - cmp0;
    CHECK(v.selection().active());  // output only scrolled it
    // A few reads, each compares at most the selected lines still on the screen (M4: the whole
    // selection's text, > 1000 lines, per read).
    CHECK(compared > 0 && compared <= 8L * rows);
    CHECK(waitFor([&] { return !v.selection().active(); }, ltermWaitMs(4000, 12000)));  // cleared screen
    std::printf("selection of %ld lines: %ld lines compared over the ticks (screen %d rows)\n", selLines, compared, rows);
}

// ---- item 5: cursor blink timeout ----
static void testBlinkTimeout() {
    ViewOptions o;
    o.profile = "amber";
    o.cursorBlink = true;
    o.cursorBlinkTimeoutMs = 1500;
    o.pauseWhenUnfocused = false;
    o.command = {"/bin/sh", "-c", "printf '$ '; exec sleep 30"};
    Win w(o);
    TerminalView& v = *w.view;
    CHECK(v.start());
    w.view->setWindowActive(true);
    pump(1000);
    CHECK(v.stats().blinkToggles > 0);
    CHECK(waitFor([&] {
        const long t0 = v.stats().blinkToggles;
        pump(1200);  // > 2 blink periods
        return v.stats().blinkToggles == t0;
    }, ltermWaitMs(6000, 15000)));
    const long frames = v.stats().contentFrames;
    pump(1500);
    CHECK(v.stats().contentFrames == frames);  // stopped, cursor solid: no frames at all
    const long t1 = v.stats().blinkToggles;
    GdkEventKey e = keyEv(GDK_KEY_a, 0);  // a key restarts it
    v.testKey(&e);
    CHECK(waitFor([&] { return v.stats().blinkToggles > t1; }, ltermWaitMs(1500, 4000)));
    std::printf("blink: stopped after the timeout, restarted by a key\n");
}

// ---- item 7: hidden tabs do not paint ----
static void testHiddenTab() {
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "i=0; while :; do echo busy line $i; i=$((i+1)); sleep 0.02; done"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.set_default_size(480, 320);
    w.show();
    TerminalView* busy = w.newTab(true);
    TerminalView* idle = w.newTab();
    CHECK(busy && idle);
    if (!busy || !idle) return;
    w.setCurrent(1);
    pump(500);
    CHECK(!busy->windowVisible());
    const long f0 = busy->stats().contentFrames, h0 = busy->stats().framesHidden;
    pump(1000);
    CHECK(busy->stats().contentFrames == f0);  // M4: ~30 frames per second while hidden
    CHECK(busy->stats().framesHidden > h0);
    w.setCurrent(0);
    CHECK(waitFor([&] { return busy->stats().contentFrames > f0; }, ltermWaitMs(1000, 4000)));
    CHECK(screenText(busy->shownGrid()).find("busy line") != std::string::npos);
    std::printf("hidden tab: %ld frame requests skipped in 1 s, 0 painted\n", busy->stats().framesHidden - h0);
}

// ---- item 9: confirm closing a running job ----
static void testConfirmClose() {
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-i"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.set_default_size(480, 320);
    w.show();
    TerminalView* busy = w.newTab(true);
    TerminalView* idle = w.newTab();
    CHECK(busy && idle);
    if (!busy || !idle) return;
    std::vector<std::string> asked;
    bool answer = false;
    w.confirmHook = [&](const std::string& q) {
        asked.push_back(q);
        return answer;
    };
    pump(500);
    CHECK(busy->foregroundJob().empty() && idle->foregroundJob().empty());  // shells at a prompt
    const std::string cmd = "sleep 30\n";
    busy->session().sendBytes(cmd.data(), cmd.size());
    CHECK(waitFor([&] { return busy->foregroundJob() == "sleep"; }, ltermWaitMs(3000, 10000)));
    CHECK(w.runningJobs() == "sleep");
    // An idle shell closes without a question.
    w.requestCloseTab(idle);
    CHECK(w.tabCount() == 1 && asked.empty());
    // Ctrl+Shift+W on the busy tab asks; "Cancel" keeps it.
    w.setCurrent(0);
    GdkEventKey e = keyEv(GDK_KEY_W, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    CHECK(w.handleKey(&e));
    pump(100);
    CHECK(asked.size() == 1 && w.tabCount() == 1);
    CHECK(!asked.empty() && asked.back().find("sleep") != std::string::npos);
    // The window's close button too.
    CHECK(w.deleteEventForTest() == true);  // kept
    CHECK(asked.size() == 2);
    answer = true;
    CHECK(w.deleteEventForTest() == false);  // confirmed: GTK may destroy it
    // "Close Tab" closes it.
    w.requestCloseTab(busy);
    pump(200);
    CHECK(asked.size() == 4 && w.tabCount() == 0);
    CHECK(waitFor([&] { return TerminalView::pendingReaps() == 0; }, ltermWaitMs(5000, 12000)));
    std::printf("confirm close: asked %zu times (\"%s\")\n", asked.size(), asked.empty() ? "" : asked.front().c_str());
}

// ---- item 10: lowering the scrollback trims on new output ----
static void testScrollbackLazyTrim() {
    ViewOptions o;
    o.profile = "plain";
    o.scrollbackLines = 5000;
    o.command = {"/bin/sh", "-c", "i=0; while [ $i -lt 300 ]; do echo $i; i=$((i+1)); done; read x; echo more; exec sleep 30"};
    Win w(o);
    TerminalView& v = *w.view;
    CHECK(v.start());
    CHECK(waitFor([&] { return v.session().scrollbackSize() >= 250; }, ltermWaitMs(3000, 10000)));
    pump(200);
    const size_t full = v.session().scrollbackSize();
    v.setScrollbackLines(50);  // a slip in Settings ...
    pump(200);
    CHECK(v.session().scrollbackSize() == full && v.pendingScrollbackLimit() == 50);
    v.setScrollbackLines(5000);  // ... and back: nothing lost
    CHECK(v.session().scrollbackSize() == full && v.pendingScrollbackLimit() == -1);
    CHECK(v.session().scrollbackLimit() == 5000);
    v.setScrollbackLines(50);
    pump(200);
    CHECK(v.session().scrollbackSize() == full);
    v.session().sendBytes("\n", 1);  // new output: now it is trimmed
    CHECK(waitFor([&] { return v.session().scrollbackSize() <= 50; }, ltermWaitMs(2000, 8000)));
    CHECK(v.pendingScrollbackLimit() == -1 && v.session().scrollbackLimit() == 50);
    v.setScrollbackLines(20000);  // raising is immediate
    CHECK(v.session().scrollbackLimit() == 20000);
    std::printf("scrollback: %zu lines kept until new output, then %zu\n", full, v.session().scrollbackSize());
}

// ---- item 4: lower maximum, size note in Settings ----
static void testScrollbackMax() {
    CHECK(kMaxScrollbackLines == 100000);
    CHECK(clampScrollbackLines(1000000) == 100000);
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.set_default_size(640, 400);
    w.show();
    TerminalView* v = w.newTab(true);
    pump(300);
    CHECK(v != nullptr);
    if (!v) return;
    w.showSettings();
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d != nullptr);
    if (!d) return;
    double lo, hi;
    d->scrollback.get_range(lo, hi);
    CHECK(hi == 100000);
    d->scrollback.set_value(1000);
    pump(50);
    const std::string small = d->scrollbackNote.get_text();
    CHECK(small.find("MB per tab") != std::string::npos && small.find("Warning") == std::string::npos);
    d->scrollback.set_value(100000);
    pump(50);
    const std::string big = d->scrollbackNote.get_text();
    const size_t est = VtermSession::scrollbackBytesFull(100000, v->session().cols());
    CHECK((est >= 100u * 1024 * 1024) == (big.find("Warning") != std::string::npos));
    CHECK(v->options().scrollbackLines == 100000);
    std::printf("settings: \"%s\" / \"%s\"\n", small.c_str(), big.c_str());
    d->response(Gtk::RESPONSE_CLOSE);
}

// ---- item 2: input methods ----
static void testInputMethod() {
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "stty raw -echo; dd bs=1 count=5 2>/dev/null | od -An -tx1; exec sleep 30"};
    Win w(o);
    TerminalView& v = *w.view;
    CHECK(v.start());
    w.view->grabTerminalFocus();
    pump(400);
    auto key = [&](guint kv) {
        GdkEventKey e = keyEv(kv, 0);
        e.window = w.win.get_window()->gobj();  // the IM needs the event's window
        v.testKey(&e);
    };
    key(GDK_KEY_dead_acute);  // dead key: nothing yet
    key(GDK_KEY_e);           // -> e acute, U+00E9 = c3 a9
    key(GDK_KEY_Multi_key);   // Compose o c -> U+00A9 = c2 a9
    key(GDK_KEY_o);
    key(GDK_KEY_c);
    key(GDK_KEY_x);           // plain key, through the IM too
    const bool ok = waitFor([&] { return screenText(v.shownGrid()).find("c3 a9 c2 a9 78") != std::string::npos; },
                            ltermWaitMs(3000, 10000));
    CHECK(ok);
    CHECK(v.imCommits() == 3);
    if (!ok) std::fprintf(stderr, "screen: %s\n", screenText(v.shownGrid()).substr(0, 240).c_str());
    std::printf("input method: dead_acute e, Compose o c, x -> c3 a9 c2 a9 78 (%ld commits)\n", v.imCommits());
}

// ---- item 3: hidden tabs free their effects buffers ----
static void testHiddenFrees(const char* renderer) {
    ViewOptions o;
    o.profile = "amber";
    o.renderer = renderer;
    parseEffectsQuality("static", o.effectsQuality);
    o.effectsFreeHiddenMs = 500;
    o.command = {"/bin/sh", "-c", "printf 'tab ready'; exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.set_default_size(480, 320);
    w.show();
    TerminalView* a = w.newTab(true);
    TerminalView* b = w.newTab();
    CHECK(a && b);
    if (!a || !b) return;
    w.setCurrent(0);
    CHECK(waitFor([&] { return screenText(a->shownGrid()).find("tab ready") != std::string::npos; },
                  ltermWaitMs(3000, 10000)));
    pump(300);
    const bool gl = a->memory().textureBytes > 0;
    const size_t tex0 = a->memory().textureBytes;
    w.setCurrent(1);
    pump(200);
    w.setCurrent(0);  // back within the delay: nothing freed
    pump(700);
    CHECK(a->stats().effectsFreed == 0);
    w.setCurrent(1);
    CHECK(waitFor([&] { return a->stats().effectsFreed == 1; }, ltermWaitMs(2000, 6000)));
    if (gl) CHECK(a->memory().textures == 1 && a->memory().textureBytes < tex0);  // the noise texture stays
    const size_t hiddenTex = a->memory().textureBytes;
    const long f0 = a->stats().contentFrames;
    w.setCurrent(0);
    CHECK(waitFor([&] { return a->stats().contentFrames > f0; }, ltermWaitMs(2000, 6000)));
    pump(300);
    if (gl) CHECK(a->memory().textureBytes == tex0);
    CHECK(screenText(a->shownGrid()).find("tab ready") != std::string::npos);
    std::printf("hidden tab frees (%s, %s): textures %.1f MB shown -> %.1f MB after 0.5 s hidden -> %.1f MB again\n",
                renderer, a->backendName().c_str(), tex0 / 1048576.0, hiddenTex / 1048576.0, a->memory().textureBytes / 1048576.0);
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
    setenv("GTK_IM_MODULE", "gtk-im-context-simple", 1);  // item 2: no ibus/fcitx here
    auto app = Gtk::Application::create(argc, argv, "org.lunduke.TerminalReviewTest", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    testFlood("plain");
    testFlood("amber");
    testFloodEnds();
    testDefaultProfile();
    testBlinkTimeout();
    testHiddenTab();
    testConfirmClose();
    testScrollbackLazyTrim();
    testScrollbackMax();
    testInputMethod();
    testHiddenFrees("gl");
    testHiddenFrees("cpu");
    testSelectionCompare();
    testStatsOff(false);
    testStatsOff(true);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("review tests passed\n");
    return 0;
}
