// SPDX-License-Identifier: GPL-3.0-or-later
// M6 item 6: memory baseline of an empty gtkmm-3 window, for comparison with Lunduke Terminal.
// Usage: gtk_baseline [drawing|glarea][-1280]  (glarea realizes a Gtk::GLArea and draws one frame; -1280: 1280x800, else 1024x640)
#include <gtkmm.h>
#include <epoxy/gl.h>
#include <cstring>
int main(int argc, char** argv) {
    const bool gl = argc > 1 && std::strncmp(argv[1], "glarea", 6) == 0;
    const bool big = argc > 1 && std::strstr(argv[1], "1280");
    int one = 1;
    auto app = Gtk::Application::create(one, argv, "org.lunduke.gtkbaseline", Gio::APPLICATION_NON_UNIQUE);
    Gtk::Window win;
    win.set_default_size(big ? 1280 : 1024, big ? 800 : 640);
    Gtk::DrawingArea da;
    Gtk::GLArea ga;
    if (gl) {
        ga.signal_render().connect([](const Glib::RefPtr<Gdk::GLContext>&) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            return true;
        });
        win.add(ga);
    } else {
        win.add(da);
    }
    win.show_all();
    return app->run(win);
}
