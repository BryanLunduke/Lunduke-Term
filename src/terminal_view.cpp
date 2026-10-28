// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal_view.hpp"

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
    : opts_(opts), painter_(opts.fontFamily, opts.fontPx),
      session_(new VtermSession(24, 80)) {
    set_can_focus(true);
    add_events(Gdk::KEY_PRESS_MASK | Gdk::BUTTON_PRESS_MASK);
    if (opts_.effects) effects_.reset(new CpuEffects(EffectParams{}, opts_.threads));
    session_->onTitle = [this] { signalTitle.emit(session_->title()); };
    t0_ = g_get_monotonic_time();
}

TerminalView::~TerminalView() {
    io_.disconnect();
    tick_.disconnect();
    if (src_) cairo_surface_destroy(src_);
    if (out_) cairo_surface_destroy(out_);
}

double TerminalView::nowMs() const { return (g_get_monotonic_time() - t0_) / 1000.0; }

bool TerminalView::start() {
    if (!session_->spawn(opts_.command)) return false;
    io_ = Glib::signal_io().connect(sigc::mem_fun(*this, &TerminalView::onPtyIo), session_->masterFd(),
                                    Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
    if (effects_) tick_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &TerminalView::onTick), 50);
    if (opts_.screenshotAfterMs > 0) {
        Glib::signal_timeout().connect_once([this] {
            shotDone_ = true;
            if (out_) cairo_surface_write_to_png(effects_ ? out_ : src_, opts_.screenshotPath.c_str());
            std::printf("screenshot %s (source %.2f ms, effects %.2f ms, total paint %.2f ms)\n",
                        opts_.screenshotPath.c_str(), lastSourceMs, lastEffectsMs, lastPaintMs);
            std::fflush(stdout);
            signalChildExited.emit();
        }, opts_.screenshotAfterMs);
    }
    return true;
}

void TerminalView::on_realize() {
    Gtk::DrawingArea::on_realize();
    grab_focus();
}

void TerminalView::layoutGrid() {
    // Keep text inside the area that stays visible under the barrel warp.
    marginX_ = std::max(12, w_ * 4 / 100);
    marginY_ = std::max(12, h_ * 4 / 100);
    const int cols = std::max(1, (w_ - 2 * marginX_) / painter_.cellWidth());
    const int rows = std::max(1, (h_ - 2 * marginY_) / painter_.cellHeight());
    // Centre the grid.
    marginX_ = (w_ - cols * painter_.cellWidth()) / 2;
    marginY_ = (h_ - rows * painter_.cellHeight()) / 2;
    session_->resize(rows, cols);
}

void TerminalView::on_size_allocate(Gtk::Allocation& a) {
    Gtk::DrawingArea::on_size_allocate(a);
    if (a.get_width() == w_ && a.get_height() == h_) return;
    w_ = std::max(1, a.get_width());
    h_ = std::max(1, a.get_height());
    if (src_) cairo_surface_destroy(src_);
    if (out_) cairo_surface_destroy(out_);
    src_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
    out_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
    outRgba_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
    if (effects_) effects_->resize(w_, h_);
    layoutGrid();
    contentDirty_ = true;
}

bool TerminalView::onPtyIo(Glib::IOCondition) {
    const bool alive = session_->readAvailable();
    if (session_->takeDamage()) {
        contentDirty_ = true;
        queue_draw();
    }
    if (!alive) {
        waitpid(session_->childPid(), nullptr, WNOHANG);
        if (!shotDone_ && opts_.screenshotAfterMs > 0) return false;  // keep window for the shot
        signalChildExited.emit();
        return false;
    }
    return true;
}

bool TerminalView::onTick() {
    queue_draw();  // animated effects (noise, flicker, glowing line) run at 20 fps like CRT 1.2.0
    return true;
}

bool TerminalView::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    if (!src_) return true;
    const auto t0 = std::chrono::steady_clock::now();
    const double t = nowMs();
    if (contentDirty_) {
        session_->fillGrid(grid_);
        cairo_t* c = cairo_create(src_);
        cairo_set_source_rgb(c, 0, 0, 0);
        cairo_paint(c);
        painter_.paint(c, grid_, marginX_, marginY_, Color8{208, 208, 208});
        cairo_destroy(c);
        const auto t1 = std::chrono::steady_clock::now();
        lastSourceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (effects_) {
            surfaceToRgba(src_, srcRgba_);
            effects_->setSource(srcRgba_.data(), w_ * 4, static_cast<float>(t));
        }
        contentDirty_ = false;
    }
    cairo_surface_t* show = src_;
    if (effects_) {
        const auto e0 = std::chrono::steady_clock::now();
        effects_->render(static_cast<float>(t), outRgba_.data());
        rgbaToSurface(outRgba_.data(), w_, h_, out_);
        lastEffectsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count();
        show = out_;
    }
    cairo_set_source_surface(cr->cobj(), show, 0, 0);
    cairo_paint(cr->cobj());
    lastPaintMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool TerminalView::on_key_press_event(GdkEventKey* ev) {
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
    return Gtk::DrawingArea::on_key_press_event(ev);
}

}  // namespace lterm
