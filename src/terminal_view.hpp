// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cpu_effects.hpp"
#include "gl_pipeline.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"
#include "vterm_session.hpp"

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

namespace lterm {

struct ViewOptions {
    std::string profile = "plain";
    int effects = -1;               // -1: the profile decides, 0: off, 1: on
    std::string renderer = "auto";  // auto (GL, C++ if GL fails) | gl | cpu
    int threads = 4;                // C++ fallback threads
    int effectsFps = 20;            // animated effects tick (cool-retro-term 1.2.0 also uses 20)
    int maxFps = 60;                // cap for content updates
    bool pauseWhenUnfocused = true; // stop the animation (not content updates) when inactive
    std::string fontFamily;         // empty: the profile's
    int fontPx = 0;                 // 0: the profile's
    std::vector<std::string> command;
    bool stats = false;             // print frame statistics on exit
};

// Frame statistics (ms), kept for --stats and the benchmarks in docs/.
struct ViewStats {
    long contentFrames = 0, ticks = 0, rowsPainted = 0, fullRepaints = 0;
    std::vector<double> paintMs, effectsSourceMs, tickMs;
};

// The terminal widget. Pipeline: libvterm -> Grid -> glyph-cached Pango/Cairo source image
// (changed rows only) -> either a plain blit (DrawingArea), the GLSL effects on a GtkGLArea
// (main path), or the multithreaded C++ effects (fallback, same output).
class TerminalView : public Gtk::Box {
public:
    explicit TerminalView(const ViewOptions& opts);
    ~TerminalView() override;
    bool start();
    VtermSession& session() { return *session_; }
    sigc::signal<void> signalChildExited;
    sigc::signal<void, std::string> signalTitle;
    sigc::signal<void, std::string> signalBackend;  // backend description changed

    void setWindowActive(bool active);
    void setWindowVisible(bool visible);
    void setEffects(bool on);
    bool effectsOn() const { return effectsOn_; }
    void setProfile(const std::string& id);
    const Profile& profile() const { return profile_; }
    std::string backendName() const;
    const ViewStats& stats() const { return stats_; }

protected:
    void on_size_allocate(Gtk::Allocation& a) override;

private:
    enum class Mode { Plain, Cpu, Gl };
    bool onKey(GdkEventKey* ev);
    bool onPtyIo(Glib::IOCondition cond);
    bool onDraw(const Cairo::RefPtr<Cairo::Context>& cr);
    bool onGlRender(const Glib::RefPtr<Gdk::GLContext>& ctx);
    void onGlRealize();
    void onGlUnrealize();
    void applyProfile();
    void applyColors();
    void chooseMode();
    void fallbackToCpu(const std::string& why);
    void layoutGrid();
    void allocateSurfaces();
    bool updateContent();      // grid -> source image, changed rows only; true if anything changed
    void requestFrame();       // capped content frame
    void onFrameTimer();
    void updateAnimation();    // start/stop the effects tick
    bool onTick();
    double nowMs() const;
    bool animationWanted() const;

    ViewOptions opts_;
    Profile profile_;
    std::unique_ptr<GridPainter> painter_;
    std::unique_ptr<VtermSession> session_;
    Gtk::DrawingArea area_;
    Gtk::GLArea gl_;
    Mode mode_ = Mode::Plain;
    bool effectsOn_ = false;
    bool glFailed_ = false;
    std::string glRenderer_, glError_;
    std::unique_ptr<CpuEffects> cpu_;
    std::unique_ptr<GlPipeline> glPipe_;
    bool glParamsStale_ = false;

    Grid cur_, next_;
    int w_ = 0, h_ = 0;
    int marginX_ = 0, marginY_ = 0;
    cairo_surface_t* src_ = nullptr;   // RGB24 source image (what the plain view shows)
    cairo_surface_t* out_ = nullptr;   // RGB24 C++ effects output
    bool contentDirty_ = true;
    bool fullRepaint_ = true;
    int srcDirtyY0_ = 0, srcDirtyY1_ = 0;  // source rows not yet handed to the effects
    bool effectsSourceStale_ = true;
    double lastContentMs_ = -1e9;
    double lastFrameMs_ = -1e9;
    bool active_ = true, visible_ = true;
    sigc::connection io_, tick_, frameTimer_;
    gint64 t0_ = 0;
    ViewStats stats_;
};

}  // namespace lterm
