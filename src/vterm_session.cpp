// SPDX-License-Identifier: GPL-3.0-or-later
#include "vterm_session.hpp"
#include "profiles.hpp"

#include <ghostty/vt.h>

#include <algorithm>
#include <chrono>
#include <climits>
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
    GhosttyRenderStateRowIterator rows = nullptr;
    GhosttyTrackedGridRef anchor = nullptr;
    GhosttyColorRgb pal[256]{};
    long pinLine = LONG_MIN;
    unsigned long pinEpoch = 0;
    bool pinOk = false;
    bool pinWrap = false;
    GhosttyGridRef pin{};
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
        must(ghostty_render_state_row_iterator_new(nullptr, &g_->rows), "ghostty_render_state_row_iterator_new");
        bool track = true;
        ghostty_mouse_encoder_set(g_->mouse, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track);
        syncMouseEncoder();
        pinAnchor();
        refreshCursor(false);
    } catch (...) {
        if (g_->anchor) ghostty_tracked_grid_ref_free(g_->anchor);
        ghostty_render_state_row_iterator_free(g_->rows);
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
    ghostty_render_state_row_iterator_free(g_->rows);
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
    noteDirty();
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
    g_->pinLine = LONG_MIN;  // grid refs do not survive a write
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
    g_->pinLine = LONG_MIN;
    viewFull_ = true;
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

static uint8_t kittyFlags(GhosttyTerminal vt);

// libghostty's legacy encoder follows fixterms: Ctrl+Shift+letter, Ctrl+@ and
// Ctrl+[ (and Ctrl+i / Ctrl+m) become CSI u sequences even when no kitty keyboard
// mode is on. A classic terminal sends the C0 byte: ^A, NUL, ESC, TAB, CR.
static int legacyCtrlByte(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') cp = cp - 'A' + 'a';
    if (cp >= 'a' && cp <= 'z') return static_cast<int>(cp & 0x1fu);
    switch (cp) {
    case ' ':
    case '@': return 0;
    case '[': return 0x1b;
    case '\\': return 0x1c;
    case ']': return 0x1d;
    case '^': return 0x1e;
    case '_':
    case '/': return 0x1f;
    case '?': return 0x7f;
    default: return -1;
    }
}

static int ghostKeyForCodepoint(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return GHOSTTY_KEY_A + static_cast<int>(cp - 'a');
    if (cp >= 'A' && cp <= 'Z') return GHOSTTY_KEY_A + static_cast<int>(cp - 'A');
    if (cp >= '0' && cp <= '9') return GHOSTTY_KEY_DIGIT_0 + static_cast<int>(cp - '0');
    switch (cp) {
    case ' ': return GHOSTTY_KEY_SPACE;
    case '\t': return GHOSTTY_KEY_TAB;
    case '\r':
    case '\n': return GHOSTTY_KEY_ENTER;
    case 0x1b: return GHOSTTY_KEY_ESCAPE;
    case 0x7f: return GHOSTTY_KEY_BACKSPACE;
    case '-':
    case '_': return GHOSTTY_KEY_MINUS;
    case '=':
    case '+': return GHOSTTY_KEY_EQUAL;
    case '[':
    case '{': return GHOSTTY_KEY_BRACKET_LEFT;
    case ']':
    case '}': return GHOSTTY_KEY_BRACKET_RIGHT;
    case '\\':
    case '|': return GHOSTTY_KEY_BACKSLASH;
    case ';':
    case ':': return GHOSTTY_KEY_SEMICOLON;
    case '\'':
    case '"': return GHOSTTY_KEY_QUOTE;
    case ',':
    case '<': return GHOSTTY_KEY_COMMA;
    case '.':
    case '>': return GHOSTTY_KEY_PERIOD;
    case '/':
    case '?': return GHOSTTY_KEY_SLASH;
    case '`':
    case '~': return GHOSTTY_KEY_BACKQUOTE;
    case '@': return GHOSTTY_KEY_DIGIT_2;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

void VtermSession::encodeKey(int key, int mod, const char* utf8, size_t utf8Len, uint32_t unshifted, KeyAction action) {
    // Kitty mode keeps libghostty's encoding, including Ctrl+Shift as a CSI u
    // sequence. With no kitty flags, those chords are the legacy control bytes.
    if (action != KeyAction::Release && kittyFlags(g_->vt) == 0 && (mod & ModCtrl) &&
        (mod & ~(ModCtrl | ModShift | ModAlt)) == 0) {
        uint32_t cp = 0;
        if (utf8 && utf8Len == 1) cp = static_cast<unsigned char>(utf8[0]);
        if ((mod & ModShift) && unshifted >= 'a' && unshifted <= 'z') cp = unshifted;
        else if ((mod & ModShift) && unshifted == '2' && (cp == '@' || cp == 0 || cp == '2')) cp = '@';
        else if ((mod & ModShift) && unshifted && legacyCtrlByte(cp) < 0 && legacyCtrlByte(unshifted) >= 0)
            cp = unshifted;
        const int b = legacyCtrlByte(cp);
        if (b >= 0) {
            char out[2];
            size_t n = 0;
            if (mod & ModAlt) out[n++] = 0x1b;
            out[n++] = static_cast<char>(b);
            sendBytes(out, n);
            return;
        }
    }
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
    // The physical key, not the shifted character: Ctrl+@ is the 2 key, Ctrl+[ is
    // bracketleft. Falling back to the shifted character covers a layout where
    // the symbol itself is unshifted.
    const uint32_t base = unshifted ? unshifted : cp;
    int key = ghostKeyForCodepoint(base);
    if (key == GHOSTTY_KEY_UNIDENTIFIED) key = ghostKeyForCodepoint(cp);
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
    if (fd_ < 0) {
        // No pty: tests collect everything. A paste with no pty is delivered
        // in paste() itself, so this is a key, a mouse report, or a reply.
        if (pastePending()) keyAfter_.append(bytes, len);
        else pendingOut_.append(bytes, len);
        return;
    }
    // While a paste is still streaming, hold later input until the paste
    // (including its bracketed-paste end marker) has been written. Dropping
    // it, or inserting it into the middle of the paste, both lose the user's text.
    if (pastePending()) keyAfter_.append(bytes, len);
    else writeQueue_.append(bytes, len);
    flushWrites();
}

bool VtermSession::drainToPty(std::string& buf) {
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = write(fd_, buf.data() + off, buf.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) {
                buf.clear();
                return true;
            }
            break;
        }
        if (n == 0) break;
        off += static_cast<size_t>(n);
    }
    if (off) buf.erase(0, off);
    return buf.empty();
}

bool VtermSession::takePasteSlice(std::string& dst, size_t maxBytes) {
    if (!maxBytes) return pastePending();
    if (pasteNeedPrefix_) {
        dst.append("\x1b[200~", 6);
        pasteNeedPrefix_ = false;
        if (dst.size() >= maxBytes) return true;
    }
    if (pasteOff_ < pasteBody_.size()) {
        const size_t n = std::min(pasteBody_.size() - pasteOff_, maxBytes - dst.size());
        dst.append(pasteBody_.data() + pasteOff_, n);
        pasteOff_ += n;
        if (pasteOff_ == pasteBody_.size()) {
            pasteBody_.clear();
            pasteBody_.shrink_to_fit();
            pasteOff_ = 0;
        }
        if (dst.size() >= maxBytes) return true;
    }
    if (pasteNeedSuffix_ && pasteOff_ == 0 && pasteBody_.empty()) {
        dst.append("\x1b[201~", 6);
        pasteNeedSuffix_ = false;
    }
    return pastePending();
}

void VtermSession::flushWrites() {
    if (fd_ < 0) return;
    for (;;) {
        if (!writeQueue_.empty() && !drainToPty(writeQueue_)) return;
        if (!pastePending()) break;
        // One chunk in the queue. The rest of the paste stays in pasteBody_
        // until the pty accepts this one (backpressure, no second full copy).
        const size_t before = writeQueue_.size();
        takePasteSlice(writeQueue_, kPasteChunkBytes);
        if (writeQueue_.size() == before) break;
    }
    if (!keyAfter_.empty() && !pastePending()) {
        writeQueue_.append(keyAfter_);
        keyAfter_.clear();
        drainToPty(writeQueue_);
    }
}

void VtermSession::paste(const std::string& text) {
    std::string body = sanitizePaste(text);
    if (body.empty()) return;
    const bool bracketed = modeOn(g_->vt, GHOSTTY_MODE_BRACKETED_PASTE);
    if (!pastePending()) {
        pasteNeedPrefix_ = bracketed;
        pasteNeedSuffix_ = bracketed;
        pasteBody_.clear();
        pasteOff_ = 0;
    }
    // A second paste while the first is still draining joins the body, ahead
    // of the closing bracket, so the end marker stays after every byte.
    pasteBody_.append(body);
    if (fd_ < 0) {
        // No reader to push back on: deliver the whole paste, then anything
        // that was held behind it. The cap is dst's size, so use a fresh buffer.
        std::string chunk;
        takePasteSlice(chunk, pasteBody_.size() + 16);
        pendingOut_.append(chunk);
        if (!keyAfter_.empty()) {
            pendingOut_.append(keyAfter_);
            keyAfter_.clear();
        }
        return;
    }
    flushWrites();
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

bool VtermSession::rowRef(long line, int col, void* out) const {
    auto* ref = static_cast<GhosttyGridRef*>(out);
    if (g_->pinLine != line || g_->pinEpoch != epoch_) {
        g_->pinLine = line;
        g_->pinEpoch = epoch_;
        g_->pinOk = false;
        g_->pinWrap = false;
        GhosttyPoint pt{};
        if (locate(line, 0, &pt) && ghostty_terminal_grid_ref(g_->vt, pt, &g_->pin) == GHOSTTY_SUCCESS) {
            g_->pinOk = true;
            GhosttyRow row = 0;
            if (ghostty_grid_ref_row(&g_->pin, &row) == GHOSTTY_SUCCESS)
                ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &g_->pinWrap);
        }
    }
    if (!g_->pinOk || col < 0 || col >= cols_) return false;
    *ref = g_->pin;
    ref->x = static_cast<uint16_t>(static_cast<int>(g_->pin.x) + col);
    return true;
}

void VtermSession::noteDirty() {
    if (static_cast<int>(viewDirty_.size()) != rows_) {
        viewDirty_.assign(static_cast<size_t>(rows_), 1);
        viewFull_ = true;
    }
    int dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    if (ghostty_render_state_get(g_->render, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty) != GHOSTTY_SUCCESS) {
        viewFull_ = true;
        return;
    }
    if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL) {
        viewFull_ = true;
        return;
    }
    if (dirty != GHOSTTY_RENDER_STATE_DIRTY_PARTIAL) return;
    if (ghostty_render_state_get(g_->render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, g_->rows) != GHOSTTY_SUCCESS) {
        viewFull_ = true;
        return;
    }
    uint16_t y = 0;
    while (ghostty_render_state_row_iterator_next_dirty(g_->rows, &y)) {
        if (y < viewDirty_.size()) viewDirty_[y] = 1;
    }
}

VtermSession::CellRead VtermSession::readAt(long line, int col) const {
    CellRead out;
    GhosttyGridRef ref{};
    if (!rowRef(line, col, &ref)) return out;
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
    const bool alt = altScreen();
    int off = 0;
    if (!alt) {
        const int sb = static_cast<int>(scrollbackSize());
        if (scrollOffset < 0) scrollOffset = 0;
        if (scrollOffset > sb) scrollOffset = sb;
        off = scrollOffset;
    }
    if (viewCache_.rows != rows_ || viewCache_.cols != cols_ || viewCacheOff_ != off || viewCacheAlt_ != alt) {
        viewCache_.resize(rows_, cols_);
        viewFull_ = true;
        viewCacheOff_ = off;
        viewCacheAlt_ = alt;
    }
    // Live screen: only rows the render state marked. Scrolled-back history is not
    // that viewport, so the visible rows are read in full (one lookup per row).
    const bool partial = !viewFull_ && off == 0 && static_cast<int>(viewDirty_.size()) == rows_;
    for (int r = 0; r < rows_; ++r) {
        if (partial && !viewDirty_[static_cast<size_t>(r)]) continue;
        const long line = topLine_ - off + r;
        for (int c = 0; c < cols_; ++c) {
            CellRead rd = readAt(line, c);
            viewCache_.at(r, c) = rd.ok ? std::move(rd.cell) : Cell{};
        }
    }
    viewFull_ = false;
    if (static_cast<int>(viewDirty_.size()) == rows_) std::fill(viewDirty_.begin(), viewDirty_.end(), 0);
    uint16_t cx = 0, cy = 0;
    bool vis = true;
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cx);
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &cy);
    ghostty_terminal_get(g_->vt, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &vis);
    const int viewRow = static_cast<int>(cy) + off;
    viewCache_.cursorCol = std::min(static_cast<int>(cx), std::max(0, cols_ - 1));
    if (!vis || viewRow < 0 || viewRow >= rows_) {
        viewCache_.cursorVisible = false;
        viewCache_.cursorRow = std::min(static_cast<int>(cy), std::max(0, rows_ - 1));
    } else {
        viewCache_.cursorVisible = true;
        viewCache_.cursorRow = viewRow;
    }
    grid = viewCache_;
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

static int wideOf(GhosttyCell cell) {
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_WIDE) return 2;
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) return 0;
    return 1;
}

std::string VtermSession::cellText(long line, int col) const {
    uint32_t cp[16];
    int w = 1;
    const int n = cellChars(line, col, cp, 16, &w);
    if (w == 0 || n <= 0) return {};
    std::string text;
    for (int i = 0; i < n; ++i) appendUtf8(text, cp[i]);
    return text;
}

int VtermSession::cellWidth(long line, int col) const {
    GhosttyGridRef ref{};
    if (!rowRef(line, col, &ref)) return 1;
    GhosttyCell cell = 0;
    if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) return 1;
    return wideOf(cell);
}

bool VtermSession::wrapped(long line) const {
    GhosttyGridRef ref{};
    if (!rowRef(line, 0, &ref)) return false;
    return g_->pinWrap;
}

int VtermSession::cellChars(long line, int col, uint32_t* out, int max, int* width) const {
    GhosttyGridRef ref{};
    if (!rowRef(line, col, &ref)) {
        if (width) *width = 1;
        return 0;
    }
    GhosttyCell cell = 0;
    if (ghostty_grid_ref_cell(&ref, &cell) != GHOSTTY_SUCCESS) {
        if (width) *width = 1;
        return 0;
    }
    const int w = wideOf(cell);
    if (width) *width = w;
    if (w == 0 || max <= 0) return 0;
    size_t n = 0;
    GhosttyResult gr = ghostty_grid_ref_graphemes(&ref, out, static_cast<size_t>(max), &n);
    if (gr == GHOSTTY_OUT_OF_SPACE) {
        std::vector<uint32_t> big(n);
        gr = ghostty_grid_ref_graphemes(&ref, big.data(), big.size(), &n);
        if (gr != GHOSTTY_SUCCESS) return 0;
        const int k = std::min(static_cast<int>(n), max);
        for (int i = 0; i < k; ++i) out[i] = big[static_cast<size_t>(i)];
        return k;
    }
    return gr == GHOSTTY_SUCCESS ? static_cast<int>(n) : 0;
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
