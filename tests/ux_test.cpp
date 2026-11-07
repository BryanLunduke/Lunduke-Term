// SPDX-License-Identifier: GPL-3.0-or-later
// M6 PR A (UX): context menu, plain/no-effects rendering, settings font size, custom profiles,
// profile labels. Needs an X display; skipped without one.
#include "sanitizer.hpp"
#include "terminal_view.hpp"
#include "custom_profiles.hpp"
#include "terminal_window.hpp"

#include <gtkmm.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cmath>

using namespace lterm;

static int failures = 0;
static std::string home;  // XDG_CONFIG_HOME of this run (removed at the end)
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

// $LTERM_SHOT_DIR set: saves a window as PNG there (the PR's screenshots).
static void shot(Gtk::Window& w, const char* name) {
    const char* dir = std::getenv("LTERM_SHOT_DIR");
    if (!dir || !*dir) return;
    pump(300);
    auto gw = w.get_window();
    if (!gw) return;
    GdkPixbuf* pb = gdk_pixbuf_get_from_window(gw->gobj(), 0, 0, gw->get_width(), gw->get_height());
    if (!pb) return;
    const std::string path = std::string(dir) + "/" + name + ".png";
    gdk_pixbuf_save(pb, path.c_str(), "png", nullptr, nullptr);
    g_object_unref(pb);
    std::printf("screenshot %s\n", path.c_str());
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
    shot(w.win, (std::string("plain-after-amber-") + renderer).c_str());
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
    Profile big = *findProfile("plain");
    big.id = "custom:Bigger";
    big.label = "Bigger";
    big.fontPx = 24;
    setCustomProfiles({big});
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
    // amber and plain now share the desktop 16 px size, so a custom profile with its own size is
    // what shows the field following the profile.
    d->profile.set_active_id("custom:Bigger");
    pump(200);
    const int bigPx = v->autoFontPx();
    const int shownBig = d->fontPx.get_value_as_int();
    std::printf("settings font size: amber uses %d px, field %d; custom uses %d px, field %d\n", amberPx, shownAmber,
                bigPx, shownBig);
    CHECK(amberPx > 0 && bigPx > 0 && amberPx != bigPx);  // else the test proves nothing
    CHECK(shownAmber == amberPx);
    CHECK(v->profile().id == "custom:Bigger");
    CHECK(shownBig == bigPx);
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
    setCustomProfiles({});
}

// ---- perf step 1: F11 toggles fullscreen ----
static void testFullscreenKey() {
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.show();
    w.newTab(true);
    pump(300);
    GdkEventKey e = keyEv(GDK_KEY_F11, 0);
    GdkEventKey shifted = keyEv(GDK_KEY_F11, GDK_SHIFT_MASK);
    CHECK(!w.handleKey(&shifted));  // Shift+F11 goes to the program
    // PR #9 review: Super+F11 too, whether GDK reports Super as MOD4, the virtual SUPER bit or both.
    for (guint super : {guint(GDK_MOD4_MASK), guint(GDK_SUPER_MASK), guint(GDK_MOD4_MASK | GDK_SUPER_MASK)}) {
        GdkEventKey s = keyEv(GDK_KEY_F11, super);
        CHECK(!w.handleKey(&s));
    }
    for (guint m : {guint(GDK_CONTROL_MASK), guint(GDK_MOD1_MASK)}) {
        GdkEventKey s = keyEv(GDK_KEY_F11, m);
        CHECK(!w.handleKey(&s));
    }
    // NumLock (MOD2) is not a modifier here: F11 with NumLock on still toggles (checked below).
    GdkEventKey numlock = keyEv(GDK_KEY_F11, GDK_MOD2_MASK);
    CHECK(!w.fullscreenState());
    CHECK(w.handleKey(&e));
    const bool on = waitFor([&] { return w.fullscreenState(); }, 3000);
    if (!on) {
        // Without a window manager nobody answers _NET_WM_STATE_FULLSCREEN (plain xvfb-run).
        std::printf("fullscreen: F11 handled, no window manager answered: state not checked\n");
        return;
    }
    const int fw = w.get_width(), fh = w.get_height();
    CHECK(w.handleKey(&numlock));
    const bool off = waitFor([&] { return !w.fullscreenState(); }, 3000);
    CHECK(off);
    pump(200);
    std::printf("fullscreen: F11 -> fullscreen %dx%d, F11 again -> %s %dx%d\n", fw, fh, off ? "windowed" : "STILL FULLSCREEN",
                w.get_width(), w.get_height());
}

// ---- perf step 1: font zoom (Ctrl+= / Ctrl++ / Ctrl+- / Ctrl+0) ----
static std::string fileText(const std::string& path);
static void testFontZoom() {
    g_mkdir_with_parents(home.c_str(), 0700);
    ViewOptions o;
    o.profile = "plain";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    const std::string saved = home + "/zoom-settings.ini";
    w.settingsFile = saved;
    w.set_default_size(800, 500);
    w.show();
    TerminalView* v = w.newTab(true);
    pump(300);
    CHECK(v);
    if (!v) return;
    // PR #9 review: under load (parallel suite, busy box) the window can still be mapping or
    // resizing when the test starts, so each check first waits, with the ltermWaitMs() bounds,
    // until the font size and the grid have settled instead of reading them after a fixed sleep.
    // Only a failing case waits the whole bound.
    const int waitMs = ltermWaitMs(3000, 10000);
    auto settled = [&](const std::function<int()>& f) {
        // the same value twice, 150 ms apart (or the bound ran out)
        int last = f();
        for (int t = 0; t < waitMs; t += 150) {
            pump(150);
            const int now = f();
            if (now == last) return now;
            last = now;
        }
        return last;
    };
    auto key = [&](guint keyval, guint state, int wantPx) {
        GdkEventKey e = keyEv(keyval, state);
        const bool taken = v->testKey(&e);
        waitFor([&] { return v->fontPxInUse() == wantPx; }, waitMs);
        settled([&] { return v->session().cols(); });
        return taken;
    };
    const guint C = GDK_CONTROL_MASK, CS = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    waitFor([&] { return v->autoFontPx() > 0 && v->session().cols() > 0; }, waitMs);
    const int P = settled([&] { return v->autoFontPx(); });
    const int cols0 = settled([&] { return v->session().cols(); });
    CHECK(P > 0 && v->fontPxInUse() == P);
    // PR #9 review: the zoom steps print no renderer line (stderr goes to a file meanwhile).
    const std::string errPath = home + "/zoom-stderr.txt";
    std::fflush(stderr);
    const int savedErr = dup(2);
    const int errFd = open(errPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (errFd >= 0) dup2(errFd, 2);
    CHECK(key(GDK_KEY_equal, C, P + 1) && v->fontPxInUse() == P + 1);
    CHECK(key(GDK_KEY_plus, CS, P + 2) && v->fontPxInUse() == P + 2);  // Ctrl++ (Shift+= on US layouts)
    CHECK(key(GDK_KEY_KP_Add, C, P + 3) && v->fontPxInUse() == P + 3);
    const int colsBig = v->session().cols();
    CHECK(colsBig < cols0);  // bigger cells, fewer columns in the same window
    CHECK(key(GDK_KEY_minus, C, P + 2) && v->fontPxInUse() == P + 2);
    CHECK(key(GDK_KEY_KP_Subtract, C, P + 1) && v->fontPxInUse() == P + 1);
    CHECK(v->options().fontPx == 0 && w.options().fontPx == 0);  // zoom is no font-px override
    CHECK(key(GDK_KEY_0, C, P) && v->fontPxInUse() == P && v->fontZoomPx() == 0);
    CHECK(v->session().cols() == cols0);
    std::fflush(stderr);
    if (errFd >= 0) {
        dup2(savedErr, 2);
        close(errFd);
    }
    close(savedErr);
    const std::string zoomErr = fileText(errPath);
    if (!zoomErr.empty()) std::fputs(zoomErr.c_str(), stderr);  // whatever was printed, as it came
    CHECK(zoomErr.find("lunduke-terminal: plain") == std::string::npos);
    std::printf("font zoom: plain %d px, %d cols; Ctrl+= Ctrl++ KP+ -> %d px, %d cols; Ctrl+- KP- Ctrl+0 -> %d px, "
                "%d cols\n", P, cols0, P + 3, colsBig, v->fontPxInUse(), v->session().cols());
    v->zoomFont(-1000);
    CHECK(v->fontPxInUse() == TerminalView::kMinZoomPx);
    v->zoomFont(1000);
    CHECK(v->fontPxInUse() == TerminalView::kMaxZoomPx);
    CHECK(key(GDK_KEY_KP_0, C, P) && v->fontPxInUse() == P);

    // M6 Settings font size: the field still shows the profile's size while zoomed, "Save as
    // default" writes no font-px, an override from the field replaces the zoom, and Ctrl+0 then
    // goes back to that override; a profile change starts at the new profile's size.
    key(GDK_KEY_equal, C, P + 1);
    key(GDK_KEY_equal, C, P + 2);
    GdkEventKey s = keyEv(GDK_KEY_S, CS);
    CHECK(w.handleKey(&s));
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d);
    if (!d) return;
    CHECK(d->fontPx.get_value_as_int() == P);
    CHECK(v->fontPxInUse() == P + 2);  // opening Settings does not undo the zoom
    d->save.clicked();
    pump(100);
    const std::string text = fileText(saved);
    CHECK(!text.empty() && text.find("font-px") == std::string::npos);
    d->fontPx.set_value(30);
    waitFor([&] { return v->fontPxInUse() == 30; }, waitMs);
    CHECK(w.options().fontPx == 30 && v->fontPxInUse() == 30 && v->fontZoomPx() == 0);
    CHECK(key(GDK_KEY_equal, C, 31) && v->fontPxInUse() == 31 && w.options().fontPx == 30);
    CHECK(d->fontPx.get_value_as_int() == 30);
    CHECK(key(GDK_KEY_0, C, 30) && v->fontPxInUse() == 30);
    key(GDK_KEY_minus, C, 29);
    d->profile.set_active_id("amber");
    waitFor([&] { return v->profile().id == "amber" && v->fontZoomPx() == 0; }, waitMs);
    const int amberPx = v->autoFontPx();
    std::printf("font zoom + settings: field %d px while zoomed (saved without font-px: %s); override 30 -> Ctrl+= 31 "
                "-> Ctrl+0 30; amber after zoom: %d px (profile %d px)\n",
                P, text.find("font-px") == std::string::npos ? "yes" : "NO", v->fontPxInUse(), amberPx);
    CHECK(v->fontPxInUse() == amberPx && v->fontZoomPx() == 0);
    d->response(Gtk::RESPONSE_CLOSE);
}

// PR #9 review: a base size outside 6..96 px steps 1 px toward the range instead of jumping
// to a limit, and zoom never moves it further out.
static void testZoomOutOfRange() {
    using V = TerminalView;
    CHECK(V::zoomedFontPx(16, 0) == 16 && V::zoomedFontPx(16, 3) == 19 && V::zoomedFontPx(16, -100) == V::kMinZoomPx);
    CHECK(V::zoomedFontPx(16, 1000) == V::kMaxZoomPx);
    CHECK(V::zoomedFontPx(120, 0) == 120 && V::zoomedFontPx(120, 1) == 120 && V::zoomedFontPx(120, -1) == 119);
    CHECK(V::zoomedFontPx(120, -1000) == V::kMinZoomPx);
    CHECK(V::zoomedFontPx(4, 0) == 4 && V::zoomedFontPx(4, -1) == 4 && V::zoomedFontPx(4, 1) == 5);
    CHECK(V::zoomedFontPx(4, 1000) == V::kMaxZoomPx);
    const int waitMs = ltermWaitMs(3000, 10000);
    for (int base : {120, 4}) {
        ViewOptions o;
        o.profile = "plain";
        o.fontPx = base;
        o.command = {"/bin/sh", "-c", "exec sleep 30"};
        Win w(o, 900, 500);
        TerminalView* v = w.view;
        v->start();
        waitFor([&] { return v->session().cols() > 0; }, waitMs);
        CHECK(v->fontPxInUse() == base);
        v->zoomFont(base > V::kMaxZoomPx ? 1 : -1);  // further out: no change
        CHECK(v->fontPxInUse() == base && v->fontZoomPx() == 0);
        v->zoomFont(base > V::kMaxZoomPx ? -1 : 1);  // one step toward the range, not to its limit
        const int stepped = v->fontPxInUse();
        CHECK(stepped == (base > V::kMaxZoomPx ? base - 1 : base + 1));
        GdkEventKey zero = keyEv(GDK_KEY_0, GDK_CONTROL_MASK);
        CHECK(v->testKey(&zero) && v->fontPxInUse() == base && v->fontZoomPx() == 0);
        std::printf("zoom from an out-of-range base: %d px -> further out %d px, toward the range %d px, Ctrl+0 %d px\n",
                    base, base, stepped, v->fontPxInUse());
    }
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

// ---- PR #7 review: the Copy item asks Selection::hasText (no text built) ----
// Cells: ' ' blank (""), 'S' a literal space, 'W' left half of a wide char, 'w' its right half,
// anything else itself. Counts every cell read.
struct FakeLines : LineSource {
    std::vector<std::string> rows;
    std::vector<bool> wrap;
    mutable long reads = 0;
    int cols() const override { return rows.empty() ? 0 : static_cast<int>(rows[0].size()); }
    long firstLine() const override { return 0; }
    long endLine() const override { return static_cast<long>(rows.size()); }
    std::string cellText(long l, int c) const override {
        ++reads;
        const char ch = rows[l][c];
        if (ch == ' ' || ch == 'w') return "";
        if (ch == 'S') return " ";
        if (ch == 'W') return "\xe6\xbc\xa2";
        return std::string(1, ch);
    }
    int cellWidth(long l, int c) const override { return rows[l][c] == 'w' ? 0 : 1; }
    bool wrapped(long l) const override { return wrap[l]; }
};

static void testSelectionHasText() {
    FakeLines f;
    std::srand(7);
    long mismatches = 0, cases = 0;
    const char cells[] = {' ', ' ', ' ', ' ', 'S', 'a', 'W'};
    for (int round = 0; round < 400; ++round) {
        const int nrows = 1 + std::rand() % 4, ncols = 2 + std::rand() % 5;
        f.rows.assign(nrows, std::string());
        f.wrap.assign(nrows, false);
        for (int r = 0; r < nrows; ++r) {
            std::string row;
            while (static_cast<int>(row.size()) < ncols) {
                char ch = cells[std::rand() % (round % 3 == 0 ? 7 : 5)];  // mostly blanks
                if (ch == 'W') {
                    if (static_cast<int>(row.size()) + 2 > ncols) ch = ' ';
                    else {
                        row += "Ww";
                        continue;
                    }
                }
                row += ch;
            }
            f.rows[r] = row;
            f.wrap[r] = std::rand() % 2;
        }
        for (int k = 0; k < 20; ++k) {
            SelPos a{std::rand() % nrows, std::rand() % ncols}, b{std::rand() % nrows, std::rand() % ncols};
            Selection s;
            s.begin(f, a, SelectMode::Char);
            s.extend(f, b);
            ++cases;
            if (s.hasText(f) != !s.text(f).empty()) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    // Cheap: a 5,000-line selection with text in its first cell reads one cell, not 400,000.
    f.rows.assign(5000, std::string(80, 'x'));
    f.wrap.assign(5000, true);  // soft-wrapped: hasText has to look at the first cell
    Selection big;
    big.begin(f, SelPos{0, 0}, SelectMode::Char);
    big.extend(f, SelPos{4999, 79});
    f.reads = 0;
    CHECK(big.hasText(f));
    const long hasReads = f.reads;
    f.reads = 0;
    CHECK(!big.text(f).empty());
    const long textReads = f.reads;
    CHECK(hasReads <= 2 && textReads >= 400000);
    std::printf("selection hasText: %ld random cases agree with !text().empty() (%ld mismatches); 5000x80 "
                "selection: hasText read %ld cell(s), text() %ld\n", cases, mismatches, hasReads, textReads);
}

// ---- PR #7 review: Ctrl+Shift+P cycles through the custom profiles too ----
static void testProfileCycle() {
    setCustomProfiles({});
    const std::string dir = customProfilesDir();
    std::string err;
    for (const char* n : {"Cycle A", "Cycle B"}) {
        Profile p = *findProfile("green");
        p.id = customProfileId(n);
        CHECK(saveCustomProfile(dir, p, &err));
    }
    const auto& b = builtinProfiles();
    ViewOptions o;
    o.profile = b.back().id;  // the last built-in
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    Win w(o);
    TerminalView& v = *w.view;
    CHECK(v.start());
    std::vector<std::string> seen{v.profile().id};
    const size_t total = b.size() + 2;
    for (size_t i = 0; i < total; ++i) {
        GdkEventKey e = keyEv(GDK_KEY_P, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
        CHECK(v.testKey(&e));
        pump(20);
        seen.push_back(v.profile().id);
    }
    std::string order;
    for (const auto& id : seen) order += (order.empty() ? "" : " > ") + id;
    std::printf("profile cycle: %s\n", order.c_str());
    // last built-in > Cycle A > Cycle B > first built-in > ... > last built-in
    CHECK(seen.size() == total + 1);
    CHECK(seen[1] == "custom:Cycle A" && seen[2] == "custom:Cycle B" && seen[3] == b.front().id);
    CHECK(seen.back() == b.back().id);
    CHECK(nextProfileId("custom:Cycle B") == b.front().id && nextProfileId("custom:Nope") == b.front().id);
    setCustomProfiles({});
    CHECK(nextProfileId(b.back().id) == b.front().id);  // no custom profiles: built-ins only
    reloadCustomProfiles();
    for (const char* n : {"custom:Cycle A", "custom:Cycle B"}) CHECK(deleteCustomProfile(dir, n, &err));
    CHECK(customProfiles().empty());
}

static std::string fileText(const std::string& path) {
    char* d = nullptr;
    gsize n = 0;
    std::string s = g_file_get_contents(path.c_str(), &d, &n, nullptr) ? std::string(d, n) : std::string("<none>");
    g_free(d);
    return s;
}

// ---- PR #7 review: a profile file skipped as broken is never written over from the dialog ----
static void testBrokenProfileFileKept() {
    const std::string dir = customProfilesDir();
    g_mkdir_with_parents(dir.c_str(), 0700);
    const std::string badColor = dir + "/badcolor.ini", broken = dir + "/broken.ini";
    const std::string badText = "[profile]\nforeground=#nothex\n", brokenText = "[profile\nhand-edited\n";
    g_file_set_contents(badColor.c_str(), badText.c_str(), -1, nullptr);
    g_file_set_contents(broken.c_str(), brokenText.c_str(), -1, nullptr);
    setCustomProfiles({});
    std::vector<std::string> warn;
    reloadCustomProfiles(&warn);
    CHECK(warn.size() == 2 && customProfiles().empty());
    ViewOptions o;
    o.profile = "amber";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.show();
    TerminalView* v = w.newTab(true);
    pump(200);
    w.showSettings();
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d && v);
    if (!d || !v) return;
    std::string err;
    // The name dialog's own check refuses it with the reason (OK stays disabled there).
    CHECK(!validateProfileName("badcolor", &err, "", dir) && err.find("could not be loaded") != std::string::npos);
    // New and Copy onto the skipped names.
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("badcolor"); };
    d->newProfile.clicked();
    pump(100);
    CHECK(d->status.get_text().find("Profile not saved") != Glib::ustring::npos &&
          d->status.get_text().find("badcolor.ini") != Glib::ustring::npos);
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("Broken"); };
    d->copyProfile.clicked();
    pump(100);
    CHECK(d->status.get_text().find("broken.ini") != Glib::ustring::npos);
    CHECK(customProfiles().empty());
    // Rename a real custom profile onto a skipped name.
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("Real"); };
    d->copyProfile.clicked();
    pump(200);
    if (d->editor) d->editor->response(Gtk::RESPONSE_CANCEL);
    CHECK(d->profile.get_active_id() == "custom:Real");
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("broken"); };
    d->renameProfile.clicked();
    pump(100);
    std::printf("broken profile file: rename status '%s'\n", d->status.get_text().c_str());
    CHECK(d->status.get_text().find("Not renamed") != Glib::ustring::npos);
    CHECK(findProfile("custom:Real") && !findProfile("custom:broken") && v->profile().id == "custom:Real");
    CHECK(fileText(badColor) == badText && fileText(broken) == brokenText);  // byte-identical
    d->confirmDeleteHook = [](const std::string&) { return true; };
    d->deleteProfile.clicked();
    pump(100);
    CHECK(customProfiles().empty());
    CHECK(fileText(badColor) == badText && fileText(broken) == brokenText);
    d->response(Gtk::RESPONSE_CLOSE);
    if (v) w.closeTab(v);
    pump(100);
    g_unlink(badColor.c_str());
    g_unlink(broken.c_str());
}

// ---- PR #7 review: deleting the custom profile saved as the default resets settings.ini ----
static void testDeleteSavedDefault() {
    setCustomProfiles({});
    const std::string dir = customProfilesDir();
    const std::string ini = "/tmp/lterm-ux-test-" + std::to_string(getpid()) + "/settings-default-test.ini";
    ViewOptions o;
    o.profile = "amber";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = ini;
    w.show();
    TerminalView* v = w.newTab(true);
    pump(200);
    w.showSettings();
    pump(100);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d && v);
    if (!d || !v) return;
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("Saved"); };
    d->copyProfile.clicked();
    pump(200);
    if (d->editor) d->editor->response(Gtk::RESPONSE_CANCEL);
    d->save.clicked();  // "Save as default": profile=custom:Saved
    pump(50);
    // Keys the dialog does not own are kept.
    std::string text = fileText(ini);
    text += "\n[other]\nkeep=yes\n";
    g_file_set_contents(ini.c_str(), text.c_str(), -1, nullptr);
    CHECK(text.find("profile=custom:Saved") != std::string::npos);
    // A second custom profile, deleted while Saved is the default: settings.ini is untouched.
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("Other"); };
    d->copyProfile.clicked();
    pump(200);
    if (d->editor) d->editor->response(Gtk::RESPONSE_CANCEL);
    d->confirmDeleteHook = [](const std::string&) { return true; };
    d->deleteProfile.clicked();
    pump(100);
    CHECK(fileText(ini) == text);
    // Delete the saved default: settings.ini says amber again, the rest is kept.
    d->profile.set_active_id("custom:Saved");
    pump(100);
    d->deleteProfile.clicked();
    pump(100);
    const std::string after = fileText(ini);
    std::printf("delete saved default: settings.ini profile line now '%s'\n",
                after.find("profile=amber") != std::string::npos ? "profile=amber" : "?");
    CHECK(after.find("profile=amber") != std::string::npos && after.find("custom:Saved") == std::string::npos);
    CHECK(after.find("keep=yes") != std::string::npos && after.find("scrollback-lines=") != std::string::npos);
    CHECK(customProfiles().empty());
    d->response(Gtk::RESPONSE_CLOSE);
    if (v) w.closeTab(v);
    pump(100);
    g_unlink(ini.c_str());
}

// Removes the test's XDG_CONFIG_HOME (PR #7 review). Only this test's own /tmp folder.
static void removeTree(const std::string& path) {
    if (path.rfind("/tmp/lterm-ux-test-", 0) != 0) return;
    if (GDir* d = g_dir_open(path.c_str(), 0, nullptr)) {
        while (const char* n = g_dir_read_name(d)) {
            const std::string child = path + "/" + n;
            if (g_file_test(child.c_str(), G_FILE_TEST_IS_DIR) && !g_file_test(child.c_str(), G_FILE_TEST_IS_SYMLINK))
                removeTree(child);
            else g_unlink(child.c_str());
        }
        g_dir_close(d);
    }
    g_rmdir(path.c_str());
}

// ---- item 4: custom profiles through the Settings dialog ----
static void testCustomProfilesUi() {
    setCustomProfiles({});
    const std::string dir = customProfilesDir();
    ViewOptions o;
    o.profile = "amber";
    o.command = {"/bin/sh", "-c", "exec sleep 30"};
    TerminalWindow w(o);
    w.settingsFile = "/dev/null";
    w.set_default_size(800, 500);
    w.show();
    TerminalView* v = w.newTab(true);
    pump(300);
    w.showSettings();
    pump(200);
    SettingsDialog* d = w.settingsDialog();
    CHECK(d && v);
    if (!d || !v) return;
    // A built-in is read-only: only New and Copy.
    CHECK(d->profile.get_active_id() == "amber");
    CHECK(!d->editProfile.get_sensitive() && !d->renameProfile.get_sensitive() && !d->deleteProfile.get_sensitive());
    CHECK(d->copyProfile.get_sensitive() && d->newProfile.get_sensitive());
    shot(*d, "settings-builtin");
    // Names are checked: a bad or taken name saves nothing.
    std::string suggested;
    for (const char* bad : {"a/b", "Amber", ""}) {
        d->askNameHook = [&, bad](const std::string&, const std::string& s) {
            suggested = s;
            return std::string(bad);
        };
        d->copyProfile.clicked();
        pump(50);
        CHECK(customProfiles().empty());
    }
    CHECK(suggested == "Amber (copy)");
    CHECK(d->status.get_text().find("Profile not saved") != Glib::ustring::npos);
    // Copy to custom profile: a file, in the list, active in the tab, editor open.
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("Mine"); };
    d->copyProfile.clicked();
    pump(200);
    CHECK(g_file_test((dir + "/Mine.ini").c_str(), G_FILE_TEST_EXISTS));
    CHECK(d->profile.get_active_id() == "custom:Mine" && d->profile.get_active_text() == "Mine");
    CHECK(v->profile().id == "custom:Mine" && v->effectsOn());
    CHECK(d->editProfile.get_sensitive() && d->renameProfile.get_sensitive() && d->deleteProfile.get_sensitive());
    CHECK(d->editor && d->editor->get_visible());
    if (!d->editor) return;
    ProfileEditor& e = *d->editor;
    CHECK(e.toggles.size() == 11);
    CHECK(e.toggles.at("bloom")->on.get_active());
    // Switch effects off and on, adjust them, change colours, font and bezel.
    e.toggles.at("bloom")->on.set_active(false);
    CHECK(!e.toggles.at("bloom")->value.get_sensitive());
    e.toggles.at("jitter")->value.set_value(0.9);
    e.toggles.at("rgb-shift")->on.set_active(true);
    e.toggles.at("rgb-shift")->value.set_value(0.4);
    e.scanlines.set_active(true);
    e.scanlinePeriod.set_value(5);
    e.brightness.set_value(0.7);
    Gdk::RGBA red;
    red.set_rgba(1, 0, 0, 1);
    e.fg.set_rgba(red);
    e.palette[4]->set_rgba(red);
    e.fonts.set_text("DejaVu Sans Mono:18; Monospace");
    e.fontPx.set_value(20);
    e.frame.set_active(true);
    e.frameSize.set_value(33);
    e.pages.set_current_page(2);
    shot(e, "profile-editor-effects");
    e.pages.set_current_page(0);
    shot(e, "profile-editor-text");
    e.response(Gtk::RESPONSE_OK);
    pump(300);
    const Profile* m = findProfile("custom:Mine");
    CHECK(m && m->fx.bloom == 0.0f && std::abs(m->fx.jitter - 0.9f) < 1e-4f && std::abs(m->fx.rgbShift - 0.4f) < 1e-4f);
    CHECK(m && m->fx.rasterization == 1 && m->fx.scanlinePeriod == 5.0f && std::abs(m->fx.brightness - 0.7f) < 1e-4f);
    CHECK(m && m->fg == (Color8{255, 0, 0}) && m->palette[4] == (Color8{255, 0, 0}));
    CHECK(m && m->fonts.size() == 2 && m->fonts[0].family == "DejaVu Sans Mono" && m->fonts[0].px == 18 && m->fontPx == 20);
    CHECK(m && m->fx.frame && m->fx.frameSize == 33.0f);
    CHECK(v->profile().fx.bloom == 0.0f && v->profile().fx.frame && v->autoFontPx() == 18);  // the tab follows
    CHECK(d->fontPx.get_value_as_int() == 18);
    std::printf("custom profile saved: bloom %.2f jitter %.2f rgb-shift %.2f scanlines %d font %s:%d frame %d\n",
                m ? m->fx.bloom : -1.0f, m ? m->fx.jitter : -1.0f, m ? m->fx.rgbShift : -1.0f, m ? m->fx.rasterization : -1,
                m && !m->fonts.empty() ? m->fonts[0].family.c_str() : "?", m && !m->fonts.empty() ? m->fonts[0].px : -1,
                m ? static_cast<int>(m->fx.frame) : -1);
    shot(*d, "settings-custom");
    // Survives a restart: reload from disk, a new window picks it by id.
    {
        const Profile before = *findProfile("custom:Mine");
        setCustomProfiles({});
        CHECK(!findProfile("custom:Mine"));
        reloadCustomProfiles();
        const Profile* r = findProfile("custom:Mine");
        CHECK(r && r->fx.jitter == before.fx.jitter && r->fx.bloom == 0.0f && r->fg == before.fg && r->fonts.size() == 2);
        ViewOptions o2 = o;
        o2.profile = "custom:Mine";
        TerminalWindow w2(o2);
        w2.settingsFile = "/dev/null";
        w2.show();
        TerminalView* v2 = w2.newTab(true);
        pump(300);
        CHECK(v2 && v2->profile().id == "custom:Mine" && v2->profile().fx.frame);
        w2.showSettings();
        pump(100);
        CHECK(w2.settingsDialog() && w2.settingsDialog()->profile.get_active_text() == "Mine");
        if (w2.settingsDialog()) w2.settingsDialog()->response(Gtk::RESPONSE_CLOSE);
        if (v2) w2.closeTab(v2);
        pump(100);
    }
    // Rename: the file moves, the tab follows.
    d->askNameHook = [](const std::string&, const std::string&) { return std::string("Renamed"); };
    d->renameProfile.clicked();
    pump(200);
    CHECK(!g_file_test((dir + "/Mine.ini").c_str(), G_FILE_TEST_EXISTS));
    CHECK(g_file_test((dir + "/Renamed.ini").c_str(), G_FILE_TEST_EXISTS));
    CHECK(d->profile.get_active_id() == "custom:Renamed" && v->profile().id == "custom:Renamed");
    // Delete: asked first; the tab goes back to the default profile.
    d->confirmDeleteHook = [](const std::string&) { return false; };
    d->deleteProfile.clicked();
    pump(100);
    CHECK(findProfile("custom:Renamed"));
    d->confirmDeleteHook = [](const std::string&) { return true; };
    d->deleteProfile.clicked();
    pump(200);
    CHECK(!findProfile("custom:Renamed") && !g_file_test((dir + "/Renamed.ini").c_str(), G_FILE_TEST_EXISTS));
    CHECK(d->profile.get_active_id() == kDefaultProfile && v->profile().id == kDefaultProfile);
    CHECK(d->profile.get_model()->children().size() == builtinProfiles().size());
    // New: Plain's values under a new name.
    d->askNameHook = [](const std::string&, const std::string& s) { return s; };
    d->newProfile.clicked();
    pump(200);
    CHECK(d->profile.get_active_id() == "custom:My profile" && !v->effectsOn());
    if (d->editor) d->editor->response(Gtk::RESPONSE_CANCEL);
    d->confirmDeleteHook = [](const std::string&) { return true; };
    d->deleteProfile.clicked();
    pump(100);
    CHECK(customProfiles().empty());
    d->response(Gtk::RESPONSE_CLOSE);
    g_rmdir(dir.c_str());
}

int main(int argc, char** argv) {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::printf("ux_test: no display, skipped\n");
        return 77;
    }
    signal(SIGPIPE, SIG_IGN);
    setenv("SHELL", "/bin/sh", 1);
    home = "/tmp/lterm-ux-test-" + std::to_string(getpid());
    setenv("XDG_CONFIG_HOME", home.c_str(), 1);
    std::atexit([] { removeTree(home); });  // also when a check aborts the run (exit paths)
    auto app = Gtk::Application::create(argc, argv, "org.lunduke.TerminalUxTest", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    testPlainRenders("gl");
    testPlainRenders("cpu");
    testSettingsFontPx();
    {
        // First launch, no --size: the window's default size is an 80x24 grid of the profile font.
        ViewOptions o;
        o.profile = "amber";
        o.renderer = "cpu";
        o.command = {"/bin/sh", "-c", "exec sleep 30"};
        const WindowSize want = defaultWindowSize(o);
        TerminalWindow w(o);
        w.settingsFile = "/dev/null";
        w.set_default_size(want.width, want.height);
        w.show();
        TerminalView* v = w.newTab(true);
        const int waitMs = ltermWaitMs(3000, 10000);
        CHECK(waitFor([&] { return v && v->session().cols() == kDefaultCols && v->session().rows() == kDefaultRows; }, waitMs));
        std::printf("default geometry: %dx%d logical -> %d cols x %d rows\n", want.width, want.height,
                    v ? v->session().cols() : -1, v ? v->session().rows() : -1);
        if (v) {
            CHECK(v->session().cols() == kDefaultCols);
            CHECK(v->session().rows() == kDefaultRows);
        }
        // --frame adds the bezel and the window grows so the grid stays 80x24.
        ViewOptions framed = o;
        framed.tube.frame = 1;
        const WindowSize withFrame = defaultWindowSize(framed);
        CHECK(withFrame.width > want.width && withFrame.height > want.height);
        const Profile* amber = findProfile("amber");
        if (amber && amber->fx.frameSize > 0) {
            const int grow = 2 * static_cast<int>(std::lround(amber->fx.frameSize));
            CHECK(withFrame.width >= want.width + grow - 2);
            CHECK(withFrame.height >= want.height + grow - 2);
        }
    }
    testFullscreenKey();
    testFontZoom();
    testZoomOutOfRange();
    testContextMenu();
    testContextMenuWindow();
    testSelectionHasText();
    testCustomProfilesUi();
    testProfileCycle();
    testBrokenProfileFileKept();
    testDeleteSavedDefault();
    removeTree(home);
    const bool left = g_file_test(home.c_str(), G_FILE_TEST_EXISTS);
    CHECK(!left);
    std::printf("ux_test: config folder %s %s\n", home.c_str(), left ? "LEFT BEHIND" : "removed");
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ux_test: all passed\n");
    return failures ? 1 : 0;
}
