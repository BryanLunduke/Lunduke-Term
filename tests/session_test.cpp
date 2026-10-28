// SPDX-License-Identifier: GPL-3.0-or-later
// Checkpoint B: scrollback, selection text, mouse encoding, paste bracketing, DECSCUSR, PTY spawn.
#include "effects.hpp"
#include "selection.hpp"
#include "vterm_session.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                          \
    do {                                                                                                        \
        auto va = (a);                                                                                          \
        auto vb = (b);                                                                                          \
        if (!(va == vb)) {                                                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n  got  [%s]\n  want [%s]\n", __FILE__, __LINE__, #a, #b, \
                         esc(va).c_str(), esc(vb).c_str());                                                     \
            ++failures;                                                                                         \
        }                                                                                                       \
    } while (0)

static std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == 0x1b) o += "\\e";
        else if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c < 0x20 || c >= 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o;
}
[[maybe_unused]] static std::string esc(const char* s) { return esc(std::string(s)); }
static std::string esc(long v) { return std::to_string(v); }
static std::string esc(int v) { return std::to_string(v); }
static std::string esc(size_t v) { return std::to_string(v); }
static std::string esc(bool v) { return v ? "true" : "false"; }

static std::string rowOf(const Grid& g, int r) {
    std::string s;
    for (int c = 0; c < g.cols; ++c) s += g.at(r, c).text.empty() ? " " : g.at(r, c).text;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void testScrollback() {
    VtermSession s(4, 20);
    s.setScrollbackLimit(5);
    for (int i = 0; i < 10; ++i) s.feed("line " + std::to_string(i) + "\r\n");
    // 10 lines + empty prompt line; 4 rows on screen -> 7 pushed, 5 kept.
    CHECK_EQ(s.topLine(), 7L);
    CHECK_EQ(s.scrollbackSize(), size_t(5));
    CHECK_EQ(s.firstLine(), 2L);
    Grid g;
    s.fillGrid(g, 0);
    CHECK_EQ(rowOf(g, 0), std::string("line 7"));
    s.fillGrid(g, 3);
    CHECK_EQ(rowOf(g, 0), std::string("line 4"));
    CHECK_EQ(rowOf(g, 3), std::string("line 7"));
    CHECK_EQ(g.cursorVisible, false);  // cursor row scrolled out of view
    s.fillGrid(g, 99);  // clamped to the history size
    CHECK_EQ(rowOf(g, 0), std::string("line 2"));
    CHECK_EQ(s.cellText(2, 5), std::string("2"));
    CHECK_EQ(s.cellText(1, 0), std::string(""));  // dropped
    // Colours survive the trip into the scrollback.
    s.feed("\x1b[31mRED\x1b[0m\r\n\r\n\r\n\r\n");
    long redLine = s.topLine() - 1;
    s.fillGrid(g, 1);
    CHECK_EQ(rowOf(g, 0), std::string("RED"));
    CHECK(g.at(0, 0).fg.r > 150 && g.at(0, 0).fg.g < 100);
    CHECK_EQ(s.cellText(redLine, 0), std::string("R"));
    // ED 3 clears the history.
    s.feed("\x1b[3J");
    CHECK_EQ(s.scrollbackSize(), size_t(0));
    // Limit 0 keeps nothing but numbering continues.
    VtermSession z(2, 10);
    z.setScrollbackLimit(0);
    z.feed("a\r\nb\r\nc\r\n");
    CHECK_EQ(z.scrollbackSize(), size_t(0));
    CHECK_EQ(z.topLine(), 2L);
    // Alt screen does not push to the scrollback.
    VtermSession a(3, 10);
    a.feed("\x1b[?1049h");
    for (int i = 0; i < 6; ++i) a.feed("x\r\n");
    CHECK(a.altScreen());
    CHECK_EQ(a.scrollbackSize(), size_t(0));
    a.feed("\x1b[?1049l");
    CHECK(!a.altScreen());
    // Soft wraps are remembered for lines in the history.
    VtermSession w(3, 10);
    w.feed("abcdefghijKLM\r\n\r\n\r\n\r\n");
    long l0 = w.firstLine();
    CHECK_EQ(w.cellText(l0, 0), std::string("a"));
    CHECK_EQ(w.wrapped(l0), true);
    CHECK_EQ(w.wrapped(l0 + 1), false);
}

static void testSelection() {
    VtermSession s(4, 30);
    s.feed("first line   \r\n");
    s.feed("ls /usr/share/doc foo-bar\r\n");
    s.feed("third\r\n");
    s.feed("abcdefghijklmnopqrstuvwxyz0123456789\r\n");  // wraps at 30
    s.feed("tail\r\n");
    Selection sel;
    const long top = s.topLine();
    // Find the line numbers by content.
    long lsLine = -1, firstLine = -1, wrapLine = -1;
    for (long l = s.firstLine(); l < s.endLine(); ++l) {
        if (s.cellText(l, 0) == "l" && s.cellText(l, 1) == "s") lsLine = l;
        if (s.cellText(l, 0) == "f" && s.cellText(l, 1) == "i") firstLine = l;
        if (s.cellText(l, 0) == "a" && s.cellText(l, 1) == "b") wrapLine = l;
    }
    CHECK(lsLine >= 0 && firstLine >= 0 && wrapLine >= 0);
    CHECK(firstLine < top);  // in the scrollback
    // Char selection: a click alone selects nothing; a drag selects inclusive cells.
    sel.begin(s, SelPos{lsLine, 3}, SelectMode::Char);
    CHECK(!sel.active());
    sel.extend(s, SelPos{lsLine, 6});
    CHECK(sel.active());
    CHECK_EQ(sel.text(s), std::string("/usr"));
    // Reverse drag gives the same text.
    sel.begin(s, SelPos{lsLine, 6}, SelectMode::Char);
    sel.extend(s, SelPos{lsLine, 3});
    CHECK_EQ(sel.text(s), std::string("/usr"));
    // Word: paths and dashes are words.
    sel.begin(s, SelPos{lsLine, 8}, SelectMode::Word);
    CHECK_EQ(sel.text(s), std::string("/usr/share/doc"));
    sel.begin(s, SelPos{lsLine, 20}, SelectMode::Word);
    CHECK_EQ(sel.text(s), std::string("foo-bar"));
    sel.begin(s, SelPos{lsLine, 2}, SelectMode::Word);  // the blank between words
    CHECK_EQ(sel.text(s), std::string(""));  // the blank run, trimmed
    // Line: trailing blanks trimmed; multi-line drag from the scrollback into the screen.
    sel.begin(s, SelPos{firstLine, 3}, SelectMode::Line);
    CHECK_EQ(sel.text(s), std::string("first line"));
    sel.extend(s, SelPos{lsLine, 0});
    CHECK_EQ(sel.text(s), std::string("first line\nls /usr/share/doc foo-bar"));
    CHECK(sel.contains(firstLine, 29));
    CHECK(!sel.contains(lsLine + 1, 0));
    // Soft-wrapped line: no newline is inserted; Line mode takes both rows.
    sel.begin(s, SelPos{wrapLine + 1, 2}, SelectMode::Line);
    CHECK_EQ(sel.text(s), std::string("abcdefghijklmnopqrstuvwxyz0123456789"));
    sel.begin(s, SelPos{wrapLine, 26}, SelectMode::Char);
    sel.extend(s, SelPos{wrapLine + 1, 5});
    CHECK_EQ(sel.text(s), std::string("0123456789"));
    // Wide characters are never split.
    VtermSession wsess(2, 10);
    wsess.feed("a\xe6\xbc\xa2" "b");
    Selection w;
    w.begin(wsess, SelPos{0, 2}, SelectMode::Char);  // right half of the wide char
    w.extend(wsess, SelPos{0, 3});
    CHECK_EQ(w.text(wsess), std::string("\xe6\xbc\xa2" "b"));
    // Custom word characters.
    Selection c;
    c.setWordChars("");
    c.begin(s, SelPos{lsLine, 5}, SelectMode::Word);
    CHECK_EQ(c.text(s), std::string("usr"));
}

static void testPointToCell() {
    int r, c;
    // Flat: margins subtracted, 10x20 cells.
    CHECK(pointToCell(25, 45, 800, 600, 0.0f, 5, 5, 10, 20, 24, 79, r, c));
    CHECK(r == 2 && c == 2);
    CHECK(!pointToCell(2, 2, 800, 600, 0.0f, 5, 5, 10, 20, 24, 79, r, c));
    CHECK(r == 0 && c == 0);
    // Curved: the centre is unchanged, near the corners the source is pushed outwards, exactly
    // like the shader's barrel().
    CHECK(pointToCell(400, 300, 800, 600, 0.12f, 0, 0, 10, 20, 30, 80, r, c));
    CHECK(r == 15 && c == 40);
    float su, sv;
    barrel(0.1f, 0.1f, 0.12f, su, sv);
    CHECK(su < 0.1f && sv < 0.1f);
    pointToCell(80, 60, 800, 600, 0.12f, 0, 0, 10, 20, 30, 80, r, c);
    CHECK_EQ(c, static_cast<int>(su * 800 / 10));
    CHECK_EQ(r, static_cast<int>(sv * 600 / 20));
    CHECK(c < 8 && r < 3);  // flat mapping would give col 8, row 3
}

static void testMouse() {
    VtermSession s(24, 80);
    // No mode: nothing is reported.
    s.mouseButton(2, 5, 1, true, VTERM_MOD_NONE);
    s.mouseButton(2, 5, 1, false, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string(""));
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_NONE));
    // X10 compatibility (?9): presses only.
    s.feed("\x1b[?9h");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_CLICK));
    s.mouseButton(2, 5, 1, true, VTERM_MOD_NONE);
    s.mouseButton(2, 5, 1, false, VTERM_MOD_NONE);
    s.mouseMove(3, 3, VTERM_MOD_NONE);
    s.mouseButton(1, 0, 3, true, VTERM_MOD_CTRL);  // no modifiers in X10
    CHECK_EQ(s.takeOutput(), std::string("\x1b[M\x20\x26\x23\x1b[M\x22\x21\x22"));
    s.feed("\x1b[?9l");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_NONE));
    s.feed("\x1b[?");  // split sequence across two reads
    s.feed("9h");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_CLICK));
    s.feed("\x1b[?9l");
    // Normal tracking (?1000), X10 byte encoding: button + 32, col + 33, row + 33.
    s.feed("\x1b[?1000h");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_CLICK));
    s.mouseButton(2, 5, 1, true, VTERM_MOD_NONE);
    s.mouseButton(2, 5, 1, false, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[M\x20\x26\x23\x1b[M\x23\x26\x23"));
    s.mouseMove(3, 7, VTERM_MOD_NONE);  // no motion reports in ?1000
    CHECK_EQ(s.takeOutput(), std::string(""));
    // Wheel = buttons 4/5 (64/65).
    s.mouseButton(0, 0, 4, true, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[M\x60\x21\x21"));
    // SGR 1006.
    s.feed("\x1b[?1006h");
    s.mouseButton(2, 5, 1, true, VTERM_MOD_NONE);
    s.mouseButton(2, 5, 1, false, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<0;6;3M\x1b[<0;6;3m"));
    s.mouseButton(2, 5, 3, true, VTERM_MOD_CTRL);  // right button with Ctrl (+16)
    s.mouseButton(2, 5, 3, false, VTERM_MOD_CTRL);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<18;6;3M\x1b[<18;6;3m"));
    s.mouseButton(9, 9, 5, true, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<65;10;10M"));
    // Button-event tracking (?1002): motion only while a button is held.
    s.feed("\x1b[?1002h");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_DRAG));
    s.mouseMove(4, 4, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string(""));
    s.mouseButton(4, 4, 1, true, VTERM_MOD_NONE);
    s.mouseMove(4, 6, VTERM_MOD_NONE);
    s.mouseButton(4, 6, 1, false, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<0;5;5M\x1b[<32;7;5M\x1b[<0;7;5m"));
    // Any-event tracking (?1003): motion without buttons (button 3 + 32 = 35).
    s.feed("\x1b[?1003h");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_MOVE));
    s.mouseMove(1, 1, VTERM_MOD_NONE);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<35;2;2M"));
    s.mouseMove(1, 1, VTERM_MOD_NONE);  // same cell: nothing
    CHECK_EQ(s.takeOutput(), std::string(""));
    s.feed("\x1b[?1003l\x1b[?1006l");
    CHECK_EQ(s.mouseMode(), int(VTERM_PROP_MOUSE_NONE));
}

static void testPaste() {
    VtermSession s(4, 20);
    s.paste("echo hi\nline2\r\nx\x1b[201~y\x03");
    CHECK_EQ(s.takeOutput(), std::string("echo hi\rline2\rx[201~y"));
    s.feed("\x1b[?2004h");
    s.paste("a\nb\x1b[201~; rm -rf ~\n");
    CHECK_EQ(s.takeOutput(), std::string("\x1b[200~a\rb[201~; rm -rf ~\r\x1b[201~"));
    s.feed("\x1b[?2004l");
    s.paste("plain\t\xc3\xa9");
    CHECK_EQ(s.takeOutput(), std::string("plain\t\xc3\xa9"));
}

static void testCursor() {
    VtermSession s(4, 20);
    CHECK_EQ(s.cursorShape(), 0);  // not set by the app: the user's setting applies
    CHECK_EQ(s.cursorBlink(), -1);
    s.feed("\x1b[4 q");
    CHECK_EQ(s.cursorShape(), int(VTERM_PROP_CURSORSHAPE_UNDERLINE));
    CHECK_EQ(s.cursorBlink(), 0);
    s.feed("\x1b[5 q");
    CHECK_EQ(s.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
    CHECK_EQ(s.cursorBlink(), 1);
    s.feed("\x1b[2 q");
    CHECK_EQ(s.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
    CHECK_EQ(s.cursorBlink(), 0);
    s.feed("\x1b[?12h");
    CHECK_EQ(s.cursorBlink(), 1);
    s.feed("\x1b[?25l");
    Grid g;
    s.fillGrid(g);
    CHECK(!g.cursorVisible);
    // DECSCNM reverse video
    s.feed("\x1b[?5h");
    CHECK(s.reverseVideo());
    // Focus reports only after ?1004.
    s.focus(true);
    CHECK_EQ(s.takeOutput(), std::string(""));
    s.feed("\x1b[?1004h");
    s.focus(false);
    s.focus(true);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[O\x1b[I"));
}

static void testGridCursorDiff() {
    Grid a, b;
    a.resize(3, 4);
    b = a;
    CHECK(changedRows(a, b) == std::vector<char>(3, 0));
    b.cursorShape = Grid::CursorShape::Bar;
    auto d = changedRows(a, b);
    CHECK(d[0] == 1 && d[1] == 0);
    b = a;
    b.at(2, 1).selected = true;
    d = changedRows(a, b);
    CHECK(d[2] == 1 && d[0] == 0);
}

static void testEnvironment() {
    const char* base[] = {"HOME=/h", "TERM=dumb", "COLUMNS=10", "LINES=3", "PATH=/bin", nullptr};
    auto env = childEnvironment(const_cast<char**>(base));
    auto has = [&](const std::string& kv) {
        for (auto& e : env)
            if (e == kv) return true;
        return false;
    };
    CHECK(has("TERM=xterm-256color"));
    CHECK(has("COLORTERM=truecolor"));
    CHECK(has("LANG=C.UTF-8"));
    CHECK(has("PATH=/bin"));
    CHECK(!has("TERM=dumb") && !has("COLUMNS=10") && !has("LINES=3"));
    const char* withLocale[] = {"LANG=de_DE.UTF-8", nullptr};
    env = childEnvironment(const_cast<char**>(withLocale));
    CHECK(has("LANG=de_DE.UTF-8") && !has("LANG=C.UTF-8"));
}

// Runs the session until the child exits; returns the wait status.
static int runToExit(VtermSession& s, std::string* transcript) {
    int status = -1;
    // Bounded by time, not by iterations: once the child has closed the PTY, poll() returns at
    // once with POLLHUP, and a few hundred iterations can pass before the child is reapable.
    for (int i = 0; i < 1000; ++i) {
        pollfd p{s.masterFd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0 && (p.revents & (POLLHUP | POLLERR))) usleep(10000);
        s.readAvailable();
        pid_t r = waitpid(s.childPid(), &status, WNOHANG);
        if (r == s.childPid()) {
            s.readAvailable();
            s.markExited();
            break;
        }
    }
    if (transcript) {
        transcript->clear();
        for (int r = 0; r < s.rows(); ++r) *transcript += s.rowText(r) + "\n";
    }
    return status;
}

static void testPty() {
    {
        VtermSession s(10, 60);
        SpawnOptions o;
        o.argv = {"/bin/sh", "-c",
                  "echo T=$TERM C=$COLORTERM; test -t 0 && echo TTY; stty size; "
                  "ps -o sid= -p $$ | tr -d ' ' > /dev/null; [ \"$(ps -o sid= -p $$ | tr -d ' ')\" = \"$$\" ] && echo "
                  "SESSIONLEADER; stty -a | grep -q iutf8 && ! stty -a | grep -q -- -iutf8 && echo IUTF8; exit 3"};
        o.cwd = "/tmp";
        CHECK(s.spawn(o));
        std::string out;
        int st = runToExit(s, &out);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
        CHECK(out.find("T=xterm-256color C=truecolor") != std::string::npos);
        CHECK(out.find("TTY") != std::string::npos);
        CHECK(out.find("10 60") != std::string::npos);
        CHECK(out.find("SESSIONLEADER") != std::string::npos);
        CHECK(out.find("IUTF8") != std::string::npos);
        if (failures) std::fprintf(stderr, "transcript:\n%s", out.c_str());
    }
    {
        VtermSession s(5, 60);
        SpawnOptions o;
        o.argv = {"/bin/sh", "-c", "pwd; kill -TERM $$"};
        o.cwd = "/tmp";
        CHECK(s.spawn(o));
        std::string out;
        int st = runToExit(s, &out);
        CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
        CHECK(out.find("/tmp") != std::string::npos);
    }
    {
        // Login shell: argv[0] starts with '-'.
        setenv("SHELL", "/bin/bash", 1);
        VtermSession s(5, 60);
        SpawnOptions o;
        o.loginShell = true;
        CHECK(s.spawn(o));
        const char* cmd = "echo ZERO=$0; exit 0\r";
        s.sendBytes(cmd, std::strlen(cmd));
        std::string out;
        int st = runToExit(s, &out);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK(out.find("ZERO=-bash") != std::string::npos);
        if (out.find("ZERO=-bash") == std::string::npos) std::fprintf(stderr, "login transcript:\n%s", out.c_str());
    }
    {
        VtermSession s(5, 60);
        CHECK(s.spawn(std::vector<std::string>{"/nonexistent/program"}));
        std::string out;
        int st = runToExit(s, &out);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 127);
        CHECK(out.find("cannot run /nonexistent/program") != std::string::npos);
    }
}

int main() {
    testScrollback();
    testSelection();
    testPointToCell();
    testMouse();
    testPaste();
    testCursor();
    testGridCursorDiff();
    testEnvironment();
    testPty();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("session tests passed\n");
    return 0;
}
