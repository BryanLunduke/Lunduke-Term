// SPDX-License-Identifier: GPL-3.0-or-later
#include "vterm_session.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lterm {

VtermSession::VtermSession(int rows, int cols) : rows_(rows), cols_(cols) {
    vt_ = vterm_new(rows, cols);
    vterm_set_utf8(vt_, 1);
    vterm_output_set_callback(vt_, &VtermSession::cbOutput, this);
    screen_ = vterm_obtain_screen(vt_);
    static VTermScreenCallbacks cbs{};
    cbs.damage = &VtermSession::cbDamage;
    cbs.movecursor = &VtermSession::cbMoveCursor;
    cbs.settermprop = &VtermSession::cbSetTermProp;
    cbs.bell = &VtermSession::cbBell;
    vterm_screen_set_callbacks(screen_, &cbs, this);
    vterm_screen_enable_altscreen(screen_, 1);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 208, 208, 208);
    vterm_color_rgb(&bg, 0, 0, 0);
    vterm_screen_set_default_colors(screen_, &fg, &bg);
    vterm_screen_reset(screen_, 1);
}

VtermSession::~VtermSession() {
    if (fd_ >= 0) close(fd_);
    if (pid_ > 0) {
        kill(pid_, SIGHUP);
        waitpid(pid_, nullptr, WNOHANG);
    }
    vterm_free(vt_);
}

bool VtermSession::spawn(const std::vector<std::string>& argv) {
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(rows_);
    ws.ws_col = static_cast<unsigned short>(cols_);
    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        std::vector<std::string> args = argv;
        if (args.empty()) {
            const char* sh = getenv("SHELL");
            args.push_back(sh && *sh ? sh : "/bin/sh");
        }
        std::vector<char*> cargv;
        for (auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    fd_ = master;
    pid_ = pid;
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    fcntl(fd_, F_SETFD, FD_CLOEXEC);
    return true;
}

bool VtermSession::readAvailable() {
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n > 0) {
            feed(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
        return false;  // EOF or EIO: child gone
    }
}

void VtermSession::feed(const char* bytes, size_t len) {
    vterm_input_write(vt_, bytes, len);
    vterm_screen_flush_damage(screen_);
}

void VtermSession::resize(int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == rows_ && cols == cols_) return;
    rows_ = rows;
    cols_ = cols;
    vterm_set_size(vt_, rows, cols);
    vterm_screen_flush_damage(screen_);
    if (fd_ >= 0) {
        struct winsize ws {};
        ws.ws_row = static_cast<unsigned short>(rows);
        ws.ws_col = static_cast<unsigned short>(cols);
        ioctl(fd_, TIOCSWINSZ, &ws);
    }
    damaged_ = true;
}

void VtermSession::sendKey(VTermKey key, VTermModifier mod) { vterm_keyboard_key(vt_, key, mod); }
void VtermSession::sendChar(uint32_t c, VTermModifier mod) { vterm_keyboard_unichar(vt_, c, mod); }

void VtermSession::sendBytes(const char* bytes, size_t len) {
    if (fd_ < 0) {
        pendingOut_.append(bytes, len);
        return;
    }
    while (len > 0) {
        ssize_t n = write(fd_, bytes, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;  // M0: tiny writes, spin is fine
            return;
        }
        bytes += n;
        len -= static_cast<size_t>(n);
    }
}

static void appendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) {
        s += static_cast<char>(0xC0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += static_cast<char>(0xE0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    }
}

void VtermSession::fillGrid(Grid& g) const {
    if (g.rows != rows_ || g.cols != cols_) g.resize(rows_, cols_);
    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            VTermScreenCell vc;
            VTermPos pos{r, c};
            Cell& cell = g.at(r, c);
            if (!vterm_screen_get_cell(screen_, pos, &vc)) {
                cell = Cell{};
                continue;
            }
            cell.text.clear();
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && vc.chars[i] && vc.chars[i] != 0xFFFFFFFFu; ++i)
                appendUtf8(cell.text, vc.chars[i]);
            vterm_screen_convert_color_to_rgb(screen_, &vc.fg);
            vterm_screen_convert_color_to_rgb(screen_, &vc.bg);
            Color8 fg{vc.fg.rgb.red, vc.fg.rgb.green, vc.fg.rgb.blue};
            Color8 bg{vc.bg.rgb.red, vc.bg.rgb.green, vc.bg.rgb.blue};
            if (vc.attrs.reverse) std::swap(fg, bg);
            cell.fg = fg;
            cell.bg = bg;
            cell.bold = vc.attrs.bold;
            cell.italic = vc.attrs.italic;
            cell.underline = vc.attrs.underline != 0;
            cell.width = vc.width;
            if (c > 0 && g.at(r, c - 1).width == 2) cell.width = 0;
        }
    }
    g.cursorRow = cursor_.row;
    g.cursorCol = cursor_.col;
    g.cursorVisible = cursorVisible_;
}

std::string VtermSession::rowText(int row) const {
    char buf[4096];
    VTermRect rect{row, row + 1, 0, cols_};
    size_t n = vterm_screen_get_text(screen_, buf, sizeof buf, rect);
    return std::string(buf, n);
}

int VtermSession::cbDamage(VTermRect, void* user) {
    static_cast<VtermSession*>(user)->damaged_ = true;
    return 1;
}

int VtermSession::cbMoveCursor(VTermPos pos, VTermPos, int visible, void* user) {
    auto* s = static_cast<VtermSession*>(user);
    s->cursor_ = pos;
    s->cursorVisible_ = visible != 0;
    s->damaged_ = true;
    return 1;
}

int VtermSession::cbSetTermProp(VTermProp prop, VTermValue* val, void* user) {
    auto* s = static_cast<VtermSession*>(user);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        s->cursorVisible_ = val->boolean;
        s->damaged_ = true;
        break;
    case VTERM_PROP_TITLE:
        if (val->string.initial) s->titleBuf_.clear();
        s->titleBuf_.append(val->string.str, val->string.len);
        if (val->string.final) {
            s->title_ = s->titleBuf_;
            if (s->onTitle) s->onTitle();
        }
        break;
    default:
        break;
    }
    return 1;
}

int VtermSession::cbBell(void* user) {
    auto* s = static_cast<VtermSession*>(user);
    if (s->onBell) s->onBell();
    return 1;
}

void VtermSession::cbOutput(const char* bytes, size_t len, void* user) {
    static_cast<VtermSession*>(user)->sendBytes(bytes, len);
}

}  // namespace lterm
