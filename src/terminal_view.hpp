// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cpu_effects.hpp"
#include "gl_pipeline.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"
#include "search.hpp"
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
    // M2: auto (reduced on software rendering) | full | reduced | static, see effects.hpp
    EffectsQuality effectsQuality = EffectsQuality::Auto;
    // Software rendering only: tick slower until the animation uses at most this fraction of
    // one core (process CPU time, so llvmpipe's threads count). 0 disables the governor.
    double cpuBudget = 0.25;
    bool pauseWhenUnfocused = true; // stop the animation (not content updates) when inactive
    std::string fontFamily;         // empty: the profile's
    int fontPx = 0;                 // 0: the profile's
    std::vector<std::string> command;
    bool stats = false;             // print frame statistics on exit
    // Checkpoint B
    long long scrollbackLines = 10000;  // clamped to 0..kMaxScrollbackLines
    bool loginShell = false;
    std::string cwd;
    std::string onExit = "hold-on-error";  // close | hold | hold-on-error
    bool bellVisual = true, bellUrgent = true, bellAudible = false;
    std::string cursorShape = "block";  // block | underline | bar (DECSCUSR overrides)
    bool cursorBlink = false;           // DECSCUSR / ?12 override
    std::string wordChars = Selection::defaultWordChars();
    // M2 item 5: bezel / RGB shift overrides of the profile (see profiles.hpp).
    TubeOverrides tube;
};

// Frame statistics (ms), kept for --stats and the benchmarks in docs/.
struct ViewStats {
    long contentFrames = 0, ticks = 0, rowsPainted = 0, fullRepaints = 0;
    std::vector<double> paintMs, effectsSourceMs, tickMs;
    double finalTickIntervalMs = 0;  // governor state on exit
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
    sigc::signal<void> signalChildExited;  // close the window (child gone and not held)
    sigc::signal<void, int> signalExitStatus;  // raw wait status, emitted once
    sigc::signal<void> signalBell;             // for the urgency hint (main decides)
    sigc::signal<void, std::string> signalTitle;
    sigc::signal<void, std::string> signalBackend;  // backend description changed

    void setWindowActive(bool active);
    void setWindowVisible(bool visible);
    void setEffects(bool on);
    bool effectsOn() const { return effectsOn_; }
    void setProfile(const std::string& id);
    const Profile& profile() const { return profile_; }
    std::string backendName() const;
    EffectsQuality effectsQuality() const { return quality_; }  // resolved (never Auto)
    bool softwareRendering() const { return software_; }
    double tickIntervalMs() const { return tickIntervalMs_; }
    int scaleFactor() const { return scale_; }
    int deviceWidth() const { return w_; }
    int deviceHeight() const { return h_; }
    // Test hook: the cell under a pointer position in logical (event) coordinates.
    bool cellAtLogical(double x, double y, int& row, int& col) const { return cellAt(x, y, row, col); }
    // Test hooks: pointer events at logical coordinates, as GTK would deliver them.
    void testButton(bool press, int button, double x, double y, guint state = 0);
    void testMotion(double x, double y, guint state);
    bool bezelActive() const { return frameActive(); }
    int marginX() const { return marginX_; }
    int marginY() const { return marginY_; }
    const ViewStats& stats() const { return stats_; }
    int scrollOffset() const { return scrollOffset_; }
    const Selection& selection() const { return sel_; }
    std::string selectionText() const { return sel_.text(*session_); }
    void copyClipboard();
    void pasteClipboard(bool primary);

    // M2 stage 3: scrollback search. setSearch() finds every match (history + screen), highlights
    // them and goes to the newest one; searchStep() moves to the next/previous one (wrapping) and
    // scrolls it into view. Output that arrives while a search is open re-runs it (debounced).
    int setSearch(const std::string& query, bool caseSensitive);
    bool searchStep(bool backward);
    void clearSearch();
    int searchCount() const { return static_cast<int>(matches_.size()); }
    int searchIndex() const { return matchIndex_; }  // -1: none
    const std::vector<SearchMatch>& searchMatches() const { return matches_; }
    sigc::signal<void> signalSearchChanged;  // matches or the current match changed
    // Settings dialog: live changes (all keep the session and its scrollback).
    const ViewOptions& options() const { return opts_; }
    void setFont(const std::string& family, int px);  // "" / 0: the profile's
    void setEffectsQuality(EffectsQuality q);
    void setScrollbackLines(long long lines);
    void setFrame(int frame);  // -1 profile default, 0 off, 1 on (only profiles with a bezel option)
    pid_t childPid() const { return session_->childPid(); }
    void grabTerminalFocus();
    // Test hooks: the last composed grid (what is painted), the pause state.
    const Grid& shownGrid() const { return cur_; }
    bool windowVisible() const { return visible_; }
    bool animating() const { return tick_.connected(); }
    bool terminalHasFocus() const;

protected:
    void on_size_allocate(Gtk::Allocation& a) override;

private:
    std::string fontWarned_;  // the --font value already warned about
    enum class Mode { Plain, Cpu, Gl };
    bool onKey(GdkEventKey* ev);
    bool onButtonPress(GdkEventButton* ev);
    bool onButtonRelease(GdkEventButton* ev);
    bool onMotion(GdkEventMotion* ev);
    bool onScroll(GdkEventScroll* ev);
    bool cellAt(double x, double y, int& row, int& col) const;
    bool frameActive() const;      // effects on with a bezel
    bool mouseReporting(guint state) const;
    void scrollBy(int lines);  // + = back into history
    void snapToBottom();
    void selectionChanged();
    void onChildExit(int status);
    void onBell();
    void updateBlink();
    void sendKeyInput();       // common handling for anything typed
    void ensureWriteWatch();
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
    void relayout(int logicalW, int logicalH);
    void onScaleChanged();     // notify::scale-factor
    void applyScale();
    void allocateSurfaces();
    bool updateContent();      // grid -> source image, changed rows only; true if anything changed
    void requestFrame();       // capped content frame
    void onFrameTimer();
    void updateAnimation();    // start/stop the effects tick
    bool onTick();
    double nowMs() const;
    bool animationWanted() const;
    void updateQuality();          // resolve effects-quality for the current renderer
    double baseTickIntervalMs() const;
    bool governTick();             // software rendering: keep the idle animation within cpuBudget

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
    // M2 effects quality: the profile's parameters after applyEffectsQuality().
    EffectParams fx_;
    EffectsQuality quality_ = EffectsQuality::Full;
    bool software_ = false;
    bool qualityDemoted_ = false;  // auto fell back to static: the budget was out of reach
    double tickIntervalMs_ = 50;
    double govWall0_ = 0, govCpu0_ = 0;
    bool cpuOutValid_ = false;     // out_ holds the effects output of the current source

    Grid cur_, next_;
    int w_ = 0, h_ = 0;        // device pixels (logical size * scale_)
    int lw_ = 0, lh_ = 0;      // logical allocation
    int scale_ = 1;            // GTK scale factor (HiDPI)
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
    // Checkpoint B state
    int scrollOffset_ = 0;
    Selection sel_;
    bool selecting_ = false;
    std::string selSnapshot_;  // the selected text; the selection is dropped when it changes
    int pressedButtons_ = 0;  // bitmask of buttons reported to the application
    double wheelAccum_ = 0;
    bool exited_ = false, holding_ = false;
    guint childWatch_ = 0;
    sigc::connection writeWatch_, bellTimer_, blink_;
    double lastBellMs_ = -1e9;
    bool bellFlash_ = false;
    bool inverseShown_ = false;  // margins currently painted for reverse video
    bool blinkOff_ = false;
    bool lastAlt_ = false;
    ViewStats stats_;
    // Search state
    void refreshSearch();         // re-run the query, keep the current match where it was
    void revealMatch(int index);  // scroll so the match is visible
    std::string searchQuery_;
    bool searchCase_ = false;
    std::vector<SearchMatch> matches_;
    int matchIndex_ = -1;
    sigc::connection searchRefresh_;
    // Async callbacks (clipboard requests) hold a weak_ptr to this; it expires in the
    // destructor, so a reply that arrives after the window closed is ignored.
    std::shared_ptr<char> alive_ = std::make_shared<char>(0);
};

}  // namespace lterm
