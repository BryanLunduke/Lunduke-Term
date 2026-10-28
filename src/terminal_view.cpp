// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal_view.hpp"

#include <epoxy/gl.h>
#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
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
    t0_ = g_get_monotonic_time();

    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&area_), static_cast<Gtk::Widget*>(&gl_)}) {
        w->set_can_focus(true);
        w->set_no_show_all(true);  // chooseMode() shows exactly one of the two
        w->add_events(Gdk::KEY_PRESS_MASK | Gdk::BUTTON_PRESS_MASK);
        w->signal_key_press_event().connect(sigc::mem_fun(*this, &TerminalView::onKey), false);
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
    if (!session_->spawn(opts_.command)) return false;
    io_ = Glib::signal_io().connect(sigc::mem_fun(*this, &TerminalView::onPtyIo), session_->masterFd(),
                                    Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
    requestFrame();
    updateAnimation();
    return true;
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
    session_->resize(rows, cols);
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
    session_->fillGrid(next_);
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
    updateAnimation();
}

void TerminalView::setWindowVisible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    if (visible) requestFrame();
    updateAnimation();
}

bool TerminalView::onPtyIo(Glib::IOCondition) {
    const bool alive = session_->readAvailable();
    if (session_->takeDamage()) {
        contentDirty_ = true;
        requestFrame();
    }
    if (!alive) {
        waitpid(session_->childPid(), nullptr, WNOHANG);
        signalChildExited.emit();
        return false;
    }
    return true;
}

bool TerminalView::onKey(GdkEventKey* ev) {
    const guint ctrlShift = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    if ((ev->state & ctrlShift) == ctrlShift) {
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
    }
    const VTermModifier mod = modsOf(ev->state);
    VTermKey key;
    if (mapKey(ev->keyval, key)) {
        session_->sendKey(key, mod);
        return true;
    }
    const guint32 uc = gdk_keyval_to_unicode(ev->keyval);
    if (uc) {
        // Shift is already reflected in the character itself.
        session_->sendChar(uc, static_cast<VTermModifier>(mod & ~VTERM_MOD_SHIFT));
        return true;
    }
    return false;
}

}  // namespace lterm
