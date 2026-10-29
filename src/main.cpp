// SPDX-License-Identifier: GPL-3.0-or-later
// Lunduke Terminal entry point.
//   lunduke-terminal [--profile ID] [--effects|--no-effects] [--renderer auto|gl|cpu]
//                    [--threads N] [--effects-fps N] [--max-fps N] [--no-pause-unfocused]
//                    [--effects-quality auto|full|reduced|static] [--cpu-budget PERCENT]
//                    [--font FAMILY] [--font-px N] [--size WxH] [--quit-after MS] [--stats] [--stats-every S]
//                    [--scrollback N] [--login] [--cwd DIR] [--on-exit close|hold|hold-on-error]
//                    [--hold] [--bell LIST] [--cursor-shape block|underline|bar]
//                    [--cursor-blink|--no-cursor-blink]
//                    [--frame|--no-frame] [--frame-size PX] [--frame-color #RRGGBB]
//                    [--frame-shininess 0..1] [--rgb-shift 0..1]
//                    [-T|--title TITLE] [--list-profiles] [-e COMMAND ARGS...]
// Settings (optional): $XDG_CONFIG_HOME/lunduke-terminal/settings.ini, group [terminal], keys
// profile, effects (true/false), renderer, threads, effects-fps, max-fps, pause-unfocused,
// effects-quality, cpu-budget (percent of one core, software rendering only, 0 = off),
// font, font-px, scrollback-lines, login-shell, cwd, on-exit, bell (comma list of visual,
// urgent, audible or none), cursor-shape, cursor-blink, word-chars, frame (true/false),
// frame-size, frame-color, frame-shininess, rgb-shift. Command-line options win.
// The settings dialog (Ctrl+Shift+S) writes profile, font, font-px, effects-quality,
// scrollback-lines and frame there ("Save as default"), keeping the other keys.
// Keys: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab, Ctrl+PgUp/PgDn switch tabs,
// Ctrl+Shift+F search the scrollback, Ctrl+Shift+S settings.
#include "terminal_window.hpp"

#include <gtkmm.h>

#include <cstdio>
#include <malloc.h>
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
        if (kf.has_key(g, "frame")) o.tube.frame = kf.get_boolean(g, "frame") ? 1 : 0;
        if (kf.has_key(g, "frame-size")) o.tube.frameSize = static_cast<float>(kf.get_double(g, "frame-size"));
        if (kf.has_key(g, "frame-color")) o.tube.frameColor = kf.get_string(g, "frame-color");
        if (kf.has_key(g, "frame-shininess")) o.tube.frameShininess = static_cast<float>(kf.get_double(g, "frame-shininess"));
        if (kf.has_key(g, "rgb-shift")) o.tube.rgbShift = static_cast<float>(kf.get_double(g, "rgb-shift"));
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "lunduke-terminal: %s: %s\n", path.c_str(), e.what().c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    lterm::ViewOptions opts;
    loadSettings(opts);
    int w = 1280, h = 800, quitAfter = 0;
    int statsEvery = 0;  // --stats-every: seconds between memory lines (M4 item 4)
    std::string title = "Lunduke Terminal";  // -T/--title (x-terminal-emulator interface)
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
        else if (a == "--stats-every" && i + 1 < argc) statsEvery = std::atoi(argv[++i]);
        else if ((a == "-T" || a == "--title") && i + 1 < argc) title = argv[++i];
        else if (a == "--scrollback" && i + 1 < argc) opts.scrollbackLines = std::strtoll(argv[++i], nullptr, 10);
        else if (a == "--login") opts.loginShell = true;
        else if (a == "--cwd" && i + 1 < argc) opts.cwd = argv[++i];
        else if (a == "--on-exit" && i + 1 < argc) opts.onExit = argv[++i];
        else if (a == "--hold") opts.onExit = "hold";
        else if (a == "--bell" && i + 1 < argc) parseBell(opts, argv[++i]);
        else if (a == "--cursor-shape" && i + 1 < argc) opts.cursorShape = argv[++i];
        else if (a == "--cursor-blink") opts.cursorBlink = true;
        else if (a == "--no-cursor-blink") opts.cursorBlink = false;
        else if (a == "--frame") opts.tube.frame = 1;
        else if (a == "--no-frame") opts.tube.frame = 0;
        else if (a == "--frame-size" && i + 1 < argc) opts.tube.frameSize = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--frame-color" && i + 1 < argc) opts.tube.frameColor = argv[++i];
        else if (a == "--frame-shininess" && i + 1 < argc) opts.tube.frameShininess = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--rgb-shift" && i + 1 < argc) opts.tube.rgbShift = static_cast<float>(std::atof(argv[++i]));
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
    auto app = Gtk::Application::create(gargc, argv, "org.lunduke.LundukeTerminal", Gio::APPLICATION_NON_UNIQUE);
    Gtk::Window::set_default_icon_name("org.lunduke.LundukeTerminal");  // data/icons (meson install)
    lterm::TerminalWindow win(opts, title);
    win.set_default_size(w, h);
    win.show();
    if (!win.newTab(true)) return 1;
    if (quitAfter > 0) Glib::signal_timeout().connect_once([&] { win.hide(); }, quitAfter);
    if (statsEvery > 0) {
        // M4 item 4: memory accounting every N s on stdout (heap from glibc, the rest from the views).
        const gint64 t0 = g_get_monotonic_time();
        Glib::signal_timeout().connect_seconds([&win, t0] {
            const struct mallinfo2 mi = mallinfo2();
            lterm::ViewMemory sum;
            for (int i = 0; i < win.tabCount(); ++i)
                if (lterm::TerminalView* v = win.view(i)) {
                    const lterm::ViewMemory m = v->memory();
                    sum.scrollbackLines += m.scrollbackLines;
                    sum.scrollbackBytes += m.scrollbackBytes;
                    sum.glyphs += m.glyphs;
                    sum.glyphBytes += m.glyphBytes;
                    sum.textures += m.textures;
                    sum.textureBytes += m.textureBytes;
                    sum.surfaceBytes += m.surfaceBytes;
                    sum.statSamples += m.statSamples;
                }
            std::printf("stats-mem t=%.0f tabs=%d heap-arena=%zu heap-mmap=%zu heap-mmap-n=%zu heap-inuse=%zu "
                        "heap-free=%zu heap-releasable=%zu sb-lines=%zu sb-bytes=%zu glyphs=%zu glyph-bytes=%zu "
                        "textures=%d texture-bytes=%zu surface-bytes=%zu stat-samples=%zu\n",
                        (g_get_monotonic_time() - t0) / 1e6, win.tabCount(), mi.arena, mi.hblkhd, mi.hblks,
                        mi.uordblks + mi.hblkhd, mi.fordblks, mi.keepcost, sum.scrollbackLines, sum.scrollbackBytes,
                        sum.glyphs, sum.glyphBytes, sum.textures, sum.textureBytes, sum.surfaceBytes,
                        sum.statSamples);
            std::fflush(stdout);
            return true;
        }, static_cast<unsigned>(statsEvery));
    }
    return app->run(win);
}
