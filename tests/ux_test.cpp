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
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>

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
    setenv("XDG_CONFIG_HOME", ("/tmp/lterm-ux-test-" + std::to_string(getpid())).c_str(), 1);
    auto app = Gtk::Application::create(argc, argv, "org.lunduke.TerminalUxTest", Gio::APPLICATION_NON_UNIQUE);
    app->register_application();
    testPlainRenders("gl");
    testPlainRenders("cpu");
    testSettingsFontPx();
    testContextMenu();
    testContextMenuWindow();
    testCustomProfilesUi();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ux_test: all passed\n");
    return failures ? 1 : 0;
}
