// SPDX-License-Identifier: GPL-3.0-or-later
// Checkpoint B: scrollback, selection text, mouse encoding, paste bracketing, DECSCUSR, PTY spawn.
#include "effects.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"
#include "selection.hpp"
#include "vterm_session.hpp"

#include <cmath>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <random>
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

// M2 HiDPI: the layout and the mouse mapping at scale 2 equal the scale-1 ones.
// The default window is the smallest 80x24 grid for the profile's cells, margins and bezel.
static void testWindowForGrid() {
    auto roundTrip = [](int cols, int rows, int cellW, int cellH, int scale, int marginMin, float frac, int inset) {
        const WindowSize s = windowSizeForGrid(cols, rows, cellW, cellH, scale, marginMin, frac, inset);
        const GridLayout g = computeGridLayout(s.width * scale, s.height * scale, scale, cellW, cellH, marginMin, frac, inset);
        CHECK_EQ(g.cols, cols);
        CHECK_EQ(g.rows, rows);
        // One logical pixel less on either axis drops a column or a row (no spare cell).
        if (s.width > 1) {
            const GridLayout narrow =
                computeGridLayout((s.width - 1) * scale, s.height * scale, scale, cellW, cellH, marginMin, frac, inset);
            CHECK(narrow.cols < cols);
        }
        if (s.height > 1) {
            const GridLayout shortH =
                computeGridLayout(s.width * scale, (s.height - 1) * scale, scale, cellW, cellH, marginMin, frac, inset);
            CHECK(shortH.rows < rows);
        }
        return s;
    };
    // plain: no fractional margin, 4 px minimum. 10x19 cells are DejaVu Sans Mono at 16 px.
    const WindowSize plain = roundTrip(80, 24, 10, 19, 1, 4, 0.0f, 0);
    CHECK_EQ(plain.width, 80 * 10 + 8);
    CHECK_EQ(plain.height, 24 * 19 + 8);
    // amber: 4% margin, at least 12 px. Same cells. Scale 2 uses device cells and the same logical size.
    const WindowSize amber = roundTrip(80, 24, 10, 19, 1, 12, 0.04f, 0);
    const WindowSize amber2 = roundTrip(80, 24, 20, 38, 2, 12, 0.04f, 0);
    CHECK_EQ(amber.width, amber2.width);
    CHECK_EQ(amber.height, amber2.height);
    // The character grid's aspect, not the old 1280x800 (1.60). Margins move it only a little.
    const double grid = (80.0 * 10) / (24.0 * 19);
    const double win = static_cast<double>(amber.width) / amber.height;
    CHECK(std::fabs(win - grid) / grid < 0.08);
    CHECK(std::fabs(win - 1280.0 / 800.0) > 0.05);
    // Bezel on every side, on top of the same margins.
    const WindowSize bezel = roundTrip(80, 24, 10, 19, 1, 12, 0.04f, 28);
    CHECK_EQ(bezel.width, amber.width + 56);
    CHECK_EQ(bezel.height, amber.height + 56);
    // The font the default profile resolves to here produces that 80x24 window.
    if (const Profile* p = findProfile(kDefaultProfile)) {
        const FontPick f = profileFont(*p);
        GridPainter gp(f.family, f.px);
        const WindowSize s = windowSizeForGrid(kDefaultCols, kDefaultRows, gp.cellWidth(), gp.cellHeight(), 1,
                                               p->marginMinPx, p->marginFrac, 0);
        const GridLayout g = computeGridLayout(s.width, s.height, 1, gp.cellWidth(), gp.cellHeight(), p->marginMinPx,
                                               p->marginFrac, 0);
        CHECK_EQ(g.cols, kDefaultCols);
        CHECK_EQ(g.rows, kDefaultRows);
        std::printf("default window: %s %d px, cell %dx%d, logical %dx%d (80x24)\n", f.family.c_str(), f.px,
                    gp.cellWidth(), gp.cellHeight(), s.width, s.height);
        // DejaVu Sans Mono 16 px is 10x19 on this box (same cell as Monospace 12). xfce4-terminal's
        // default 80x24 client window measured 818x485; the extra pixels here are amber's 4% margin.
        if (f.family == "DejaVu Sans Mono" && f.px == 16) {
            CHECK_EQ(gp.cellWidth(), 10);
            CHECK_EQ(gp.cellHeight(), 19);
            CHECK_EQ(s.width, amber.width);
            CHECK_EQ(s.height, amber.height);
        }
    }
}

static void testScale2Mapping() {
    for (float frac : {0.0f, 0.05f}) {
        const GridLayout g1 = computeGridLayout(808, 464, 1, 10, 19, 4, frac);
        const GridLayout g2 = computeGridLayout(1616, 928, 2, 20, 38, 4, frac);
        CHECK_EQ(g1.rows, g2.rows);
        CHECK_EQ(g1.cols, g2.cols);
        CHECK_EQ(2 * g1.marginX, g2.marginX);
        CHECK_EQ(2 * g1.marginY, g2.marginY);
        if (frac == 0.0f) {
            CHECK_EQ(g1.rows, 24);
            CHECK_EQ(g1.cols, 80);
        }
        // Every logical pointer position (event coordinates) maps to the same cell; the view
        // multiplies the event position by the scale before mapping.
        for (float curv : {0.0f, 0.12f}) {
            int bad = 0;
            for (double y = 0.25; y < 464; y += 3.5)
                for (double x = 0.25; x < 808; x += 3.5) {
                    int r1, c1, r2, c2;
                    const bool in1 = pointToCell(x, y, 808, 464, curv, g1.marginX, g1.marginY, 10, 19, g1.rows, g1.cols, r1, c1);
                    const bool in2 = pointToCell(2 * x, 2 * y, 1616, 928, curv, g2.marginX, g2.marginY, 20, 38, g2.rows,
                                                 g2.cols, r2, c2);
                    if (in1 != in2 || r1 != r2 || c1 != c2) ++bad;
                }
            CHECK_EQ(bad, 0);
        }
    }
    // Selection by mouse at scale 2: dragging over device pixels selects the same text.
    VtermSession s(24, 80);
    const char* t = "line 02: the quick brown fox\r\nline 03: jumps over the lazy dog";
    s.feed(t, std::strlen(t));
    const GridLayout g = computeGridLayout(1616, 928, 2, 20, 38, 4, 0.0f);
    int r0, c0, r1, c1;
    // logical (99, 13) .. (189, 32) -> device x2
    pointToCell(2 * 99.0, 2 * 13.0, 1616, 928, 0.0f, g.marginX, g.marginY, 20, 38, g.rows, g.cols, r0, c0);
    pointToCell(2 * 189.0, 2 * 32.0, 1616, 928, 0.0f, g.marginX, g.marginY, 20, 38, g.rows, g.cols, r1, c1);
    Selection sel;
    sel.begin(s, SelPos{s.topLine() + r0, c0}, SelectMode::Char);
    sel.extend(s, SelPos{s.topLine() + r1, c1});
    CHECK_EQ(sel.text(s), std::string("the quick brown fox\nline 03: jumps over"));
}

// M2 item 5: pointer mapping with the bezel, at scale 1 and 2 (same logical answers).
static void testBezelMapping() {
    EffectParams fx;
    fx.frame = true;
    fx.frameSize = 28;
    fx.screenCurvature = 0.3f;
    const float curv = fx.screenCurvature * 0.4f;
    for (int scale : {1, 2}) {
        EffectParams p = scaleEffectParams(fx, scale);
        const int W = 808 * scale, H = 464 * scale;
        const FrameGeometry g = frameGeometry(p, W, H);
        CHECK_EQ(static_cast<int>(g.inset), 28 * scale);
        const GridLayout lay = computeGridLayout(W, H, scale, 10 * scale, 19 * scale, 4, 0.0f, 28);
        CHECK_EQ(lay.marginX, (4 + 28) * scale);
        int r, c;
        auto at = [&](double lx, double ly) {
            return pointToCell(lx * scale, ly * scale, W, H, curv, lay.marginX, lay.marginY, 10 * scale, 19 * scale, lay.rows,
                               lay.cols, r, c, g);
        };
        CHECK(!at(10, 232));                 // left bezel
        CHECK(!at(404, 6));                  // top bezel
        CHECK(!at(800, 460));                // outer corner
        CHECK(!at(29.5, 29.5));              // just inside the inset but in the glass's rounded corner
        CHECK(at(404, 232));                 // centre of the glass
        CHECK(at(404, 232) && r == lay.rows / 2 && (c == lay.cols / 2 || c == lay.cols / 2 - 1));
        // Off-screen points still give clamped cells (drags clamp to the edge).
        at(1, 1);
        CHECK_EQ(r, 0);
        CHECK_EQ(c, 0);
        at(807, 463);
        CHECK_EQ(r, lay.rows - 1);
        CHECK_EQ(c, lay.cols - 1);
        // Without curvature the glass interior maps 1:1 to the pixel grid.
        pointToCell((4 + 28 + 10 * 3 + 5) * scale, (4 + 28 + 19 * 2 + 9) * scale, W, H, 0.0f, lay.marginX, lay.marginY,
                    10 * scale, 19 * scale, lay.rows, lay.cols, r, c, g);
        CHECK_EQ(r, 2);
        CHECK_EQ(c, 3);
    }
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

// vttest 6.7: DECREQTPARM is answered (libvterm alone ignores CSI x); other CSI x forms are not.
static void testReports() {
    VtermSession s(24, 80);
    const char* q = "\x1b[x\x1b[1x\x1b[5x";
    s.feed(q, std::strlen(q));
    CHECK_EQ(s.takeOutput(), std::string("\x1b[2;1;1;128;128;1;0x\x1b[3;1;1;128;128;1;0x"));
    const char* da = "\x1b[c";
    s.feed(da, std::strlen(da));
    CHECK_EQ(s.takeOutput(), std::string("\x1b[?1;2c"));
}

// M6 item 6: scrollback cells are packed (16 bytes). Every field must survive the trip into the
// history (read back through fillGrid) and back onto the screen (sb_popline on a resize).
static std::vector<VTermScreenCell> screenRow(VtermSession& s, int row, int cols) {
    VTermScreen* scr = vterm_obtain_screen(s.vtermForCheck());
    std::vector<VTermScreenCell> v(static_cast<size_t>(cols));
    for (int c = 0; c < cols; ++c) vterm_screen_get_cell(scr, VTermPos{row, c}, &v[static_cast<size_t>(c)]);
    return v;
}

// libvterm itself (master too) does not restore two things on sb_popline: DECDWL/DECDHL (line
// attributes it reports from its line info) and the colours of a wide character's right half
// (chars[0] == -1). Those are left out; everything we store is compared.
static bool sameCell(const VTermScreenCell& a, const VTermScreenCell& b) {
    const bool rightHalf = a.chars[0] == static_cast<uint32_t>(-1);
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; ++i) {
        if (a.chars[i] != b.chars[i]) return false;
        if (a.chars[i] == 0) break;
    }
    const VTermScreenCellAttrs &x = a.attrs, &y = b.attrs;
    return a.width == b.width && x.bold == y.bold && x.underline == y.underline && x.italic == y.italic &&
           x.blink == y.blink && x.reverse == y.reverse && x.conceal == y.conceal && x.strike == y.strike &&
           x.font == y.font && x.small == y.small && x.baseline == y.baseline &&
           (rightHalf || (std::memcmp(&a.fg, &b.fg, sizeof a.fg) == 0 && std::memcmp(&a.bg, &b.bg, sizeof a.bg) == 0));
}

static void testScrollbackCells() {
    const int cols = 40;
    // Row 0: every attribute, indexed / 256 / RGB / default colours, combining marks (1 and 5),
    // a wide character, then the same on a DECDWL line (row 1) and a DECDHL top half (row 2).
    const std::string row0 =
        "\x1b[1;3;4:2;5;7;8;9;13mA\x1b[0m"
        "\x1b[31;42mB\x1b[38;5;200;48;2;10;20;30mC\x1b[0m"
        "e\xcc\x81"                                              // e + U+0301
        "a\xcc\x80\xcc\x81\xcc\x82\xcc\x83\xcc\x84"              // a + 5 combining marks
        "\xe6\xbc\xa2"                                           // U+6F22 (wide)
        "\x1b[73mx\x1b[74my\x1b[0m\x1b[4:3mz\x1b[0m";
    VtermSession s(4, cols);
    s.feed(row0 + "\r\n\x1b#6\x1b[1mDW\x1b[0m\r\n\x1b#3TOP\r\n");
    std::vector<std::vector<VTermScreenCell>> before;
    for (int r = 0; r < 3; ++r) before.push_back(screenRow(s, r, cols));
    // The input really sets what is compared.
    const VTermScreenCell& A = before[0][0];
    CHECK(A.attrs.bold && A.attrs.italic && A.attrs.underline == 2 && A.attrs.blink && A.attrs.reverse &&
          A.attrs.conceal && A.attrs.strike && A.attrs.font == 3);
    CHECK(VTERM_COLOR_IS_RGB(&before[0][2].bg) && VTERM_COLOR_IS_INDEXED(&before[0][2].fg));
    CHECK(before[0][4].chars[5] != 0 && before[0][3].chars[1] != 0 && before[0][5].width == 2);
    CHECK(before[0][7].attrs.small && before[0][7].attrs.baseline && before[0][9].attrs.underline == 3);
    Grid gBefore;
    s.fillGrid(gBefore, 0);
    // Push the three rows into the history.
    s.feed("\r\n\r\n\r\n\r\n");
    CHECK(s.scrollbackSize() >= 3);
    Grid gHist;
    // History: the three rows and the empty fourth; scrolled back by 4 they are rows 0..2.
    CHECK_EQ(s.scrollbackSize(), size_t(4));
    s.fillGrid(gHist, 4);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < cols; ++c) CHECK(gHist.at(r, c) == gBefore.at(r, c));
    // Growing the window pulls the history back onto the screen (sb_popline).
    s.resize(8, cols);
    CHECK_EQ(s.scrollbackSize(), size_t(0));
    int bad = 0;
    for (int r = 0; r < 3; ++r) {
        const auto after = screenRow(s, r, cols);
        for (int c = 0; c < cols; ++c) {
            const VTermScreenCell &x = after[static_cast<size_t>(c)], &y = before[static_cast<size_t>(r)][static_cast<size_t>(c)];
            if (!sameCell(x, y)) {
                ++bad;
                if (getenv("DBG")) std::fprintf(stderr, "r%d c%d ch %x/%x w %d/%d dwl %d/%d dhl %d/%d fg %d/%d bg %d/%d\n", r, c, x.chars[0], y.chars[0], x.width, y.width, x.attrs.dwl, y.attrs.dwl, x.attrs.dhl, y.attrs.dhl, x.fg.type, y.fg.type, x.bg.type, y.bg.type);
            }
        }
    }
    CHECK_EQ(bad, 0);
    // The packed size: 16 bytes per stored cell (VTermScreenCell is 40).
    CHECK_EQ(VtermSession::scrollbackBytesFull(1, 100) - VtermSession::scrollbackBytesFull(1, 0), size_t(1600));
    std::printf("scrollback cells: %d rows x %d columns round-tripped; %zu bytes per 100-column line\n", 3, cols,
                VtermSession::scrollbackBytesFull(1, 100));
}

// M6 review: unpackCell found a cell's extra combining characters by scanning the line's whole
// side array (quadratic in the line width). sbExtraFind is a binary search; it must return what
// the linear scan returned, and random lines full of combining marks must come back unchanged.
static size_t sbExtraFindLinear(const uint32_t* extra, size_t records, uint32_t col) {
    for (size_t r = 0; r < records; ++r)
        if (extra[r * VTERM_MAX_CHARS_PER_CELL] == col) return r;
    return records;
}

static void testScrollbackCombiningLookup() {
    std::mt19937 rng(20261102);
    int mismatches = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        std::vector<uint32_t> extra;
        size_t records = 0;
        for (uint32_t col = 0; col < 300; ++col) {
            if (rng() % 4 != 0) continue;
            extra.push_back(col);
            for (int k = 1; k < VTERM_MAX_CHARS_PER_CELL; ++k) extra.push_back(rng());
            ++records;
        }
        for (uint32_t col = 0; col <= 301; ++col)
            mismatches += sbExtraFind(extra.data(), records, col) != sbExtraFindLinear(extra.data(), records, col);
    }
    CHECK_EQ(mismatches, 0);

    const int rows = 6, cols = 200, lines = 300;
    VtermSession s(rows, cols);
    static const char* marks[] = {"\xcc\x81", "\xcc\x80", "\xcc\x82", "\xcc\x83", "\xcc\x88", "\xcd\xa3"};
    std::vector<std::vector<std::string>> want;  // cellText: base + every combining character
    long combining = 0;
    for (int i = 0; i < lines; ++i) {
        std::string line;
        int width = 0;
        while (width < cols - 10) {
            const unsigned k = rng() % 10;
            if (k == 0) line += "\x1b[" + std::to_string(31 + rng() % 7) + "m";
            if (k == 1) {
                line += "\xe6\xbc\xa2";  // wide
                width += 2;
                continue;
            }
            line += static_cast<char>('a' + rng() % 26);
            const unsigned n = rng() % 7;  // 0..6 marks; libvterm keeps up to 5
            for (unsigned m = 0; m < n; ++m) line += marks[rng() % 6];
            ++width;
        }
        s.feed(line + "\x1b[0m");
        std::vector<std::string> row(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            row[static_cast<size_t>(c)] = s.cellText(i, c);
            combining += row[static_cast<size_t>(c)].size() > 3;  // a 1-3 byte base plus at least one mark
        }
        want.push_back(row);
        s.feed("\r\n");
    }
    for (int i = 0; i < rows; ++i) s.feed("\r\n");
    CHECK(s.firstLine() <= 0 && s.topLine() >= lines);
    int bad = 0;
    for (int i = 0; i < lines; ++i)
        for (int c = 0; c < cols; ++c) bad += s.cellText(i, c) != want[static_cast<size_t>(i)][static_cast<size_t>(c)];
    CHECK_EQ(bad, 0);
    CHECK(combining > lines * 50);  // the streams really are full of combining cells
    std::printf("scrollback combining lookup: 3000 random side arrays match the linear scan; %d lines, %ld combining cells round-tripped\n",
                lines, combining);
}

int main() {
    testReports();
    testScrollback();
    testScrollbackCells();
    testScrollbackCombiningLookup();
    testSelection();
    testPointToCell();
    testWindowForGrid();
    testScale2Mapping();
    testBezelMapping();
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
