// SPDX-License-Identifier: GPL-3.0-or-later
// Regression test for third_party/libvterm-patches/0001 (REP of a wide glyph at the right margin).
// Stock libvterm 0.3.3 dereferences NULL at src/screen.c:190 (putglyph marks the right half of a
// wide glyph with getcell(row, col + 1), which is NULL when the glyph starts in the last column)
// and reports a damage rect one column past the screen. Found by tools/fuzz/vterm_fuzz; the input
// is tests/data/fuzz/crash-46ca723d-libvterm-rep.bin, and case 3 is its LTERM_FUZZ_TRACE output.
// Under ASan/UBSan (build-san) the stock library fails cases 1 and 3; the release build may only
// corrupt memory, so the checks below also look at the cells and the damage rects.
#include "vterm_session.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

extern "C" {
#include <vterm.h>
}

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct DamageCheck {
    int rows = 0, cols = 0, bad = 0, calls = 0;
};

static int onDamage(VTermRect r, void* user) {
    auto* d = static_cast<DamageCheck*>(user);
    ++d->calls;
    if (r.start_row < 0 || r.start_col < 0 || r.end_row > d->rows || r.end_col > d->cols || r.start_row >= r.end_row ||
        r.start_col >= r.end_col)
        ++d->bad;
    return 1;
}

// Case 1: libvterm on its own. "中" (U+4E2D, width 2) from column 0, then CSI 9 b. With an odd
// width the repeats reach the last column, where the glyph's right half has no cell.
static void rawCase(int cols, bool autowrap, int damageMerge) {
    VTerm* vt = vterm_new(3, cols);
    vterm_set_utf8(vt, 1);
    VTermScreen* scr = vterm_obtain_screen(vt);
    DamageCheck d;
    d.rows = 3;
    d.cols = cols;
    VTermScreenCallbacks cb;
    std::memset(&cb, 0, sizeof cb);
    cb.damage = onDamage;
    vterm_screen_set_callbacks(scr, &cb, &d);
    vterm_screen_set_damage_merge(scr, static_cast<VTermDamageSize>(damageMerge));
    vterm_screen_reset(scr, 1);
    std::string in = autowrap ? "" : "\x1b[?7l";
    in += "\x1b[1;1H\xe4\xb8\xad\x1b[9b";
    vterm_input_write(vt, in.data(), in.size());
    vterm_screen_flush_damage(scr);
    CHECK(d.bad == 0);
    CHECK(d.calls > 0);
    // Even columns hold whole glyphs, odd ones their right halves (chars[0] == -1). With an odd
    // width the last column holds the clipped glyph, which get_cell() reports as width 1 (it
    // looks at the next column for the right half). One column: the glyph wraps to row 1.
    const int row = cols == 1 ? 1 : 0;
    for (int c = 0; c < cols; ++c) {
        VTermScreenCell cell;
        CHECK(vterm_screen_get_cell(scr, VTermPos{row, c}, &cell) == 1);
        if (c % 2 == 0) {
            CHECK(cell.chars[0] == 0x4E2D);
            CHECK(cell.width == (c + 1 < cols ? 2 : 1));
        } else {
            CHECK(cell.chars[0] == static_cast<uint32_t>(-1));
        }
    }
    // The row below is untouched: the clipped glyph did not spill over.
    VTermScreenCell next;
    vterm_screen_get_cell(scr, VTermPos{row + 1, 0}, &next);
    CHECK(next.chars[0] == 0);
    VTermPos cur;
    vterm_state_get_cursorpos(vterm_obtain_state(vt), &cur);
    CHECK(cur.row == row && cur.col == cols - 1);
    vterm_free(vt);
}

int main() {
    alarm(120);
    for (int cols = 1; cols <= 12; ++cols)
        for (int aw = 0; aw < 2; ++aw)
            for (int dm = VTERM_DAMAGE_CELL; dm <= VTERM_DAMAGE_SCROLL; ++dm) rawCase(cols, aw != 0, dm);

    // Case 2: the same through VtermSession (the session lets REP of a wide glyph through).
    {
        VtermSession s(3, 5);
        const std::string in = "\x1b[1;1H\xe4\xb8\xad\x1b[9b";
        s.feed(in.data(), in.size());
        CHECK(s.rowText(1).empty());
        Grid g;
        s.fillGrid(g);
        CHECK(g.at(0, 4).width == 1 && g.at(0, 4).text.empty());
        // normalizeCell() shows the clipped glyph in the last column as a blank.
        CHECK(s.rowText(0) == "\xe4\xb8\xad\xe4\xb8\xad");
        s.feed("\r\nok", 4);
        CHECK(s.rowText(1) == "ok");
    }

    // Case 3: the fuzzer input (crash-46ca723d-libvterm-rep.bin) replayed from its trace.
    {
        VtermSession s(58, 58);
        s.setScrollbackLimit(57);
        s.resize(57, 33);
        s.resize(57, 35);
        s.resize(57, 33);
        s.resize(4, 33);
        s.resize(56, 252);
        s.resize(57, 33);
        s.resize(59, 33);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(59, 33);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(59, 33);
        s.setScrollbackLimit(6);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(57, 33);
        s.resize(64, 33);
        const std::string chunk(
            "\x1b\x5b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b"
            "\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b"
            "\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39\x39\x83\x86\xe3\x82\xb9"
            "\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97"
            "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"
            "\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86"
            "\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xa3\x83\x88\x3b\xe6\xbc"
            "\xa2\xe5\xad\x97\x00\x00\x00\x00\x08\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3"
            "\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6"
            "\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3"
            "\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5"
            "\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x8a\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3"
            "\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\xe6\xbc\xa2\xe5\xad\x97\xe3"
            "\x83\x86\xe3\x82\xb9\xe3\x83\x88\x1b\x5b\x30\x3b\x37\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39"
            "\x39\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x20\x20\x20\x1b\x5b\x31\x62\x1b\x5b\x35\x38\x64"
            "\x20\x20\x20\x20\xb8\xad\xe6\x96\x87\x1b\x5b\x31\x3b\x31\x48\x1b\x5b\x31\x40\x58\x1b\x5b\x32\x3b"
            "\x32\x48\x59\x1b\x5b\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39"
            "\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39"
            "\x39\x39\x39\x39\x39\x3b\x39\x39\x39\x39\x39\x39\x39\x39\x39\x39",
            496);
        for (char c : chunk) s.feed(&c, 1);
        s.feed("\x1b[2J\x1b[Hok", 9);
        CHECK(s.rowText(0) == "ok");
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("libvterm REP regression OK");
    return 0;
}
