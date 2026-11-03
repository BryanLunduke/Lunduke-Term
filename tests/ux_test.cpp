// SPDX-License-Identifier: GPL-3.0-or-later
// M6 PR A (UX): context menu, plain/no-effects rendering, settings font size, custom profiles,
// profile labels. Needs an X display; skipped without one.
#include "sanitizer.hpp"
#include "terminal_view.hpp"
#include "terminal_window.hpp"

#include <gtkmm.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <algorithm>

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

// Pixels of the window as the X server shows them (what the user sees) that are clearly
// brighter than the background: text. -1 when the window cannot be read.
static long brightPixels(Gtk::Window& w, int minLevel = 110) {
    auto gw = w.get_window();
    if (!gw) return -1;
    GdkPixbuf* pb = gdk_pixbuf_get_from_window(gw->gobj(), 0, 0, gw->get_width(), gw->get_height());
    if (!pb) return -1;
    const int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
    const guchar* px = gdk_pixbuf_read_pixels(pb);
    long count = 0;
    for (int y = 0; y < gdk_pixbuf_get_height(pb); ++y)
        for (int x = 0; x < gdk_pixbuf_get_width(pb); ++x) {
            const guchar* p = px + y * rs + x * n;
            if (std::max({p[0], p[1], p[2]}) >= minLevel) ++count;
        }
    g_object_unref(pb);
    return count;
}

static const char* kText = "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo HELLO-PLAIN-TEST-$i; done; exec sleep 60";

// ---- item 2: plain / effects off must render the text ----
static void testPlainRenders(const char* renderer) {
    ViewOptions o;
    o.profile = "amber";
    o.renderer = renderer;
    o.command = {"/bin/sh", "-c", kText};
    Win w(o);
    CHECK(w.view->start());
    CHECK(waitFor([&] { return screenText(w.view->shownGrid()).find("HELLO-PLAIN-TEST-12") != std::string::npos; }, 5000));
    pump(800);
    const std::string backend = w.view->backendName();
    if (std::string(renderer) == "gl") CHECK(backend.find("GLSL") != std::string::npos);  // the GtkGLArea really ran
    const long fx = brightPixels(w.win);
    GdkEventKey e = keyEv(GDK_KEY_E, GDK_CONTROL_MASK | GDK_SHIFT_MASK);  // Ctrl+Shift+E: effects off
    w.view->testKey(&e);
    CHECK(!w.view->effectsOn());
    pump(800);
    const long off = brightPixels(w.win);
    w.view->setEffects(true);
    pump(800);
    w.view->setProfile("plain");
    CHECK(!w.view->effectsOn());
    pump(800);
    const long plain = brightPixels(w.win);
    const long total = static_cast<long>(w.win.get_window()->get_width()) * w.win.get_window()->get_height();
    std::printf("plain render (%s; %s): bright pixels of %ld: amber %ld, Ctrl+Shift+E off %ld, profile plain %ld\n",
                renderer, backend.c_str(), total, fx, off, plain);
    // Text: some bright pixels, but far from all of them. The bug showed only the (light)
    // theme background, i.e. every pixel bright.
    for (long n : {fx, off, plain}) {
        CHECK(n > 500);
        CHECK(n < total * 4 / 10);
    }
}

int main(int argc, char** argv) {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::printf("ux_test: no display, skipped\n");
        return 77;
    }
    signal(SIGPIPE, SIG_IGN);
    setenv("SHELL", "/bin/sh", 1);
    setenv("XDG_CONFIG_HOME", ("/tmp/lterm-ux-test-" + std::to_string(getpid())).c_str(), 1);
    auto app = Gtk::Application::create(argc, argv, "org.lunduke.TerminalUxTest", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    testPlainRenders("gl");
    testPlainRenders("cpu");
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ux_test: all passed\n");
    return failures ? 1 : 0;
}
