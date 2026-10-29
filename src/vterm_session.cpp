// SPDX-License-Identifier: GPL-3.0-or-later
#include "vterm_session.hpp"
#include "profiles.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
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

// Length of the valid UTF-8 sequence at s[i] (1-4), or 0 if it is not valid (overlong forms,
// surrogates, > U+10FFFF, truncated). cp receives the code point.
static size_t utf8Seq(const std::string& s, size_t i, uint32_t& cp) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n;
    uint32_t min;
    if (c < 0x80) {
        cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2, cp = c & 0x1F, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3, cp = c & 0x0F, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4, cp = c & 0x07, min = 0x10000;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

std::string sanitizePaste(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        const size_t n = utf8Seq(text, i, cp);
        if (n == 0) {  // invalid byte (incl. a raw 8-bit C1 such as 0x9B): dropped
            ++i;
            continue;
        }
        if (cp == '\r') {
            out += '\r';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (cp == '\n') {
            out += '\r';
        } else if (cp == '\t' || (cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp <= 0x9f))) {
            out.append(text, i, n);
        }
        // other C0 controls (ESC, ^C, ...), DEL and C1 controls are dropped
        i += n;
    }
    return out;
}

size_t clampScrollbackLines(long long requested) {
    if (requested <= 0) return 0;
    return static_cast<unsigned long long>(requested) > kMaxScrollbackLines ? kMaxScrollbackLines
                                                                           : static_cast<size_t>(requested);
}

void truncateUtf8(std::string& s, size_t max) {
    if (s.size() <= max) return;
    size_t n = max;
    // Back up to the start of the sequence that straddles the cut.
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
}

// At least 2 columns: libvterm 0.3.3 writes a wide character's second cell without checking it
// exists, so a wide character on a 1-column screen is a NULL write (the system libvterm0
// segfaults; fuzz finding F8).
VtermSession::VtermSession(int rows, int cols) : rows_(std::max(1, rows)), cols_(std::max(2, cols)) {
    vt_ = vterm_new(rows_, cols_);
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
    static VTermStateFallbacks fbs{};
    fbs.csi = &VtermSession::cbCsiFallback;
    vterm_screen_set_unrecognised_fallbacks(screen_, &fbs, this);
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
    hangup(0);  // the owner (TerminalView) hangs up and reaps first; this is the safety net
    if (fd_ >= 0) close(fd_);
    vterm_free(vt_);
}

pid_t VtermSession::hangup(int waitMs) {
    if (pid_ <= 0) return -1;
    const pid_t pid = pid_;
    pid_ = -1;
    kill(-pid, SIGHUP);  // the child is a session/process-group leader
    kill(pid, SIGHUP);
    // Closing the master also hangs up the PTY for programs that ignore SIGHUP from kill(2).
    for (int waited = 0;; waited += 5) {
        int st;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return -1;  // reaped (or reaped elsewhere)
        if (waited >= waitMs) return pid;
        usleep(5000);
    }
}

// Closes every fd >= 3 in the forked child; only async-signal-safe calls. close_range(2) needs
// Linux 5.9; older kernels (ENOSYS) or seccomp filters (EPERM) get the close() loop.
void closeFrom3(int maxFd) {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#endif
    for (int fd = 3; fd < maxFd; ++fd) close(fd);
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
    // Upper bound for the close() fallback, computed here (getrlimit is not async-signal-safe).
    struct rlimit rl {};
    int maxFd = 65536;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        maxFd = static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 1 << 20));

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
        closeFrom3(maxFd);  // incl. master, GL/X11 sockets
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

void VtermSession::feedRaw(const char* bytes, size_t len) {
    if (len) vterm_input_write(vt_, bytes, len);
}

void VtermSession::snapshotWraps() {
    wrapSnap_.assign(static_cast<size_t>(rows_) + 1, 0);
    for (int r = 0; r < rows_; ++r) {
        const VTermLineInfo* li = vterm_state_get_lineinfo(state_, r);
        wrapSnap_[static_cast<size_t>(r)] = li && li->continuation;
    }
    wrapSnapActive_ = true;
    wrapSnapPushes_ = 0;
}

// Feeds the bytes to libvterm, but every CSI S (scroll up) and CSI M (delete lines) on its own,
// with the line continuation flags saved just before it, so lines that leave the screen
// several at a time keep their soft-wrap flag (see cbPushLine).
static void appendColorSgr(std::string& out, const VTermColor& c, bool fg) {
    char buf[32];
    if ((fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) || (!fg && VTERM_COLOR_IS_DEFAULT_BG(&c))) return;
    if (VTERM_COLOR_IS_INDEXED(&c)) std::snprintf(buf, sizeof buf, ";%d;5;%d", fg ? 38 : 48, c.indexed.idx);
    else std::snprintf(buf, sizeof buf, ";%d;2;%d;%d;%d", fg ? 38 : 48, c.rgb.red, c.rgb.green, c.rgb.blue);
    out += buf;
}

// The current pen as SGR sequences ("ESC [ m" for the default pen). Applied after "ESC [ m".
std::string VtermSession::penSgr() const {
    std::string p;
    VTermValue v;
    auto flag = [&](VTermAttr a, const char* code) {
        if (vterm_state_get_penattr(state_, a, &v) && v.boolean) (p += ';') += code;
    };
    flag(VTERM_ATTR_BOLD, "1");
    flag(VTERM_ATTR_ITALIC, "3");
    flag(VTERM_ATTR_BLINK, "5");
    flag(VTERM_ATTR_REVERSE, "7");
    flag(VTERM_ATTR_CONCEAL, "8");
    flag(VTERM_ATTR_STRIKE, "9");
    if (vterm_state_get_penattr(state_, VTERM_ATTR_UNDERLINE, &v) && v.number) {
        if (v.number == VTERM_UNDERLINE_DOUBLE) p += ";21";
        else if (v.number == VTERM_UNDERLINE_CURLY) p += ";4:3";
        else p += ";4";
    }
    if (vterm_state_get_penattr(state_, VTERM_ATTR_FONT, &v) && v.number > 0 && v.number < 10)
        p += ";" + std::to_string(10 + v.number);
    if (vterm_state_get_penattr(state_, VTERM_ATTR_SMALL, &v) && v.boolean &&
        vterm_state_get_penattr(state_, VTERM_ATTR_BASELINE, &v))
        p += v.number == VTERM_BASELINE_RAISE ? ";73" : v.number == VTERM_BASELINE_LOWER ? ";74" : "";
    // Colours in a second sequence: everything in one could reach 20 arguments, and libvterm
    // 0.3.3 writes past its args[16] (F1; feedRaw() bypasses the guard; fuzz finding F11).
    // At most 10 arguments in each.
    std::string c;
    if (vterm_state_get_penattr(state_, VTERM_ATTR_FOREGROUND, &v)) appendColorSgr(c, v.color, true);
    if (vterm_state_get_penattr(state_, VTERM_ATTR_BACKGROUND, &v)) appendColorSgr(c, v.color, false);
    if (p.empty() && c.empty()) return "\x1b[m";
    std::string out;
    if (!p.empty()) out += "\x1b[" + p.substr(1) + "m";
    if (!c.empty()) out += "\x1b[" + c.substr(1) + "m";
    return out;
}

void VtermSession::clampCursorAfterRestore() {
    VTermPos p;
    vterm_state_get_cursorpos(state_, &p);
    if (p.row < rows_ && p.col < cols_) return;
    const int r = std::min(p.row, rows_ - 1), c = std::min(p.col, cols_ - 1);
    // CUP is relative to the scroll region / margins in origin mode (DECOM), which libvterm does
    // not expose: find the origin with CUP 1;1 first, then move relative to it (libvterm keeps
    // the cursor inside the region in that mode, as a VT does).
    placeCursor(r, c);
}

void VtermSession::placeCursor(int r, int c) {
    feedRaw("\x1b[H", 3);
    VTermPos o;
    vterm_state_get_cursorpos(state_, &o);
    const std::string cup =
        "\x1b[" + std::to_string(std::max(0, r - o.row) + 1) + ";" + std::to_string(std::max(0, c - o.col) + 1) + "H";
    feedRaw(cup.data(), cup.size());
}

void VtermSession::restoreCharsets() {
    cs_ = savedCs_;
    char seq[8] = {0x1b, '(', cs_.g0, 0x1b, ')', cs_.g1, static_cast<char>(cs_.shifted ? 0x0e : 0x0f), 0};
    feedRaw(seq, 7);
}

void VtermSession::feed(const char* bytes, size_t len) {
    size_t done = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = bytes[i];
        switch (csiState_) {
        case 0:
            if (c == 0x1b) {
                csiState_ = 1;
                csiStart_ = static_cast<long>(i);
            } else if (c == 0x0e) cs_.shifted = true;
            else if (c == 0x0f) cs_.shifted = false;
            else {
                // Track the last printed code point for REP (see lastCp_). Anything unusual
                // (broken UTF-8) makes it 0 = unknown, which only makes REP be dropped.
                const unsigned char u = static_cast<unsigned char>(c);
                if (u >= 0x20 && u < 0x7f) {
                    lastCp_ = u;
                    u8need_ = 0;
                } else if (u >= 0x80 && u < 0xc0) {
                    if (u8need_ > 0) {
                        u8acc_ = (u8acc_ << 6) | (u & 0x3f);
                        if (--u8need_ == 0) lastCp_ = u8acc_;
                    } else {
                        lastCp_ = 0;
                    }
                } else if (u >= 0xc2 && u < 0xf5) {
                    u8need_ = u < 0xe0 ? 1 : u < 0xf0 ? 2 : 3;
                    u8acc_ = u & (u < 0xe0 ? 0x1f : u < 0xf0 ? 0x0f : 0x07);
                    lastCp_ = 0;
                } else if (u >= 0x80) {
                    u8need_ = 0;
                    lastCp_ = 0;
                }
            }
            break;  // (8-bit CSI 0x9B is not a control in UTF-8 mode, which we always use)
        case 1:
        case 5: {
            // After ESC. State 5: the ESC came inside a control string. libvterm then ends the
            // string, but the next byte is not "hoisted" into a C1 control: ESC [ there is not
            // a CSI (and ESC \ is the string terminator).
            const bool inString = csiState_ == 5;
            const unsigned char u = static_cast<unsigned char>(c);
            if (c == 0x1b) {
                csiStart_ = static_cast<long>(i);
            } else if (c == 0x18 || c == 0x1a) {
                csiState_ = 0;
            } else if (u < 0x20 || u == 0x7f) {
                // C0 controls (and NUL/DEL) are executed / ignored without ending the escape
            } else if (u >= 0x80) {
                // libvterm ignores a byte >= 0x80 after ESC and stays in the escape (a string
                // it was in is over, though), so ESC 0xC3 [ is still a CSI (F12).
                csiState_ = 1;
            } else if (inString && c == '\\') {
                csiState_ = 0;
            } else if (!inString && c == '[') {
                csiState_ = 2;
                csiParams_.clear();
                csiSeps_ = csiDigits_ = 0;
                csiArgs_ = csiInter_ = false;
            } else if (c == '(' || c == ')') {
                escTarget_ = c == '(' ? 0 : 1;
                csiState_ = 3;
            } else if (!inString && c == ']') {
                csiState_ = 7;  // OSC command number
                csiDigits_ = 0;
            } else if (!inString && c == 'P') {
                csiState_ = 6;  // DCS: command bytes, not yet a string
            } else if (!inString && (c == '_' || c == '^' || c == 'X')) {
                csiState_ = 4;  // APC / PM / SOS string
            } else {
                if (c == '7') savedCs_ = cs_;
                else if (c == '8') {
                    feedRaw(bytes + done, i + 1 - done);
                    done = i + 1;
                    clampCursorAfterRestore();
                    restoreCharsets();
                } else if (c == 'c') cs_ = savedCs_ = Charsets{};  // RIS
                csiState_ = 0;
            }
            break;
        }
        case 6:  // DCS command (libvterm DCS_COMMAND is not a string state: ESC starts over)
            if (c == 0x1b) {
                csiState_ = 1;
                csiStart_ = static_cast<long>(i);
            } else if (c == 0x18 || c == 0x1a) {
                csiState_ = 0;
            } else if (c >= 0x40 && c <= 0x7e) {
                csiState_ = 4;
            }
            break;
        case 7:  // OSC command number: libvterm's int accumulator overflows too (keep 9 digits)
            if (c >= '0' && c <= '9') {
                if (csiDigits_ || c != '0') ++csiDigits_;
                if (csiDigits_ > 9) {
                    feedRaw(bytes + done, i - done);
                    done = i + 1;
                }
                break;
            }
            csiState_ = 4;
            if (c == 0x07 || c == 0x18 || c == 0x1a) csiState_ = 0;
            else if (c == 0x1b) csiState_ = 5;
            break;
        case 2: {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 0x30 && u <= 0x3f) {
                // Mirror libvterm's CSI states: a parameter byte after an intermediate, or a
                // leader byte (< = > ?) after the arguments started, ends the CSI as invalid.
                if (csiInter_ || (u >= 0x3c && csiArgs_)) {
                    csiState_ = 0;
                    break;
                }
                if (u < 0x3c) csiArgs_ = true;
                if (c == ';' || c == ':') {
                    ++csiSeps_;
                    csiDigits_ = 0;
                } else if (u <= '9' && (csiDigits_ || c != '0')) {
                    ++csiDigits_;
                }
                if (csiSeps_ > 15 || csiDigits_ > 9) {  // see csiSeps_ / csiDigits_: drop this byte
                    feedRaw(bytes + done, i - done);
                    done = i + 1;
                    break;
                }
            } else if (u >= 0x20 && u <= 0x2f) {
                csiInter_ = true;
            } else if (u >= 0x80) {
                csiState_ = 0;  // not part of a CSI in UTF-8 mode: libvterm drops the sequence
                break;
            }
            if (c >= 0x40 && c <= 0x7e) {  // final byte
                if ((c == 'r' || c == 's') && !csiParams_.empty()) regionSet_ = true;
                if (c == 'b' && !csiInter_ && (csiParams_.empty() || csiParams_[0] < 0x3c) &&
                    (lastCp_ == 0 || libvtermZeroWidth(lastCp_))) {
                    // REP after a width-0 glyph: libvterm would loop forever (F7). CAN instead
                    // of the final byte aborts the sequence (its start may be in an earlier chunk).
                    feedRaw(bytes + done, i - done);
                    feedRaw("\x18", 1);
                    done = i + 1;
                    csiState_ = 0;
                    break;
                }
                if (c == 'S' || c == 'M') {
                    // The sequence may have started in an earlier chunk; nothing up to here
                    // changed the screen since then, so the snapshot is still exact. (Bytes up
                    // to `done` may already be fed when arguments were dropped.)
                    const size_t start = csiStart_ >= 0 && static_cast<size_t>(csiStart_) >= done
                                             ? static_cast<size_t>(csiStart_) : done;
                    feedRaw(bytes + done, start - done);
                    snapshotWraps();
                    feedRaw(bytes + start, i + 1 - start);
                    wrapSnapActive_ = false;
                    done = i + 1;
                } else if ((c == 'h' || c == 'l') && csiParams_ == "?47") {
                    // xterm's original alternate screen; libvterm only knows 1047/1049. Feed
                    // the (ignored) original, then its libvterm equivalent.
                    feedRaw(bytes + done, i + 1 - done);
                    done = i + 1;
                    feedRaw(c == 'h' ? "\x1b[?1047h" : "\x1b[?1047l", 8);
                } else if ((c == 'h' || c == 'l') && (csiParams_ == "?1048" || csiParams_ == "?1049")) {
                    if (c == 'h') savedCs_ = cs_;
                    else {
                        feedRaw(bytes + done, i + 1 - done);
                        done = i + 1;
                        clampCursorAfterRestore();
                        restoreCharsets();
                    }
                }
                csiState_ = 0;
            } else if (c == 0x1b) {
                csiState_ = 1;
                csiStart_ = static_cast<long>(i);
            } else if (c == 0x18 || c == 0x1a) {
                csiState_ = 0;  // CAN / SUB abort the sequence; other C0 controls do not end it
            } else if (csiParams_.size() < 16 && u >= 0x20) {
                csiParams_ += c;
            }
            break;
        }
        case 4:  // inside a control string, until BEL or ST (ESC \\)
            if (c == 0x07 || c == 0x18 || c == 0x1a) csiState_ = 0;
            else if (c == 0x1b) csiState_ = 5;
            break;
        case 3:  // ESC ( / ESC ) : intermediates, then the final byte names the set
            if (c >= 0x30 && c <= 0x7e) {
                (escTarget_ ? cs_.g1 : cs_.g0) = c;
                csiState_ = 0;
            } else if (c == 0x1b) {
                csiState_ = 1;
                csiStart_ = static_cast<long>(i);
            } else if (c < 0x20 || c > 0x2f) {
                csiState_ = 0;
            }
            break;
        }
    }
    feedRaw(bytes + done, len - done);
    csiStart_ = -1;  // a sequence still open continues in the next chunk
    scanModes(bytes, len);
    vterm_screen_flush_damage(screen_);
}

void VtermSession::clearTopContinuation() {
    if (const VTermLineInfo* li = vterm_state_get_lineinfo(state_, 0); li && li->continuation)
        const_cast<VTermLineInfo*>(li)->continuation = 0;
}

// libvterm 0.3.3 resize_buffer() reflows the active buffer bottom-up. When a logical line does
// not fit any more (it would start above row 0) it stops there and keeps the cursor only if
// `old_row_start <= cursor.row < old_row_end`; old_row_end is inclusive, so a cursor on the last
// row of that line (the usual place: the shell prompt after a long wrapped command, or any
// line that rewraps taller than the new height) is left at -1 and libvterm calls abort():
// "screen_resize failed to update cursor position" (found by the fuzzer; a 5x3 terminal with
// "abcdefg" resized to 1x3 kills the process). This walks the same rows with the same
// arithmetic, reading the active buffer through the public API, and says whether that happens.
bool VtermSession::reflowWouldAbort(int newRows, int newCols) const {
    const int oldRows = rows_, oldCols = cols_;
    VTermPos cur;
    vterm_state_get_cursorpos(state_, &cur);
    auto cont = [&](int r) {
        const VTermLineInfo* li = vterm_state_get_lineinfo(state_, r);
        return li && li->continuation;
    };
    auto popcount = [&](int r) {
        VTermScreenCell cell;
        int c = oldCols - 1;
        while (c >= 0 && vterm_screen_get_cell(screen_, VTermPos{r, c}, &cell) && cell.chars[0] == 0) --c;
        return c + 1;
    };
    int oldRow = oldRows - 1, newRow = newRows - 1, finalBlank = newRows;
    bool found = false;
    while (oldRow >= 0) {
        const int end = oldRow;
        while (oldRow >= 0 && cont(oldRow)) --oldRow;
        const int start = oldRow;
        if (start < 0) return false;  // clearTopContinuation() prevents this
        long width = 0;
        for (int r = start; r <= end; ++r) width += (r < oldRows - 1 && cont(r + 1)) ? oldCols : popcount(r);
        if (finalBlank == newRow + 1 && width == 0) finalBlank = newRow;
        const int height = width ? static_cast<int>((width + newCols - 1) / newCols) : 1;
        int newStart = newRow - height + 1;
        const int spare = newRows - finalBlank;
        if (newStart < 0 && spare >= 0 && !found) {  // (cursor not placed yet: the test passes)
            const int down = std::min(-newStart, spare);
            newStart += down;
            finalBlank += down;
        }
        if (newStart < 0) {
            if (start <= cur.row && cur.row < end) found = true;  // libvterm's (buggy) test
            break;
        }
        if (cur.row >= start && cur.row <= end) found = true;  // placed while copying the line
        oldRow = start - 1;
        newRow = newStart - 1;
    }
    if (cur.row <= oldRow) found = true;
    return !found;
}

void VtermSession::resize(int rows, int cols) {
    if (rows < 1) rows = 1;
    if (cols < 2) cols = 2;  // see the constructor (F8)
    if (rows == rows_ && cols == cols_) return;
    clearTopContinuation();
    // See reflowWouldAbort(): in that case resize without reflow (lines are cut at the new
    // width instead of rewrapped, this once) rather than let libvterm abort the process.
    const bool noReflow = reflowWouldAbort(rows, cols);
    if (noReflow) {
        vterm_screen_enable_reflow(screen_, false);
        ++reflowFallbacks_;
    }
    snapshotWraps();  // shrinking pushes the top rows to the scrollback, several at once
    ++epoch_;         // reflow: search caches start over
    const int oldRows = rows_, oldCols = cols_;
    rows_ = rows;
    cols_ = cols;
    // libvterm fills the cells a resize/reflow creates with the *current* pen, which in a
    // full-screen program is whatever it drew last (vim's reverse status line), and on the
    // main screen behind the alternate one too: blanks came back reverse after leaving vim.
    // Resize with the default pen, then give the application its pen back. Only between
    // sequences: SGR fed into the middle of a CSI or a control string would corrupt it.
    const std::string pen = csiState_ == 0 ? penSgr() : std::string();
    const bool swapPen = !pen.empty() && pen != "\x1b[m";
    if (swapPen) feedRaw("\x1b[m", 3);
    // libvterm 0.3.3 reflow bug: when the top row is the continuation of a line that has
    // already scrolled out, resize_buffer() walks back to row -1 looking for the start of the
    // line and reads cells from before its buffer (heap garbage on screen, invalid UTF-8 handed
    // to Pango, and an out-of-bounds read). The start lives in our scrollback, which keeps its
    // own wrapped flag, so on screen row 0 can simply start a line for libvterm. The lineinfo
    // array is libvterm's mutable state; only the const accessor is public.
    clearTopContinuation();
    vterm_set_size(vt_, rows, cols);  // reflow + scrollback push/pop happen in here
    if (noReflow) vterm_screen_enable_reflow(screen_, true);
    if (regionSet_ && (rows < oldRows || cols < oldCols)) {
        // F10: reset the margins (xterm resets them on every resize). A sequence still open
        // here is cancelled first (CAN), so libvterm and the guard agree on the parser state;
        // C0 controls inside it would otherwise already hit the broken region.
        if (csiState_ != 0) {
            feedRaw("\x18", 1);
            csiState_ = 0;
            lastCp_ = 0;
            u8need_ = 0;
        }
        VTermPos p;
        vterm_state_get_cursorpos(state_, &p);
        feedRaw("\x1b[r\x1b[s", 6);  // DECSTBM / DECSLRM defaults (libvterm: CSI s is always DECSLRM)
        placeCursor(std::min(p.row, rows - 1), std::min(p.col, cols - 1));
        regionSet_ = false;
    }
    if (swapPen) feedRaw(pen.data(), pen.size());
    wrapSnapActive_ = false;
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

// The width libvterm 0.3.3 itself gives a code point (its own table, not exported): measured
// once per code point on a private 1x8 terminal and remembered. Code points below U+1100 are
// all narrow there.
int libvtermWidth(uint32_t cp) {
    if (cp < 0x1100) return 1;
    static std::mutex m;
    static std::vector<int8_t> bmp(0x10000, 0);
    static std::unordered_map<uint32_t, int8_t> astral;
    static VTerm* probe = nullptr;
    std::lock_guard<std::mutex> lock(m);
    int8_t* slot = cp < 0x10000 ? &bmp[cp] : &astral[cp];
    if (*slot) return *slot;
    if (!probe) {
        probe = vterm_new(1, 8);
        vterm_set_utf8(probe, 1);
        vterm_screen_reset(vterm_obtain_screen(probe), 1);
    }
    std::string q = "\x1b[H";
    appendUtf8(q, cp > 0x10FFFF ? 0xFFFD : cp);
    vterm_input_write(probe, q.data(), q.size());
    VTermPos p;
    vterm_state_get_cursorpos(vterm_obtain_state(probe), &p);
    *slot = p.col == 2 ? 2 : 1;
    return *slot;
}

bool libvtermZeroWidth(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7f) return false;
    static std::mutex m;
    static std::unordered_map<uint32_t, bool> cache;
    static VTerm* probe = nullptr;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(cp);
    if (it != cache.end()) return it->second;
    if (!probe) {
        probe = vterm_new(1, 8);
        vterm_set_utf8(probe, 1);
        vterm_screen_reset(vterm_obtain_screen(probe), 1);
    }
    // At the home position a width-0 code point neither moves the cursor nor (being alone
    // there) combines into a wider glyph.
    std::string q = "\x1b[2;3H\x1b[H";
    appendUtf8(q, cp > 0x10FFFF ? 0xFFFD : cp);
    vterm_input_write(probe, q.data(), q.size());
    VTermPos p;
    vterm_state_get_cursorpos(vterm_obtain_state(probe), &p);
    if (cache.size() > 4096) cache.clear();
    return cache[cp] = p.col == 0;
}

// libvterm 0.3.3 keeps a wide character as the cell plus a marker in the next cell, and does not
// keep the two consistent when only one half is overwritten or erased (xterm erases the whole
// character then): a narrow character printed over the left half came back as a 2-cell "x"
// hiding the right half, a wide character whose right half was overwritten or erased came back
// 1 cell wide (drawn over its neighbour), ICH/DCH inside it left a 2-cell blank, and a reflow-free
// resize can leave a wide character in the last column. This makes such cells what a VT would
// show: a narrow character / blank 1 cell wide, a half-covered wide character a blank (keeping
// its colours). It also replaces code points that are not Unicode scalar values (libvterm
// decodes F4 90 80 80 to U+110000) with U+FFFD, so no invalid UTF-8 reaches Pango or the
// clipboard.
void VtermSession::normalizeCell(int col, VTermScreenCell& c) const {
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i] && c.chars[i] != 0xFFFFFFFFu; ++i)
        if (c.chars[i] > 0x10FFFF || (c.chars[i] >= 0xD800 && c.chars[i] <= 0xDFFF)) c.chars[i] = 0xFFFD;
    const uint32_t base = c.chars[0];
    if (base == 0 || base == 0xFFFFFFFFu) {
        if (c.width > 1) c.width = 1;
        return;
    }
    const int w = libvtermWidth(base);
    if (c.width == 2 && (w != 2 || col == cols_ - 1)) {
        if (w == 2) c.chars[0] = 0;  // a wide character with no room for its right half
        c.width = 1;
    } else if (c.width == 1 && w == 2) {
        c.chars[0] = 0;  // right half overwritten / erased
    }
}

bool VtermSession::getCell(long line, int col, VTermScreenCell& out) const {
    if (col < 0 || col >= cols_) return false;
    if (line >= pushed_) {
        long r = line - pushed_;
        if (r >= rows_) return false;
        if (!vterm_screen_get_cell(screen_, VTermPos{static_cast<int>(r), col}, &out)) return false;
        normalizeCell(col, out);
        return true;
    }
    long idx = static_cast<long>(sb_.size()) - (pushed_ - line);
    if (idx < 0) return false;
    const SbLine& l = sb_[static_cast<size_t>(idx)];
    if (col < static_cast<int>(l.cells.size())) out = l.cells[static_cast<size_t>(col)];
    else out = blankCell();
    normalizeCell(col, out);
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
            if (c > 0 && g.at(r, c - 1).width == 2) {
                // The right half of a wide character takes its colours and attributes from the
                // left half: libvterm's marker cell keeps an older pen, so an inverse or coloured
                // wide character had its right half in the default colours (M4 item 5).
                cell = g.at(r, c - 1);
                cell.text.clear();
                cell.width = 0;
                continue;
            }
            toCell(vc, cell);
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

int VtermSession::cellChars(long line, int col, uint32_t* out, int max, int* width) const {
    VTermScreenCell vc;
    if (!getCell(line, col, vc)) {
        *width = 1;
        return 0;
    }
    if (col > 0) {
        VTermScreenCell prev;
        if (getCell(line, col - 1, prev) && prev.width == 2) {
            *width = 0;
            return 0;
        }
    }
    *width = vc.width;
    int n = 0;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && n < max && vc.chars[i] && vc.chars[i] != 0xFFFFFFFFu; ++i)
        out[n++] = vc.chars[i];
    return n;
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
    sbLimit_ = std::min(lines, kMaxScrollbackLines);
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
    // Line k of a push batch is wrapped when old row k+1 is a continuation. libvterm shifts its
    // line info before the screen pushes the scrolled-out rows, so for the usual one-line
    // scroll (a linefeed at the bottom) the old row 1 is now row 0. Multi-line pushes (CSI S,
    // CSI M at the top, a resize) lose the flags of all but the last pushed row, so those use
    // the snapshot taken just before (feed() / resize()).
    if (s->wrapSnapActive_) {
        const size_t k = static_cast<size_t>(s->wrapSnapPushes_++) + 1;
        l.wrapped = k < s->wrapSnap_.size() && s->wrapSnap_[k];
    } else {
        const VTermLineInfo* li = vterm_state_get_lineinfo(s->state_, 0);
        l.wrapped = li && li->continuation;
    }
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
    ++s->epoch_;
    return 1;
}

int VtermSession::cbSbClear(void* user) {
    auto* s = static_cast<VtermSession*>(user);
    s->sb_.clear();
    ++s->epoch_;
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
    // Built from the normalised cells (getCell()), so it says what the screen shows; the same
    // rules as vterm_screen_get_text(): blanks become spaces, trailing blanks are dropped, the
    // right half of a wide character is skipped. (The old version used a fixed 4 KiB buffer with
    // vterm_screen_get_text(), which returns the length it needed and was read past its end on
    // long rows of combining characters: fuzz_regress_test F4.)
    std::string out;
    size_t pending = 0;
    const long line = pushed_ + row;
    for (int c = 0; c < cols_; ++c) {
        uint32_t cp[VTERM_MAX_CHARS_PER_CELL];
        int w = 1;
        const int n = cellChars(line, c, cp, VTERM_MAX_CHARS_PER_CELL, &w);
        if (w == 0) continue;
        if (n == 0) {
            ++pending;
            continue;
        }
        out.append(pending, ' ');
        pending = 0;
        for (int k = 0; k < n; ++k) appendUtf8(out, cp[k]);
    }
    return out;
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
        // Still the primary screen's line info here (libvterm switches after this callback).
        // A resize while the alternate screen is up reflows the primary too, so it must not
        // be left with a continuation in row 0 either (see resize()).
        if (val->boolean && !s->altScreen_) s->clearTopContinuation();
        s->altScreen_ = val->boolean;
        s->damaged_ = true;
        break;
    case VTERM_PROP_REVERSE:
        s->reverse_ = val->boolean;
        s->damaged_ = true;
        break;
    case VTERM_PROP_TITLE:
        if (val->string.initial) s->titleBuf_.clear();
        // Bounded: a program (or `cat` of a hostile file) can send an endless OSC 0.
        if (s->titleBuf_.size() <= kMaxTitleBytes)
            s->titleBuf_.append(val->string.str, std::min(val->string.len, kMaxTitleBytes + 1 - s->titleBuf_.size()));
        if (val->string.final) {
            truncateUtf8(s->titleBuf_, kMaxTitleBytes);
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

// CSI sequences libvterm does not know. DECREQTPARM (CSI 0 x / CSI 1 x, VT100): answer as
// xterm does, "no parity, 8 bits, 38400 baud, clock multiplier 1, no STP flags".
int VtermSession::cbCsiFallback(const char* leader, const long args[], int argcount, const char* intermed,
                                char command, void* user) {
    auto* s = static_cast<VtermSession*>(user);
    if (command == 'x' && !leader && !intermed && argcount <= 1) {
        const long a = argcount == 0 || CSI_ARG_IS_MISSING(args[0]) ? 0 : CSI_ARG(args[0]);
        if (a == 0 || a == 1) {
            char buf[32];
            const int n = std::snprintf(buf, sizeof buf, "\x1b[%ld;1;1;128;128;1;0x", a + 2);
            cbOutput(buf, static_cast<size_t>(n), s);
            return 1;
        }
    }
    return 0;
}

void VtermSession::cbOutput(const char* bytes, size_t len, void* user) {
    auto* s = static_cast<VtermSession*>(user);
    if (s->capture_) s->capture_->append(bytes, len);
    else s->sendBytes(bytes, len);
}

size_t VtermSession::scrollbackBytes() const {
    size_t b = 0;
    for (const SbLine& l : sb_) b += sizeof(SbLine) + l.cells.capacity() * sizeof(VTermScreenCell);
    return b;
}

}  // namespace lterm
