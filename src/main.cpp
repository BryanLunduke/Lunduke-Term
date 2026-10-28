// SPDX-License-Identifier: GPL-3.0-or-later
// Lunduke Terminal entry point.
//   lunduke-terminal [--profile ID] [--effects|--no-effects] [--renderer auto|gl|cpu]
//                    [--threads N] [--effects-fps N] [--max-fps N] [--no-pause-unfocused]
//                    [--font FAMILY] [--font-px N] [--size WxH] [--quit-after MS] [--stats]
//                    [--list-profiles] [-e COMMAND ARGS...]
// Settings (optional): $XDG_CONFIG_HOME/lunduke-terminal/settings.ini, group [terminal], keys
// profile, effects (true/false), renderer, threads, effects-fps, max-fps, pause-unfocused,
// font, font-px. Command-line options win.
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void loadSettings(lterm::ViewOptions& o) {
    const std::string path = Glib::build_filename(Glib::get_user_config_dir(), "lunduke-terminal", "settings.ini");
    if (!Glib::file_test(path, Glib::FILE_TEST_EXISTS)) return;
    Glib::KeyFile kf;
    try {
        kf.load_from_file(path);
        const char* g = "terminal";
        if (kf.has_key(g, "profile")) o.profile = kf.get_string(g, "profile");
        if (kf.has_key(g, "effects")) o.effects = kf.get_boolean(g, "effects") ? 1 : 0;
        if (kf.has_key(g, "renderer")) o.renderer = kf.get_string(g, "renderer");
        if (kf.has_key(g, "threads")) o.threads = kf.get_integer(g, "threads");
        if (kf.has_key(g, "effects-fps")) o.effectsFps = kf.get_integer(g, "effects-fps");
        if (kf.has_key(g, "max-fps")) o.maxFps = kf.get_integer(g, "max-fps");
        if (kf.has_key(g, "pause-unfocused")) o.pauseWhenUnfocused = kf.get_boolean(g, "pause-unfocused");
        if (kf.has_key(g, "font")) o.fontFamily = kf.get_string(g, "font");
        if (kf.has_key(g, "font-px")) o.fontPx = kf.get_integer(g, "font-px");
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "lunduke-terminal: %s: %s\n", path.c_str(), e.what().c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    lterm::ViewOptions opts;
    loadSettings(opts);
    int w = 1280, h = 800, quitAfter = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-effects") opts.effects = 0;
        else if (a == "--effects") opts.effects = 1;
        else if (a == "--profile" && i + 1 < argc) opts.profile = argv[++i];
        else if (a == "--renderer" && i + 1 < argc) opts.renderer = argv[++i];
        else if (a == "--threads" && i + 1 < argc) opts.threads = std::atoi(argv[++i]);
        else if (a == "--effects-fps" && i + 1 < argc) opts.effectsFps = std::atoi(argv[++i]);
        else if (a == "--max-fps" && i + 1 < argc) opts.maxFps = std::atoi(argv[++i]);
        else if (a == "--no-pause-unfocused") opts.pauseWhenUnfocused = false;
        else if (a == "--font" && i + 1 < argc) opts.fontFamily = argv[++i];
        else if (a == "--font-px" && i + 1 < argc) opts.fontPx = std::atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--quit-after" && i + 1 < argc) quitAfter = std::atoi(argv[++i]);
        else if (a == "--stats") opts.stats = true;
        else if (a == "--list-profiles") {
            for (const auto& p : lterm::builtinProfiles())
                std::printf("%-16s %s%s\n", p.id.c_str(), p.label.c_str(), p.effects ? "" : " [default]");
            return 0;
        } else if (a == "-e") {
            for (++i; i < argc; ++i) opts.command.push_back(argv[i]);
        } else {
            std::fprintf(stderr, "lunduke-terminal: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (opts.renderer != "auto" && opts.renderer != "gl" && opts.renderer != "cpu") {
        std::fprintf(stderr, "lunduke-terminal: --renderer must be auto, gl or cpu\n");
        return 2;
    }
    int gargc = 1;
    auto app = Gtk::Application::create(gargc, argv, "org.lunduke.Terminal", Gio::APPLICATION_NON_UNIQUE);
    Gtk::Window win;
    win.set_title("Lunduke Terminal");
    win.set_default_size(w, h);
    win.add_events(Gdk::VISIBILITY_NOTIFY_MASK | Gdk::STRUCTURE_MASK);
    lterm::TerminalView view(opts);
    win.add(view);
    view.signalTitle.connect([&](const std::string& t) { win.set_title(t + " - Lunduke Terminal"); });
    view.signalChildExited.connect([&] { win.hide(); });
    view.signalBackend.connect([](const std::string& b) {
        std::fprintf(stderr, "lunduke-terminal: %s\n", b.c_str());
    });

    // Pause the effects animation when the window is inactive, iconified or fully covered.
    bool iconified = false, obscured = false, mapped = false;
    auto updateVisible = [&] { view.setWindowVisible(mapped && !iconified && !obscured); };
    win.property_is_active().signal_changed().connect([&] { view.setWindowActive(win.is_active()); });
    win.signal_window_state_event().connect([&](GdkEventWindowState* e) {
        iconified = (e->new_window_state & (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_WITHDRAWN)) != 0;
        updateVisible();
        return false;
    });
    win.signal_visibility_notify_event().connect([&](GdkEventVisibility* e) {
        obscured = e->state == GDK_VISIBILITY_FULLY_OBSCURED;
        updateVisible();
        return false;
    });
    win.signal_map_event().connect([&](GdkEventAny*) {
        mapped = true;
        updateVisible();
        return false;
    });
    win.signal_unmap_event().connect([&](GdkEventAny*) {
        mapped = false;
        updateVisible();
        return false;
    });

    win.show_all();
    view.setWindowActive(win.is_active());
    if (!view.start()) {
        std::fprintf(stderr, "lunduke-terminal: could not start the shell\n");
        return 1;
    }
    if (quitAfter > 0) Glib::signal_timeout().connect_once([&] { win.hide(); }, quitAfter);
    return app->run(win);
}
