// SPDX-License-Identifier: GPL-3.0-or-later
#include "vterm_session.hpp"
#include "profiles.hpp"

#include <ghostty/vt.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

#ifndef LTERM_VERSION
#define LTERM_VERSION "unknown"
#endif

namespace lterm {

struct VtermSession::Ghost {
    GhosttyTerminal vt = nullptr;
    GhosttyKeyEncoder keys = nullptr;
    GhosttyKeyEvent keyEv = nullptr;
    GhosttyMouseEncoder mouse = nullptr;
    GhosttyMouseEvent mouseEv = nullptr;
    GhosttyRenderState render = nullptr;
    GhosttyTrackedGridRef anchor = nullptr;
    GhosttyColorRgb pal[256]{};
};

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
    env.push_back("TERM_PROGRAM_VERSION=" LTERM_VERSION);
    if (!haveLocale) env.push_back("LANG=C.UTF-8");
    return env;
}

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
        if (n == 0) {
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
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
}

int codepointWidth(uint32_t cp) { return ghostty_unicode_codepoint_width(cp); }

static void appendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s += static_cast<char>(c);
    } else if (c < 0x800) {
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

static int utf8Of(uint32_t cp, char out[4]) {
    std::string s;
    appendUtf8(s, cp);
    std::memcpy(out, s.data(), s.size());
    return static_cast<int>(s.size());
}

static bool modeOn(GhosttyTerminal vt, GhosttyMode mode) {
    GhosttyTerminalModeConfig cfg{};
    cfg.mode = mode;
    if (ghostty_terminal_get(vt, GHOSTTY_TERMINAL_DATA_MODE, &cfg) != GHOSTTY_SUCCESS) return false;
    return cfg.value;
}

static void must(GhosttyResult r, const char* what) {
    if (r != GHOSTTY_SUCCESS) throw std::runtime_error(what);
}

extern "C" void ltermOnBell(GhosttyTerminal, void* user) {
    static_cast<VtermSession*>(user)->onBellFired();
}
extern "C" void ltermOnTitle(GhosttyTerminal, void* user) {
    static_cast<VtermSession*>(user)->onTitleFired();
}
extern "C" void ltermOnWrite(GhosttyTerminal, void* user, const uint8_t* data, size_t len) {
    static_cast<VtermSession*>(user)->onWriteFired(data, len);
}
extern "C" GhosttyString ltermOnVersion(GhosttyTerminal, void*) {
    static const std::string v = std::string("lunduke-terminal ") + LTERM_VERSION;
    GhosttyString s;
    s.ptr = reinterpret_cast<const uint8_t*>(v.data());
    s.len = v.size();
    return s;
}

void VtermSession::onBellFired() {
    if (onBell) onBell();
}

void VtermSession::onTitleFired() {
    GhosttyString str{};
    if (ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_TITLE, &str) != GHOSTTY_SUCCESS) return;
    std::string t(reinterpret_cast<const char*>(str.ptr ? str.ptr : reinterpret_cast<const uint8_t*>("")), str.len);
    truncateUtf8(t, kMaxTitleBytes);
    if (t == title_) return;
    title_ = std::move(t);
    if (onTitle) onTitle();
}

void VtermSession::onWriteFired(const uint8_t* data, size_t len) {
    sendBytes(reinterpret_cast<const char*>(data), len);
}

VtermSession::VtermSession(int rows, int cols) : rows_(std::max(1, rows)), cols_(std::max(1, cols)) {
    if (rows_ > 65535) rows_ = 65535;
    if (cols_ > 65535) cols_ = 65535;
    g_ = new Ghost();
    try {
        must(ghostty_terminal_new(nullptr, &g_->vt, static_cast<uint16_t>(cols_), static_cast<uint16_t>(rows_)),
             "ghostty_terminal_new");
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_USERDATA, this);
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_BELL, reinterpret_cast<const void*>(ltermOnBell));
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, reinterpret_cast<const void*>(ltermOnTitle));
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(ltermOnWrite));
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_XTVERSION, reinterpret_cast<const void*>(ltermOnVersion));
        const char* tn = "xterm-256color";
        GhosttyString termName{reinterpret_cast<const uint8_t*>(tn), std::strlen(tn)};
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_TERMINFO_NAME, &termName);
        GhosttyTerminalModeConfig grapheme{GHOSTTY_MODE_GRAPHEME_CLUSTER, true};
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT, &grapheme);
        uint64_t noImages = 0;
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &noImages);
        size_t unlimited = SIZE_MAX;
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &unlimited);
        size_t lines = sbLimit_;
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &lines);
        ghostty_color_palette_default(g_->pal);
        // xterm's usual 16. libghostty's built-in palette is a softer set; profiles replace
        // these again in setColors(). Indexed 0-15 in tests expect the classic values.
        static const uint8_t xterm16[16][3] = {
            {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0}, {0, 0, 238},   {205, 0, 205},
            {0, 205, 205},   {229, 229, 229}, {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
            {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
        };
        for (int i = 0; i < 16; ++i) g_->pal[i] = GhosttyColorRgb{xterm16[i][0], xterm16[i][1], xterm16[i][2]};
        ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, g_->pal);
        must(ghostty_key_encoder_new(nullptr, &g_->keys), "ghostty_key_encoder_new");
        must(ghostty_key_event_new(nullptr, &g_->keyEv), "ghostty_key_event_new");
        must(ghostty_mouse_encoder_new(nullptr, &g_->mouse), "ghostty_mouse_encoder_new");
        must(ghostty_mouse_event_new(nullptr, &g_->mouseEv), "ghostty_mouse_event_new");
        must(ghostty_render_state_new(nullptr, &g_->render), "ghostty_render_state_new");
        bool track = true;
        ghostty_mouse_encoder_set(g_->mouse, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track);
        syncMouseEncoder();
        pinAnchor();
        refreshCursor(false);
    } catch (...) {
        if (g_->anchor) ghostty_tracked_grid_ref_free(g_->anchor);
        ghostty_render_state_free(g_->render);
        ghostty_mouse_event_free(g_->mouseEv);
        ghostty_mouse_encoder_free(g_->mouse);
        ghostty_key_event_free(g_->keyEv);
        ghostty_key_encoder_free(g_->keys);
        ghostty_terminal_free(g_->vt);
        delete g_;
        g_ = nullptr;
        throw;
    }
}

VtermSession::~VtermSession() {
    hangup(0);
    if (fd_ >= 0) close(fd_);
    if (!g_) return;
    if (g_->anchor) ghostty_tracked_grid_ref_free(g_->anchor);
    ghostty_render_state_free(g_->render);
    ghostty_mouse_event_free(g_->mouseEv);
    ghostty_mouse_encoder_free(g_->mouse);
    ghostty_key_event_free(g_->keyEv);
    ghostty_key_encoder_free(g_->keys);
    ghostty_terminal_free(g_->vt);
    delete g_;
}

void VtermSession::pinAnchor() {
    if (altScreen()) return;
    GhosttyPoint pt{};
    pt.tag = GHOSTTY_POINT_TAG_ACTIVE;
    if (!g_->anchor) {
        if (ghostty_terminal_grid_ref_track(g_->vt, pt, &g_->anchor) != GHOSTTY_SUCCESS) g_->anchor = nullptr;
    } else {
        ghostty_tracked_grid_ref_set(g_->anchor, g_->vt, pt);
    }
    size_t sb = 0;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb);
    anchorY_ = static_cast<uint32_t>(sb);
    anchorGlobal_ = topLine_;
    anchorLive_ = g_->anchor && ghostty_tracked_grid_ref_has_value(g_->anchor);
}

void VtermSession::syncLines() {
    if (altScreen()) return;
    size_t sb = 0;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb);
    if (anchorLive_ && g_->anchor && ghostty_tracked_grid_ref_has_value(g_->anchor)) {
        GhosttyPointCoordinate c{};
        if (ghostty_tracked_grid_ref_point(g_->anchor, GHOSTTY_POINT_TAG_SCREEN, &c) == GHOSTTY_SUCCESS) {
            const long neu = anchorGlobal_ + static_cast<long>(sb) - static_cast<long>(c.y);
            // c.y falling is scrollback trimmed above the anchor. Surviving lines keep
            // their global numbers, so the search cache can drop that prefix instead of
            // treating the history as new. A cleared or invalidated anchor still bumps
            // the epoch below.
            topLine_ = neu;
        }
    } else if (anchorLive_) {
        ++epoch_;
        topLine_ += static_cast<long>(sb) + rows_;
    }
    pinAnchor();
}

void VtermSession::refreshCursor(bool fromApp) {
    if (ghostty_render_state_update(g_->render, g_->vt) != GHOSTTY_SUCCESS) return;
    GhosttyCursorStyle st = GHOSTTY_CURSOR_STYLE_BLOCK;
    bool blink = false;
    ghostty_render_state_get(g_->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &st);
    ghostty_render_state_get(g_->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING, &blink);
    int shape = static_cast<int>(CursorShape::Block);
    if (st == GHOSTTY_CURSOR_STYLE_UNDERLINE) shape = static_cast<int>(CursorShape::Underline);
    else if (st == GHOSTTY_CURSOR_STYLE_BAR) shape = static_cast<int>(CursorShape::Bar);
    const int b = blink ? 1 : 0;
    if (fromApp && (shape != lastStyle_ || b != lastBlink_)) appCursor_ = true;
    lastStyle_ = shape;
    lastBlink_ = b;
    if (appCursor_) {
        cursorShape_ = shape;
        cursorBlink_ = b;
    } else {
        cursorShape_ = static_cast<int>(CursorShape::Unset);
        cursorBlink_ = -1;
    }
}

void VtermSession::setDefaultCursor(CursorShape shape, bool blink) {
    defaultShape_ = static_cast<int>(shape == CursorShape::Unset ? CursorShape::Block : shape);
    defaultBlink_ = blink ? 1 : 0;
    GhosttyCursorStyle st = GHOSTTY_CURSOR_STYLE_BLOCK;
    if (shape == CursorShape::Underline) st = GHOSTTY_CURSOR_STYLE_UNDERLINE;
    else if (shape == CursorShape::Bar) st = GHOSTTY_CURSOR_STYLE_BAR;
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_STYLE, &st);
    bool b = blink;
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &b);
    if (!appCursor_) refreshCursor(false);
}

void VtermSession::syncMouseEncoder() {
    ghostty_mouse_encoder_set_from_terminal(g_->mouse, g_->vt);
    GhosttyMouseEncoderSize sz{};
    sz.size = sizeof sz;
    sz.cell_width = 1;
    sz.cell_height = 1;
    sz.screen_width = static_cast<uint32_t>(std::max(cols_, 1));
    sz.screen_height = static_cast<uint32_t>(std::max(rows_, 1));
    ghostty_mouse_encoder_set(g_->mouse, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &sz);
    bool track = true;
    ghostty_mouse_encoder_set(g_->mouse, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track);
    ghostty_mouse_encoder_set(g_->mouse, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &buttonDown_);
    mouseDirty_ = false;
}

pid_t VtermSession::hangup(int waitMs) {
    if (pid_ <= 0) return -1;
    const pid_t pid = pid_;
    pid_ = -1;
    kill(-pid, SIGHUP);
    kill(pid, SIGHUP);
    for (int waited = 0;; waited += 5) {
        int st;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return -1;
        if (waited >= waitMs) return pid;
        usleep(5000);
    }
}

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
        closeFrom3(maxFd);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int sig = 1; sig < 32; ++sig) signal(sig, SIG_DFL);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            if (chdir(homeDir.c_str()) != 0) {
            }
        }
        execvpe(file.c_str(), cargv.data(), cenv.data());
        int err = errno;
        ssize_t w = write(2, failMsg.data(), failMsg.size());
        const char* es = strerror(err);
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

bool VtermSession::readAvailable(int64_t budgetUs, bool* more) {
    if (more) *more = false;
    char buf[16384];
    using Clock = std::chrono::steady_clock;
    const bool timed = budgetUs > 0;
    const Clock::time_point deadline = Clock::now() + std::chrono::microseconds(timed ? budgetUs : 0);
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n > 0) {
            feed(buf, static_cast<size_t>(n));
            if (timed) {
                // Yield while bytes are already waiting, even when the parser is fast enough
                // to empty a kernel buffer inside the time budget. Otherwise a flood wakes the
                // fd watch, drains it, and paints an uncapped frame every time.
                pollfd pfd{};
                pfd.fd = fd_;
                pfd.events = POLLIN;
                const bool waiting = Clock::now() >= deadline || (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN));
                if (waiting) {
                    if (more) *more = true;
                    return true;
                }
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return true;
        return false;
    }
}

void VtermSession::feed(const char* bytes, size_t len) {
    if (!len) return;
    fedBytes_ += len;
    compSettled_ = false;
    ghostty_terminal_vt_write(g_->vt, reinterpret_cast<const uint8_t*>(bytes), len);
    syncLines();
    refreshCursor(true);
    mouseDirty_ = true;
    damaged_ = true;
}

void VtermSession::resize(int rows, int cols) {
    rows = std::max(1, std::min(rows, 65535));
    cols = std::max(1, std::min(cols, 65535));
    ++epoch_;
    ghostty_terminal_resize(g_->vt, static_cast<uint16_t>(cols), static_cast<uint16_t>(rows), 1, 1);
    uint16_t rr = 0, cc = 0;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_ROWS, &rr);
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_COLS, &cc);
    rows_ = rr ? rr : rows;
    cols_ = cc ? cc : cols;
    syncLines();
    syncMouseEncoder();
    refreshCursor(false);
    if (fd_ >= 0) {
        struct winsize ws {};
        ws.ws_row = static_cast<unsigned short>(rows_);
        ws.ws_col = static_cast<unsigned short>(cols_);
        ioctl(fd_, TIOCSWINSZ, &ws);
    }
    damaged_ = true;
}

bool VtermSession::compressScrollback(bool full) {
    uint64_t act = 0;
    if (ghostty_terminal_compression_activity(g_->vt, &act) != GHOSTTY_SUCCESS) return false;
    if (!full && act == compActivity_ && compSettled_) return false;
    if (act != compActivity_) {
        compActivity_ = act;
        compSettled_ = false;
    }
    GhosttyTerminalCompressionResult result = GHOSTTY_TERMINAL_COMPRESSION_RESULT_UNSUPPORTED;
    const GhosttyTerminalCompressionMode mode =
        full ? GHOSTTY_TERMINAL_COMPRESSION_MODE_FULL : GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL;
    if (ghostty_terminal_compress(g_->vt, mode, &result) != GHOSTTY_SUCCESS) return false;
    compSettled_ = result != GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING;
    return result == GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING;
}

size_t VtermSession::takeFedBytes() {
    const size_t n = fedBytes_;
    fedBytes_ = 0;
    return n;
}

void VtermSession::setScrollbackLimit(size_t lines) {
    sbLimit_ = lines > kMaxScrollbackLines ? kMaxScrollbackLines : lines;
    size_t n = sbLimit_;
    size_t unlimited = SIZE_MAX;
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &unlimited);
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &n);
    syncLines();
    damaged_ = true;
}

static int ghostKey(TermKey key) {
    switch (key) {
    case TermKey::Enter: return GHOSTTY_KEY_ENTER;
    case TermKey::Tab: return GHOSTTY_KEY_TAB;
    case TermKey::Backspace: return GHOSTTY_KEY_BACKSPACE;
    case TermKey::Escape: return GHOSTTY_KEY_ESCAPE;
    case TermKey::Up: return GHOSTTY_KEY_ARROW_UP;
    case TermKey::Down: return GHOSTTY_KEY_ARROW_DOWN;
    case TermKey::Left: return GHOSTTY_KEY_ARROW_LEFT;
    case TermKey::Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case TermKey::Insert: return GHOSTTY_KEY_INSERT;
    case TermKey::Delete: return GHOSTTY_KEY_DELETE;
    case TermKey::Home: return GHOSTTY_KEY_HOME;
    case TermKey::End: return GHOSTTY_KEY_END;
    case TermKey::PageUp: return GHOSTTY_KEY_PAGE_UP;
    case TermKey::PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case TermKey::Kp0: return GHOSTTY_KEY_NUMPAD_0;
    case TermKey::Kp1: return GHOSTTY_KEY_NUMPAD_1;
    case TermKey::Kp2: return GHOSTTY_KEY_NUMPAD_2;
    case TermKey::Kp3: return GHOSTTY_KEY_NUMPAD_3;
    case TermKey::Kp4: return GHOSTTY_KEY_NUMPAD_4;
    case TermKey::Kp5: return GHOSTTY_KEY_NUMPAD_5;
    case TermKey::Kp6: return GHOSTTY_KEY_NUMPAD_6;
    case TermKey::Kp7: return GHOSTTY_KEY_NUMPAD_7;
    case TermKey::Kp8: return GHOSTTY_KEY_NUMPAD_8;
    case TermKey::Kp9: return GHOSTTY_KEY_NUMPAD_9;
    case TermKey::KpMultiply: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case TermKey::KpAdd: return GHOSTTY_KEY_NUMPAD_ADD;
    case TermKey::KpSubtract: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case TermKey::KpDecimal: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case TermKey::KpDivide: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case TermKey::KpEnter: return GHOSTTY_KEY_NUMPAD_ENTER;
    case TermKey::KpEqual: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case TermKey::F1: return GHOSTTY_KEY_F1;
    case TermKey::F2: return GHOSTTY_KEY_F2;
    case TermKey::F3: return GHOSTTY_KEY_F3;
    case TermKey::F4: return GHOSTTY_KEY_F4;
    case TermKey::F5: return GHOSTTY_KEY_F5;
    case TermKey::F6: return GHOSTTY_KEY_F6;
    case TermKey::F7: return GHOSTTY_KEY_F7;
    case TermKey::F8: return GHOSTTY_KEY_F8;
    case TermKey::F9: return GHOSTTY_KEY_F9;
    case TermKey::F10: return GHOSTTY_KEY_F10;
    case TermKey::F11: return GHOSTTY_KEY_F11;
    case TermKey::F12: return GHOSTTY_KEY_F12;
    }
    return GHOSTTY_KEY_UNIDENTIFIED;
}

static GhosttyMods ghostMods(int mod) {
    GhosttyMods m = 0;
    if (mod & ModShift) m = static_cast<GhosttyMods>(m | GHOSTTY_MODS_SHIFT);
    if (mod & ModAlt) m = static_cast<GhosttyMods>(m | GHOSTTY_MODS_ALT);
    if (mod & ModCtrl) m = static_cast<GhosttyMods>(m | GHOSTTY_MODS_CTRL);
    return m;
}

void VtermSession::encodeKey(int key, int mod, const char* utf8, size_t utf8Len, uint32_t unshifted, KeyAction action) {
    ghostty_key_encoder_set_from_terminal(g_->keys, g_->vt);
    GhosttyKeyAction act = GHOSTTY_KEY_ACTION_PRESS;
    if (action == KeyAction::Release) act = GHOSTTY_KEY_ACTION_RELEASE;
    else if (action == KeyAction::Repeat) act = GHOSTTY_KEY_ACTION_REPEAT;
    ghostty_key_event_set_action(g_->keyEv, act);
    ghostty_key_event_set_key(g_->keyEv, static_cast<GhosttyKey>(key));
    ghostty_key_event_set_mods(g_->keyEv, ghostMods(mod));
    // Consumed mods are cleared every event. Leaving Alt or Ctrl consumed from
    // an earlier translation made the next press of that modifier encode as a
    // plain character.
    ghostty_key_event_set_consumed_mods(g_->keyEv, 0);
    GhosttyString text{};
    text.ptr = reinterpret_cast<const uint8_t*>(utf8 ? utf8 : "");
    text.len = utf8 ? utf8Len : 0;
    ghostty_key_event_set_utf8(g_->keyEv, text);
    // The event object is reused. A zero here used to leave the previous key's
    // codepoint in place, so kitty mode turned Alt+é after Ctrl+a into Alt+a.
    ghostty_key_event_set_unshifted_codepoint(g_->keyEv, unshifted);
    uint8_t buf[128];
    size_t n = 0;
    GhosttyResult r = ghostty_key_encoder_encode(g_->keys, g_->keyEv, buf, sizeof buf, &n);
    std::string big;
    if (r == GHOSTTY_OUT_OF_SPACE) {
        big.resize(n);
        size_t n2 = 0;
        r = ghostty_key_encoder_encode(g_->keys, g_->keyEv, reinterpret_cast<uint8_t*>(big.data()), big.size(), &n2);
        n = n2;
    }
    if (r != GHOSTTY_SUCCESS || n == 0) return;
    sendBytes(big.empty() ? reinterpret_cast<char*>(buf) : big.data(), n);
}

void VtermSession::sendKey(TermKey key, int mod, KeyAction action) {
    encodeKey(ghostKey(key), mod, nullptr, 0, 0, action);
}

static uint8_t kittyFlags(GhosttyTerminal vt) {
    uint8_t flags = 0;
    if (ghostty_terminal_get(vt, GHOSTTY_TERMINAL_DATA_KITTY_KEYBOARD_FLAGS, &flags) != GHOSTTY_SUCCESS) return 0;
    return flags;
}

bool VtermSession::reportsKeyEvents() const {
    return (kittyFlags(g_->vt) & GHOSTTY_KITTY_KEY_REPORT_EVENTS) != 0;
}

bool VtermSession::reportsAllKeys() const {
    return (kittyFlags(g_->vt) & GHOSTTY_KITTY_KEY_REPORT_ALL) != 0;
}

void VtermSession::sendPhysical(int ghostKey, int mod, const char* utf8, size_t utf8Len, uint32_t unshifted,
                                KeyAction action) {
    encodeKey(ghostKey, mod, utf8, utf8Len, unshifted, action);
}

void VtermSession::sendChar(uint32_t cp, int mod, uint32_t unshifted, KeyAction action) {
    int key = GHOSTTY_KEY_UNIDENTIFIED;
    if (cp >= 'a' && cp <= 'z') key = GHOSTTY_KEY_A + static_cast<int>(cp - 'a');
    else if (cp >= 'A' && cp <= 'Z') key = GHOSTTY_KEY_A + static_cast<int>(cp - 'A');
    else if (cp >= '0' && cp <= '9') key = GHOSTTY_KEY_DIGIT_0 + static_cast<int>(cp - '0');
    else if (cp == ' ') key = GHOSTTY_KEY_SPACE;
    else if (cp == '\t') key = GHOSTTY_KEY_TAB;
    else if (cp == '\r' || cp == '\n') key = GHOSTTY_KEY_ENTER;
    else if (cp == 0x1b) key = GHOSTTY_KEY_ESCAPE;
    else if (cp == 0x7f) key = GHOSTTY_KEY_BACKSPACE;
    char utf8[4];
    const bool printable = cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp <= 0x9f);
    const int n = printable ? utf8Of(cp, utf8) : 0;
    // Unshifted codepoint of this event, including non-ASCII (Alt+é is U+00E9,
    // not "no codepoint"). Control keys have none, which clears the field.
    // An explicit unshifted value (the key with Shift released) wins, so
    // Shift+A is reported as "a" plus Shift rather than as the letter A.
    const uint32_t uns = unshifted ? unshifted : (printable ? cp : 0);
    encodeKey(key, mod, n ? utf8 : nullptr, static_cast<size_t>(n), uns, action);
}

void VtermSession::sendBytes(const char* bytes, size_t len) {
    if (!len) return;
    std::string& q = fd_ < 0 ? pendingOut_ : writeQueue_;
    if (q.size() >= kMaxWriteQueueBytes) return;
    if (q.size() + len > kMaxWriteQueueBytes) len = kMaxWriteQueueBytes - q.size();
    q.append(bytes, len);
    if (fd_ >= 0) flushWrites();
}

void VtermSession::flushWrites() {
    size_t off = 0;
    while (off < writeQueue_.size()) {
        ssize_t n = write(fd_, writeQueue_.data() + off, writeQueue_.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) off = writeQueue_.size();
            break;
        }
        off += static_cast<size_t>(n);
    }
    if (off) writeQueue_.erase(0, off);
}

void VtermSession::paste(const std::string& text) {
    std::string& q = fd_ < 0 ? pendingOut_ : writeQueue_;
    if (q.size() >= kMaxWriteQueueBytes) return;
    std::string body = sanitizePaste(text);
    const bool bracketed = modeOn(g_->vt, GHOSTTY_MODE_BRACKETED_PASTE);
    // Bracketed paste adds ESC[200~ and ESC[201~. Keep the whole sequence, including
    // the closing bracket, inside the queue cap. The tail of an oversized paste is
    // dropped until the program reads and the queue drains.
    const size_t overhead = bracketed ? 12 : 0;
    const size_t room = kMaxWriteQueueBytes - q.size();
    if (overhead >= room) return;
    if (body.size() > room - overhead) truncateUtf8(body, room - overhead);
    if (body.empty()) return;
    size_t need = 0;
    GhosttyResult r = ghostty_paste_encode(body.data(), body.size(), bracketed, nullptr, 0, &need);
    if (r == GHOSTTY_SUCCESS) return;
    if (r != GHOSTTY_OUT_OF_SPACE) return;
    std::string buf(need, '\0');
    size_t n = 0;
    r = ghostty_paste_encode(body.data(), body.size(), bracketed, reinterpret_cast<uint8_t*>(buf.data()), buf.size(), &n);
    if (r == GHOSTTY_SUCCESS && n) sendBytes(buf.data(), n);
}

static GhosttyMouseButton mouseButtonId(int button) {
    switch (button) {
    case 1: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case 2: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case 3: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case 4: return GHOSTTY_MOUSE_BUTTON_FOUR;
    case 5: return GHOSTTY_MOUSE_BUTTON_FIVE;
    case 6: return GHOSTTY_MOUSE_BUTTON_SIX;
    case 7: return GHOSTTY_MOUSE_BUTTON_SEVEN;
    case 8: return GHOSTTY_MOUSE_BUTTON_EIGHT;
    default: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

void VtermSession::emitMouse(int row, int col, int button, int action, int mod) {
    if (mouseDirty_) syncMouseEncoder();
    else ghostty_mouse_encoder_set(g_->mouse, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &buttonDown_);
    ghostty_mouse_event_set_action(g_->mouseEv, static_cast<GhosttyMouseAction>(action));
    if (button > 0) ghostty_mouse_event_set_button(g_->mouseEv, mouseButtonId(button));
    else ghostty_mouse_event_clear_button(g_->mouseEv);
    ghostty_mouse_event_set_mods(g_->mouseEv, ghostMods(mod));
    const int rr = std::max(0, std::min(row, rows_ - 1));
    const int cc = std::max(0, std::min(col, cols_ - 1));
    GhosttyMousePosition pos{static_cast<float>(cc), static_cast<float>(rr)};
    ghostty_mouse_event_set_position(g_->mouseEv, pos);
    uint8_t buf[128];
    size_t n = 0;
    if (ghostty_mouse_encoder_encode(g_->mouse, g_->mouseEv, buf, sizeof buf, &n) == GHOSTTY_SUCCESS && n)
        sendBytes(reinterpret_cast<char*>(buf), n);
}

void VtermSession::mouseMove(int row, int col, int mod) {
    emitMouse(row, col, heldButton_, GHOSTTY_MOUSE_ACTION_MOTION, mod);
}

void VtermSession::mouseButton(int row, int col, int button, bool pressed, int mod) {
    if (button >= 1 && button <= 3) {
        buttonDown_ = pressed;
        heldButton_ = pressed ? button : 0;
    }
    emitMouse(row, col, button, pressed ? GHOSTTY_MOUSE_ACTION_PRESS : GHOSTTY_MOUSE_ACTION_RELEASE, mod);
}

void VtermSession::focus(bool in) {
    if (!modeOn(g_->vt, GHOSTTY_MODE_FOCUS_EVENT)) return;
    uint8_t buf[16];
    size_t n = 0;
    if (ghostty_focus_encode(in ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, buf, sizeof buf, &n) == GHOSTTY_SUCCESS && n)
        sendBytes(reinterpret_cast<char*>(buf), n);
}

void VtermSession::setColors(Color8 fg, Color8 bg, const std::array<Color8, 16>& palette, float minFgGrey) {
    fg_ = fg;
    bg_ = bg;
    palette_ = palette;
    havePalette_ = true;
    minFgGrey_ = minFgGrey;
    for (int i = 0; i < 16; ++i) g_->pal[i] = GhosttyColorRgb{palette[i].r, palette[i].g, palette[i].b};
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, g_->pal);
    GhosttyColorRgb f{fg.r, fg.g, fg.b}, b{bg.r, bg.g, bg.b};
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &f);
    ghostty_terminal_set(g_->vt, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &b);
    damaged_ = true;
}

bool VtermSession::altScreen() const {
    GhosttyTerminalScreen sc = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &sc);
    return sc == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
}

bool VtermSession::reverseVideo() const { return modeOn(g_->vt, GHOSTTY_MODE_REVERSE_COLORS); }

int VtermSession::mouseMode() const {
    if (modeOn(g_->vt, GHOSTTY_MODE_MOUSE_EVENT_ANY)) return static_cast<int>(MouseMode::Move);
    if (modeOn(g_->vt, GHOSTTY_MODE_MOUSE_EVENT_BUTTON)) return static_cast<int>(MouseMode::Drag);
    if (modeOn(g_->vt, GHOSTTY_MODE_MOUSE_EVENT_NORMAL) || modeOn(g_->vt, GHOSTTY_MODE_MOUSE_EVENT_X10))
        return static_cast<int>(MouseMode::Click);
    return static_cast<int>(MouseMode::None);
}

size_t VtermSession::scrollbackSize() const {
    if (altScreen()) return 0;
    size_t sb = 0;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb);
    return sb;
}

long VtermSession::firstLine() const {
    if (altScreen()) return topLine_;
    const long sb = static_cast<long>(scrollbackSize());
    return sb > topLine_ ? 0 : topLine_ - sb;
}

bool VtermSession::locate(long line, int col, void* out) const {
    auto* pt = static_cast<GhosttyPoint*>(out);
    *pt = GhosttyPoint{};
    if (col < 0 || col >= cols_) return false;
    if (altScreen()) {
        const long row = line - topLine_;
        if (row < 0 || row >= rows_) return false;
        pt->tag = GHOSTTY_POINT_TAG_ACTIVE;
        pt->value.coordinate.x = static_cast<uint16_t>(col);
        pt->value.coordinate.y = static_cast<uint32_t>(row);
        return true;
    }
    size_t sb = 0, total = 0;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &sb);
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &total);
    const long y = line - (topLine_ - static_cast<long>(sb));
    if (y < 0 || static_cast<size_t>(y) >= total) return false;
    pt->tag = GHOSTTY_POINT_TAG_SCREEN;
    pt->value.coordinate.x = static_cast<uint16_t>(col);
    pt->value.coordinate.y = static_cast<uint32_t>(y);
    return true;
}

Color8 VtermSession::resolveColor(uint8_t tag, uint8_t a, uint8_t b, uint8_t c, bool foreground) const {
    if (tag == GHOSTTY_STYLE_COLOR_NONE) return foreground ? fg_ : bg_;
    if (tag == GHOSTTY_STYLE_COLOR_PALETTE) {
        if (havePalette_ && a < 16) return palette_[a];
        Color8 o{g_->pal[a].r, g_->pal[a].g, g_->pal[a].b};
        return foreground ? liftToGrey(o, minFgGrey_) : o;
    }
    Color8 o{a, b, c};
    return foreground ? liftToGrey(o, minFgGrey_) : o;
}

VtermSession::CellRead VtermSession::readAt(long line, int col) const {
    CellRead out;
    GhosttyPoint pt{};
    if (!locate(line, col, &pt)) return out;
    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(g_->vt, pt, &ref) != GHOSTTY_SUCCESS) return out;
    out.ok = true;
    GhosttyCell cell = 0;
    ghostty_grid_ref_cell(&ref, &cell);
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_WIDE) out.width = 2;
    else if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) out.width = 0;
    else out.width = 1;
    size_t n = 0;
    GhosttyResult gr = ghostty_grid_ref_graphemes(&ref, out.cp, 16, &n);
    if (gr == GHOSTTY_OUT_OF_SPACE) {
        std::vector<uint32_t> big(n);
        gr = ghostty_grid_ref_graphemes(&ref, big.data(), big.size(), &n);
        if (gr == GHOSTTY_SUCCESS) {
            out.n = static_cast<int>(std::min(n, sizeof out.cp / sizeof out.cp[0]));
            for (int i = 0; i < out.n; ++i) out.cp[i] = big[static_cast<size_t>(i)];
        }
    } else if (gr == GHOSTTY_SUCCESS) {
        out.n = static_cast<int>(n);
    }
    GhosttyStyle st{};
    ghostty_grid_ref_style(&ref, &st);
    out.cell.conceal = (st.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) != 0;
    for (int i = 0; i < out.n; ++i) appendUtf8(out.cell.text, out.cp[i]);
    auto bytes = [](GhosttyStyleColor col, uint8_t& a, uint8_t& b, uint8_t& c) {
        if (col.tag == GHOSTTY_STYLE_COLOR_PALETTE) {
            a = col.value.palette;
            b = c = 0;
        } else if (col.tag == GHOSTTY_STYLE_COLOR_RGB) {
            a = col.value.rgb.r;
            b = col.value.rgb.g;
            c = col.value.rgb.b;
        } else {
            a = b = c = 0;
        }
    };
    uint8_t fr, fg, fb, br, bg, bb;
    bytes(st.fg_color, fr, fg, fb);
    bytes(st.bg_color, br, bg, bb);
    Color8 fgc = resolveColor(st.fg_color.tag, fr, fg, fb, true);
    Color8 bgc = resolveColor(st.bg_color.tag, br, bg, bb, false);
    const bool inv = ((st.flags & GHOSTTY_STYLE_FLAG_INVERSE) != 0) != reverseVideo();
    if (inv) std::swap(fgc, bgc);
    out.cell.fg = fgc;
    out.cell.bg = bgc;
    out.cell.bold = (st.flags & GHOSTTY_STYLE_FLAG_BOLD) != 0;
    out.cell.italic = (st.flags & GHOSTTY_STYLE_FLAG_ITALIC) != 0;
    out.cell.faint = (st.flags & GHOSTTY_STYLE_FLAG_FAINT) != 0;
    out.cell.blink = (st.flags & GHOSTTY_STYLE_FLAG_BLINK) != 0;
    out.cell.strikethrough = (st.flags & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) != 0;
    out.cell.overline = (st.flags & GHOSTTY_STYLE_FLAG_OVERLINE) != 0;
    switch (GHOSTTY_STYLE_UNDERLINE(st)) {
    case GHOSTTY_SGR_UNDERLINE_SINGLE: out.cell.underline = Underline::Single; break;
    case GHOSTTY_SGR_UNDERLINE_DOUBLE: out.cell.underline = Underline::Double; break;
    case GHOSTTY_SGR_UNDERLINE_CURLY: out.cell.underline = Underline::Curly; break;
    case GHOSTTY_SGR_UNDERLINE_DOTTED: out.cell.underline = Underline::Dotted; break;
    case GHOSTTY_SGR_UNDERLINE_DASHED: out.cell.underline = Underline::Dashed; break;
    default: out.cell.underline = Underline::None; break;
    }
    if (st.underline_color.tag != GHOSTTY_STYLE_COLOR_NONE) {
        uint8_t ur, ug, ub;
        bytes(st.underline_color, ur, ug, ub);
        out.cell.underlineColored = true;
        out.cell.underlineColor = resolveColor(st.underline_color.tag, ur, ug, ub, true);
    }
    out.cell.width = out.width;
    return out;
}

void VtermSession::fillGrid(Grid& grid, int scrollOffset) const {
    grid.resize(rows_, cols_);
    int off = 0;
    if (!altScreen()) {
        const int sb = static_cast<int>(scrollbackSize());
        if (scrollOffset < 0) scrollOffset = 0;
        if (scrollOffset > sb) scrollOffset = sb;
        off = scrollOffset;
    }
    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            CellRead rd = readAt(topLine_ - off + r, c);
            if (rd.ok) grid.at(r, c) = std::move(rd.cell);
        }
    }
    uint16_t cx = 0, cy = 0;
    bool vis = true;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cx);
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &cy);
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &vis);
    const int viewRow = static_cast<int>(cy) + off;
    grid.cursorCol = std::min(static_cast<int>(cx), std::max(0, cols_ - 1));
    if (!vis || viewRow < 0 || viewRow >= rows_) {
        grid.cursorVisible = false;
        grid.cursorRow = std::min(static_cast<int>(cy), std::max(0, rows_ - 1));
    } else {
        grid.cursorVisible = true;
        grid.cursorRow = viewRow;
    }
}

std::string VtermSession::rowText(int row) const {
    std::string out;
    size_t pending = 0;
    const long line = topLine_ + row;
    for (int c = 0; c < cols_; ++c) {
        uint32_t cp[16];
        int w = 1;
        const int n = cellChars(line, c, cp, 16, &w);
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

std::string VtermSession::cellText(long line, int col) const {
    CellRead rd = readAt(line, col);
    if (!rd.ok || rd.width == 0) return {};
    return rd.cell.text;
}

int VtermSession::cellWidth(long line, int col) const {
    CellRead rd = readAt(line, col);
    return rd.ok ? rd.width : 1;
}

bool VtermSession::wrapped(long line) const {
    GhosttyPoint pt{};
    if (!locate(line, 0, &pt)) return false;
    GhosttyGridRef ref{};
    if (ghostty_terminal_grid_ref(g_->vt, pt, &ref) != GHOSTTY_SUCCESS) return false;
    GhosttyRow row = 0;
    ghostty_grid_ref_row(&ref, &row);
    bool wrap = false;
    ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &wrap);
    return wrap;
}

int VtermSession::cellChars(long line, int col, uint32_t* out, int max, int* width) const {
    CellRead rd = readAt(line, col);
    if (width) *width = rd.ok ? rd.width : 1;
    if (!rd.ok || rd.width == 0) return 0;
    const int n = std::min(rd.n, max);
    for (int i = 0; i < n; ++i) out[i] = rd.cp[i];
    return n;
}

size_t VtermSession::scrollbackBytes() const {
    GhosttyTerminalMemoryUsage m{};
    m.size = sizeof m;
    if (ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_MEMORY_USAGE, &m) != GHOSTTY_SUCCESS) return 0;
    return static_cast<size_t>(m.primary_resident_bytes + m.alternate_resident_bytes);
}

size_t VtermSession::scrollbackBytesFull(size_t lines, int cols) {
    // Page-granular history. 32 bytes/cell plus a per-line overhead is the
    // order of magnitude the settings note uses; RSS is measured separately.
    const size_t c = static_cast<size_t>(std::max(cols, 1));
    return lines * c * 32 + lines * 64;
}

}  // namespace lterm
