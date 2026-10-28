// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal_view.hpp"

#include <epoxy/gl.h>
#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>

namespace lterm {

namespace {
VTermModifier modsOf(guint state) {
    int m = VTERM_MOD_NONE;
    if (state & GDK_SHIFT_MASK) m |= VTERM_MOD_SHIFT;
    if (state & GDK_CONTROL_MASK) m |= VTERM_MOD_CTRL;
    if (state & GDK_MOD1_MASK) m |= VTERM_MOD_ALT;
    return static_cast<VTermModifier>(m);
}

bool mapKey(guint keyval, VTermKey& out) {
    switch (keyval) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: out = VTERM_KEY_ENTER; return true;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: out = VTERM_KEY_TAB; return true;
    case GDK_KEY_BackSpace: out = VTERM_KEY_BACKSPACE; return true;
    case GDK_KEY_Escape: out = VTERM_KEY_ESCAPE; return true;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: out = VTERM_KEY_UP; return true;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: out = VTERM_KEY_DOWN; return true;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: out = VTERM_KEY_LEFT; return true;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: out = VTERM_KEY_RIGHT; return true;
    case GDK_KEY_Insert: out = VTERM_KEY_INS; return true;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: out = VTERM_KEY_DEL; return true;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: out = VTERM_KEY_HOME; return true;
    case GDK_KEY_End: case GDK_KEY_KP_End: out = VTERM_KEY_END; return true;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: out = VTERM_KEY_PAGEUP; return true;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: out = VTERM_KEY_PAGEDOWN; return true;
    default: break;
    }
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12) {
        out = static_cast<VTermKey>(VTERM_KEY_FUNCTION(static_cast<int>(keyval - GDK_KEY_F1 + 1)));
        return true;
    }
    return false;
}
}  // namespace
TerminalView::TerminalView(const ViewOptions& opts)
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL), opts_(opts) {
    const Profile* p = findProfile(opts_.profile);
    if (!p) {
        std::fprintf(stderr, "lunduke-terminal: unknown profile '%s', using plain\n", opts_.profile.c_str());
        p = findProfile("plain");
    }
    profile_ = *p;
    effectsOn_ = opts_.effects < 0 ? profile_.effects : opts_.effects != 0;
    glFailed_ = opts_.renderer == "cpu";
    session_.reset(new VtermSession(24, 80));
    session_->onTitle = [this] { signalTitle.emit(session_->title()); };
    session_->onBell = [this] { onBell(); };
    session_->setScrollbackLimit(static_cast<size_t>(std::max(0, opts_.scrollbackLines)));
    sel_.setWordChars(opts_.wordChars);
    t0_ = g_get_monotonic_time();

    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&area_), static_cast<Gtk::Widget*>(&gl_)}) {
        w->set_can_focus(true);
        w->set_no_show_all(true);  // chooseMode() shows exactly one of the two
        w->add_events(Gdk::KEY_PRESS_MASK | Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK |
                      Gdk::POINTER_MOTION_MASK | Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
        w->signal_key_press_event().connect(sigc::mem_fun(*this, &TerminalView::onKey), false);
        w->signal_button_press_event().connect(sigc::mem_fun(*this, &TerminalView::onButtonPress), false);
        w->signal_button_release_event().connect(sigc::mem_fun(*this, &TerminalView::onButtonRelease), false);
        w->signal_motion_notify_event().connect(sigc::mem_fun(*this, &TerminalView::onMotion), false);
        w->signal_scroll_event().connect(sigc::mem_fun(*this, &TerminalView::onScroll), false);
        w->set_hexpand(true);
        w->set_vexpand(true);
        pack_start(*w, true, true);
    }
    area_.signal_draw().connect(sigc::mem_fun(*this, &TerminalView::onDraw));
    gl_.set_required_version(3, 3);
    gl_.set_has_alpha(false);
    gl_.set_has_depth_buffer(false);
    gl_.set_auto_render(false);
    gl_.signal_realize().connect(sigc::mem_fun(*this, &TerminalView::onGlRealize));
    gl_.signal_unrealize().connect(sigc::mem_fun(*this, &TerminalView::onGlUnrealize), false);
    gl_.signal_render().connect(sigc::mem_fun(*this, &TerminalView::onGlRender), false);
    applyProfile();
    chooseMode();
}

TerminalView::~TerminalView() {
    if (childWatch_) g_source_remove(childWatch_);
    writeWatch_.disconnect();
    bellTimer_.disconnect();
    blink_.disconnect();
    io_.disconnect();
    tick_.disconnect();
    frameTimer_.disconnect();
    if (opts_.stats) {
        auto med = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::printf("stats: backend=\"%s\" content-frames=%ld full-repaints=%ld rows-painted=%ld ticks=%ld "
                    "median paint %.2f ms, median effects-source %.2f ms, median tick %.2f ms, glyphs cached %zu\n",
                    backendName().c_str(), stats_.contentFrames, stats_.fullRepaints, stats_.rowsPainted,
                    stats_.ticks, med(stats_.paintMs), med(stats_.effectsSourceMs), med(stats_.tickMs),
                    painter_ ? painter_->cachedGlyphs() : size_t(0));
        std::fflush(stdout);
    }
    if (glPipe_) {
        // Normally released in onGlUnrealize; if the widget is still realized, release with its context.
        if (gl_.get_realized()) {
            gl_.make_current();
            if (!gl_.has_error() && gdk_gl_context_get_current()) glPipe_->release();
        }
        glPipe_.reset();  // the destructor itself never touches GL
    }
    if (src_) cairo_surface_destroy(src_);
    if (out_) cairo_surface_destroy(out_);
}

double TerminalView::nowMs() const { return (g_get_monotonic_time() - t0_) / 1000.0; }

std::string TerminalView::backendName() const {
    switch (mode_) {
    case Mode::Plain: return profile_.id + ", no effects";
    case Mode::Cpu: return profile_.id + ", effects: C++ fallback (" + std::to_string(opts_.threads) + " threads)";
    case Mode::Gl: return profile_.id + ", effects: GLSL on " + (glRenderer_.empty() ? "GtkGLArea" : glRenderer_);
    }
    return "?";
}

bool TerminalView::start() {
    if (!session_->spawn(SpawnOptions{opts_.command, opts_.loginShell, opts_.cwd})) return false;
    io_ = Glib::signal_io().connect(sigc::mem_fun(*this, &TerminalView::onPtyIo), session_->masterFd(),
                                    Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
    // GLib reaps the child (SIGCHLD) and reports the wait status on the main loop.
    childWatch_ = g_child_watch_add(
        session_->childPid(),
        [](GPid, gint status, gpointer user) { static_cast<TerminalView*>(user)->onChildExit(status); }, this);
    requestFrame();
    updateAnimation();
    updateBlink();
    return true;
}

void TerminalView::onChildExit(int status) {
    childWatch_ = 0;
    exited_ = true;
    session_->markExited();
    if (io_.connected()) {  // drain what the child wrote last
        session_->readAvailable();
        io_.disconnect();
    }
    signalExitStatus.emit(status);
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const bool hold = opts_.onExit == "hold" || (opts_.onExit == "hold-on-error" && !ok);
    if (!hold) {
        signalChildExited.emit();
        return;
    }
    holding_ = true;
    char msg[256];
    if (WIFSIGNALED(status))
        std::snprintf(msg, sizeof msg, "\r\n\033[0;7m[process killed by signal %d (%s)]\033[0m press Enter to close",
                      WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        std::snprintf(msg, sizeof msg, "\r\n\033[0;7m[process exited with status %d]\033[0m press Enter to close",
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    session_->feed(msg, std::strlen(msg));
    snapToBottom();
    session_->takeDamage();
    contentDirty_ = true;
    requestFrame();
    updateBlink();
}

void TerminalView::onBell() {
    const double t = nowMs();
    if (t - lastBellMs_ < 200.0) return;  // rate limit: `yes $'\a'` must not strobe
    lastBellMs_ = t;
    if (opts_.bellAudible && get_window()) get_window()->beep();
    if (opts_.bellVisual) {
        bellFlash_ = true;
        contentDirty_ = true;
        requestFrame();
        bellTimer_.disconnect();
        bellTimer_ = Glib::signal_timeout().connect(
            [this] {
                bellFlash_ = false;
                contentDirty_ = true;
                requestFrame();
                return false;
            },
            120);
    }
    if (opts_.bellUrgent) signalBell.emit();
}

void TerminalView::updateBlink() {
    int b = session_->cursorBlink();
    const bool blink = b < 0 ? opts_.cursorBlink : b != 0;
    const bool want = blink && active_ && visible_ && !holding_ && scrollOffset_ == 0;
    if (want && !blink_.connected()) {
        blink_ = Glib::signal_timeout().connect(
            [this] {
                blinkOff_ = !blinkOff_;
                contentDirty_ = true;
                requestFrame();
                return true;
            },
            530);
    } else if (!want && blink_.connected()) {
        blink_.disconnect();
    }
    if (!want && blinkOff_) {
        blinkOff_ = false;
        contentDirty_ = true;
        requestFrame();
    }
}

void TerminalView::applyProfile() {
    const std::string family = opts_.fontFamily.empty() ? profile_.fontFamily : opts_.fontFamily;
    const int px = opts_.fontPx > 0 ? opts_.fontPx : profile_.fontPx;
    painter_.reset(new GridPainter(family, px));
    applyColors();
    cpu_.reset();
    glParamsStale_ = true;
    if (w_ > 0) layoutGrid();
    fullRepaint_ = contentDirty_ = effectsSourceStale_ = true;
}

void TerminalView::applyColors() {
    const bool fxPal = effectsOn_ && profile_.hasEffectsPalette;
    session_->setColors(profile_.fg, profile_.bg, fxPal ? profile_.effectsPalette : profile_.palette,
                        fxPal ? profile_.effectsMinFgGrey : profile_.minFgGrey);
}

void TerminalView::setProfile(const std::string& id) {
    const Profile* p = findProfile(id);
    if (!p) return;
    profile_ = *p;
    applyProfile();
    if (opts_.effects < 0) effectsOn_ = profile_.effects;
    applyColors();
    chooseMode();
    updateContent();
    requestFrame();
    updateAnimation();
}

void TerminalView::setEffects(bool on) {
    effectsOn_ = on;
    applyColors();
    chooseMode();
    fullRepaint_ = contentDirty_ = effectsSourceStale_ = true;
    updateContent();
    requestFrame();
    updateAnimation();
}

void TerminalView::chooseMode() {
    Mode m = !effectsOn_ ? Mode::Plain : (glFailed_ ? Mode::Cpu : Mode::Gl);
    if (m == Mode::Cpu && !cpu_) {
        cpu_.reset(new CpuEffects(profile_.fx, opts_.threads));
        if (w_ > 0) cpu_->resize(w_, h_);
    }
    if (m != Mode::Cpu) cpu_.reset();  // frees ~45 MB of float buffers when not in use
    const bool hadFocus = area_.has_focus() || gl_.has_focus();
    mode_ = m;
    if (m == Mode::Gl) {
        gl_.show();
        area_.hide();
        if (hadFocus || !get_realized()) gl_.grab_focus();
    } else {
        area_.show();
        gl_.hide();
        if (hadFocus || !get_realized()) area_.grab_focus();
    }
    effectsSourceStale_ = true;
    signalBackend.emit(backendName());
}

void TerminalView::fallbackToCpu(const std::string& why) {
    if (glFailed_) return;
    glFailed_ = true;
    glError_ = why;
    std::fprintf(stderr, "lunduke-terminal: GL effects unavailable (%s); using the C++ fallback\n", why.c_str());
    Glib::signal_idle().connect_once([this] {
        chooseMode();
        requestFrame();
        updateAnimation();
    });
}

void TerminalView::onGlRealize() {
    gl_.make_current();
    if (gl_.has_error()) {
        GError* err = gtk_gl_area_get_error(gl_.gobj());
        fallbackToCpu(std::string("GtkGLArea: ") + (err && err->message ? err->message : "context creation failed"));
        return;
    }
    const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    glRenderer_ = r ? r : "?";
    glParamsStale_ = true;
    signalBackend.emit(backendName());
}

void TerminalView::onGlUnrealize() {
    gl_.make_current();
    if (glPipe_ && !gl_.has_error() && gdk_gl_context_get_current()) glPipe_->release();
    glPipe_.reset();
}

bool TerminalView::onGlRender(const Glib::RefPtr<Gdk::GLContext>&) {
    if (glFailed_ || mode_ != Mode::Gl) return true;
    if (!glPipe_ || glParamsStale_) {
        if (glPipe_) glPipe_->release();
        glPipe_.reset(new GlPipeline(profile_.fx));
        glParamsStale_ = false;
        effectsSourceStale_ = true;
        if (!glPipe_->ok()) {
            fallbackToCpu(glPipe_->error());
            glPipe_->release();
            glPipe_.reset();
            return true;
        }
    }
    const double t = nowMs();
    if (glPipe_->width() != w_ || glPipe_->height() != h_) {
        glPipe_->resize(w_, h_);
        effectsSourceStale_ = true;
    }
    const bool content = effectsSourceStale_ || srcDirtyY1_ > srcDirtyY0_;
    if (content) {
        const auto e0 = std::chrono::steady_clock::now();
        cairo_surface_flush(src_);
        const int y0 = effectsSourceStale_ ? 0 : srcDirtyY0_, y1 = effectsSourceStale_ ? h_ : srcDirtyY1_;
        glPipe_->updateSource(cairo_image_surface_get_data(src_), cairo_image_surface_get_stride(src_),
                              PixelOrder::BGRA, static_cast<float>(t), y0, y1);
        if (opts_.stats) glFinish();
        stats_.effectsSourceMs.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count());
        effectsSourceStale_ = false;
        srcDirtyY0_ = srcDirtyY1_ = 0;
    }
    const auto r0 = std::chrono::steady_clock::now();
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glPipe_->render(static_cast<float>(t), static_cast<unsigned>(fbo), true);
    if (opts_.stats) glFinish();
    if (!content) {
        ++stats_.ticks;
        stats_.tickMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count());
    }
    lastFrameMs_ = t;
    return true;
}

bool TerminalView::onDraw(const Cairo::RefPtr<Cairo::Context>& cr) {
    if (!src_) return true;
    if (mode_ == Mode::Cpu && cpu_) {
        const double t = nowMs();
        const bool content = effectsSourceStale_ || srcDirtyY1_ > srcDirtyY0_;
        if (content) {
            const auto e0 = std::chrono::steady_clock::now();
            cairo_surface_flush(src_);
            const int y0 = effectsSourceStale_ ? 0 : srcDirtyY0_, y1 = effectsSourceStale_ ? h_ : srcDirtyY1_;
            cpu_->updateSource(cairo_image_surface_get_data(src_), cairo_image_surface_get_stride(src_),
                               PixelOrder::BGRA, static_cast<float>(t), y0, y1);
            stats_.effectsSourceMs.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count());
            effectsSourceStale_ = false;
            srcDirtyY0_ = srcDirtyY1_ = 0;
        }
        const auto r0 = std::chrono::steady_clock::now();
        cairo_surface_flush(out_);
        cpu_->renderTo(static_cast<float>(t), cairo_image_surface_get_data(out_), cairo_image_surface_get_stride(out_),
                       PixelOrder::BGRA);
        cairo_surface_mark_dirty(out_);
        if (!content) {
            ++stats_.ticks;
            stats_.tickMs.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count());
        }
        lastFrameMs_ = t;
        cairo_set_source_surface(cr->cobj(), out_, 0, 0);
    } else {
        cairo_set_source_surface(cr->cobj(), src_, 0, 0);
    }
    cairo_paint(cr->cobj());
    return true;
}

void TerminalView::layoutGrid() {
    marginX_ = std::max(profile_.marginMinPx, static_cast<int>(w_ * profile_.marginFrac));
    marginY_ = std::max(profile_.marginMinPx, static_cast<int>(h_ * profile_.marginFrac));
    const int cols = std::max(1, (w_ - 2 * marginX_) / painter_->cellWidth());
    const int rows = std::max(1, (h_ - 2 * marginY_) / painter_->cellHeight());
    if (profile_.marginFrac > 0) {  // centre the grid inside the tube
        marginX_ = (w_ - cols * painter_->cellWidth()) / 2;
        marginY_ = (h_ - rows * painter_->cellHeight()) / 2;
    }
    if (rows != session_->rows() || cols != session_->cols()) {
        session_->resize(rows, cols);
        sel_.clear();  // reflow moves text around
        scrollOffset_ = std::min(scrollOffset_, static_cast<int>(session_->scrollbackSize()));
    }
}

void TerminalView::allocateSurfaces() {
    if (src_) cairo_surface_destroy(src_);
    if (out_) cairo_surface_destroy(out_);
    src_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
    out_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
}

void TerminalView::on_size_allocate(Gtk::Allocation& a) {
    Gtk::Box::on_size_allocate(a);
    const int w = std::max(1, a.get_width()), h = std::max(1, a.get_height());
    if (w == w_ && h == h_) return;
    w_ = w;
    h_ = h;
    allocateSurfaces();
    if (cpu_) cpu_->resize(w_, h_);
    layoutGrid();
    fullRepaint_ = contentDirty_ = effectsSourceStale_ = true;
    updateContent();
    if (mode_ == Mode::Gl) gl_.queue_render();
}

bool TerminalView::updateContent() {
    if (!src_ || (!contentDirty_ && !fullRepaint_)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    session_->fillGrid(next_, scrollOffset_);
    // Selection, cursor style, reverse video / visual bell are applied to the composed grid, so the
    // changed-rows repaint picks them up like any other cell change.
    if (sel_.active()) {
        const long top = session_->topLine() - scrollOffset_;
        for (int r = 0; r < next_.rows; ++r)
            for (int c = 0; c < next_.cols; ++c)
                if (sel_.contains(top + r, c)) {
                    Cell& cell = next_.at(r, c);
                    cell.selected = true;
                    std::swap(cell.fg, cell.bg);
                }
    }
    if (session_->reverseVideo() != bellFlash_)
        for (Cell& cell : next_.cells) std::swap(cell.fg, cell.bg);
    const int shape = session_->cursorShape();
    if (shape == VTERM_PROP_CURSORSHAPE_UNDERLINE) next_.cursorShape = Grid::CursorShape::Underline;
    else if (shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) next_.cursorShape = Grid::CursorShape::Bar;
    else if (shape == VTERM_PROP_CURSORSHAPE_BLOCK) next_.cursorShape = Grid::CursorShape::Block;
    else
        next_.cursorShape = opts_.cursorShape == "underline" ? Grid::CursorShape::Underline
                            : opts_.cursorShape == "bar"     ? Grid::CursorShape::Bar
                                                             : Grid::CursorShape::Block;
    next_.cursorHollow = !active_;
    if (blinkOff_ || holding_) next_.cursorVisible = false;
    const bool full = fullRepaint_ || next_.rows != cur_.rows || next_.cols != cur_.cols;
    std::vector<char> rows = full ? std::vector<char>(static_cast<size_t>(next_.rows), 1) : changedRows(cur_, next_);
    int r0 = -1, r1 = -1, n = 0;
    for (int r = 0; r < next_.rows; ++r)
        if (rows[r]) {
            if (r0 < 0) r0 = r;
            r1 = r;
            ++n;
        }
    contentDirty_ = false;
    if (n == 0 && !full) return false;
    cairo_t* c = cairo_create(src_);
    if (full) {
        cairo_set_source_rgb(c, profile_.bg.r / 255.0, profile_.bg.g / 255.0, profile_.bg.b / 255.0);
        cairo_paint(c);
    }
    painter_->paintRows(c, next_, marginX_, marginY_, profile_.cursor, profile_.bg, full ? nullptr : &rows, w_);
    cairo_destroy(c);
    std::swap(cur_, next_);
    fullRepaint_ = false;
    int y0 = 0, y1 = h_;
    if (!full) {
        y0 = std::max(0, marginY_ + r0 * painter_->cellHeight());
        y1 = std::min(h_, marginY_ + (r1 + 1) * painter_->cellHeight());
    }
    if (srcDirtyY1_ > srcDirtyY0_) {
        srcDirtyY0_ = std::min(srcDirtyY0_, y0);
        srcDirtyY1_ = std::max(srcDirtyY1_, y1);
    } else {
        srcDirtyY0_ = y0;
        srcDirtyY1_ = y1;
    }
    ++stats_.contentFrames;
    stats_.rowsPainted += n;
    if (full) ++stats_.fullRepaints;
    stats_.paintMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    lastContentMs_ = nowMs();
    if (mode_ == Mode::Plain) {
        area_.queue_draw_area(0, y0, w_, y1 - y0);
        srcDirtyY0_ = srcDirtyY1_ = 0;  // the plain view reads src_ directly
    }
    return true;
}

void TerminalView::requestFrame() {
    if (frameTimer_.connected()) return;
    // Effects re-run bloom/burn-in/static passes for every content frame: cap those at 30 fps.
    const int fps = std::max(1, mode_ == Mode::Plain ? opts_.maxFps : std::min(opts_.maxFps, 30));
    const double delay = std::max(0.0, lastFrameMs_ + 1000.0 / fps - nowMs());
    frameTimer_ = Glib::signal_timeout().connect(
        [this] {
            onFrameTimer();
            return false;
        },
        static_cast<unsigned>(delay));
}

void TerminalView::onFrameTimer() {
    if (mode_ == Mode::Plain) lastFrameMs_ = nowMs();
    const bool changed = updateContent();
    if (mode_ == Mode::Cpu && (changed || effectsSourceStale_)) area_.queue_draw();
    if (mode_ == Mode::Gl && (changed || effectsSourceStale_)) gl_.queue_render();
    if (changed) updateAnimation();  // burn-in needs ticks until the ghost has faded
}

bool TerminalView::animationWanted() const {
    if (!effectsOn_ || mode_ == Mode::Plain || !visible_) return false;
    if (profile_.fx.animated() && (active_ || !opts_.pauseWhenUnfocused)) return true;
    return nowMs() < lastContentMs_ + profile_.fx.burnInFadeMs() + 100.0;
}

void TerminalView::updateAnimation() {
    const bool want = animationWanted();
    if (want && !tick_.connected())
        tick_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &TerminalView::onTick),
                                               1000 / std::max(1, opts_.effectsFps));
    else if (!want && tick_.connected())
        tick_.disconnect();
}

bool TerminalView::onTick() {
    if (!animationWanted()) return false;  // paused (unfocused/hidden) or burn-in settled
    if (mode_ == Mode::Gl) gl_.queue_render();
    else if (mode_ == Mode::Cpu) area_.queue_draw();
    return true;
}

void TerminalView::setWindowActive(bool active) {
    if (active == active_) return;
    active_ = active;
    if (!exited_) session_->focus(active);  // DECSET 1004 focus reports
    contentDirty_ = true;                   // hollow <-> solid cursor (one row)
    requestFrame();
    updateAnimation();
    updateBlink();
}

void TerminalView::setWindowVisible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    if (visible) requestFrame();
    updateAnimation();
    updateBlink();
}

bool TerminalView::onPtyIo(Glib::IOCondition) {
    const long top0 = session_->topLine();
    const bool alive = session_->readAvailable();
    const long pushed = session_->topLine() - top0;
    if (pushed != 0 && scrollOffset_ > 0)  // keep a scrolled-back view still while output arrives
        scrollOffset_ = std::clamp(scrollOffset_ + static_cast<int>(pushed), 0,
                                   static_cast<int>(session_->scrollbackSize()));
    scrollOffset_ = std::min(scrollOffset_, static_cast<int>(session_->scrollbackSize()));  // ED 3
    if (sel_.active() && sel_.start().line < session_->firstLine()) sel_.clear();  // history dropped
    if (session_->altScreen() != lastAlt_) {
        lastAlt_ = session_->altScreen();
        sel_.clear();
        scrollOffset_ = 0;
    }
    if (session_->takeDamage()) {
        contentDirty_ = true;
        requestFrame();
        updateBlink();  // DECSCUSR / ?12 may have changed blinking
        // Output overwrote the selected text (clear, an editor redraw): drop the selection. A
        // selection that merely scrolled keeps its text and stays (it is anchored to lines).
        if (sel_.active() && !selecting_ && sel_.end().line >= session_->topLine() &&
            sel_.text(*session_) != selSnapshot_)
            sel_.clear();
    }
    ensureWriteWatch();  // replies (DA, DSR) that did not fit
    if (!alive) return false;  // EOF/HUP; the child watch decides what happens to the window
    return true;
}

void TerminalView::ensureWriteWatch() {
    if (writeWatch_.connected() || !session_->wantsWrite() || session_->masterFd() < 0) return;
    writeWatch_ = Glib::signal_io().connect(
        [this](Glib::IOCondition) {
            session_->flushWrites();
            return session_->wantsWrite() && !exited_;
        },
        session_->masterFd(), Glib::IO_OUT | Glib::IO_ERR | Glib::IO_HUP);
}

void TerminalView::sendKeyInput() {
    snapToBottom();
    if (blinkOff_) {  // typing shows the cursor immediately
        blinkOff_ = false;
        contentDirty_ = true;
        requestFrame();
    }
    ensureWriteWatch();
}

void TerminalView::scrollBy(int lines) {
    const int off = std::clamp(scrollOffset_ + lines, 0, static_cast<int>(session_->scrollbackSize()));
    if (off == scrollOffset_) return;
    scrollOffset_ = off;
    contentDirty_ = true;
    requestFrame();
    updateBlink();
}

void TerminalView::snapToBottom() {
    if (scrollOffset_ > 0) scrollBy(-scrollOffset_);
}

void TerminalView::selectionChanged() {
    contentDirty_ = true;
    requestFrame();
}

void TerminalView::copyClipboard() {
    const std::string t = sel_.text(*session_);
    if (!t.empty()) Gtk::Clipboard::get(GDK_SELECTION_CLIPBOARD)->set_text(t);
}

void TerminalView::pasteClipboard(bool primary) {
    if (holding_) return;
    Gtk::Clipboard::get(primary ? GDK_SELECTION_PRIMARY : GDK_SELECTION_CLIPBOARD)
        ->request_text([this](const Glib::ustring& t) {
            if (t.empty() || holding_ || exited_) return;
            snapToBottom();
            session_->paste(t.raw());
            ensureWriteWatch();
        });
}

bool TerminalView::cellAt(double x, double y, int& row, int& col) const {
    const float curvature = effectsOn_ && mode_ != Mode::Plain ? profile_.fx.screenCurvature * 0.4f : 0.0f;
    return pointToCell(x, y, w_, h_, curvature, marginX_, marginY_, painter_->cellWidth(), painter_->cellHeight(),
                       session_->rows(), session_->cols(), row, col);
}

bool TerminalView::mouseReporting(guint state) const {
    return session_->mouseMode() != VTERM_PROP_MOUSE_NONE && !(state & GDK_SHIFT_MASK) && !holding_ && !exited_;
}

bool TerminalView::onButtonPress(GdkEventButton* ev) {
    if (mode_ == Mode::Gl) gl_.grab_focus();
    else area_.grab_focus();
    int row, col;
    cellAt(ev->x, ev->y, row, col);
    const VTermModifier mod = modsOf(ev->state);
    if (mouseReporting(ev->state)) {
        if (ev->type != GDK_BUTTON_PRESS) return true;  // GTK's extra 2/3-click events
        if (ev->button >= 1 && ev->button <= 3) {
            pressedButtons_ |= 1 << ev->button;
            session_->mouseButton(row, col, static_cast<int>(ev->button), true, mod);
            ensureWriteWatch();
        }
        return true;
    }
    const SelPos p{session_->topLine() - scrollOffset_ + row, col};
    if (ev->button == 1) {
        if (ev->type == GDK_2BUTTON_PRESS) sel_.begin(*session_, p, SelectMode::Word);
        else if (ev->type == GDK_3BUTTON_PRESS) sel_.begin(*session_, p, SelectMode::Line);
        else if ((ev->state & GDK_SHIFT_MASK) && sel_.active()) sel_.extend(*session_, p);
        else sel_.begin(*session_, p, SelectMode::Char);
        selecting_ = true;
        selectionChanged();
        return true;
    }
    if (ev->button == 2 && ev->type == GDK_BUTTON_PRESS) {
        pasteClipboard(true);
        return true;
    }
    return false;
}

bool TerminalView::onButtonRelease(GdkEventButton* ev) {
    int row, col;
    cellAt(ev->x, ev->y, row, col);
    if (ev->button >= 1 && ev->button <= 3 && (pressedButtons_ & (1 << ev->button))) {
        pressedButtons_ &= ~(1 << ev->button);
        session_->mouseButton(row, col, static_cast<int>(ev->button), false, modsOf(ev->state));
        ensureWriteWatch();
        return true;
    }
    if (ev->button == 1 && selecting_) {
        selecting_ = false;
        if (sel_.active()) {
            selSnapshot_ = sel_.text(*session_);
            if (!selSnapshot_.empty()) Gtk::Clipboard::get(GDK_SELECTION_PRIMARY)->set_text(selSnapshot_);
            else sel_.clear();  // only blanks: nothing to copy, do not leave a highlight behind
            selectionChanged();
        }
        return true;
    }
    return false;
}

bool TerminalView::onMotion(GdkEventMotion* ev) {
    int row, col;
    cellAt(ev->x, ev->y, row, col);
    if (pressedButtons_ || mouseReporting(ev->state)) {
        // libvterm only emits for ?1002 (with a button held) / ?1003 and only on cell changes.
        session_->mouseMove(row, col, modsOf(ev->state));
        ensureWriteWatch();
        return true;
    }
    if (selecting_ && (ev->state & GDK_BUTTON1_MASK)) {
        // Dragging past the top/bottom edge scrolls the history.
        if (ev->y < marginY_) scrollBy(1);
        else if (ev->y >= h_ - marginY_) scrollBy(-1);
        sel_.extend(*session_, SelPos{session_->topLine() - scrollOffset_ + row, col});
        selectionChanged();
        return true;
    }
    return false;
}

bool TerminalView::onScroll(GdkEventScroll* ev) {
    int steps = 0;  // + = up / back
    if (ev->direction == GDK_SCROLL_UP) steps = 1;
    else if (ev->direction == GDK_SCROLL_DOWN) steps = -1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        wheelAccum_ -= ev->delta_y;
        steps = static_cast<int>(wheelAccum_);
        wheelAccum_ -= steps;
    }
    if (steps == 0) return true;
    int row, col;
    cellAt(ev->x, ev->y, row, col);
    if (mouseReporting(ev->state)) {
        for (int i = 0; i < std::abs(steps); ++i)
            session_->mouseButton(row, col, steps > 0 ? 4 : 5, true, modsOf(ev->state));
        ensureWriteWatch();
        return true;
    }
    if (session_->altScreen() && !exited_) {
        // Alternate scroll: full-screen programs without mouse mode (less, man) get arrow keys.
        for (int i = 0; i < 3 * std::abs(steps); ++i)
            session_->sendKey(steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
        ensureWriteWatch();
        return true;
    }
    scrollBy(3 * steps);
    return true;
}

bool TerminalView::onKey(GdkEventKey* ev) {
    const guint ctrlShift = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    const guint mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    if (mods == ctrlShift) {
        const guint k = gdk_keyval_to_lower(ev->keyval);
        if (k == GDK_KEY_e) {  // toggle effects
            setEffects(!effectsOn_);
            return true;
        }
        if (k == GDK_KEY_p) {  // next built-in profile
            const auto& all = builtinProfiles();
            size_t i = 0;
            while (i < all.size() && all[i].id != profile_.id) ++i;
            setProfile(all[(i + 1) % all.size()].id);
            return true;
        }
        if (k == GDK_KEY_c) {
            copyClipboard();
            return true;
        }
        if (k == GDK_KEY_v) {
            pasteClipboard(false);
            return true;
        }
    }
    if (mods == GDK_SHIFT_MASK && !session_->altScreen()) {
        const int page = std::max(1, session_->rows() - 1);
        switch (ev->keyval) {
        case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: scrollBy(page); return true;
        case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: scrollBy(-page); return true;
        case GDK_KEY_Home: case GDK_KEY_KP_Home: scrollBy(1 << 30); return true;
        case GDK_KEY_End: case GDK_KEY_KP_End: snapToBottom(); return true;
        default: break;
        }
    }
    if (mods == GDK_SHIFT_MASK && (ev->keyval == GDK_KEY_Insert || ev->keyval == GDK_KEY_KP_Insert)) {
        pasteClipboard(true);
        return true;
    }
    if (holding_ || exited_) {
        if (holding_ && (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter ||
                         ev->keyval == GDK_KEY_Escape || ev->keyval == GDK_KEY_q))
            signalChildExited.emit();
        return true;
    }
    const VTermModifier mod = modsOf(ev->state);
    VTermKey key;
    if (mapKey(ev->keyval, key)) {
        session_->sendKey(key, mod);
        sendKeyInput();
        return true;
    }
    const guint32 uc = gdk_keyval_to_unicode(ev->keyval);
    if (uc) {
        // Shift is already reflected in the character itself.
        session_->sendChar(uc, static_cast<VTermModifier>(mod & ~VTERM_MOD_SHIFT));
        sendKeyInput();
        return true;
    }
    return false;
}

}  // namespace lterm
