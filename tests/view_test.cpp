// SPDX-License-Identifier: GPL-3.0-or-later
// M2 item 1, view level (needs an X display; skipped without one). Built with AddressSanitizer
// so a use-after-free in a late clipboard reply fails the test instead of passing silently.
//   (b) scrollback-lines clamp in the view
//   (d) closing the view while a clipboard request is pending
//   (g) the child is reaped after the view hangs up on it
#include "terminal_view.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <gtkmm.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("view tests passed\n");
    return 0;
}
