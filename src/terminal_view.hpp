// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cpu_effects.hpp"
#include "grid_painter.hpp"
#include "vterm_session.hpp"

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

namespace lterm {

struct ViewOptions {
    bool effects = true;
    int threads = 4;
    std::string fontFamily = "Terminus (TTF), Terminus, DejaVu Sans Mono, Monospace";
    int fontPx = 22;
    std::vector<std::string> command;
    int screenshotAfterMs = 0;   // M0 helper: write the composited frame and quit
    std::string screenshotPath;
};

// The terminal widget: libvterm grid -> Pango/Cairo source image -> CPU effects -> window.
class TerminalView : public Gtk::DrawingArea {
public:
    explicit TerminalView(const ViewOptions& opts);
    ~TerminalView() override;
    bool start();
    VtermSession& session() { return *session_; }
    sigc::signal<void> signalChildExited;
    sigc::signal<void, std::string> signalTitle;

    // Last measured times (ms) for the status/benchmark log.
    double lastPaintMs = 0, lastSourceMs = 0, lastEffectsMs = 0;

protected:
    void on_size_allocate(Gtk::Allocation& a) override;
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
    bool on_key_press_event(GdkEventKey* ev) override;
    void on_realize() override;

private:
    bool onPtyIo(Glib::IOCondition cond);
    bool onTick();
    void layoutGrid();
    double nowMs() const;

    ViewOptions opts_;
    GridPainter painter_;
    std::unique_ptr<VtermSession> session_;
    std::unique_ptr<CpuEffects> effects_;
    Grid grid_;
    int w_ = 0, h_ = 0;
    int marginX_ = 0, marginY_ = 0;
    cairo_surface_t* src_ = nullptr;
    cairo_surface_t* out_ = nullptr;
    std::vector<uint8_t> srcRgba_, outRgba_;
    bool contentDirty_ = true;
    sigc::connection io_, tick_;
    gint64 t0_ = 0;
    bool shotDone_ = false;
};

}  // namespace lterm
