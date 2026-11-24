// SPDX-License-Identifier: GPL-3.0-or-later
// VT session: libghostty-vt plus the PTY. One terminal per widget (a future tab removal
// only deletes the widget). Scrollback, reflow, and key/mouse encoding live in libghostty.
#pragma once

#include "grid.hpp"
#include "selection.hpp"

#include <array>
#include <cstdint>
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
constexpr size_t kMaxScrollbackLines = 100000;     // scrollback-lines setting / --scrollback
constexpr size_t kDefaultScrollbackLines = 5000;   // when settings.ini and --scrollback give none
// Bytes of input waiting on a PTY that is not reading. Further paste is dropped
// until some of it is written, so a stuck reader cannot grow this without bound.
constexpr size_t kMaxWriteQueueBytes = 1u << 20;
// Negative -> 0, above kMaxScrollbackLines -> kMaxScrollbackLines.
size_t clampScrollbackLines(long long requested);
// Child side of spawn(): closes every fd >= 3 (close_range, or close() up to maxFd).
void closeFrom3(int maxFd);
// Truncates to at most max bytes without splitting a UTF-8 sequence.
void truncateUtf8(std::string& s, size_t max);

// Display width (0, 1, or 2) of one code point, from libghostty's width table.
int codepointWidth(uint32_t cp);

// Modifier bits OR-ed into sendKey / sendChar / mouse. Same positions the view has always used.
constexpr int ModNone = 0;
constexpr int ModShift = 1;
constexpr int ModAlt = 2;
constexpr int ModCtrl = 4;

// Matches GhosttyKeyAction. Release is only encoded when the application asked
// for kitty keyboard event types.
enum class KeyAction : int { Release = 0, Press = 1, Repeat = 2 };

// Keys the view maps from GDK. Printable characters go through sendChar.
enum class TermKey : int {
    Enter, Tab, Backspace, Escape,
    Up, Down, Left, Right,
    Insert, Delete, Home, End, PageUp, PageDown,
    Kp0, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9,
    KpMultiply, KpAdd, KpSubtract, KpDecimal, KpDivide, KpEnter, KpEqual,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

// What the application asked the mouse to do. Click covers X10 (?9) and normal tracking (?1000).
enum class MouseMode : int { None = 0, Click = 1, Drag = 2, Move = 3 };

// DECSCUSR. Unset: the app has not changed the cursor; the user's setting applies.
enum class CursorShape : int { Unset = 0, Block = 1, Underline = 2, Bar = 3 };

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

    // Reads whatever is available on the PTY and feeds libghostty. Returns false on EOF/HUP.
    // With budgetUs > 0 it stops after that much time even if more is waiting (an output flood)
    // and sets *more; the caller lets the main loop run (redraw, keys) before reading on.
    bool readAvailable(int64_t budgetUs = 0, bool* more = nullptr);
    // Feeds bytes directly (tests, local messages; no PTY needed).
    void feed(const char* bytes, size_t len);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    void resize(int rows, int cols);  // ghostty resize (reflow) + TIOCSWINSZ
    int rows() const { return rows_; }
    int cols() const override { return cols_; }

    // Input. Key and mouse bytes come from libghostty's encoders.
    // `unshifted` is the key with Shift released (0: use `codepoint` when it is
    // printable). Kitty mode reports that codepoint plus the Shift modifier;
    // legacy mode still sends the shifted character.
    void sendKey(TermKey key, int mod, KeyAction action = KeyAction::Press);
    void sendChar(uint32_t codepoint, int mod, uint32_t unshifted = 0, KeyAction action = KeyAction::Press);
    // A physical key the TermKey / codepoint mapping does not cover (modifiers,
    // punctuation). `ghostKey` is a GhosttyKey value.
    void sendPhysical(int ghostKey, int mod, const char* utf8, size_t utf8Len, uint32_t unshifted,
                      KeyAction action = KeyAction::Press);
    // Kitty keyboard protocol flag "report event types" (press, repeat, release).
    bool reportsKeyEvents() const;
    // Kitty keyboard protocol flag "report all keys as escape codes" (modifier keys).
    bool reportsAllKeys() const;
    void sendBytes(const char* bytes, size_t len);
    void flushWrites();
    bool wantsWrite() const { return !writeQueue_.empty(); }
    void paste(const std::string& text);  // sanitised, bracketed when the app enabled mode 2004
    // Mouse in screen cell coordinates (0-based).
    void mouseMove(int row, int col, int mod);
    void mouseButton(int row, int col, int button, bool pressed, int mod);
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
    // User cursor, used until the application sends DECSCUSR or mode 12.
    void setDefaultCursor(CursorShape shape, bool blink);
    std::string rowText(int row) const;

    // Scrollback. Lines have global numbers. Screen row r is line topLine() + r while the
    // primary screen is showing. libghostty prunes history a page at a time, so the line cap
    // is a ceiling plus at most one page, not an exact count.
    void setScrollbackLimit(size_t lines);  // clamped to kMaxScrollbackLines
    size_t scrollbackLimit() const { return sbLimit_; }
    // Compress scrollback that has gone idle. Incremental does one bounded step
    // and returns true when another step is worth scheduling. Full scans once.
    bool compressScrollback(bool full = false);
    // Bytes fed since the last takeFedBytes(). The view uses this to malloc_trim
    // after a large flood has gone idle.
    size_t takeFedBytes();
    size_t scrollbackSize() const;
    size_t scrollbackBytes() const;  // resident bytes libghostty reports for both screens
    // Settings-dialog estimate of `lines` rows at `cols` columns. libghostty's real use is
    // measured by scrollbackBytes() and by the RSS benchmark.
    static size_t scrollbackBytesFull(size_t lines, int cols);
    long topLine() const { return topLine_; }
    long firstLine() const override;
    long endLine() const override { return topLine_ + rows_; }
    std::string cellText(long line, int col) const override;
    int cellWidth(long line, int col) const override;
    bool wrapped(long line) const override;
    long stableEnd() const override { return topLine_; }
    unsigned long contentEpoch() const override { return epoch_; }
    int cellChars(long line, int col, uint32_t* out, int max, int* width) const override;

    int cursorShape() const { return static_cast<int>(cursorShape_); }
    int cursorBlink() const { return cursorBlink_; }  // -1 not set by the app, 0/1
    int mouseMode() const;
    bool altScreen() const;
    bool reverseVideo() const;

    bool takeDamage() {
        bool d = damaged_;
        damaged_ = false;
        return d;
    }
    const std::string& title() const { return title_; }
    std::function<void()> onTitle;
    std::function<void()> onBell;
    // libghostty callbacks (C function pointers cannot be private members).
    void onBellFired();
    void onTitleFired();
    void onWriteFired(const uint8_t* data, size_t len);

private:
    struct Ghost;  // libghostty handles, kept out of this header
    struct CellRead {
        Cell cell;
        uint32_t cp[16]{};
        int n = 0;
        int width = 1;
        bool ok = false;
    };
    void ensureGhost();
    void pinAnchor();
    void syncLines();
    void refreshCursor(bool fromApp);
    void syncMouseEncoder();
    void encodeKey(int ghostKey, int mod, const char* utf8, size_t utf8Len, uint32_t unshifted, KeyAction action);
    void emitMouse(int row, int col, int button, int action, int mod);
    bool locate(long line, int col, void* point) const;
    CellRead readAt(long line, int col) const;
    Color8 resolveColor(uint8_t tag, uint8_t a, uint8_t b, uint8_t c, bool foreground) const;

    Ghost* g_ = nullptr;
    int rows_, cols_;
    int fd_ = -1;
    pid_t pid_ = -1;
    bool damaged_ = true;
    int cursorShape_ = static_cast<int>(CursorShape::Unset);
    int cursorBlink_ = -1;
    int defaultShape_ = static_cast<int>(CursorShape::Block);
    int defaultBlink_ = 0;
    bool appCursor_ = false;
    int lastStyle_ = static_cast<int>(CursorShape::Block);
    int lastBlink_ = 0;
    bool mouseDirty_ = true;
    bool buttonDown_ = false;
    int heldButton_ = 0;
    std::string title_;
    std::string versionStr_;
    std::string pendingOut_, writeQueue_;
    Color8 fg_{208, 208, 208}, bg_{0, 0, 0};
    std::array<Color8, 16> palette_{};
    bool havePalette_ = false;
    float minFgGrey_ = 0.0f;
    size_t sbLimit_ = kDefaultScrollbackLines;
    uint64_t compActivity_ = 0;
    bool compSettled_ = false;
    size_t fedBytes_ = 0;
    long topLine_ = 0;
    long anchorGlobal_ = 0;
    uint32_t anchorY_ = 0;
    bool anchorLive_ = false;
    unsigned long epoch_ = 1;
};

}  // namespace lterm
