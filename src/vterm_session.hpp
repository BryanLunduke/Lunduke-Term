// SPDX-License-Identifier: GPL-3.0-or-later
// libvterm + PTY session: owns the VTerm, the scrollback, the child process and the PTY master.
#pragma once

#include "grid.hpp"
#include "selection.hpp"

#include <vterm.h>

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace lterm {

struct SpawnOptions {
    std::vector<std::string> argv;  // empty: the user's shell ($SHELL, passwd entry, /bin/sh)
    bool loginShell = false;        // argv[0] "-bash" (only for the user's shell)
    std::string cwd;                // empty: inherit
};

// Environment the child gets on top of ours (TERM, COLORTERM, a UTF-8 locale when none is set,
// COLUMNS/LINES removed). Exposed for the tests.
std::vector<std::string> childEnvironment(char** base);

// Paste preparation: newlines become CR (what a keyboard Enter sends) and control characters
// other than TAB/CR are dropped (so pasted text cannot smuggle ESC sequences such as the
// bracketed-paste terminator "\e[201~" into the application).
std::string sanitizePaste(const std::string& text);

class VtermSession : public LineSource {
public:
    VtermSession(int rows, int cols);
    ~VtermSession() override;
    VtermSession(const VtermSession&) = delete;
    VtermSession& operator=(const VtermSession&) = delete;

    // Starts the child on a new PTY (posix_openpt, setsid, TIOCSCTTY; no utmp entry).
    bool spawn(const SpawnOptions& opts);
    bool spawn(const std::vector<std::string>& argv) { return spawn(SpawnOptions{argv, false, {}}); }
    int masterFd() const { return fd_; }
    pid_t childPid() const { return pid_; }
    // Kills the child's process group with SIGHUP (window closed while it still runs).
    void hangup();
    void markExited() { pid_ = -1; }

    // Reads whatever is available on the PTY and feeds libvterm. Returns false on EOF/HUP.
    bool readAvailable();
    // Feeds bytes directly (tests, local messages; no PTY needed).
    void feed(const char* bytes, size_t len);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    void resize(int rows, int cols);  // vterm_set_size (+ reflow) + TIOCSWINSZ
    int rows() const { return rows_; }
    int cols() const override { return cols_; }

    // Input.
    void sendKey(VTermKey key, VTermModifier mod);
    void sendChar(uint32_t codepoint, VTermModifier mod);
    void sendBytes(const char* bytes, size_t len);
    void flushWrites();
    bool wantsWrite() const { return !writeQueue_.empty(); }
    void paste(const std::string& text);  // sanitised, bracketed when the app enabled mode 2004
    // Mouse in screen cell coordinates (0-based); libvterm encodes for the active mode
    // (X10 / normal / button-event / any-event, X10 / UTF-8 / SGR 1006 / urxvt encodings).
    void mouseMove(int row, int col, VTermModifier mod);
    void mouseButton(int row, int col, int button, bool pressed, VTermModifier mod);
    void focus(bool in);
    // Without a PTY (tests) everything the terminal would send is collected here.
    std::string takeOutput() {
        std::string s;
        s.swap(pendingOut_);
        return s;
    }

    // Screen (+ scrollback when scrollOffset > 0: the view shows scrollOffset lines of history
    // above the screen and hides as many bottom rows).
    void fillGrid(Grid& g, int scrollOffset = 0) const;
    void setColors(Color8 fg, Color8 bg, const std::array<Color8, 16>& palette, float minFgGrey);
    std::string rowText(int row) const;

    // Scrollback. Lines have global numbers that never change while they exist: screen row r is
    // line topLine() + r; history goes down to firstLine().
    void setScrollbackLimit(size_t lines);
    size_t scrollbackSize() const { return sb_.size(); }
    long topLine() const { return pushed_; }
    long firstLine() const override { return pushed_ - static_cast<long>(sb_.size()); }
    long endLine() const override { return pushed_ + rows_; }
    std::string cellText(long line, int col) const override;
    int cellWidth(long line, int col) const override;
    bool wrapped(long line) const override;

    // Terminal properties set by the application.
    int cursorShape() const { return cursorShape_; }  // VTERM_PROP_CURSORSHAPE_*, 0 = not set
    int cursorBlink() const { return cursorBlink_; }  // -1 not set by the app, 0/1
    // VTERM_PROP_MOUSE_*; X10 (?9, tracked by us since libvterm ignores it) reports as CLICK.
    int mouseMode() const { return mouseMode_ == VTERM_PROP_MOUSE_NONE && x10_ ? VTERM_PROP_MOUSE_CLICK : mouseMode_; }
    bool altScreen() const { return altScreen_; }
    bool reverseVideo() const { return reverse_; }

    bool takeDamage() {
        bool d = damaged_;
        damaged_ = false;
        return d;
    }
    const std::string& title() const { return title_; }
    std::function<void()> onTitle;
    std::function<void()> onBell;

private:
    struct SbLine {
        std::vector<VTermScreenCell> cells;  // trailing blank cells trimmed
        bool wrapped = false;                // continues on the next line
    };
    Color8 mapColor(VTermColor c, bool foreground) const;
    void toCell(const VTermScreenCell& vc, Cell& cell) const;
    bool getCell(long line, int col, VTermScreenCell& out) const;
    void scanModes(const char* bytes, size_t len);
    static int cbDamage(VTermRect, void* user);
    static int cbMoveCursor(VTermPos pos, VTermPos, int visible, void* user);
    static int cbSetTermProp(VTermProp prop, VTermValue* val, void* user);
    static int cbBell(void* user);
    static int cbPushLine(int cols, const VTermScreenCell* cells, void* user);
    static int cbPopLine(int cols, VTermScreenCell* cells, void* user);
    static int cbSbClear(void* user);
    static void cbOutput(const char* s, size_t len, void* user);

    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
    VTermState* state_ = nullptr;
    int rows_, cols_;
    int fd_ = -1;
    pid_t pid_ = -1;
    bool damaged_ = true;
    bool ready_ = false;  // ignore the property values libvterm reports during its reset
    VTermPos cursor_{0, 0};
    bool cursorVisible_ = true;
    int cursorShape_ = 0, cursorBlink_ = -1, mouseMode_ = 0;
    bool altScreen_ = false, reverse_ = false;
    bool x10_ = false;
    int scanState_ = 0;
    std::string scanParams_;
    std::string title_, titleBuf_;
    std::string pendingOut_, writeQueue_;
    std::string* capture_ = nullptr;
    Color8 fg_{208, 208, 208}, bg_{0, 0, 0};
    std::array<Color8, 16> palette_{};
    bool havePalette_ = false;
    float minFgGrey_ = 0.0f;
    std::deque<SbLine> sb_;
    size_t sbLimit_ = 10000;
    long pushed_ = 0;  // lines ever pushed to the scrollback (minus popped)
};

}  // namespace lterm
