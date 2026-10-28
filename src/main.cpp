// SPDX-License-Identifier: GPL-3.0-or-later
// Lunduke Terminal - M0 spike entry point.
//   lunduke-terminal [--no-effects] [--threads N] [--font FAMILY] [--font-px N]
//                    [--size WxH] [--screenshot-after MS PATH] [-e COMMAND ARGS...]
#include "terminal_view.hpp"

#include <gtkmm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    lterm::ViewOptions opts;
    int w = 1280, h = 800;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-effects") opts.effects = false;
        else if (a == "--threads" && i + 1 < argc) opts.threads = std::atoi(argv[++i]);
        else if (a == "--font" && i + 1 < argc) opts.fontFamily = argv[++i];
        else if (a == "--font-px" && i + 1 < argc) opts.fontPx = std::atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--screenshot-after" && i + 2 < argc) {
            opts.screenshotAfterMs = std::atoi(argv[++i]);
            opts.screenshotPath = argv[++i];
        } else if (a == "-e") {
            for (++i; i < argc; ++i) opts.command.push_back(argv[i]);
        } else {
            std::fprintf(stderr, "lunduke-terminal: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    int gargc = 1;
    auto app = Gtk::Application::create(gargc, argv, "org.lunduke.Terminal.M0",
                                        Gio::APPLICATION_NON_UNIQUE);
    Gtk::Window win;
    win.set_title("Lunduke Terminal (M0 spike)");
    win.set_default_size(w, h);
    lterm::TerminalView view(opts);
    win.add(view);
    view.signalTitle.connect([&](const std::string& t) { win.set_title(t + " - Lunduke Terminal"); });
    view.signalChildExited.connect([&] { win.hide(); });
    win.show_all();
    if (!view.start()) {
        std::fprintf(stderr, "lunduke-terminal: could not start the shell\n");
        return 1;
    }
    return app->run(win);
}
