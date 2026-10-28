// SPDX-License-Identifier: GPL-3.0-or-later
// Lunduke Terminal entry point.
//   lunduke-terminal [--profile ID] [--effects|--no-effects] [--renderer auto|gl|cpu]
//                    [--threads N] [--effects-fps N] [--max-fps N] [--no-pause-unfocused]
//                    [--effects-quality auto|full|reduced|static] [--cpu-budget PERCENT]
//                    [--font FAMILY] [--font-px N] [--size WxH] [--quit-after MS] [--stats]
//                    [--scrollback N] [--login] [--cwd DIR] [--on-exit close|hold|hold-on-error]
//                    [--hold] [--bell LIST] [--cursor-shape block|underline|bar]
//                    [--cursor-blink|--no-cursor-blink]
//                    [--list-profiles] [-e COMMAND ARGS...]
// Settings (optional): $XDG_CONFIG_HOME/lunduke-terminal/settings.ini, group [terminal], keys
// profile, effects (true/false), renderer, threads, effects-fps, max-fps, pause-unfocused,
// effects-quality, cpu-budget (percent of one core, software rendering only, 0 = off),
// font, font-px, scrollback-lines, login-shell, cwd, on-exit, bell (comma list of visual,
// urgent, audible or none), cursor-shape, cursor-blink, word-chars. Command-line options win.
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>

namespace {

void parseBell(lterm::ViewOptions& o, const std::string& list) {
    o.bellVisual = o.bellUrgent = o.bellAudible = false;
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string t = list.substr(i, j - i);
        t.erase(0, t.find_first_not_of(' '));
        t.erase(t.find_last_not_of(' ') + 1);
        if (t == "visual") o.bellVisual = true;
        else if (t == "urgent") o.bellUrgent = true;
        else if (t == "audible") o.bellAudible = true;
        else if (!t.empty() && t != "none") std::fprintf(stderr, "lunduke-terminal: unknown bell type '%s'\n", t.c_str());
        i = j + 1;
    }
}

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
        if (kf.has_key(g, "effects-quality") && !parseEffectsQuality(kf.get_string(g, "effects-quality"), o.effectsQuality))
            std::fprintf(stderr, "lunduke-terminal: effects-quality must be auto, full, reduced or static\n");
        if (kf.has_key(g, "cpu-budget")) o.cpuBudget = kf.get_double(g, "cpu-budget") / 100.0;
        if (kf.has_key(g, "pause-unfocused")) o.pauseWhenUnfocused = kf.get_boolean(g, "pause-unfocused");
        if (kf.has_key(g, "font")) o.fontFamily = kf.get_string(g, "font");
        if (kf.has_key(g, "font-px")) o.fontPx = kf.get_integer(g, "font-px");
        if (kf.has_key(g, "scrollback-lines")) o.scrollbackLines = kf.get_int64(g, "scrollback-lines");
        if (kf.has_key(g, "login-shell")) o.loginShell = kf.get_boolean(g, "login-shell");
        if (kf.has_key(g, "cwd")) o.cwd = kf.get_string(g, "cwd");
        if (kf.has_key(g, "on-exit")) o.onExit = kf.get_string(g, "on-exit");
        if (kf.has_key(g, "bell")) parseBell(o, kf.get_string(g, "bell"));
        if (kf.has_key(g, "cursor-shape")) o.cursorShape = kf.get_string(g, "cursor-shape");
        if (kf.has_key(g, "cursor-blink")) o.cursorBlink = kf.get_boolean(g, "cursor-blink");
        if (kf.has_key(g, "word-chars")) o.wordChars = kf.get_string(g, "word-chars");
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
        else if (a == "--effects-quality" && i + 1 < argc) {
            if (!parseEffectsQuality(argv[++i], opts.effectsQuality)) {
                std::fprintf(stderr, "lunduke-terminal: --effects-quality must be auto, full, reduced or static\n");
                return 2;
            }
        } else if (a == "--cpu-budget" && i + 1 < argc) opts.cpuBudget = std::atof(argv[++i]) / 100.0;
        else if (a == "--font" && i + 1 < argc) opts.fontFamily = argv[++i];
        else if (a == "--font-px" && i + 1 < argc) opts.fontPx = std::atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--quit-after" && i + 1 < argc) quitAfter = std::atoi(argv[++i]);
        else if (a == "--stats") opts.stats = true;
        else if (a == "--scrollback" && i + 1 < argc) opts.scrollbackLines = std::strtoll(argv[++i], nullptr, 10);
        else if (a == "--login") opts.loginShell = true;
        else if (a == "--cwd" && i + 1 < argc) opts.cwd = argv[++i];
        else if (a == "--on-exit" && i + 1 < argc) opts.onExit = argv[++i];
        else if (a == "--hold") opts.onExit = "hold";
        else if (a == "--bell" && i + 1 < argc) parseBell(opts, argv[++i]);
        else if (a == "--cursor-shape" && i + 1 < argc) opts.cursorShape = argv[++i];
        else if (a == "--cursor-blink") opts.cursorBlink = true;
        else if (a == "--no-cursor-blink") opts.cursorBlink = false;
        else if (a == "--list-profiles") {
            for (const auto& p : lterm::builtinProfiles()) {
                const auto f = lterm::profileFont(p);
                std::printf("%-16s %-22s font: %s %dpx%s%s\n", p.id.c_str(), (p.label + (p.effects ? "" : " [default]")).c_str(),
                            f.family.c_str(), f.px, f.fallback ? " (fallback)" : "", f.monospace ? "" : " (not monospaced!)");
            }
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
    if (opts.onExit != "close" && opts.onExit != "hold" && opts.onExit != "hold-on-error") {
        std::fprintf(stderr, "lunduke-terminal: --on-exit must be close, hold or hold-on-error\n");
        return 2;
    }
    if (opts.cursorShape != "block" && opts.cursorShape != "underline" && opts.cursorShape != "bar") {
        std::fprintf(stderr, "lunduke-terminal: --cursor-shape must be block, underline or bar\n");
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
    view.signalExitStatus.connect([&](int status) {
        char t[64];
        if (WIFSIGNALED(status)) std::snprintf(t, sizeof t, "[killed: signal %d] ", WTERMSIG(status));
        else std::snprintf(t, sizeof t, "[exited: %d] ", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        win.set_title(t + win.get_title());
    });
    // Bell: urgency hint while the window is not focused; cleared when it gets focus.
    view.signalBell.connect([&] {
        if (!win.is_active()) win.set_urgency_hint(true);
    });
    view.signalBackend.connect([](const std::string& b) {
        std::fprintf(stderr, "lunduke-terminal: %s\n", b.c_str());
    });

    // Pause the effects animation when the window is inactive, iconified or fully covered.
    bool iconified = false, obscured = false, mapped = false;
    auto updateVisible = [&] { view.setWindowVisible(mapped && !iconified && !obscured); };
    win.property_is_active().signal_changed().connect([&] {
        view.setWindowActive(win.is_active());
        if (win.is_active()) win.set_urgency_hint(false);
    });
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
