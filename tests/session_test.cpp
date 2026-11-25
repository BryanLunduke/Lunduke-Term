// SPDX-License-Identifier: GPL-3.0-or-later
// Checkpoint B: scrollback, selection text, mouse encoding, paste bracketing, DECSCUSR, PTY spawn.
#include "effects.hpp"
#include "grid_painter.hpp"
#include "profiles.hpp"
#include "selection.hpp"
#include "vterm_session.hpp"

#include <ghostty/vt.h>

#include <cmath>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LTERM_VERSION
#define LTERM_VERSION "unknown"
#endif

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
    CHECK(s.topLine() >= 7);
    // libghostty prunes history a page at a time, so a cap of 5 keeps the whole small page.
    CHECK(s.scrollbackSize() >= 5);
    CHECK(s.scrollbackSize() < 80);
    Grid g;
    s.fillGrid(g, 0);
    CHECK_EQ(rowOf(g, 0), std::string("line 7"));
    s.fillGrid(g, 3);
    CHECK_EQ(rowOf(g, 0), std::string("line 4"));
    CHECK_EQ(rowOf(g, 3), std::string("line 7"));
    CHECK_EQ(g.cursorVisible, false);  // cursor row scrolled out of view
    s.fillGrid(g, 99);  // clamped to the history size
    CHECK(rowOf(g, 0).find("line ") == 0);
    CHECK(s.firstLine() <= s.topLine());
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
    // A cap of 0 still retains the open page (a handful of lines), not an unbounded history.
    CHECK(z.scrollbackSize() < 16);
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
        std::printf("tight grid: %s %d px, cell %dx%d, logical %dx%d (80x24)\n", f.family.c_str(), f.px,
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

// Retro profiles: the default window is 4:3 and the profile's grid still fills it. Plain and
// modern stay tight. A small work area shrinks the cells instead of the column count.
static void testCrtWindow() {
    auto check = [](const char* id, int cols, int rows, bool crt, int maxW, int maxH) {
        const Profile* p = findProfile(id);
        CHECK(p);
        if (!p) return;
        const FontPick f = profileFont(*p);
        GridPainter gp(f.family, f.px, 1);
        const CellBox box = fitCellBox(gp.cellWidth(), gp.cellHeight(), 1, cols, rows, p->marginMinPx, p->marginFrac,
                                       0, crt, maxW, maxH);
        const WindowSize s = windowSizeForGrid(cols, rows, box.cellW, box.cellH, 1, p->marginMinPx, p->marginFrac, 0);
        const GridLayout g = computeGridLayout(s.width, s.height, 1, box.cellW, box.cellH, p->marginMinPx, p->marginFrac, 0);
        CHECK_EQ(g.cols, cols);
        CHECK_EQ(g.rows, rows);
        if (maxW > 0) CHECK(s.width <= maxW);
        if (maxH > 0) CHECK(s.height <= maxH);
        const double aspect = static_cast<double>(s.width) / std::max(1, s.height);
        if (crt) {
            CHECK(std::fabs(aspect - 4.0 / 3.0) < 0.03);
            CHECK(box.cellH >= gp.cellHeight() || (maxW > 0 && s.width < maxW));
        }
        std::printf("%s window %dx%d aspect %.3f cells %dx%d -> %dx%d (limit %dx%d)\n", id, s.width, s.height, aspect,
                    box.cellW, box.cellH, g.cols, g.rows, maxW, maxH);
    };
    check("amber", 80, 24, true, 0, 0);
    check("ibm-dos", 80, 24, true, 0, 0);
    check("pixel-40col", 40, 25, true, 0, 0);
    check("plain", 80, 24, false, 0, 0);
    check("modern", 80, 24, false, 0, 0);
    check("amber", 80, 24, true, 800, 600);
    check("amber", 80, 24, true, 1280, 800);
    check("ibm-dos", 80, 24, true, 800, 600);
    check("pixel-40col", 40, 25, true, 800, 600);
    // HiDPI: the logical window at scale 2 matches scale 1.
    if (const Profile* p = findProfile("amber")) {
        const FontPick f = profileFont(*p);
        GridPainter g1(f.family, f.px, 1), g2(f.family, f.px, 2);
        const CellBox b1 = fitCellBox(g1.cellWidth(), g1.cellHeight(), 1, 80, 24, p->marginMinPx, p->marginFrac, 0, true);
        const CellBox b2 = fitCellBox(g2.cellWidth(), g2.cellHeight(), 2, 80, 24, p->marginMinPx, p->marginFrac, 0, true);
        const WindowSize s1 = windowSizeForGrid(80, 24, b1.cellW, b1.cellH, 1, p->marginMinPx, p->marginFrac, 0);
        const WindowSize s2 = windowSizeForGrid(80, 24, b2.cellW, b2.cellH, 2, p->marginMinPx, p->marginFrac, 0);
        CHECK_EQ(s1.width, s2.width);
        CHECK_EQ(s1.height, s2.height);
        CHECK_EQ(b2.cellW, b1.cellW * 2);
        CHECK_EQ(b2.cellH, b1.cellH * 2);
    }
    // Plain is not forced onto 4:3.
    if (const Profile* p = findProfile("plain")) {
        const FontPick f = profileFont(*p);
        GridPainter gp(f.family, f.px, 1);
        const CellBox box = fitCellBox(gp.cellWidth(), gp.cellHeight(), 1, 80, 24, p->marginMinPx, p->marginFrac, 0, false);
        CHECK_EQ(box.cellW, gp.cellWidth());
        CHECK_EQ(box.cellH, gp.cellHeight());
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
    s.mouseButton(2, 5, 1, true, ModNone);
    s.mouseButton(2, 5, 1, false, ModNone);
    CHECK_EQ(s.takeOutput(), std::string(""));
    CHECK_EQ(s.mouseMode(), int(MouseMode::None));
    // X10 compatibility (?9): presses only.
    s.feed("\x1b[?9h");
    CHECK_EQ(s.mouseMode(), int(MouseMode::Click));
    s.mouseButton(2, 5, 1, true, ModNone);
    s.mouseButton(2, 5, 1, false, ModNone);
    s.mouseMove(3, 3, ModNone);
    s.mouseButton(1, 0, 3, true, ModCtrl);  // no modifiers in X10
    CHECK_EQ(s.takeOutput(), std::string("\x1b[M\x20\x26\x23\x1b[M\x22\x21\x22"));
    s.feed("\x1b[?9l");
    CHECK_EQ(s.mouseMode(), int(MouseMode::None));
    s.feed("\x1b[?");  // split sequence across two reads
    s.feed("9h");
    CHECK_EQ(s.mouseMode(), int(MouseMode::Click));
    s.feed("\x1b[?9l");
    // Normal tracking (?1000), X10 byte encoding: button + 32, col + 33, row + 33.
    s.feed("\x1b[?1000h");
    CHECK_EQ(s.mouseMode(), int(MouseMode::Click));
    s.mouseButton(2, 5, 1, true, ModNone);
    s.mouseButton(2, 5, 1, false, ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[M\x20\x26\x23\x1b[M\x23\x26\x23"));
    s.mouseMove(3, 7, ModNone);  // no motion reports in ?1000
    CHECK_EQ(s.takeOutput(), std::string(""));
    // Wheel = buttons 4/5 (64/65).
    s.mouseButton(0, 0, 4, true, ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[M\x60\x21\x21"));
    // SGR 1006.
    s.feed("\x1b[?1006h");
    s.mouseButton(2, 5, 1, true, ModNone);
    s.mouseButton(2, 5, 1, false, ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<0;6;3M\x1b[<0;6;3m"));
    s.mouseButton(2, 5, 3, true, ModCtrl);  // right button with Ctrl (+16)
    s.mouseButton(2, 5, 3, false, ModCtrl);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<18;6;3M\x1b[<18;6;3m"));
    s.mouseButton(9, 9, 5, true, ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<65;10;10M"));
    // Button-event tracking (?1002): motion only while a button is held.
    s.feed("\x1b[?1002h");
    CHECK_EQ(s.mouseMode(), int(MouseMode::Drag));
    s.mouseMove(4, 4, ModNone);
    CHECK_EQ(s.takeOutput(), std::string(""));
    s.mouseButton(4, 4, 1, true, ModNone);
    s.mouseMove(4, 6, ModNone);
    s.mouseButton(4, 6, 1, false, ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<0;5;5M\x1b[<32;7;5M\x1b[<0;7;5m"));
    // Any-event tracking (?1003): motion without buttons (button 3 + 32 = 35).
    s.feed("\x1b[?1003h");
    CHECK_EQ(s.mouseMode(), int(MouseMode::Move));
    s.mouseMove(1, 1, ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[<35;2;2M"));
    s.mouseMove(1, 1, ModNone);  // same cell: nothing
    CHECK_EQ(s.takeOutput(), std::string(""));
    // Modes are independent: resetting 1003 leaves 1002. Turn the rest off too.
    s.feed("\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l\x1b[?9l");
    CHECK_EQ(s.mouseMode(), int(MouseMode::None));
}

// The key event is reused. A later key with no ASCII unshifted codepoint must not
// keep the previous one: kitty disambiguate mode would report Alt+é as Alt+a.
static void testUnshiftedCodepoint() {
    VtermSession s(4, 40);
    s.feed("\x1b[=1u");  // kitty keyboard: disambiguate
    s.takeOutput();
    s.sendChar('a', ModCtrl);
    const std::string ctrlA = s.takeOutput();
    s.sendChar(0x00E9, ModAlt);  // é
    const std::string altE = s.takeOutput();
    std::printf("kitty unshifted: ctrl+a %s, alt+eacute %s\n", ctrlA.c_str(), altE.c_str());
    CHECK(ctrlA.find("\x1b[97;5u") != std::string::npos);
    CHECK(altE.find("\x1b[97;") == std::string::npos);  // not the leftover 'a'
    CHECK(altE.find("233") != std::string::npos);       // U+00E9
    CHECK(altE.find(";3u") != std::string::npos);       // Alt
    // A following arrow must not inherit é either.
    s.sendKey(TermKey::Up, ModNone);
    const std::string up = s.takeOutput();
    CHECK(up.find("233") == std::string::npos);
    CHECK(up.find("\x1b[A") != std::string::npos || up.find("\x1bOA") != std::string::npos);
}

// Kitty keyboard protocol: Shift is the unshifted key plus the Shift modifier,
// a fresh Alt/Ctrl chord keeps that modifier, and releases are sent only when
// the application asks for event types.
static void testKittyKeyboard() {
    VtermSession legacy(4, 40);
    legacy.sendChar('A', ModShift, 'a');
    CHECK_EQ(legacy.takeOutput(), std::string("A"));  // legacy: the character, not a CSI
    legacy.sendChar('A', ModCtrl | ModShift, 'a');
    CHECK_EQ(legacy.takeOutput(), std::string("\x01"));  // Ctrl+Shift+A is ^A, not CSI u
    legacy.sendChar('@', ModCtrl | ModShift, '2');
    CHECK_EQ(legacy.takeOutput(), std::string("\0", 1));  // Ctrl+@ is NUL
    legacy.sendChar('[', ModCtrl, '[');
    CHECK_EQ(legacy.takeOutput(), std::string("\x1b"));  // Ctrl+[ is ESC
    legacy.sendChar('i', ModCtrl, 'i');
    CHECK_EQ(legacy.takeOutput(), std::string("\t"));  // Ctrl+i is TAB
    legacy.sendChar('m', ModCtrl, 'm');
    CHECK_EQ(legacy.takeOutput(), std::string("\r"));  // Ctrl+m is CR
    legacy.sendChar('a', ModAlt | ModCtrl, 'a');
    CHECK_EQ(legacy.takeOutput(), std::string("\x1b\x01"));
    legacy.sendChar('x', ModAlt);
    CHECK_EQ(legacy.takeOutput(), std::string("\x1bx"));

    VtermSession s(4, 40);
    s.feed("\x1b[=5u");  // disambiguate + report alternates
    s.takeOutput();
    CHECK(!s.reportsKeyEvents());
    s.sendChar('A', ModShift, 'a');
    CHECK_EQ(s.takeOutput(), std::string("\x1b[97:65;2u"));
    // First chord after the mode switch still carries Alt / Ctrl.
    s.sendChar('x', ModAlt);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[120;3u"));
    s.sendChar('c', ModCtrl);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[99;5u"));
    s.sendChar('a', ModNone, 0, KeyAction::Release);
    CHECK_EQ(s.takeOutput(), std::string(""));  // releases off

    s.feed("\x1b[=31u");  // every kitty flag
    s.takeOutput();
    CHECK(s.reportsKeyEvents());
    CHECK(s.reportsAllKeys());
    s.sendChar('a', ModNone);
    CHECK_EQ(s.takeOutput(), std::string("\x1b[97;;97u"));  // associated text, press
    s.sendChar('a', ModNone, 'a', KeyAction::Release);
    const std::string up = s.takeOutput();
    CHECK(up.find(":3") != std::string::npos);  // event type release
    CHECK(up.find("97") != std::string::npos);
    s.sendPhysical(GHOSTTY_KEY_CONTROL_LEFT, ModCtrl, nullptr, 0, 0);
    const std::string ctrl = s.takeOutput();
    std::printf("kitty ctrl press: %s\n", ctrl.c_str());
    CHECK(!ctrl.empty());
    CHECK(ctrl.find(";5") != std::string::npos);  // Ctrl modifier is present on the key itself
    s.sendPhysical(GHOSTTY_KEY_ALT_LEFT, ModAlt, nullptr, 0, 0);
    const std::string alt = s.takeOutput();
    CHECK(!alt.empty());
    CHECK(alt.find(";3") != std::string::npos);
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

static void testScrollbackCompress() {
    VtermSession s(24, 40);
    s.setScrollbackLimit(2000);
    std::string line(40, 'a');
    line += "\r\n";
    std::string chunk;
    for (int i = 0; i < 1500; ++i) chunk += line;
    s.feed(chunk);
    const size_t before = s.scrollbackBytes();
    s.compressScrollback(true);
    const size_t after = s.scrollbackBytes();
    std::printf("scrollback compress: %zu -> %zu bytes\n", before, after);
    CHECK(after > 0);
    CHECK(after <= before);
    CHECK(s.cellText(s.topLine() - 1, 0) == "a");
}

// A paste larger than the old 1 MiB cap arrives intact, brackets included, and a
// key typed while a pty is not reading is delivered after the paste, not dropped.
static void testPasteStream() {
    VtermSession s(4, 20);
    s.paste(std::string(1000, 'q'));
    s.paste("Z");
    CHECK_EQ(s.takeOutput(), std::string(1000, 'q') + "Z");

    const size_t n = (2u << 20);
    std::string body(n, 'a');
    body[n / 2] = '\n';  // must survive as CR, not be the point where a cap cuts the paste
    VtermSession big(4, 20);
    big.paste(body);
    big.paste("XYZ");
    const std::string out = big.takeOutput();
    CHECK_EQ(out.size(), n + 3);
    CHECK_EQ(out.compare(0, n / 2, std::string(n / 2, 'a')), 0);
    CHECK_EQ(out[n / 2], '\r');
    CHECK_EQ(out.compare(n / 2 + 1, n - n / 2 - 1, std::string(n - n / 2 - 1, 'a')), 0);
    CHECK_EQ(out.compare(n, 3, "XYZ"), 0);

    VtermSession br(4, 20);
    br.feed("\x1b[?2004h");
    br.takeOutput();
    br.paste(body);
    const std::string wrapped = br.takeOutput();
    CHECK_EQ(wrapped.size(), n + 12);
    CHECK_EQ(wrapped.compare(0, 6, "\x1b[200~"), 0);
    CHECK_EQ(wrapped.compare(wrapped.size() - 6, 6, "\x1b[201~"), 0);
    CHECK_EQ(wrapped[6 + n / 2], '\r');  // the newline is still in the middle, not cut off
    std::printf("paste stream: plain %zu bytes, bracketed %zu bytes\n", out.size(), wrapped.size());

    // The child is not reading yet. The paste blocks on the pty, and a key typed
    // then must still be delivered after it (not dropped, not spliced in).
    char path[] = "/tmp/lterm-paste-XXXXXX";
    const int tfd = mkstemp(path);
    CHECK(tfd >= 0);
    if (tfd >= 0) close(tfd);
    VtermSession pty(4, 40);
    SpawnOptions opt;
    const std::string nbytes = std::to_string(n + 1);
    opt.argv = {"/bin/sh", "-c",
                "stty raw -echo; printf READY; head -c \"$1\" > \"$2\"; printf ' DONE'", "sh", nbytes.c_str(), path};
    CHECK(pty.spawn(opt));
    const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string screen;
    while (std::chrono::steady_clock::now() < readyBy) {
        pollfd p{pty.masterFd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0) pty.readAvailable();
        screen.clear();
        for (int r = 0; r < pty.rows(); ++r) screen += pty.rowText(r);
        if (screen.find("READY") != std::string::npos) break;
    }
    CHECK(screen.find("READY") != std::string::npos);
    const std::string streamed(n, 'b');
    pty.paste(streamed);
    pty.sendBytes("Z", 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::string screen;
    while (std::chrono::steady_clock::now() < deadline) {
        pty.flushWrites();
        pollfd p{pty.masterFd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0) pty.readAvailable();
        screen.clear();
        for (int r = 0; r < pty.rows(); ++r) screen += pty.rowText(r);
        if (screen.find("DONE") != std::string::npos) break;
    }
    CHECK(screen.find("DONE") != std::string::npos);
    FILE* f = std::fopen(path, "rb");
    CHECK(f != nullptr);
    std::string got;
    if (f) {
        char buf[65536];
        size_t k;
        while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) got.append(buf, k);
        std::fclose(f);
    }
    std::remove(path);
    CHECK_EQ(got.size(), n + 1);
    CHECK(got.size() == n + 1 && got.compare(0, n, streamed) == 0 && got.back() == 'Z');
    pty.hangup(50);
    std::printf("paste stream pty: child received %zu bytes\n", got.size());
}

// A profile colour change repaints cells that are already on screen.
static void testPasteConfirm() {
    CHECK(!pasteNeedsConfirm("echo hi", false, true));
    CHECK(pasteNeedsConfirm("echo hi\nls", false, true));
    CHECK(!pasteNeedsConfirm("echo hi\nls", true, true));  // bracketed paste: the app asked for the lines
    CHECK(pasteNeedsConfirm("sudo reboot", false, true));
    CHECK(pasteNeedsConfirm("sudo reboot", true, true));  // sudo still asks inside brackets
    CHECK(pasteNeedsConfirm("echo hi; sudo -n true", true, true));
    CHECK(!pasteNeedsConfirm("pseudocode\n", true, true));
    CHECK(!pasteNeedsConfirm("sudo reboot", false, false));  // don't ask again
    std::printf("paste confirm: multi-line, bracketed skip, sudo kept\n");
}

static void testLinks() {
    VtermSession s(4, 60);
    s.feed("see https://example.com/a. next\r\n");
    int a = -1, b = -1;
    const long line = s.topLine();
    // "see " is 4 columns, then the URL.
    CHECK(s.linkAt(line, 4, &a, &b).find("https://example.com/a") != std::string::npos);
    CHECK(a == 4);
    CHECK(s.linkAt(line, 0).empty());
    s.feed("\x1b]8;;https://lunduke.com/x\x1b\\click\x1b]8;;\x1b\\");
    const long row = s.topLine() + 1;
    const std::string uri = s.linkAt(row, 0, &a, &b);
    CHECK(uri == "https://lunduke.com/x");
    CHECK(a == 0 && b == 4);  // "click"
    CHECK(s.hyperlinkAt(row, 0) == uri);
    CHECK(s.hyperlinkAt(row, 5).empty());
    std::printf("links: url span %d-%d, osc 8 %s\n", a, b, uri.c_str());
}

static void testOsc52() {
    VtermSession s(4, 20);
    // "hi" is aGk= ; "c" is the system clipboard. The read form must not echo it back.
    s.feed("\x1b]52;c;aGk=\x07");
    CHECK(s.osc52Text() == "hi");
    s.feed("\x1b]52;c;?\x07");
    const std::string reply = s.takeOutput();
    CHECK(reply.find("aGk=") == std::string::npos);
    CHECK(reply.find("hi") == std::string::npos);
    s.feed("\x1b]52;c;\x07");
    CHECK(s.osc52Text().empty());
    std::printf("osc 52: write %s, read suppressed, clear ok\n", "hi");
}

static void testOscColors() {
    VtermSession s(4, 40);
    std::array<Color8, 16> pal{};
    pal[1] = Color8{205, 0, 0};
    s.setColors(Color8{200, 200, 200}, Color8{0, 0, 0}, pal, 0);
    s.setCursorColor(Color8{200, 200, 200});
    s.feed("\x1b[31mX");
    Grid g;
    s.fillGrid(g);
    CHECK(g.at(0, 0).fg.r == 205 && g.at(0, 0).fg.b == 0);
    s.feed("\x1b]4;1;#0000ff\x1b\\");
    s.fillGrid(g);
    CHECK(g.at(0, 0).fg.r == 0 && g.at(0, 0).fg.b == 255);
    s.feed("\x1b[0m\x1b]10;#00ff00\x1b\\Z");
    s.fillGrid(g);
    CHECK(g.at(0, 1).text == "Z");
    CHECK(g.at(0, 1).fg.g == 255 && g.at(0, 1).fg.r == 0);
    s.feed("\x1b]11;#010203\x1b\\");
    s.fillGrid(g);
    CHECK(g.at(0, 1).bg.r == 1 && g.at(0, 1).bg.g == 2 && g.at(0, 1).bg.b == 3);
    s.feed("\x1b]12;#abcdef\x1b\\");
    s.fillGrid(g);
    CHECK(s.cursorColor().r == 0xab && s.cursorColor().g == 0xcd && s.cursorColor().b == 0xef);
    s.takeOutput();
    s.feed("\x1b]10;?\x1b\\");
    const std::string q10 = s.takeOutput();
    CHECK(q10.find("\x1b]10;") != std::string::npos);
    CHECK(q10.find("rgb:") != std::string::npos);
    s.feed("\x1b]11;?\x1b\\");
    const std::string q11 = s.takeOutput();
    CHECK(q11.find("\x1b]11;") != std::string::npos);
    s.feed("\x1b]4;1;?\x1b\\");
    const std::string q4 = s.takeOutput();
    CHECK(q4.find("\x1b]4;1;") != std::string::npos);
    s.feed("\x1b]104;1\x1b\\\x1b[31mQ");
    s.fillGrid(g);
    CHECK(g.at(0, 2).text == "Q");
    CHECK(g.at(0, 2).fg.r == 205 && g.at(0, 2).fg.b == 0);
    s.feed("\x1b[0m\x1b]110\x1b\\W");
    s.fillGrid(g);
    CHECK(g.at(0, 3).text == "W");
    CHECK(g.at(0, 3).fg.r == 200 && g.at(0, 3).fg.g == 200);
    s.feed("\x1b]111\x1b\\");
    s.fillGrid(g);
    CHECK(g.at(0, 3).bg.r == 0 && g.at(0, 3).bg.g == 0 && g.at(0, 3).bg.b == 0);
    s.feed("\x1b]112\x1b\\");
    s.fillGrid(g);
    CHECK(s.cursorColor().r == 200 && s.cursorColor().g == 200 && s.cursorColor().b == 200);
    std::printf("osc colors: palette, fg/bg/cursor, queries, resets\n");
}

static void testSyncOutput() {
    VtermSession s(4, 20);
    int began = 0, ended = 0;
    s.onSyncHold = [&](bool held) {
        if (held) ++began;
        else ++ended;
    };
    CHECK(!s.syncHeld());
    s.feed("\x1b[?2026h");
    CHECK(s.syncHeld());
    CHECK(began == 1);
    s.feed("HELLO");
    CHECK(s.syncHeld());  // the text is in the model; presentation is the view's job
    s.feed("\x1b[?2026h");  // already held: no second begin
    CHECK(began == 1);
    s.feed("\x1b[?2026l");
    CHECK(!s.syncHeld());
    CHECK(ended == 1);
    Grid g;
    s.fillGrid(g);
    CHECK(g.at(0, 0).text == "H" && g.at(0, 4).text == "O");
}

static void testColorRefresh() {
    VtermSession s(4, 20);
    std::array<Color8, 16> pal{};
    s.setColors(Color8{200, 180, 40}, Color8{0, 0, 0}, pal, 0);
    s.feed("A");
    Grid g;
    s.fillGrid(g);
    CHECK(g.at(0, 0).text == "A");
    CHECK(g.at(0, 0).fg.r == 200);
    s.setColors(Color8{10, 20, 30}, Color8{1, 2, 3}, pal, 0);
    s.fillGrid(g);
    CHECK(g.at(0, 0).fg.r == 10 && g.at(0, 0).fg.g == 20 && g.at(0, 0).bg.b == 3);
}

static void testCursor() {
    VtermSession s(4, 20);
    CHECK_EQ(s.cursorShape(), 0);  // not set by the app: the user's setting applies
    CHECK_EQ(s.cursorBlink(), -1);
    s.feed("\x1b[4 q");
    CHECK_EQ(s.cursorShape(), int(CursorShape::Underline));
    CHECK_EQ(s.cursorBlink(), 0);
    s.feed("\x1b[5 q");
    CHECK_EQ(s.cursorShape(), int(CursorShape::Bar));
    CHECK_EQ(s.cursorBlink(), 1);
    s.feed("\x1b[2 q");
    CHECK_EQ(s.cursorShape(), int(CursorShape::Block));
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
    CHECK(has("TERM_PROGRAM=lunduke-terminal"));
    CHECK(has("TERM_PROGRAM_VERSION=" LTERM_VERSION));
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

// Primary device attributes. libghostty answers as a VT220 with ANSI color and does not
// implement DECREQTPARM (CSI x), which libvterm's session used to answer itself.
static void testReports() {
    VtermSession s(24, 80);
    s.feed("\x1b[c");
    const std::string da = s.takeOutput();
    CHECK(da.find("\x1b[?62;") != std::string::npos);
    CHECK(da.back() == 'c');
    s.feed("\x1b[x\x1b[1x\x1b[5x");
    CHECK(s.takeOutput().find('x') == std::string::npos);
}

// Attributes, combining marks and a wide character survive scrollback and a resize reflow.
static void testScrollbackCells() {
    const int cols = 40;
    const std::string row0 =
        "\x1b[1;3;4:3;9;53;58:2::10:20:30mA\x1b[0m"
        "\x1b[31;42mB\x1b[38;5;200;48;2;10;20;30mC\x1b[0m"
        "e\xcc\x81"
        "\xe6\xbc\xa2"
        "\x1b[4:2mZ\x1b[0m";
    VtermSession s(4, cols);
    s.feed(row0 + "\r\nplain\r\n");
    Grid before;
    s.fillGrid(before, 0);
    CHECK(before.at(0, 0).text == "A");
    CHECK(before.at(0, 0).bold && before.at(0, 0).italic);
    CHECK(before.at(0, 0).underline == Underline::Curly);
    CHECK(before.at(0, 0).strikethrough && before.at(0, 0).overline);
    CHECK(before.at(0, 0).underlineColored);
    CHECK(before.at(0, 0).underlineColor.r == 10 && before.at(0, 0).underlineColor.b == 30);
    CHECK(before.at(0, 1).fg.r > 150 && before.at(0, 1).bg.g > 80);
    CHECK(before.at(0, 2).text == "C");
    CHECK(before.at(0, 3).text == "e\xcc\x81");
    CHECK(before.at(0, 4).width == 2 && before.at(0, 5).width == 0);
    // Push the rows into history, then read them back through the scrollback.
    s.feed("\r\n\r\n\r\n\r\n");
    CHECK(s.scrollbackSize() >= 2);
    Grid hist;
    s.fillGrid(hist, static_cast<int>(s.scrollbackSize()));
    CHECK(hist.at(0, 0).text == "A");
    CHECK(hist.at(0, 0).underline == Underline::Curly && hist.at(0, 0).strikethrough);
    CHECK(hist.at(0, 3).text == "e\xcc\x81");
    CHECK(hist.at(0, 4).width == 2);
    // Growing the window reflows; the styled row is still readable.
    s.resize(8, cols);
    Grid after;
    s.fillGrid(after, 0);
    bool found = false;
    for (int r = 0; r < after.rows; ++r)
        if (after.at(r, 0).text == "A" && after.at(r, 0).underline == Underline::Curly) found = true;
    CHECK(found);
    CHECK(s.scrollbackBytes() > 0);
    std::printf("scrollback cells: styled row round-tripped; resident %zu bytes\n", s.scrollbackBytes());
}

// Combining marks and wide characters in a few hundred lines come back unchanged.
static void testScrollbackCombiningLookup() {
    std::mt19937 rng(20261102);
    const int rows = 6, cols = 40, lines = 40;
    VtermSession s(rows, cols);
    static const char* marks[] = {"\xcc\x81", "\xcc\x80", "\xcc\x82", "\xcc\x83", "\xcc\x88", "\xcd\xa3"};
    std::vector<std::vector<std::string>> want;
    long combining = 0;
    const long base = s.topLine();
    for (int i = 0; i < lines; ++i) {
        std::string line;
        int width = 0;
        while (width < cols - 4) {
            const unsigned k = rng() % 10;
            if (k == 0) line += "\x1b[" + std::to_string(31 + rng() % 7) + "m";
            if (k == 1) {
                line += "\xe6\xbc\xa2";
                width += 2;
                continue;
            }
            line += static_cast<char>('a' + rng() % 26);
            const unsigned n = rng() % 4;
            for (unsigned m = 0; m < n; ++m) line += marks[rng() % 6];
            ++width;
        }
        s.feed(line + "\x1b[0m");
        std::vector<std::string> row(static_cast<size_t>(cols));
        const long ln = s.topLine() + s.rows() - 1;  // cursor row before the newline, filled below
        (void)ln;
        s.feed("\r\n");
        const long stored = s.topLine() - 1;
        for (int c = 0; c < cols; ++c) {
            row[static_cast<size_t>(c)] = s.cellText(stored, c);
            combining += row[static_cast<size_t>(c)].size() > 3;
        }
        want.push_back(row);
        (void)base;
    }
    int bad = 0;
    for (int i = 0; i < lines; ++i) {
        const long ln = s.topLine() - (lines - i);
        for (int c = 0; c < cols; ++c)
            bad += s.cellText(ln, c) != want[static_cast<size_t>(i)][static_cast<size_t>(c)];
    }
    CHECK_EQ(bad, 0);
    CHECK(combining > 10);
    std::printf("scrollback combining: %d lines, %ld combining cells round-tripped\n", lines, combining);
}

int main() {
    testReports();
    testScrollback();
    testScrollbackCells();
    testScrollbackCombiningLookup();
    testSelection();
    testPointToCell();
    testWindowForGrid();
    testCrtWindow();
    testScale2Mapping();
    testBezelMapping();
    testMouse();
    testUnshiftedCodepoint();
    testKittyKeyboard();
    testPaste();
    testPasteStream();
    testColorRefresh();
    testSyncOutput();
    testOscColors();
    testOsc52();
    testLinks();
    testPasteConfirm();
    testScrollbackCompress();
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
