// SPDX-License-Identifier: GPL-3.0-or-later
// Lunduke Terminal entry point.
//   lunduke-terminal [--profile ID] [--effects|--no-effects] [--renderer auto|gl|cpu]
//                    [--threads N] [--effects-fps N] [--max-fps N] [--no-pause-unfocused]
//                    [--effects-quality auto|full|reduced|static] [--cpu-budget PERCENT]
//                    [--font FAMILY] [--font-px N] [--size WxH] [--quit-after MS] [--stats] [--stats-every S]
//                    [--scrollback N] [--login]
//                    [--cwd|--workdir|--working-directory DIR] [--on-exit close|hold|hold-on-error]
//                    [--hold] [--bell LIST] [--cursor-shape block|underline|bar]
//                    [--cursor-blink|--no-cursor-blink]
//                    [--frame|--no-frame] [--frame-size PX] [--frame-color #RRGGBB]
//                    [--frame-shininess 0..1] [--rgb-shift 0..1]
//                    [-T|--title TITLE] [--list-profiles] [-h|--help] [--version]
//                    [-e|-x|--execute COMMAND ARGS...]
// --quit-after is in milliseconds. --help/--version print to stdout and exit 0; an unknown
// option exits 2, and so does a bad --renderer, --on-exit, --cursor-shape or --effects-quality
// value (other values are not checked).
// Settings (optional): $XDG_CONFIG_HOME/lunduke-terminal/settings.ini, group [terminal], keys
// profile, effects (true/false), renderer, threads, effects-fps, max-fps, pause-unfocused,
// effects-quality, cpu-budget (percent of one core, software rendering only, 0 = off),
// font, font-px, scrollback-lines, login-shell, cwd, on-exit, bell (comma list of visual,
// urgent, audible or none), cursor-shape, cursor-blink, word-chars, frame (true/false),
// frame-size, frame-color, frame-shininess, rgb-shift. Command-line options win.
// The settings dialog (Ctrl+Shift+S) writes profile, font, font-px, effects-quality,
// scrollback-lines and frame there ("Save as default"), keeping the other keys.
// Keys: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab, Ctrl+PgUp/PgDn switch tabs,
// Ctrl+Shift+F search the scrollback, Ctrl+Shift+S settings, F11 fullscreen,
// Ctrl+=/Ctrl++/Ctrl+- font zoom (per tab, not saved), Ctrl+0 zoom reset.
#include "custom_profiles.hpp"
#include "settings_load.hpp"
#include "terminal_window.hpp"

#include <gtkmm.h>

#include <cstdio>
#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>

#ifndef LTERM_VERSION
#define LTERM_VERSION "unknown"
#endif

namespace {

// Keep in sync with the option parser in main() and the README.
const char kUsage[] =
    "Usage: lunduke-terminal [OPTION...] [-e COMMAND [ARGS...]]\n"
    "A terminal emulator with CRT effects.\n"
    "\n"
    "Profile and effects:\n"
    "  --profile ID              color/effects profile (see --list-profiles)\n"
    "  --list-profiles           list the built-in and custom profiles and exit\n"
    "  --effects, --no-effects   force the CRT effects on or off (default: the profile decides)\n"
    "  --renderer auto|gl|cpu    effects back-end (default auto: GL, the C++ path if GL fails)\n"
    "  --effects-quality auto|full|reduced|static\n"
    "                            effects level (default auto: reduced on software rendering)\n"
    "  --effects-fps N           animated effects ticks per second (default 20)\n"
    "  --max-fps N               cap for content updates (default 60)\n"
    "  --cpu-budget PERCENT      software rendering only: slow the animation until painting it\n"
    "                            uses at most PERCENT of one core (default 15, 0 = off)\n"
    "  --threads N               C++ fallback threads (default 4)\n"
    "  --no-pause-unfocused      keep animating while the window is inactive\n"
    "  --frame, --no-frame       bezel on/off; --frame-size PX, --frame-color #RRGGBB,\n"
    "                            --frame-shininess 0..1 tune it\n"
    "  --rgb-shift 0..1          chromatic RGB shift\n"
    "\n"
    "Text and window:\n"
    "  --font FAMILY             font family (default: the profile's)\n"
    "  --font-px N               font size in pixels (default: the profile's)\n"
    "  --size WxH                initial window size in pixels (default: the profile's\n"
    "                            grid, 80x24 or 40x25, in a 4:3 window for retro profiles;\n"
    "                            plain and modern stay a tight 80x24. Fits the screen.)\n"
    "  -T, --title TITLE         window title (also --title=TITLE)\n"
    "  --scrollback N            scrollback lines (default 5000)\n"
    "  --scrollbar auto|on|off   scrollback bar (default auto: on for plain and\n"
    "                            modern, off for CRT profiles so the tube is unchanged)\n"
    "  --cursor-shape block|underline|bar, --cursor-blink, --no-cursor-blink\n"
    "  --bell LIST               comma list of visual, urgent, audible or none\n"
    "                            (default: urgent. No screen flash and no beep.)\n"
    "\n"
    "Session:\n"
    "  -e COMMAND [ARGS...]      run COMMAND instead of the shell. The rest of the line is\n"
    "                            the command and its arguments (passed to exec, not a shell)\n"
    "                            and must be last. -x and --execute are the same form, which\n"
    "                            exo and xfce4-terminal pass\n"
    "  --login                   start the shell as a login shell\n"
    "  --cwd DIR, --workdir DIR, --working-directory DIR\n"
    "                            working directory (also --cwd=DIR, --workdir=DIR,\n"
    "                            --working-directory=DIR). Thunar and other launchers use these\n"
    "  --on-exit close|hold|hold-on-error\n"
    "                            what the tab does when the command exits (default hold-on-error)\n"
    "  --hold                    same as --on-exit hold\n"
    "\n"
    "Testing and diagnostics:\n"
    "  --quit-after MS           close the window after MS milliseconds\n"
    "  --stats                   print frame statistics on exit\n"
    "  --stats-every S           print a memory line on stdout every S seconds\n"
    "  -h, --help                show this help and exit\n"
    "  --version                 show the version and exit\n"
    "\n"
    "Keys:\n"
    "  Ctrl+Shift+T / Ctrl+Shift+W   new tab / close tab\n"
    "  Ctrl+PgUp / Ctrl+PgDn         previous / next tab\n"
    "  Ctrl+Shift+F                  search the scrollback\n"
    "  Ctrl+Shift+S                  settings\n"
    "  Ctrl+Shift+C / Ctrl+Shift+V   copy / paste (Shift+Insert pastes the primary selection)\n"
    "  Ctrl+Shift+E                  toggle the effects\n"
    "  Ctrl+Shift+P                  next profile\n"
    "  Shift+PgUp/PgDn/Home/End      scroll the scrollback\n"
    "  F11                           toggle fullscreen\n"
    "  Ctrl+= or Ctrl++ / Ctrl+-     font 1 px bigger / smaller (this tab only, not saved)\n"
    "  Ctrl+0                        font back to the profile's size (or the --font-px/Settings one)\n"
    "\n"
    "Settings: $XDG_CONFIG_HOME/lunduke-terminal/settings.ini (command-line options win).\n";


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
    std::string text;
    try {
        text = Glib::file_get_contents(path);
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "lunduke-terminal: %s: %s\n", path.c_str(), e.what().c_str());
        return;
    }
    const lterm::SettingsValues s = lterm::parseSettingsIni(text);
    for (const auto& w : s.warnings)
        std::fprintf(stderr, "lunduke-terminal: %s: %s\n", path.c_str(), w.c_str());
    if (s.hasProfile) o.profile = s.profile;
    if (s.hasEffects) o.effects = s.effects ? 1 : 0;
    if (s.hasRenderer) o.renderer = s.renderer;
    if (s.hasThreads) o.threads = s.threads;
    if (s.hasEffectsFps) o.effectsFps = s.effectsFps;
    if (s.hasMaxFps) o.maxFps = s.maxFps;
    if (s.hasEffectsQuality) o.effectsQuality = s.effectsQuality;
    if (s.hasCpuBudget) o.cpuBudget = s.cpuBudget / 100.0;
    if (s.hasPause) o.pauseWhenUnfocused = s.pauseWhenUnfocused;
    if (s.hasFont) o.fontFamily = s.fontFamily;
    if (s.hasFontPx) o.fontPx = s.fontPx;
    if (s.hasScrollback) o.scrollbackLines = s.scrollbackLines;
    if (s.hasLogin) o.loginShell = s.loginShell;
    if (s.hasCwd) o.cwd = s.cwd;
    if (s.hasOnExit) o.onExit = s.onExit;
    if (s.hasBell) parseBell(o, s.bell);
    if (s.hasCursorShape) o.cursorShape = s.cursorShape;
    if (s.hasCursorBlink) o.cursorBlink = s.cursorBlink;
    if (s.hasWordChars) o.wordChars = s.wordChars;
    if (s.hasFrame) o.tube.frame = s.frame;
    if (s.hasFrameSize) o.tube.frameSize = s.frameSize;
    if (s.hasFrameColor) o.tube.frameColor = s.frameColor;
    if (s.hasFrameShininess) o.tube.frameShininess = s.frameShininess;
    if (s.hasRgbShift) o.tube.rgbShift = s.rgbShift;
    if (s.hasScrollbar) o.scrollbar = s.scrollbar;
}

// An option as a separate argument (`--name value`) or, for a long option, `--name=value`.
// 1 = matched and *value set, 0 = not this option, -1 = this option, argument missing
// (a message was printed).
int takeOpt(const std::string& a, const char* name, int& i, int argc, char** argv, std::string& value) {
    const std::string n = name;
    if (a == n) {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "lunduke-terminal: %s requires an argument\nTry 'lunduke-terminal --help'.\n", name);
            return -1;
        }
        value = argv[++i];
        return 1;
    }
    if (n.compare(0, 2, "--") != 0) return 0;
    const std::string pref = n + "=";
    if (a.compare(0, pref.size(), pref) == 0) {
        value = a.substr(pref.size());
        return 1;
    }
    return 0;
}

// First match wins, so the longer directory option is listed before --workdir.
int takeDir(const std::string& a, int& i, int argc, char** argv, std::string& value) {
    if (int t = takeOpt(a, "--working-directory", i, argc, argv, value)) return t;
    if (int t = takeOpt(a, "--workdir", i, argc, argv, value)) return t;
    return takeOpt(a, "--cwd", i, argc, argv, value);
}

int takeTitle(const std::string& a, int& i, int argc, char** argv, std::string& value) {
    if (int t = takeOpt(a, "--title", i, argc, argv, value)) return t;
    return takeOpt(a, "-T", i, argc, argv, value);
}

// `--size 80x24`, `--size=80x24` and `80X24`.
bool parseWindowSize(const std::string& s, int& w, int& h) {
    char sep = 0;
    if (std::sscanf(s.c_str(), "%d%c%d", &w, &sep, &h) != 3) return false;
    if (sep != 'x' && sep != 'X') return false;
    return w > 0 && h > 0;
}

}  // namespace

int main(int argc, char** argv) {
    // --help / --version before anything reads settings or profiles (their warnings would be noise).
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-e" || a == "-x" || a == "--execute") break;  // the rest belongs to the command
        if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (a == "--version") {
            std::printf("lunduke-terminal %s\n", LTERM_VERSION);
            return 0;
        }
    }
    lterm::ViewOptions opts;
    {
        // M6 item 4: custom profiles (profiles/*.ini next to settings.ini) before the settings and
        // --profile pick one; a corrupt file is skipped with a message.
        std::vector<std::string> warnings;
        lterm::reloadCustomProfiles(&warnings);
        for (const auto& w : warnings) std::fprintf(stderr, "lunduke-terminal: profile %s\n", w.c_str());
    }
    loadSettings(opts);
    int w = 0, h = 0, quitAfter = 0;
    bool sizeSet = false;
    int statsEvery = 0;  // --stats-every: seconds between memory lines (M4 item 4)
    std::string title = "Lunduke Terminal";  // -T/--title (x-terminal-emulator interface)
    std::string optArg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-effects") opts.effects = 0;
        else if (a == "--effects") opts.effects = 1;
        else if (int taken = takeOpt(a, "--profile", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.profile = optArg;
        }
        else if (int taken = takeOpt(a, "--renderer", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.renderer = optArg;
        } else if (int taken = takeOpt(a, "--threads", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.threads = std::atoi(optArg.c_str());
        } else if (int taken = takeOpt(a, "--effects-fps", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.effectsFps = std::atoi(optArg.c_str());
        } else if (int taken = takeOpt(a, "--max-fps", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.maxFps = std::atoi(optArg.c_str());
        } else if (a == "--no-pause-unfocused") opts.pauseWhenUnfocused = false;
        else if (int taken = takeOpt(a, "--effects-quality", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            if (!parseEffectsQuality(optArg, opts.effectsQuality)) {
                std::fprintf(stderr, "lunduke-terminal: --effects-quality must be auto, full, reduced or static\n");
                return 2;
            }
        } else if (int taken = takeOpt(a, "--cpu-budget", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.cpuBudget = std::atof(optArg.c_str()) / 100.0;
        } else if (int taken = takeOpt(a, "--font", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.fontFamily = optArg;
        } else if (int taken = takeOpt(a, "--font-px", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.fontPx = std::atoi(optArg.c_str());
        } else if (int taken = takeOpt(a, "--size", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            if (!parseWindowSize(optArg, w, h)) {
                std::fprintf(stderr, "lunduke-terminal: --size must be WxH (for example 80x24)\n");
                return 2;
            }
            sizeSet = true;
        } else if (int taken = takeOpt(a, "--quit-after", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            quitAfter = std::atoi(optArg.c_str());
        } else if (a == "--stats") opts.stats = true;
        else if (int taken = takeOpt(a, "--stats-every", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            statsEvery = std::atoi(optArg.c_str());
        } else if (int taken = takeOpt(a, "--scrollback", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.scrollbackLines = std::strtoll(optArg.c_str(), nullptr, 10);
        } else if (a == "--login") opts.loginShell = true;
        else if (int taken = takeDir(a, i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.cwd = optArg;
        } else if (int taken = takeTitle(a, i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            title = optArg;
        } else if (int taken = takeOpt(a, "--on-exit", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.onExit = optArg;
        } else if (a == "--hold") opts.onExit = "hold";
        else if (int taken = takeOpt(a, "--bell", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            parseBell(opts, optArg);
        } else if (int taken = takeOpt(a, "--cursor-shape", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.cursorShape = optArg;
        } else if (a == "--cursor-blink") opts.cursorBlink = true;
        else if (a == "--no-cursor-blink") opts.cursorBlink = false;
        else if (a == "--frame") opts.tube.frame = 1;
        else if (a == "--no-frame") opts.tube.frame = 0;
        else if (int taken = takeOpt(a, "--frame-size", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.tube.frameSize = static_cast<float>(std::atof(optArg.c_str()));
        } else if (int taken = takeOpt(a, "--frame-color", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.tube.frameColor = optArg;
        } else if (int taken = takeOpt(a, "--frame-shininess", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.tube.frameShininess = static_cast<float>(std::atof(optArg.c_str()));
        } else if (int taken = takeOpt(a, "--rgb-shift", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            opts.tube.rgbShift = static_cast<float>(std::atof(optArg.c_str()));
        } else if (int taken = takeOpt(a, "--scrollbar", i, argc, argv, optArg)) {
            if (taken < 0) return 2;
            if (optArg == "auto") opts.scrollbar = -1;
            else if (optArg == "on") opts.scrollbar = 1;
            else if (optArg == "off") opts.scrollbar = 0;
            else {
                std::fprintf(stderr, "lunduke-terminal: --scrollbar must be auto, on or off\n");
                return 2;
            }
        }
        else if (a == "--list-profiles") {
            std::fputs(lterm::profileListText().c_str(), stdout);
            return 0;
        } else if (a == "-e" || a == "-x" || a == "--execute") {
            for (++i; i < argc; ++i) opts.command.push_back(argv[i]);
        } else {
            std::fprintf(stderr, "lunduke-terminal: unknown option %s\nTry 'lunduke-terminal --help'.\n",
                         a.c_str());
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
    // First launch only. A --size wins. A window manager that restores a previous size paints
    // over this default; nothing here writes a geometry back, so a saved size is not reset.
    if (!sizeSet) {
        const lterm::WindowSize s = lterm::defaultWindowSize(opts);
        w = s.width;
        h = s.height;
    }
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
