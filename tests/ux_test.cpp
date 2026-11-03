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

// ---- item 3: the Font size field follows the profile ----
static void testSettingsFontPx() {
    ViewOptions o;
    o.profile = "amber";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.show();
    TerminalView* v = w.newTab(true);
    pump(300);
    GdkEventKey e = keyEv(GDK_KEY_S, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    CHECK(w.handleKey(&e));
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d && v);
    if (!d || !v) return;
    const int amberPx = v->autoFontPx();
    const int shownAmber = d->fontPx.get_value_as_int();
    d->profile.set_active_id("plain");
    pump(200);
    const int plainPx = v->autoFontPx();
    const int shownPlain = d->fontPx.get_value_as_int();
    std::printf("settings font size: amber uses %d px, field %d; plain uses %d px, field %d\n", amberPx, shownAmber,
                plainPx, shownPlain);
    CHECK(amberPx > 0 && plainPx > 0 && amberPx != plainPx);  // else the test proves nothing
    CHECK(shownAmber == amberPx);
    CHECK(v->profile().id == "plain");
    CHECK(shownPlain == plainPx);
    CHECK(w.options().fontPx == 0);  // showing the profile's size is no override
    // An override is applied; a profile change then shows (and uses) the new profile's size.
    d->fontPx.set_value(30);
    pump(200);
    CHECK(w.options().fontPx == 30);
    CHECK(d->fontPx.get_value_as_int() == 30);
    d->profile.set_active_id("amber");
    pump(200);
    std::printf("settings font size: override 30, then amber: field %d, override %d\n", d->fontPx.get_value_as_int(),
                w.options().fontPx);
    CHECK(d->fontPx.get_value_as_int() == amberPx);
    CHECK(w.options().fontPx == 0);
    CHECK(v->autoFontPx() == amberPx);
    d->response(Gtk::RESPONSE_CLOSE);
}

// ---- item 1: right-click menu ----
static bool menuShown(TerminalView& v) { return v.contextMenu().get_visible(); }
static void closeMenu(TerminalView& v) {
    v.contextMenu().popdown();
    pump(100);
}

static void testContextMenu() {
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "echo READY-MENU; exec cat"};
    Win w(o);
    TerminalView& v = *w.view;
    CHECK(v.start());
    CHECK(waitFor([&] { return screenText(v.shownGrid()).find("READY-MENU") != std::string::npos; }, 5000));
    CHECK(v.contextCopy().get_label() == "_Copy" && v.contextPaste().get_label() == "_Paste" &&
          v.contextSettings().get_label() == "_Settings");
    const double x0 = v.marginX() + 2, y0 = v.marginY() + 4;
    // No selection: Copy is disabled, Paste and Settings are not.
    v.testButton(true, 3, x0 + 100, y0 + 40);
    v.testButton(false, 3, x0 + 100, y0 + 40);
    pump(200);
    CHECK(menuShown(v));
    CHECK(!v.contextCopy().get_sensitive());
    CHECK(v.contextPaste().get_sensitive() && v.contextSettings().get_sensitive());
    closeMenu(v);
    CHECK(!menuShown(v));
    // Select "READY" (5 cells) with the left button, then Copy from the menu.
    int r, c0, c1;
    double x1 = x0;
    CHECK(v.cellAtLogical(x0, y0, r, c0) && r == 0 && c0 == 0);
    while (v.cellAtLogical(x1, y0, r, c1) && c1 < 4) x1 += 1;
    v.testButton(true, 1, x0, y0);
    v.testMotion(x1, y0, GDK_BUTTON1_MASK);
    v.testButton(false, 1, x1, y0, GDK_BUTTON1_MASK);
    pump(100);
    std::printf("context menu: selection '%s'\n", v.selectionText().c_str());
    CHECK(v.selectionText() == "READY");
    v.testButton(true, 3, x1, y0);
    v.testButton(false, 3, x1, y0);
    pump(200);
    CHECK(menuShown(v));
    CHECK(v.contextCopy().get_sensitive());
    Gtk::Clipboard::get(GDK_SELECTION_CLIPBOARD)->set_text("nothing");
    v.contextCopy().activate();
    closeMenu(v);
    pump(100);
    const Glib::ustring clip = Gtk::Clipboard::get(GDK_SELECTION_CLIPBOARD)->wait_for_text();
    std::printf("context menu: Copy put '%s' on the clipboard\n", clip.c_str());
    CHECK(clip == "READY");
    // Paste sends the clipboard to the application (cat echoes it back).
    Gtk::Clipboard::get(GDK_SELECTION_CLIPBOARD)->set_text("PASTED-BY-MENU");
    pump(50);
    v.testButton(true, 3, x0 + 100, y0 + 40);
    v.testButton(false, 3, x0 + 100, y0 + 40);
    pump(100);
    v.contextPaste().activate();
    closeMenu(v);
    CHECK(waitFor([&] { return screenText(v.shownGrid()).find("PASTED-BY-MENU") != std::string::npos; }, 3000));
}

// The menu stays out of the way of mouse-reporting applications (Shift+right-click still opens
// it) and "Settings" opens the window's settings dialog.
static void testContextMenuWindow() {
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "printf 'MOUSE-ON\\033[?1000h'; exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.show();
    TerminalView* v = w.newTab(true);
    CHECK(v && waitFor([&] { return screenText(v->shownGrid()).find("MOUSE-ON") != std::string::npos; }, 5000));
    if (!v) return;
    pump(200);
    const double x = v->marginX() + 40, y = v->marginY() + 40;
    v->testButton(true, 3, x, y);
    v->testButton(false, 3, x, y);
    pump(200);
    CHECK(!menuShown(*v));  // the application got the click
    v->testButton(true, 3, x, y, GDK_SHIFT_MASK);
    v->testButton(false, 3, x, y, GDK_SHIFT_MASK);
    pump(200);
    CHECK(menuShown(*v));
    CHECK(!w.settingsDialog() || !w.settingsDialog()->get_visible());
    v->contextSettings().activate();
    closeMenu(*v);
    pump(200);
    CHECK(w.settingsDialog() && w.settingsDialog()->get_visible());
    std::printf("context menu: mouse reporting %s the plain right-click; Shift+right-click menu; Settings %s\n",
                "takes", w.settingsDialog() && w.settingsDialog()->get_visible() ? "opened the dialog" : "did nothing");
    if (w.settingsDialog()) w.settingsDialog()->response(Gtk::RESPONSE_CLOSE);
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
    testSettingsFontPx();
    testContextMenu();
    testContextMenuWindow();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ux_test: all passed\n");
    return failures ? 1 : 0;
}
