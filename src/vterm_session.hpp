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
// Also drops C1 controls (U+0080-U+009F, e.g. U+009B = CSI, which some applications still
// honour) and bytes that are not valid UTF-8, as xterm does in UTF-8 mode.
std::string sanitizePaste(const std::string& text);

// Limits against hostile or runaway input.
constexpr size_t kMaxTitleBytes = 4096;            // OSC 0/2 window title
constexpr size_t kMaxScrollbackLines = 1000000;    // scrollback-lines setting / --scrollback
// Negative -> 0, above kMaxScrollbackLines -> kMaxScrollbackLines.
size_t clampScrollbackLines(long long requested);
// Child side of spawn(): closes every fd >= 3 (close_range, or close() up to maxFd).
void closeFrom3(int maxFd);
// Truncates to at most max bytes without splitting a UTF-8 sequence.
void truncateUtf8(std::string& s, size_t max);

// Cell width (1 or 2) libvterm gives a code point; see vterm_session.cpp.
int libvtermWidth(uint32_t cp);
// True when libvterm 0.3.3 prints the code point with width 0 (a combining mark or other
// zero-width character on its own). Probed and cached like libvtermWidth.
bool libvtermZeroWidth(uint32_t cp);

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
    // Kills the child's process group with SIGHUP (window closed while it still runs) and reaps
    // the child if it exits within waitMs. Returns the pid if it is still running (the caller
    // must arrange for it to be reaped, e.g. a GLib child watch), else -1.
    pid_t hangup(int waitMs = 50);
    void markExited() { pid_ = -1; }

    // Reads whatever is available on the PTY and feeds libvterm. Returns false on EOF/HUP.
    bool readAvailable();
    // Feeds bytes directly (tests, local messages; no PTY needed).
    void feed(const char* bytes, size_t len);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    void resize(int rows, int cols);  // vterm_set_size (+ reflow) + TIOCSWINSZ
    int rows() const { return rows_; }
    // Resizes done without reflow to dodge libvterm's abort() (see reflowWouldAbort()).
    int reflowFallbacks() const { return reflowFallbacks_; }
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
    void setScrollbackLimit(size_t lines);  // clamped to kMaxScrollbackLines
    size_t scrollbackLimit() const { return sbLimit_; }
    size_t scrollbackSize() const { return sb_.size(); }
    size_t scrollbackBytes() const;  // M4 item 4: cell storage of the history (capacity, not size)
    long topLine() const { return pushed_; }
    long firstLine() const override { return pushed_ - static_cast<long>(sb_.size()); }
    long endLine() const override { return pushed_ + rows_; }
    std::string cellText(long line, int col) const override;
    int cellWidth(long line, int col) const override;
    bool wrapped(long line) const override;
    long stableEnd() const override { return pushed_; }  // the scrollback
    unsigned long contentEpoch() const override { return epoch_; }
    int cellChars(long line, int col, uint32_t* out, int max, int* width) const override;

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
    void normalizeCell(int col, VTermScreenCell& c) const;
    bool getCell(long line, int col, VTermScreenCell& out) const;
    void scanModes(const char* bytes, size_t len);
    static int cbDamage(VTermRect, void* user);
    static int cbMoveCursor(VTermPos pos, VTermPos, int visible, void* user);
    static int cbSetTermProp(VTermProp prop, VTermValue* val, void* user);
    static int cbBell(void* user);
    static int cbCsiFallback(const char* leader, const long args[], int argcount, const char* intermed, char command,
                             void* user);
    static int cbPushLine(int cols, const VTermScreenCell* cells, void* user);
    static int cbPopLine(int cols, VTermScreenCell* cells, void* user);
    static int cbSbClear(void* user);
    static void cbOutput(const char* s, size_t len, void* user);
    void feedRaw(const char* bytes, size_t len);
    void snapshotWraps();  // see wrapSnap_
    void clearTopContinuation();  // libvterm 0.3.3 reflow reads row -1 otherwise (see resize())
    bool reflowWouldAbort(int newRows, int newCols) const;  // libvterm 0.3.3 abort(), see resize()

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
    int csiState_ = 0;    // feed(): 0 ground, 1 after ESC, 2 inside CSI, 3 ESC ( ), 4 control string,
                          // 5 ESC in a string, 6 DCS command, 7 OSC number (mirrors libvterm's parser)
    long csiStart_ = -1;  // index of the ESC of the open CSI in the current chunk
    // libvterm's DECSC/DECRC (ESC 7/8, CSI ?1048/?1049) do not save the character set state
    // (G0/G1 designations, SO/SI), which a VT100 does; feed() tracks it and restores it.
    struct Charsets {
        char g0 = 'B', g1 = 'B';
        bool shifted = false;  // SO: G1 invoked into GL
    };
    Charsets cs_, savedCs_;
    int escTarget_ = 0;       // feed() state 3: 0 = G0, 1 = G1 being designated
    std::string csiParams_;   // parameters of the open CSI (bounded)
    // libvterm 0.3.3 stores CSI arguments in a fixed long[16] without a bounds check: a 17th
    // argument overwrites the parser's callbacks pointer (crash, or worse, from any program's
    // output). feed() counts the separators and drops the parameter bytes after the 15th.
    int csiSeps_ = 0;
    int reflowFallbacks_ = 0;
    // Its argument accumulator (long * 10 + digit) also overflows (undefined behaviour), and a
    // value >= 2^31 collides with its CSI_ARG_FLAG_MORE bit: feed() keeps at most 9 significant
    // digits per argument (the rest are dropped, so the value stays huge and gets clamped).
    int csiDigits_ = 0;
    bool csiArgs_ = false;   // a digit or separator was seen (leader bytes are over)
    bool csiInter_ = false;  // an intermediate byte was seen
    // Last code point printed as text (0: none yet / unknown) and the UTF-8 decoder for it.
    // libvterm 0.3.3 REP (CSI Ps b) loops forever when the last printed glyph had width 0 (a
    // lone combining mark, or nothing printed yet), so feed() drops REP then (fuzz finding F7).
    uint32_t lastCp_ = 0;
    uint32_t u8acc_ = 0;
    int u8need_ = 0;
    void restoreCharsets();
    std::string penSgr() const;
    // Continuation flags of the screen rows, taken just before an operation that may push
    // several lines to the scrollback at once (CSI S, CSI M, resize). libvterm shifts its line
    // info before the pushes, which loses the flags of all but the last pushed row.
    std::vector<char> wrapSnap_;
    bool wrapSnapActive_ = false;
    int wrapSnapPushes_ = 0;
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
    unsigned long epoch_ = 0;  // bumped when scrollback lines are popped/cleared or on resize
};

}  // namespace lterm
