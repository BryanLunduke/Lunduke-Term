// SPDX-License-Identifier: GPL-3.0-or-later
// M2 item 1, view level (needs an X display; skipped without one). Built with AddressSanitizer
// so a use-after-free in a late clipboard reply fails the test instead of passing silently.
//   (b) scrollback-lines clamp in the view
//   (d) closing the view while a clipboard request is pending
//   (g) the child is reaped after the view hangs up on it
// M2 stage 3: tabs, scrollback search (view + search bar), the settings dialog.
#include "sanitizer.hpp"
#include "terminal_view.hpp"
#include "terminal_window.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <glib/gstdio.h>
#include <gtkmm.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void pump(int ms) {
    const gint64 end = g_get_monotonic_time() + ms * 1000;
    auto ctx = Glib::MainContext::get_default();
    while (g_get_monotonic_time() < end) {
        while (ctx->pending()) ctx->iteration(false);
        g_usleep(2000);
    }
}

static bool gone(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d", pid);
    return access(path, F_OK) != 0;
}

struct Win {
    Gtk::Window win;
    TerminalView* view;
    explicit Win(const ViewOptions& o) {
        view = new TerminalView(o);
        win.add(*view);
        win.set_default_size(320, 200);
        win.show_all();
        pump(50);
    }
    void dropView() {
        win.remove();
        delete view;
        view = nullptr;
    }
    ~Win() {
        if (view) dropView();
    }
};

// A separate X client that owns CLIPBOARD and PRIMARY and answers each request only after a
// delay, so the request is still pending when the view goes away (an in-process owner can
// answer before we get the chance to close the view).
static pid_t startSlowOwner(int delayMs) {
    int ready[2];
    if (pipe(ready) != 0) return -1;
    pid_t pid = fork();
    if (pid != 0) {
        close(ready[1]);
        char c;
        if (read(ready[0], &c, 1) != 1) pid = -1;
        close(ready[0]);
        return pid;
    }
    close(ready[0]);
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // never outlive the test (e.g. when ASan aborts it)
    if (getppid() == 1) _exit(0);
    const int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);  // don't hold the test runner's pipes open
    dup2(devnull, 2);
    Display* d = XOpenDisplay(nullptr);
    if (!d) _exit(1);
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    Atom clip = XInternAtom(d, "CLIPBOARD", False), utf8 = XInternAtom(d, "UTF8_STRING", False);
    Atom targets = XInternAtom(d, "TARGETS", False);
    XSetSelectionOwner(d, clip, w, CurrentTime);
    XSetSelectionOwner(d, XA_PRIMARY, w, CurrentTime);
    XSync(d, False);
    if (write(ready[1], "r", 1) != 1) _exit(1);
    const char text[] = "slow paste";
    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type != SelectionRequest) continue;
        XSelectionRequestEvent& rq = ev.xselectionrequest;
        usleep(delayMs * 1000);
        XSelectionEvent reply{};
        reply.type = SelectionNotify;
        reply.requestor = rq.requestor;
        reply.selection = rq.selection;
        reply.target = rq.target;
        reply.time = rq.time;
        reply.property = rq.property ? rq.property : rq.target;
        if (rq.target == targets) {
            Atom list[2] = {targets, utf8};
            XChangeProperty(d, rq.requestor, reply.property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(list), 2);
        } else {
            XChangeProperty(d, rq.requestor, reply.property, utf8, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(text), sizeof text - 1);
        }
        XSendEvent(d, rq.requestor, False, 0, reinterpret_cast<XEvent*>(&reply));
        XFlush(d);
    }
}

static void testClipboardAfterClose() {
    const pid_t owner = startSlowOwner(120);
    CHECK(owner > 0);
    pump(50);
    for (int i = 0; i < 5; ++i) {
        ViewOptions o;
        o.command = {"/bin/cat"};
        o.onExit = "close";
        Win w(o);
        CHECK(w.view->start());
        w.view->pasteClipboard(false);
        w.view->pasteClipboard(true);
        pump(10);
        w.dropView();  // closed while both requests are pending
        pump(400);     // replies arrive now; they must not touch the dead view (ASan checks)
    }
    {   // Control: with the view alive the paste does arrive.
        ViewOptions o;
        o.command = {"/bin/cat"};
        Win w(o);
        CHECK(w.view->start());
        w.view->pasteClipboard(false);
        bool seen = false;
        for (int i = 0; i < 100 && !seen; ++i) {
            pump(20);
            seen = w.view->session().rowText(0).find("slow paste") != std::string::npos;
        }
        CHECK(seen);
    }
    if (owner > 0) {
        kill(owner, SIGKILL);
        waitpid(owner, nullptr, 0);
    }
}

static void testReapAfterHangup() {
    pid_t pid;
    {
        ViewOptions o;
        o.command = {"/bin/sleep", "30"};
        Win w(o);
        CHECK(w.view->start());
        pid = w.view->session().childPid();
        CHECK(pid > 0);
        pump(50);
    }  // window closed while the child runs
    pump(100);
    int st;
    errno = 0;
    CHECK(waitpid(pid, &st, WNOHANG) == -1 && errno == ECHILD);
    CHECK(gone(pid));  // not left as a zombie
    {   // A child that ignores SIGHUP and exits a bit later is reaped by the GLib child watch.
        ViewOptions o;
        o.command = {"/bin/sh", "-c", "trap '' HUP; sleep 0.6"};
        Win w(o);
        CHECK(w.view->start());
        pid = w.view->session().childPid();
        pump(150);
    }
    CHECK(!gone(pid));  // still running right after the close
    for (int i = 0; i < 100 && !gone(pid); ++i) pump(20);
    CHECK(gone(pid));
}

static void testScrollbackClampView() {
    ViewOptions o;
    o.command = {"/bin/true"};
    o.scrollbackLines = 1LL << 40;
    Win w(o);
    CHECK(w.view->session().scrollbackLimit() == kMaxScrollbackLines);
    ViewOptions n;
    n.command = {"/bin/true"};
    n.scrollbackLines = -3;
    Win w2(n);
    CHECK(w2.view->session().scrollbackLimit() == 0);
}

// M2 HiDPI: the GTK scale factor changes at run time (as on a move to a monitor with another
// scale): the view follows notify::scale-factor, renders at device resolution and keeps the
// same logical grid and mouse mapping.
static void testScaleChange(const char* profile) {
    GdkDisplay* dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(dpy)) {
        std::printf("scale test skipped: not X11\n");
        return;
    }
    ViewOptions o;
    o.command = {"/bin/cat"};
    o.profile = profile;
    Win w(o);
    w.win.resize(400, 240);
    CHECK(w.view->start());
    pump(200);
    const int s0 = w.view->scaleFactor();
    const int lw = w.view->get_allocated_width(), lh = w.view->get_allocated_height();
    const int rows = w.view->session().rows(), cols = w.view->session().cols();
    CHECK(w.view->deviceWidth() == lw * s0);
    int r1, c1, r2, c2;
    w.view->cellAtLogical(lw * 0.37, lh * 0.61, r1, c1);
    gdk_x11_display_set_window_scale(dpy, 2 * s0);
    for (int i = 0; i < 50 && w.view->scaleFactor() != 2 * s0; ++i) pump(20);
    pump(100);
    std::printf("%s (%s): scale %d -> %d: device %dx%d -> %dx%d, grid %dx%d -> %dx%d\n", profile,
                w.view->backendName().c_str(), s0, w.view->scaleFactor(), lw * s0,
                lh * s0, w.view->deviceWidth(), w.view->deviceHeight(), cols, rows, w.view->session().cols(),
                w.view->session().rows());
    CHECK(w.view->scaleFactor() == 2 * s0);
    const int lw2 = w.view->get_allocated_width(), lh2 = w.view->get_allocated_height();
    CHECK(lw2 == lw && lh2 == lh);  // same logical size
    CHECK(w.view->deviceWidth() == lw2 * 2 * s0 && w.view->deviceHeight() == lh2 * 2 * s0);
    CHECK(w.view->session().rows() == rows && w.view->session().cols() == cols);
    w.view->cellAtLogical(lw * 0.37, lh * 0.61, r2, c2);
    CHECK(r1 == r2 && c1 == c2);
    gdk_x11_display_set_window_scale(dpy, s0);
    for (int i = 0; i < 50 && w.view->scaleFactor() != s0; ++i) pump(20);
    CHECK(w.view->scaleFactor() == s0 && w.view->deviceWidth() == lw * s0);
}

// M2 item 5: amber with the bezel. The grid sits inside the glass; a click on the bezel or in a
// rounded corner selects nothing; a drag that leaves the glass clamps to the edge cells.
static void testBezelPointer(const char* renderer) {
    ViewOptions o;
    o.renderer = renderer;
    o.command = {"/bin/sh", "-c", "printf 'alpha beta gamma\\nsecond line here\\n'; exec cat"};
    o.profile = "amber";
    o.effects = 1;
    o.tube.frame = 1;
    o.tube.frameSize = 24;
    Win w(o);
    w.win.resize(640, 400);
    CHECK(w.view->start());
    pump(400);
    CHECK(w.view->bezelActive());
    const int lw = w.view->get_allocated_width(), lh = w.view->get_allocated_height();
    std::printf("bezel (%s): %dx%d, grid %dx%d, margins %d,%d\n", w.view->backendName().c_str(), lw, lh,
                w.view->session().cols(), w.view->session().rows(), w.view->marginX(), w.view->marginY());
    CHECK(w.view->marginX() >= 24 && w.view->marginY() >= 24);  // the margins include the bezel
    int r, c;
    CHECK(!w.view->cellAtLogical(6, lh / 2.0, r, c));        // left bezel
    CHECK(!w.view->cellAtLogical(lw / 2.0, 4, r, c));        // top bezel
    CHECK(!w.view->cellAtLogical(lw - 3, lh - 3, r, c));     // corner
    CHECK(w.view->cellAtLogical(lw / 2.0, lh / 2.0, r, c));  // glass
    // Click on the bezel: nothing selected (a press + release elsewhere on the bezel).
    w.view->testButton(true, 1, 8, 60);
    w.view->testMotion(8, 120, GDK_BUTTON1_MASK);
    w.view->testButton(false, 1, 8, 120);
    CHECK(!w.view->selection().active());
    // Drag from inside "alpha" out over the left/top bezel: clamps to row 0 col 0.
    int r0, c0;
    CHECK(w.view->cellAtLogical(lw * 0.30, lh * 0.18, r0, c0));
    // Find a point on the first row: walk down from the top edge of the glass.
    double y0 = -1, x0 = -1;
    for (double y = 0; y < lh && y0 < 0; y += 1)
        for (double x = lw * 0.35; x < lw * 0.6; x += 1)
            if (w.view->cellAtLogical(x, y, r, c) && r == 0 && c >= 3) {
                y0 = y + 3;
                x0 = x;
                break;
            }
    CHECK(y0 > 0);
    w.view->cellAtLogical(x0, y0, r0, c0);
    w.view->testButton(true, 1, x0, y0);
    w.view->testMotion(2, 2, GDK_BUTTON1_MASK);  // onto the bezel corner
    w.view->testButton(false, 1, 2, 2);
    std::printf("bezel drag from cell %d,%d to the corner: '%s'\n", r0, c0, w.view->selectionText().c_str());
    CHECK(w.view->selection().active());
    CHECK(w.view->selection().start().col == 0);
    CHECK(w.view->selectionText() == std::string("alpha beta gamma").substr(0, c0 + 1));
}

// ---- M2 stage 3: tabs, search, settings dialog ----

static GdkEventKey keyEv(guint keyval, guint state) {
    GdkEventKey e{};
    e.type = GDK_KEY_PRESS;
    e.keyval = keyval;
    e.state = state;
    return e;
}
static const guint kCS = GDK_CONTROL_MASK | GDK_SHIFT_MASK;

static void testTabs() {
    setenv("SHELL", "/bin/sh", 1);
    ViewOptions o;
    o.profile = "amber";  // animated effects: hidden tabs must not tick
    o.onExit = "close";
    o.command = {"/bin/sh", "-c", "echo first tab; exec sleep 30"};
    {
        TerminalWindow w(o);
        w.set_default_size(480, 320);
        w.show();
        CHECK(w.newTab(true) != nullptr);
        pump(300);
        CHECK(w.tabCount() == 1 && !w.tabsShown());
        GdkEventKey e = keyEv(GDK_KEY_T, kCS);
        CHECK(w.handleKey(&e));
        pump(300);
        CHECK(w.tabCount() == 2 && w.tabsShown() && w.currentIndex() == 1);
        CHECK(w.view(0)->session().rowText(0).find("first tab") == 0);
        CHECK(w.view(1)->session().rowText(0).find("first tab") == std::string::npos);  // a shell, not -e
        // Only the shown tab is "visible"; the other one is paused (no effects tick).
        CHECK(!w.view(0)->windowVisible() && !w.view(0)->animating());
        e = keyEv(GDK_KEY_Page_Up, GDK_CONTROL_MASK);
        CHECK(w.handleKey(&e));
        pump(200);
        CHECK(w.currentIndex() == 0);
        CHECK(!w.view(1)->windowVisible() && !w.view(1)->animating());
        e = keyEv(GDK_KEY_Page_Down, GDK_CONTROL_MASK);
        CHECK(w.handleKey(&e));
        pump(200);
        CHECK(w.currentIndex() == 1);
        // Ctrl+Shift+W closes the current tab; the tab bar goes with the second tab.
        e = keyEv(GDK_KEY_W, kCS);
        CHECK(w.handleKey(&e));
        pump(200);
        CHECK(w.tabCount() == 1 && !w.tabsShown() && w.currentIndex() == 0);
        // A tab whose child exits closes itself (deferred: emitted from inside the view).
        TerminalView* t2 = w.newTab();
        CHECK(t2 != nullptr);
        pump(300);
        CHECK(w.tabCount() == 2);
        if (t2) kill(t2->childPid(), SIGKILL);
        for (int i = 0; i < 100 && w.tabCount() == 2; ++i) pump(20);
        CHECK(w.tabCount() == 1);
        // Closing the last tab hides the window.
        TerminalView* last = w.view(0);
        w.closeTab(last);
        pump(100);
        CHECK(w.tabCount() == 0 && !w.get_visible());
    }
    {
        // Destroying a window with several live tabs (and an idle close pending) is clean.
        TerminalWindow w(o);
        w.show();
        w.newTab(true);
        w.newTab();
        w.newTab();
        pump(200);
        CHECK(w.tabCount() == 3);
        kill(w.view(2)->childPid(), SIGKILL);
        pump(5);
    }
    pump(300);  // late idle callbacks must not touch the dead window (ASan)
}

// setSearch() answers at once when the cache is complete; under ASan a slice may not be enough
// for even a few hundred lines, so the search finishes from idle (searchPending()).
static int searchSync(TerminalView& v, const std::string& q, bool cs) {
    int n = v.setSearch(q, cs);
    for (int i = 0; i < 2000 && v.searchPending(); ++i) pump(5);
    return v.searchPending() ? -1 : (n ? n : v.searchCount());
}

static void testSearchView() {
    ViewOptions o;
    o.command = {"/bin/sh", "-c",
                 "i=0; while [ $i -lt 200 ]; do echo row $i; i=$((i+1)); done; echo 'find ME here';"
                 "i=0; while [ $i -lt 100 ]; do echo after $i; i=$((i+1)); done; echo 'ME again'; exec cat"};
    Win w(o);
    CHECK(w.view->start());
    for (int i = 0; i < 100 && w.view->session().scrollbackSize() < 250; ++i) pump(20);
    pump(200);
    TerminalView& v = *w.view;
    CHECK(searchSync(v, "me", false) == 2);
    CHECK(v.searchIndex() == 1);        // the newest one, on screen
    CHECK(v.scrollOffset() == 0);
    pump(100);
    // Highlighted: the current match is orange, on the row that shows "ME again".
    {
        const auto& m = v.searchMatches()[1];
        const long top = v.session().topLine() - v.scrollOffset();
        const int row = static_cast<int>(m.start.line - top);
        CHECK(row >= 0 && row < v.shownGrid().rows);
        if (row >= 0 && row < v.shownGrid().rows) {
            const Cell& c = v.shownGrid().at(row, m.start.col);
            CHECK(c.bg == (Color8{0xf5, 0x79, 0x00}));
            CHECK(c.text == "M");
            CHECK(v.shownGrid().at(row, m.end.col + 1).bg != (Color8{0xf5, 0x79, 0x00}));
        }
    }
    CHECK(v.searchStep(true));          // older: "find ME here", in history
    CHECK(v.searchIndex() == 0);
    CHECK(v.scrollOffset() > 0);
    {
        const auto& m = v.searchMatches()[0];
        const long top = v.session().topLine() - v.scrollOffset();
        CHECK(m.start.line >= top && m.start.line < top + v.session().rows());
        pump(100);
        const int row = static_cast<int>(m.start.line - top);
        if (row >= 0 && row < v.shownGrid().rows) CHECK(v.shownGrid().at(row, m.start.col).bg == (Color8{0xf5, 0x79, 0x00}));
    }
    CHECK(v.searchStep(true) && v.searchIndex() == 1);   // wraps around to the newest
    CHECK(v.searchStep(false) && v.searchIndex() == 0);  // and back
    // The case switch rebuilds the index; wait for the search to finish (searchSync), or a
    // slow (ASan) run sees a pending 0.
    CHECK(searchSync(v, "me", true) == 0);
    CHECK(!v.searchStep(false));
    CHECK(searchSync(v, "ME", true) == 2);
    CHECK(searchSync(v, "row 1", false) >= 100);  // row 1, row 10.., row 100..
    // Output while the search is open re-runs it (debounced).
    CHECK(v.setSearch("zebra", false) == 0);
    for (char ch : std::string("zebra\r")) v.session().sendChar(static_cast<unsigned char>(ch), VTERM_MOD_NONE);
    v.session().flushWrites();
    for (int i = 0; i < 100 && v.searchCount() == 0; ++i) pump(20);
    CHECK(v.searchCount() >= 1);
    v.clearSearch();
    CHECK(v.searchCount() == 0 && v.searchIndex() == -1);
    pump(100);
    for (const Cell& c : v.shownGrid().cells) CHECK(c.bg != (Color8{0xf5, 0x79, 0x00}) && c.bg != (Color8{0xc4, 0xa0, 0x00}));
    // A pending debounced refresh when the view goes away (ASan).
    v.setSearch("zebra", false);
    for (char ch : std::string("zebra\r")) v.session().sendChar(static_cast<unsigned char>(ch), VTERM_MOD_NONE);
    v.session().flushWrites();
    pump(60);
    w.dropView();
    pump(400);
}

// M3 1(a): a 100 000-line scrollback. Typing must not block the main loop: the cache is built in
// short idle slices (the query is pending meanwhile), then each keystroke is a quick find().
static void testSearchBig() {
    ViewOptions o;
    o.scrollbackLines = 100000;
    Win w(o);
    TerminalView& v = *w.view;
    std::string chunk;
    for (int i = 0; i < 100000; ++i) {
        chunk += "[" + std::to_string(i) + "] build step" + (i % 1000 == 500 ? " Needle" : "") + "\r\n";
        if (chunk.size() > 65536) {
            v.session().feed(chunk);
            chunk.clear();
        }
    }
    v.session().feed(chunk);
    CHECK(v.session().scrollbackSize() >= 99000);
    v.resetSearchStats();
    v.prepareSearch(false);  // the search bar opened
    CHECK(v.setSearch("n", false) == 0 && v.searchPending());  // not cached yet: pending, no block
    const double first = v.searchMaxMainMs();
    int turns = 0;
    for (; turns < 2000 && v.searchPending(); ++turns) pump(2);
    CHECK(!v.searchPending());
    const int n1 = v.searchCount();
    double worstKey = 0;
    for (const char* q : {"ne", "nee", "need", "needl", "needle"}) {
        v.resetSearchStats();
        v.setSearch(q, false);
        CHECK(!v.searchPending());
        worstKey = std::max(worstKey, v.searchMaxMainMs());
    }
    std::printf("view search, 100000 lines (%s): first keystroke %.2f ms (pending, %d turns), "
                "worst later keystroke %.2f ms, 'n' %d / 'needle' %d matches\n",
                ltermBuildLabel(), first, turns, worstKey, n1, v.searchCount());
    CHECK(v.searchCount() == 100);
    CHECK(v.searchIndex() == 99);
    CHECK(v.searchStep(true) && v.searchIndex() == 98);  // Enter: older
    CHECK(v.searchStep(false) && v.searchIndex() == 99); // Shift+Enter: newer
    CHECK(first < 100.0);    // bounded slice (4 ms nominal; ASan and a loaded box get slack)
    CHECK(worstKey < 200.0);
    // A query that is still pending when the view goes away (ASan: the idle worker must stop).
    v.clearSearch();
    CHECK(v.setSearch("needle", false) == 0 && v.searchPending());
    w.dropView();
    pump(100);
}

static void testSearchBar() {
    ViewOptions o;
    o.command = {"/bin/sh", "-c", "echo alpha; echo beta ALPHA; exec sleep 30"};
    TerminalWindow w(o);
    w.set_default_size(480, 320);
    w.show();
    w.newTab(true);
    pump(300);
    GdkEventKey e = keyEv(GDK_KEY_F, kCS);
    CHECK(w.handleKey(&e));
    CHECK(w.searchShown());
    w.searchEntry().set_text("alpha");
    for (int i = 0; i < 50 && w.searchStatus().empty(); ++i) pump(20);  // search-changed is debounced
    std::printf("search bar: '%s'\n", w.searchStatus().c_str());
    CHECK(w.searchStatus() == "2 of 2");
    w.searchEntry().activate();  // Enter: older match
    pump(50);
    CHECK(w.searchStatus() == "1 of 2");
    w.searchCase().set_active(true);
    pump(50);
    CHECK(w.searchStatus() == "1 of 1");
    w.searchEntry().set_text("nothing here");
    for (int i = 0; i < 50 && w.searchStatus() != "No matches"; ++i) pump(20);
    CHECK(w.searchStatus() == "No matches");
    // Search follows the tab: a new tab gets the query re-run on its own scrollback.
    w.searchEntry().set_text("alpha");
    for (int i = 0; i < 50 && w.searchStatus() != "1 of 1"; ++i) pump(20);
    w.newTab();
    pump(400);
    CHECK(w.view(0)->searchCount() == 0);  // the old tab's highlights are gone
    CHECK(w.searchStatus() == "No matches");
    w.hideSearch();
    CHECK(!w.searchShown());
    for (int i = 0; i < w.tabCount(); ++i) CHECK(w.view(i)->searchCount() == 0);
}

static void testSettingsDialog() {
    const std::string dir = Glib::build_filename(Glib::get_tmp_dir(), "lterm-view-test-" + std::to_string(getpid()));
    const std::string ini = Glib::build_filename(dir, "settings.ini");
    g_mkdir_with_parents(dir.c_str(), 0700);
    Glib::file_set_contents(ini, "# kept\n[terminal]\ncursor-shape=bar\nprofile=green\n");
    ViewOptions o;
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = ini;
    w.set_default_size(480, 320);
    w.show();
    w.newTab(true);
    w.newTab();
    pump(300);
    GdkEventKey e = keyEv(GDK_KEY_S, kCS);
    CHECK(w.handleKey(&e));
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d != nullptr && d->get_visible());
    if (!d) return;
    CHECK(d->profile.get_active_id() == "plain");
    CHECK(!d->frame.get_sensitive());  // plain has no bezel option
    d->profile.set_active_id("amber");
    pump(100);
    CHECK(d->frame.get_sensitive());
    for (int i = 0; i < w.tabCount(); ++i) CHECK(w.view(i)->profile().id == "amber");
    d->frame.set_active_id("1");
    d->quality.set_active_id("static");
    d->fontPx.set_value(19);
    d->scrollback.set_value(500);
    pump(300);
    for (int i = 0; i < w.tabCount(); ++i) {
        TerminalView& v = *w.view(i);
        CHECK(v.options().tube.frame == 1);
        CHECK(v.profile().id == "amber" && v.profile().fx.frame);
        CHECK(v.effectsQuality() == EffectsQuality::Static);
        CHECK(v.options().fontPx == 19);
        CHECK(v.session().scrollbackLimit() == 500);
    }
    CHECK(w.view(w.currentIndex())->bezelActive());
    d->font.set_active_id("DejaVu Sans Mono");
    pump(100);
    CHECK(w.view(0)->options().fontFamily == "DejaVu Sans Mono");
    // New tabs get the dialog's settings.
    TerminalView* t3 = w.newTab();
    pump(200);
    CHECK(t3 && t3->profile().id == "amber" && t3->options().fontPx == 19 && t3->session().scrollbackLimit() == 500);
    // Save as default: the dialog's keys are written, the others kept.
    d->save.clicked();
    Glib::KeyFile kf;
    try {
        kf.load_from_file(ini);
        CHECK(kf.get_string("terminal", "profile") == "amber");
        CHECK(kf.get_string("terminal", "cursor-shape") == "bar");
        CHECK(kf.get_boolean("terminal", "frame"));
        CHECK(kf.get_integer("terminal", "font-px") == 19);
        CHECK(kf.get_string("terminal", "font") == "DejaVu Sans Mono");
        CHECK(kf.get_string("terminal", "effects-quality") == "static");
        CHECK(kf.get_int64("terminal", "scrollback-lines") == 500);
    } catch (const Glib::Error& err) {
        std::fprintf(stderr, "settings.ini: %s\n", err.what().c_str());
        ++failures;
    }
    std::string text;
    try { text = Glib::file_get_contents(ini); } catch (...) {}
    CHECK(text.find("# kept") != std::string::npos);
    // "Profile default" drops the frame key again; plain keeps its plain look.
    d->profile.set_active_id("plain");
    d->frame.set_active_id("-1");
    d->font.set_active_id("");
    d->fontPx.set_value(0);
    pump(200);
    d->save.clicked();
    try {
        Glib::KeyFile k2;
        k2.load_from_file(ini);
        CHECK(!k2.has_key("terminal", "frame") && !k2.has_key("terminal", "font") && !k2.has_key("terminal", "font-px"));
    } catch (...) { ++failures; }
    CHECK(!w.view(0)->effectsOn() && !w.view(0)->bezelActive());
    d->response(Gtk::RESPONSE_CLOSE);
    CHECK(!d->get_visible());
    g_unlink(ini.c_str());
    g_rmdir(dir.c_str());
}

// M3 item 1b: the governor measures only the effects pass. A long burst of output keeps amber
// auto animating at its level; an (injected) expensive renderer demotes it to static, and once
// rendering is cheap again a probe brings auto back up.
static bool waitFor(const std::function<bool()>& f, int ms) {
    for (int t = 0; t < ms && !f(); t += 20) pump(20);
    return f();
}

static void testGovernor(const char* renderer) {
    ViewOptions o;
    o.profile = "amber";
    o.renderer = renderer;
    o.pauseWhenUnfocused = false;
    o.command = {"/bin/sh", "-c", "seq 1 400000; sleep 60"};
    Win w(o);
    w.view->governor().configure([&] {
        GovernorConfig c = w.view->governor().config();
        c.firstProbeMs = 1500;
        return c;
    }());
    CHECK(w.view->start());
    pump(300);
    const EffectsQuality level = w.view->effectsQuality();
    CHECK(w.view->softwareRendering());
    CHECK(level == EffectsQuality::Reduced);
    // Heavy output (seq) for a few seconds: no demotion, still animating.
    const auto t0 = std::chrono::steady_clock::now();
    pump(4000);
    std::printf("governor %s (%s): after 4 s of output: quality %s, demoted %d, tick %.0f ms, frame %.1f ms\n",
                renderer, w.view->backendName().c_str(), effectsQualityName(w.view->effectsQuality()),
                w.view->qualityDemoted(), w.view->tickIntervalMs(), w.view->governor().lastFrameMs());
    CHECK(!w.view->qualityDemoted());
    CHECK(w.view->effectsQuality() == level);
    CHECK(w.view->animating());
    // A renderer that needs 400 ms per frame: over budget even at 1 fps -> static.
    w.view->setRenderCostForTest(400);
    CHECK(waitFor([&] { return w.view->qualityDemoted(); }, 8000));
    CHECK(w.view->effectsQuality() == EffectsQuality::Static);
    CHECK(!w.view->animating());
    const double demotedAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // M4 item 2: a probe whose first frame is far over budget (3900 ms, like GL in the TCG VM)
    // fails after that one frame, and the back-off doubles (1.5 s -> 3 s).
    w.view->setRenderCostForTest(3900);
    CHECK(waitFor([&] { return w.view->governor().probeAborted() && w.view->qualityDemoted(); }, 6000));
    CHECK(w.view->governor().probeFrames() == 1);
    CHECK(w.view->governor().probeDelayMs() == 3000);
    std::printf("governor %s: costly probe aborted after %d frame(s), retry in %.1f s\n", renderer,
                w.view->governor().probeFrames(), w.view->governor().probeDelayMs() / 1000.0);
    // Load drops: the next probe (3 s later) brings auto back, and it stays.
    w.view->setRenderCostForTest(0);
    CHECK(waitFor([&] { return !w.view->qualityDemoted(); }, 8000));
    CHECK(w.view->effectsQuality() == level);
    CHECK(waitFor([&] { return w.view->governor().state() == EffectsGovernor::State::Normal; }, 6000));
    CHECK(!w.view->qualityDemoted());
    CHECK(w.view->animating());
    // ... and the tick speeds back up. How far depends on the real frame cost (ASan + llvmpipe with
    // other tests running can hold it above the base interval, which is the governor working), so
    // only require clear recovery from the 1 fps floor here; governor_test checks the exact steps.
    CHECK(waitFor([&] { return w.view->tickIntervalMs() <= w.view->governor().config().maxIntervalMs / 2; }, 15000));
    waitFor([&] { return w.view->tickIntervalMs() <= w.view->governor().config().baseIntervalMs; }, 5000);
    std::printf("governor %s: demoted at %.1f s, back to %s, tick %.0f ms\n", renderer, demotedAt,
                effectsQualityName(w.view->effectsQuality()), w.view->tickIntervalMs());
    // An explicit choice clears the state.
    w.view->setEffectsQuality(EffectsQuality::Full);
    CHECK(w.view->effectsQuality() == EffectsQuality::Full);
}

// M3 item 1e: a settings.ini that is not a valid key file no longer blocks Save: the dialog
// warns, the old text is kept in settings.ini.corrupt and a fresh file is written atomically.
static void testCorruptSettings() {
    const std::string dir = Glib::build_filename(Glib::get_tmp_dir(), "lterm-corrupt-test-" + std::to_string(getpid()));
    const std::string ini = Glib::build_filename(dir, "settings.ini");
    g_mkdir_with_parents(dir.c_str(), 0700);
    const std::string bad = "[terminal\nprofile=green\n\xff\xfe garbage\n";
    Glib::file_set_contents(ini, bad);
    ViewOptions o;
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = ini;
    w.set_default_size(480, 320);
    w.show();
    w.newTab(true);
    pump(200);
    w.showSettings();
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d != nullptr);
    if (!d) return;
    d->profile.set_active_id("amber");
    pump(100);
    d->save.clicked();
    pump(50);
    const std::string st = d->status.get_text();
    std::printf("corrupt settings: status '%s'\n", st.c_str());
    CHECK(st.rfind("Warning:", 0) == 0);
    CHECK(st.find("Saved") != std::string::npos);
    try {
        Glib::KeyFile kf;
        kf.load_from_file(ini);
        CHECK(kf.get_string("terminal", "profile") == "amber");
    } catch (const Glib::Error& err) {
        std::fprintf(stderr, "settings.ini after save: %s\n", err.what().c_str());
        ++failures;
    }
    std::string kept;
    try { kept = Glib::file_get_contents(ini + ".corrupt"); } catch (...) {}
    CHECK(kept == bad);
    // Saving again: no warning now. And nothing else (no temporary file) is left in the dir.
    d->save.clicked();
    CHECK(d->status.get_text().rfind("Saved to", 0) == 0);
    int entries = 0;
    if (GDir* gd = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (const char* n = g_dir_read_name(gd)) {
            ++entries;
            CHECK(std::string(n) == "settings.ini" || std::string(n) == "settings.ini.corrupt");
        }
        g_dir_close(gd);
    }
    CHECK(entries == 2);
    // Still an error (not a warning) when the file cannot be written at all.
    std::string err, warn;
    CHECK(!saveSettings(o, "/proc/self/lterm-no-such/settings.ini", &err, &warn));
    CHECK(!err.empty());
    d->response(Gtk::RESPONSE_CLOSE);
    g_unlink((ini + ".corrupt").c_str());
    g_unlink(ini.c_str());
    g_rmdir(dir.c_str());
}

static char procState(pid_t pid) {  // 0: gone
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    std::string st;
    try { st = Glib::file_get_contents(path); } catch (...) { return 0; }
    const size_t p = st.rfind(')');
    return p != std::string::npos && p + 2 < st.size() ? st[p + 2] : '?';
}

// M3 item 1c: closing tabs never blocks on the child; one that ignores SIGHUP is killed later.
static void testTabClose() {
    ViewOptions o;
    o.command = {"/bin/sh", "-c", "trap '' HUP; sleep 30"};  // ignores SIGHUP: needs the SIGKILL
    {
        TerminalWindow w(o);
        w.set_default_size(480, 320);
        w.trimDelayMs = 300;
        w.show();
        const int n = 20;
        std::vector<pid_t> pids;
        for (int i = 0; i < n; ++i) {
            TerminalView* v = w.newTab(true);
            CHECK(v);
            if (v) pids.push_back(v->childPid());
            pump(5);
        }
        pump(400);
        double worst = 0, total = 0;
        while (w.tabCount() > 1) {
            TerminalView* v = w.view(w.tabCount() - 1);
            const auto t0 = std::chrono::steady_clock::now();
            w.closeTab(v);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, ms);
            total += ms;
            pump(2);
        }
        CHECK(w.tabCount() == 1);
        CHECK(TerminalView::pendingReaps() == n - 1);
        // M4 item 4: the burst of closes schedules one malloc_trim, after trimDelayMs.
        CHECK(w.trims() == 0);
        // Right after the closes the children still run (they ignore SIGHUP)...
        int running = 0;
        for (int i = 1; i < n; ++i) running += procState(pids[i]) != 0;
        // ...and are killed after the grace period and reaped: no zombies.
        const auto c0 = std::chrono::steady_clock::now();
        CHECK(waitFor([&] { return TerminalView::pendingReaps() == 0; }, 5000));
        const double goneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        int left = 0;
        for (int i = 1; i < n; ++i) left += procState(pids[i]) != 0;
        CHECK(left == 0);
        std::printf("tab close (%s): %d closes, worst %.2f ms, total %.2f ms; %d still running right after, "
                    "all reaped %.0f ms later\n", ltermBuildLabel(), n - 1, worst, total, running, goneMs);
        CHECK(worst < 25.0);  // M2: >= 50 ms per tab whose child ignores SIGHUP
        CHECK(waitFor([&] { return w.trims() >= 1; }, 3000));
        pump(500);
        CHECK(w.trims() == 1);  // debounced: one trim for the 19 closes
        std::printf("tab close trim: %d trim(s), RSS dropped %ld kB (ASan has its own allocator: ~0 here)\n", w.trims(), w.lastTrimKb());
    }
}

// M3 item 1d: deferred closes use tab ids (run under ASan: a stale pointer would show).
static void testTabIds() {
    ViewOptions o;
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    {   // The close button clicked twice: one close.
        TerminalWindow w(o);
        w.show();
        for (int i = 0; i < 4; ++i) w.newTab(true);
        pump(200);
        TerminalView* v1 = w.view(1);
        Gtk::Button* b = w.closeButton(v1);
        CHECK(b && w.tabId(v1) != 0);
        if (b) {
            b->clicked();
            b->clicked();
        }
        pump(100);
        CHECK(w.tabCount() == 3);
        CHECK(w.tabId(v1) == 0);
        // A deferred close for a tab that is closed (and maybe replaced at the same address)
        // before it runs: the new tab survives.
        TerminalView* v = w.view(1);
        w.closeButton(v)->clicked();  // deferred
        w.closeTab(v);                // closed right away
        TerminalView* nv = w.newTab(true);
        const bool sameAddress = nv == v;
        pump(100);
        CHECK(w.tabCount() == 3);
        CHECK(nv && w.tabId(nv) != 0);
        // Close while a close is in progress: a handler that runs while the page goes away
        // closes the same tab and another one.
        TerminalView* a = w.view(0);
        TerminalView* other = w.view(2);
        a->signal_unmap().connect([&w, a, other] {
            w.closeTab(a);      // already out of the window: nothing
            w.closeTab(other);  // deferred until the first close is done
        });
        w.setCurrent(0);
        pump(50);
        w.closeTab(a);
        CHECK(w.tabId(other) != 0);  // not yet
        pump(100);
        CHECK(w.tabCount() == 1);
        CHECK(w.tabId(other) == 0);
        std::printf("tab ids: double click -> 1 close, late close after reuse%s -> new tab kept, nested close ok\n",
                    sameAddress ? " (same address)" : "");
        // Reordered tabs: page numbers and views agree.
        TerminalView* x = w.newTab(true);
        TerminalView* y = w.newTab(true);
        pump(100);
        w.reorderForTest(y, 0);
        CHECK(w.view(0) == y);
        w.setCurrent(0);
        pump(50);
        CHECK(w.currentView() == y && y->windowVisible() && !x->windowVisible());
        w.closeButton(y)->clicked();  // pending when the window goes away
    }
    pump(100);  // the idle close must not touch the destroyed window
    CHECK(waitFor([&] { return TerminalView::pendingReaps() == 0; }, 5000));
}

int main(int argc, char** argv) {
    if (!getenv("DISPLAY") || !*getenv("DISPLAY")) {
        std::printf("view test skipped: no DISPLAY\n");
        return 77;
    }
    signal(SIGPIPE, SIG_IGN);
    auto app = Gtk::Application::create(argc, argv, "org.lunduke.TerminalViewTest", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    testScrollbackClampView();
    testClipboardAfterClose();
    testReapAfterHangup();
    testScaleChange("plain");
    testScaleChange("amber");
    testBezelPointer("auto");
    testBezelPointer("cpu");
    testTabs();
    testSearchView();
    testSearchBig();
    testSearchBar();
    testSettingsDialog();
    testCorruptSettings();
    testTabClose();
    testTabIds();
    testGovernor("auto");
    testGovernor("cpu");
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("view tests passed\n");
    return 0;
}
