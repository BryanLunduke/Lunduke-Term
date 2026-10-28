// SPDX-License-Identifier: GPL-3.0-or-later
#include "vterm_session.hpp"
#include "profiles.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace lterm {

std::vector<std::string> childEnvironment(char** base) {
    std::vector<std::string> env;
    bool haveLocale = false;
    for (char** e = base; e && *e; ++e) {
        std::string kv = *e;
        auto key = kv.substr(0, kv.find('='));
        if (key == "TERM" || key == "COLORTERM" || key == "COLUMNS" || key == "LINES" || key == "TERM_PROGRAM" ||
            key == "TERM_PROGRAM_VERSION")
            continue;
        if ((key == "LANG" || key == "LC_ALL" || key == "LC_CTYPE") && kv.size() > key.size() + 1) haveLocale = true;
        env.push_back(kv);
    }
    env.push_back("TERM=xterm-256color");
    env.push_back("COLORTERM=truecolor");
    env.push_back("TERM_PROGRAM=lunduke-terminal");
    env.push_back("TERM_PROGRAM_VERSION=0.1");
    if (!haveLocale) env.push_back("LANG=C.UTF-8");
    return env;
}

std::string sanitizePaste(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            out += '\r';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out += '\r';
        } else if (c == '\t' || c >= 0x20) {
            if (c == 0x7f) continue;
            out += static_cast<char>(c);
        }
        // other C0 controls (ESC, ^C, ...) are dropped
    }
    return out;
}

VtermSession::VtermSession(int rows, int cols) : rows_(rows), cols_(cols) {
    vt_ = vterm_new(rows, cols);
    vterm_set_utf8(vt_, 1);
    vterm_output_set_callback(vt_, &VtermSession::cbOutput, this);
    screen_ = vterm_obtain_screen(vt_);
    state_ = vterm_obtain_state(vt_);
    static VTermScreenCallbacks cbs{};
    cbs.damage = &VtermSession::cbDamage;
    cbs.movecursor = &VtermSession::cbMoveCursor;
    cbs.settermprop = &VtermSession::cbSetTermProp;
    cbs.bell = &VtermSession::cbBell;
    cbs.sb_pushline = &VtermSession::cbPushLine;
    cbs.sb_popline = &VtermSession::cbPopLine;
    cbs.sb_clear = &VtermSession::cbSbClear;
    vterm_screen_set_callbacks(screen_, &cbs, this);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, true);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 208, 208, 208);
    vterm_color_rgb(&bg, 0, 0, 0);
    vterm_screen_set_default_colors(screen_, &fg, &bg);
    vterm_screen_reset(screen_, 1);
    ready_ = true;
}

VtermSession::~VtermSession() {
    hangup();
    if (fd_ >= 0) close(fd_);
    vterm_free(vt_);
}

void VtermSession::hangup() {
    if (pid_ > 0) {
        kill(-pid_, SIGHUP);  // the child is a session/process-group leader
        kill(pid_, SIGHUP);
        pid_ = -1;  // reaped by the GLib child watch (or init when we exit)
    }
}

bool VtermSession::spawn(const SpawnOptions& opts) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return false;
    char slaveName[128];
    if (grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, slaveName, sizeof slaveName) != 0) {
        close(master);
        return false;
    }
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(rows_);
    ws.ws_col = static_cast<unsigned short>(cols_);
    ioctl(master, TIOCSWINSZ, &ws);

    // Everything the child needs is prepared before fork(): GTK has threads, so the child may
    // only use async-signal-safe calls until exec.
    std::vector<std::string> args = opts.argv;
    std::string file;
    if (args.empty()) {
        const char* sh = getenv("SHELL");
        std::string shell = sh && *sh ? sh : "";
        if (shell.empty()) {
            struct passwd* pw = getpwuid(getuid());
            if (pw && pw->pw_shell && *pw->pw_shell) shell = pw->pw_shell;
        }
        if (shell.empty() || access(shell.c_str(), X_OK) != 0) shell = "/bin/sh";
        file = shell;
        std::string base = shell.substr(shell.rfind('/') + 1);
        args.push_back(opts.loginShell ? "-" + base : shell);
    } else {
        file = args[0];
    }
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    std::vector<std::string> env = childEnvironment(environ);
    std::vector<char*> cenv;
    for (auto& e : env) cenv.push_back(const_cast<char*>(e.c_str()));
    cenv.push_back(nullptr);
    const char* home = getenv("HOME");
    std::string homeDir = home ? home : "/";
    std::string cwd = opts.cwd;
    std::string failMsg = "lunduke-terminal: cannot run " + file + ": ";

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return false;
    }
    if (pid == 0) {
        setsid();
        int slave = open(slaveName, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        struct termios t;
        if (tcgetattr(slave, &t) == 0) {
            t.c_iflag |= IUTF8;
            t.c_cc[VERASE] = 0x7f;
            tcsetattr(slave, TCSANOW, &t);
        }
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        for (int fd = 3; fd < 1024; ++fd) close(fd);  // incl. master, GL/X11 sockets
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int sig = 1; sig < 32; ++sig) signal(sig, SIG_DFL);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            if (chdir(homeDir.c_str()) != 0) {
                // stay where we are
            }
        }
        execvpe(file.c_str(), cargv.data(), cenv.data());
        int err = errno;
        ssize_t w = write(2, failMsg.data(), failMsg.size());
        const char* es = strerror(err);  // not strictly async-signal-safe; we exit right after
        w = write(2, es, strlen(es));
        w = write(2, "\r\n", 2);
        (void)w;
        _exit(127);
    }
    fd_ = master;
    pid_ = pid;
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
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
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return true;
        return false;  // EOF or EIO: child gone
    }
}

// libvterm ignores DECSET 9 (X10 mouse); watch for it in the input stream ourselves.
void VtermSession::scanModes(const char* bytes, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const char c = bytes[i];
        switch (scanState_) {
        case 0: if (c == 0x1b) scanState_ = 1; break;
        case 1: scanState_ = c == '[' ? 2 : (c == 0x1b ? 1 : 0); break;
        case 2:
            if (c == '?') {
                scanState_ = 3;
                scanParams_.clear();
            } else scanState_ = c == 0x1b ? 1 : 0;
            break;
        case 3:
            if ((c >= '0' && c <= '9') || c == ';') {
                if (scanParams_.size() < 64) scanParams_ += c;
            } else {
                if (c == 'h' || c == 'l') {
                    size_t p = 0;
                    while (p <= scanParams_.size()) {
                        size_t q = scanParams_.find(';', p);
                        if (q == std::string::npos) q = scanParams_.size();
                        if (scanParams_.compare(p, q - p, "9") == 0) x10_ = c == 'h';
                        // any other mouse mode replaces X10 (as in xterm)
                        if (c == 'h' && q > p && scanParams_.compare(p, q - p, "9") != 0) {
                            const std::string m = scanParams_.substr(p, q - p);
                            if (m == "1000" || m == "1002" || m == "1003") x10_ = false;
                        }
                        p = q + 1;
                    }
                }
                scanState_ = c == 0x1b ? 1 : 0;
            }
            break;
        }
    }
}

void VtermSession::feed(const char* bytes, size_t len) {
    scanModes(bytes, len);
    vterm_input_write(vt_, bytes, len);
    vterm_screen_flush_damage(screen_);
}

void VtermSession::resize(int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == rows_ && cols == cols_) return;
    rows_ = rows;
    cols_ = cols;
    vterm_set_size(vt_, rows, cols);  // reflow + scrollback push/pop happen in here
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
    writeQueue_.append(bytes, len);
    flushWrites();
}

// Non-blocking: whatever the PTY does not take now stays queued (large pastes into an app that
// is not reading) and the view retries from an IO_OUT watch.
void VtermSession::flushWrites() {
    size_t off = 0;
    while (off < writeQueue_.size()) {
        ssize_t n = write(fd_, writeQueue_.data() + off, writeQueue_.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) off = writeQueue_.size();  // EIO: child gone, drop
            break;
        }
        off += static_cast<size_t>(n);
    }
    writeQueue_.erase(0, off);
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

void VtermSession::setColors(Color8 fg, Color8 bg, const std::array<Color8, 16>& palette, float minFgGrey) {
    fg_ = fg;
    bg_ = bg;
    palette_ = palette;
    havePalette_ = true;
    minFgGrey_ = minFgGrey;
    damaged_ = true;
}

Color8 VtermSession::mapColor(VTermColor c, bool foreground) const {
    if (foreground && VTERM_COLOR_IS_DEFAULT_FG(&c)) return fg_;
    if (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&c)) return bg_;
    if (havePalette_ && VTERM_COLOR_IS_INDEXED(&c) && c.indexed.idx < 16) return palette_[c.indexed.idx];
    vterm_screen_convert_color_to_rgb(screen_, &c);
    Color8 o{c.rgb.red, c.rgb.green, c.rgb.blue};
    return foreground ? liftToGrey(o, minFgGrey_) : o;
}

void VtermSession::toCell(const VTermScreenCell& vc, Cell& cell) const {
    cell.text.clear();
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && vc.chars[i] && vc.chars[i] != 0xFFFFFFFFu; ++i)
        appendUtf8(cell.text, vc.chars[i]);
    Color8 fg = mapColor(vc.fg, true);
    Color8 bg = mapColor(vc.bg, false);
    if (vc.attrs.reverse) std::swap(fg, bg);
    cell.fg = fg;
    cell.bg = bg;
    cell.bold = vc.attrs.bold;
    cell.italic = vc.attrs.italic;
    cell.underline = vc.attrs.underline != 0;
    cell.width = vc.width;
    cell.selected = false;
}

static VTermScreenCell blankCell() {
    VTermScreenCell c{};
    c.width = 1;
    VTermColor fg, bg;
    vterm_color_rgb(&fg, 208, 208, 208);
    vterm_color_rgb(&bg, 0, 0, 0);
    fg.type |= VTERM_COLOR_DEFAULT_FG;
    bg.type |= VTERM_COLOR_DEFAULT_BG;
    c.fg = fg;
    c.bg = bg;
    return c;
}

bool VtermSession::getCell(long line, int col, VTermScreenCell& out) const {
    if (col < 0 || col >= cols_) return false;
    if (line >= pushed_) {
        long r = line - pushed_;
        if (r >= rows_) return false;
        return vterm_screen_get_cell(screen_, VTermPos{static_cast<int>(r), col}, &out) != 0;
    }
    long idx = static_cast<long>(sb_.size()) - (pushed_ - line);
    if (idx < 0) return false;
    const SbLine& l = sb_[static_cast<size_t>(idx)];
    if (col < static_cast<int>(l.cells.size())) out = l.cells[static_cast<size_t>(col)];
    else out = blankCell();
    return true;
}

void VtermSession::fillGrid(Grid& g, int scrollOffset) const {
    if (g.rows != rows_ || g.cols != cols_) g.resize(rows_, cols_);
    scrollOffset = std::clamp(scrollOffset, 0, static_cast<int>(sb_.size()));
    const VTermScreenCell blank = blankCell();
    for (int r = 0; r < rows_; ++r) {
        const long line = pushed_ - scrollOffset + r;
        for (int c = 0; c < cols_; ++c) {
            VTermScreenCell vc;
            if (!getCell(line, c, vc)) vc = blank;
            Cell& cell = g.at(r, c);
            toCell(vc, cell);
            if (c > 0 && g.at(r, c - 1).width == 2) cell.width = 0;
        }
    }
    g.cursorRow = cursor_.row + scrollOffset;
    g.cursorCol = cursor_.col;
    g.cursorVisible = cursorVisible_ && g.cursorRow < rows_;
}

std::string VtermSession::cellText(long line, int col) const {
    VTermScreenCell vc;
    if (!getCell(line, col, vc)) return {};
    std::string s;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && vc.chars[i] && vc.chars[i] != 0xFFFFFFFFu; ++i)
        appendUtf8(s, vc.chars[i]);
    return s;
}

int VtermSession::cellWidth(long line, int col) const {
    VTermScreenCell vc;
    if (col > 0 && getCell(line, col - 1, vc) && vc.width == 2) return 0;
    if (!getCell(line, col, vc)) return 1;
    return vc.width;
}

bool VtermSession::wrapped(long line) const {
    if (line >= pushed_) {
        long r = line - pushed_ + 1;
        if (r >= rows_) return false;
        const VTermLineInfo* li = vterm_state_get_lineinfo(state_, static_cast<int>(r));
        return li && li->continuation;
    }
    long idx = static_cast<long>(sb_.size()) - (pushed_ - line);
    if (idx < 0) return false;
    return sb_[static_cast<size_t>(idx)].wrapped;
}

void VtermSession::setScrollbackLimit(size_t lines) {
    sbLimit_ = lines;
    while (sb_.size() > sbLimit_) sb_.pop_front();
}

int VtermSession::cbPushLine(int cols, const VTermScreenCell* cells, void* user) {
    auto* s = static_cast<VtermSession*>(user);
    if (s->sbLimit_ == 0) {
        ++s->pushed_;
        return 1;
    }
    SbLine l;
    int n = cols;
    while (n > 0 && cells[n - 1].chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&cells[n - 1].bg) &&
           !cells[n - 1].attrs.reverse)
        --n;
    l.cells.assign(cells, cells + n);
    // libvterm shifts its line info before the screen pushes the scrolled-out rows, so for the
    // usual one-line scroll (a linefeed at the bottom) the old row 1 - the line that would
    // continue this one - is now row 0. Multi-line scrolls (CSI S) lose the flag for all but
    // the last pushed row; selection then just inserts a newline there.
    const VTermLineInfo* li = vterm_state_get_lineinfo(s->state_, 0);
    l.wrapped = li && li->continuation;
    s->sb_.push_back(std::move(l));
    if (s->sb_.size() > s->sbLimit_) s->sb_.pop_front();
    ++s->pushed_;
    return 1;
}

int VtermSession::cbPopLine(int cols, VTermScreenCell* cells, void* user) {
    auto* s = static_cast<VtermSession*>(user);
    if (s->sb_.empty()) return 0;
    const SbLine& l = s->sb_.back();
    const VTermScreenCell blank = blankCell();
    for (int c = 0; c < cols; ++c)
        cells[c] = c < static_cast<int>(l.cells.size()) ? l.cells[static_cast<size_t>(c)] : blank;
    s->sb_.pop_back();
    --s->pushed_;
    return 1;
}

int VtermSession::cbSbClear(void* user) {
    auto* s = static_cast<VtermSession*>(user);
    s->sb_.clear();
    s->damaged_ = true;
    return 1;
}

void VtermSession::paste(const std::string& text) {
    std::string bracketOpen;
    capture_ = &bracketOpen;
    vterm_keyboard_start_paste(vt_);  // emits ESC[200~ only when the app set mode 2004
    capture_ = nullptr;
    const bool bracketed = !bracketOpen.empty();
    std::string body = sanitizePaste(text);
    std::string all = bracketOpen + body;
    if (bracketed) {
        std::string close;
        capture_ = &close;
        vterm_keyboard_end_paste(vt_);
        capture_ = nullptr;
        all += close;
    }
    sendBytes(all.data(), all.size());
}

void VtermSession::mouseMove(int row, int col, VTermModifier mod) { vterm_mouse_move(vt_, row, col, mod); }

void VtermSession::mouseButton(int row, int col, int button, bool pressed, VTermModifier mod) {
    if (x10_ && mouseMode_ == VTERM_PROP_MOUSE_NONE) {
        // X10 compatibility: presses only, no modifiers, legacy byte encoding.
        if (!pressed || button < 1 || button > 5) return;
        int code = button <= 3 ? button - 1 : 64 + (button - 4);
        char b[6] = {0x1b, '[', 'M', static_cast<char>(32 + code), static_cast<char>(33 + std::min(col, 222)),
                     static_cast<char>(33 + std::min(row, 222))};
        sendBytes(b, sizeof b);
        return;
    }
    vterm_mouse_move(vt_, row, col, mod);
    vterm_mouse_button(vt_, button, pressed, mod);
}

void VtermSession::focus(bool in) {
    if (in) vterm_state_focus_in(state_);
    else vterm_state_focus_out(state_);
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
    case VTERM_PROP_CURSORSHAPE:
        if (s->ready_) s->cursorShape_ = val->number;
        s->damaged_ = true;
        break;
    case VTERM_PROP_CURSORBLINK:
        if (s->ready_) s->cursorBlink_ = val->boolean ? 1 : 0;
        s->damaged_ = true;
        break;
    case VTERM_PROP_MOUSE:
        s->mouseMode_ = val->number;
        break;
    case VTERM_PROP_ALTSCREEN:
        s->altScreen_ = val->boolean;
        s->damaged_ = true;
        break;
    case VTERM_PROP_REVERSE:
        s->reverse_ = val->boolean;
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
    auto* s = static_cast<VtermSession*>(user);
    if (s->capture_) s->capture_->append(bytes, len);
    else s->sendBytes(bytes, len);
}

}  // namespace lterm
