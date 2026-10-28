// SPDX-License-Identifier: GPL-3.0-or-later
// M2 item 1, view level (needs an X display; skipped without one). Built with AddressSanitizer
// so a use-after-free in a late clipboard reply fails the test instead of passing silently.
//   (b) scrollback-lines clamp in the view
//   (d) closing the view while a clipboard request is pending
//   (g) the child is reaped after the view hangs up on it
#include "terminal_view.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
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
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("view tests passed\n");
    return 0;
}
