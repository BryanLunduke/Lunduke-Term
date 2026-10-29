// SPDX-License-Identifier: GPL-3.0-or-later
// M4 item 5: Unicode and wide characters through VtermSession (libvterm 0.3.3), selection/copy
// and search. What libvterm does with each case, and what the terminal shows, is written down
// in docs/M4-NOTES.md (item 5); these tests pin it.
#include "search.hpp"
#include "selection.hpp"
#include "vterm_session.hpp"

#include <glib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void feed(VtermSession& s, const std::string& str) { s.feed(str.data(), str.size()); }
static long screenLine(const VtermSession& s, int row) { return s.endLine() - s.rows() + row; }

// "cp/w" per cell of a screen row, e.g. "61/1 6F22/2 /0" (code points joined by '+').
static std::string cells(const VtermSession& s, int row) {
    std::string o;
    char b[16];
    for (int c = 0; c < s.cols(); ++c) {
        uint32_t cp[8];
        int w = 0;
        const int n = s.cellChars(screenLine(s, row), c, cp, 8, &w);
        if (c) o += ' ';
        for (int k = 0; k < n; ++k) {
            std::snprintf(b, sizeof b, "%s%X", k ? "+" : "", cp[k]);
            o += b;
        }
        o += "/" + std::to_string(w);
    }
    return o;
}

static std::string selectRow(const VtermSession& s, int row, int c0, int c1) {
    Selection sel;
    sel.begin(s, SelPos{screenLine(s, row), c0}, SelectMode::Char);
    sel.extend(s, SelPos{screenLine(s, row), c1});
    return sel.text(s);
}

int main() {
    const std::string kan = "\xe6\xbc\xa2", ji = "\xe5\xad\x97";  // 漢 字 (U+6F22, U+5B57)

    // --- CJK wide characters at the right edge -------------------------------------------
    {
        VtermSession s(3, 5);
        feed(s, "abcd" + kan);  // does not fit in the last column: wraps whole, col 4 stays blank
        CHECK(cells(s, 0) == "61/1 62/1 63/1 64/1 /1");
        CHECK(cells(s, 1) == "6F22/2 /0 /1 /1 /1");
        CHECK(s.wrapped(screenLine(s, 0)));
        CHECK(s.rowText(0) == "abcd" && s.rowText(1) == kan);
        Grid g;
        s.fillGrid(g);
        CHECK(g.cursorRow == 1 && g.cursorCol == 2);
        CHECK(g.at(1, 0).width == 2 && g.at(1, 1).width == 0 && g.at(0, 4).width == 1);
    }
    {
        VtermSession s(3, 5);
        feed(s, "abc" + kan);  // fits exactly: no wrap until the next character
        CHECK(cells(s, 0) == "61/1 62/1 63/1 6F22/2 /0");
        CHECK(!s.wrapped(screenLine(s, 0)));
        feed(s, "x");
        CHECK(s.rowText(1) == "x" && s.wrapped(screenLine(s, 0)));
    }
    // --- Overwriting / erasing half of a wide character (normalizeCell) --------------------
    {
        auto run = [&](const std::string& tail) {
            VtermSession s(1, 6);
            feed(s, kan + ji + tail);
            return cells(s, 0) + " | " + s.rowText(0);
        };
        CHECK(run("\x1b[1;1Hx") == "78/1 /1 5B57/2 /0 /1 /1 | x " + ji);   // left half: was a 2-cell "x"
        CHECK(run("\x1b[1;2Hx") == "/1 78/1 5B57/2 /0 /1 /1 |  x" + ji);   // right half: 漢 was drawn 1 cell
        CHECK(run("\x1b[1;2H\x1b[X") == "/1 /1 5B57/2 /0 /1 /1 |   " + ji);  // ECH on the right half
        CHECK(run("\x1b[1;2H\x1b[K") == "/1 /1 /1 /1 /1 /1 | ");            // EL from the right half
        CHECK(run("\x1b[1;2H\x1b[@") == "/1 /1 /1 5B57/2 /0 /1 |    " + ji);  // ICH inside it
        CHECK(run("\x1b[1;1H\x1b[P") == "/1 5B57/2 /0 /1 /1 /1 |  " + ji);   // DCH on the left half
        CHECK(run("\x1b[1;3H" + kan) == "6F22/2 /0 6F22/2 /0 /1 /1 | " + kan + kan);  // wide over wide
        CHECK(run("\x1b[1;2H" + kan) == "/1 6F22/2 /0 /1 /1 /1 |  " + kan);  // wide straddling two
    }
    CHECK(libvtermWidth('a') == 1 && libvtermWidth(0x6F22) == 2 && libvtermWidth(0x1F600) == 2);
    CHECK(libvtermWidth(0x2764) == 1 && libvtermWidth(0x1F1EB) == 1 && libvtermWidth(0x1F3FD) == 2);

    // --- Combining marks ------------------------------------------------------------------
    {
        VtermSession s(2, 10);
        feed(s, "e\xcc\x81" "a\xcc\x88\xcc\xa3");  // é (decomposed), ạ̈
        CHECK(cells(s, 0).rfind("65+301/1 61+308+323/1 /1", 0) == 0);
        Grid g;
        s.fillGrid(g);
        CHECK(g.at(0, 0).text == "e\xcc\x81" && g.cursorCol == 2);
        // At most 6 code points per cell (VTERM_MAX_CHARS_PER_CELL): marks 6.. are dropped.
        feed(s, "\r\no");
        for (int i = 0; i < 8; ++i) feed(s, "\xcc\x81");
        CHECK(cells(s, 1).rfind("6F+301+301+301+301+301/1 /1", 0) == 0);
        // A mark at the start of a line has no base: libvterm drops it.
        VtermSession t(1, 4);
        feed(t, "\xcc\x81" "ab");
        CHECK(t.rowText(0) == "ab");
        // A mark after a wide character joins the wide cell.
        VtermSession u(1, 4);
        feed(u, kan + "\xcc\x88" "z");
        CHECK(cells(u, 0) == "6F22+308/2 /0 7A/1 /1");
        // Split between feed() calls (the PTY can cut anywhere): still one cell.
        VtermSession v(1, 4);
        feed(v, "e");
        feed(v, "\xcc");
        feed(v, "\x81" "f");
        CHECK(cells(v, 0) == "65+301/1 66/1 /1 /1");
    }
    // --- Emoji ----------------------------------------------------------------------------
    {
        VtermSession s(1, 30);
        feed(s, "\xf0\x9f\x98\x80|");                          // 😀 U+1F600: wide
        feed(s, "\xe2\x9d\xa4\xef\xb8\x8f|");                  // ❤️ U+2764 U+FE0F: VS16 does not widen
        feed(s, "\xe2\x9c\x8c\xf0\x9f\x8f\xbd|");              // ✌🏽: base + wide skin-tone cell
        feed(s, "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7|");  // 👩‍👧: ZWJ joins the first cell
        feed(s, "\xf0\x9f\x87\xab\xf0\x9f\x87\xae|");          // 🇫🇮: two narrow regional indicators
        feed(s, "#\xef\xb8\x8f\xe2\x83\xa3|");                 // #️⃣: one cell, 3 code points
        CHECK(cells(s, 0).rfind("1F600/2 /0 7C/1 2764+FE0F/1 7C/1 270C/1 1F3FD/2 /0 7C/1 1F469+200D/2 /0 1F467/2 /0 "
                                "7C/1 1F1EB/1 1F1EE/1 7C/1 23+FE0F+20E3/1 7C/1",
                                0) == 0);
        Grid g;
        s.fillGrid(g);
        CHECK(g.cursorCol == 19);
    }
    // --- No bidi: right-to-left text stays in logical order, one cell per character ----------
    {
        VtermSession s(1, 16);
        feed(s, "ab \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d 1");  // ab שלום 1
        CHECK(cells(s, 0).rfind("61/1 62/1 20/1 5E9/1 5DC/1 5D5/1 5DD/1 20/1 31/1", 0) == 0);
    }
    // --- Invalid UTF-8 ----------------------------------------------------------------------
    {
        VtermSession s(1, 24);
        feed(s, "A\xc3(B\xe2\x82" "C\xf0\x9f\x98" "D\xed\xa0\x80" "E\xc0\x80" "F\xf4\x90\x80\x80" "G\xff");
        const std::string c = cells(s, 0);
        CHECK(c.find("110000") == std::string::npos);  // libvterm decodes F4 90 80 80 to U+110000
        CHECK(c.rfind("41/1 FFFD/1 28/1 42/1 FFFD/1 43/1 FFFD/1 44/1 FFFD/1 45/1 FFFD/1 46/1 FFFD/1 47/1 FFFD/1", 0) == 0);
        for (int col = 0; col < s.cols(); ++col) {
            const std::string t = s.cellText(screenLine(s, 0), col);
            CHECK(g_utf8_validate(t.data(), static_cast<gssize>(t.size()), nullptr));
        }
        const std::string r = s.rowText(0);
        CHECK(g_utf8_validate(r.data(), static_cast<gssize>(r.size()), nullptr));
        // A sequence split across feed() calls is decoded once it is complete.
        VtermSession t(1, 4);
        feed(t, "\xe6");
        feed(t, "\xbc");
        feed(t, "\xa2" "a");
        CHECK(cells(t, 0) == "6F22/2 /0 61/1 /1");
    }
    // --- Selection / copy round trip ---------------------------------------------------------
    {
        const std::string line = kan + ji + " e\xcc\x81 a\xcc\x88\xcc\xa3 \xf0\x9f\x98\x80 \xe2\x9c\x8c\xf0\x9f\x8f\xbd "
                                            "\xf0\x9f\x87\xab\xf0\x9f\x87\xae \xd7\xa9\xd7\x9c";
        VtermSession s(3, 40);
        feed(s, line);
        const std::string copied = selectRow(s, 0, 0, s.cols() - 1);
        CHECK(copied == line);
        CHECK(selectRow(s, 0, 1, 2) == kan + ji);   // starts on 漢's right half: the whole character
        CHECK(selectRow(s, 0, 0, 0) == "");         // a click without moving selects nothing
        CHECK(selectRow(s, 0, 2, 3) == ji);
        // The copied text goes through paste unchanged and gives the same cells again.
        CHECK(sanitizePaste(copied) == copied);
        VtermSession t(3, 40);
        feed(t, sanitizePaste(copied));
        CHECK(cells(t, 0) == cells(s, 0));
        t.takeOutput();
        t.paste(copied);
        CHECK(t.takeOutput() == copied);  // no bracketed paste mode: the bytes as they are
        // Word selection over CJK: a run of wide characters is one word.
        Selection w;
        w.begin(s, SelPos{screenLine(s, 0), 1}, SelectMode::Word);
        CHECK(w.text(s) == kan + ji);
        Selection w2;  // double-click on the right half of 字
        w2.begin(s, SelPos{screenLine(s, 0), 3}, SelectMode::Word);
        CHECK(w2.text(s) == kan + ji && w2.start().col == 0 && w2.end().col == 3);
    }
    // --- Search over wide and combining text -------------------------------------------------
    {
        VtermSession s(3, 20);
        feed(s, "x" + kan + ji + "y e\xcc\x81t\xc3\xa9");  // x漢字y ét(decomposed) + é precomposed
        auto m = findAll(s, ji, true);
        CHECK(m.size() == 1 && m[0].start.col == 3 && m[0].end.col == 4);  // both halves
        m = findAll(s, kan + ji + "y", true);
        CHECK(m.size() == 1 && m[0].start.col == 1 && m[0].end.col == 5);
        m = findAll(s, "e\xcc\x81", true);  // the decomposed é: its own cell
        CHECK(m.size() == 1 && m[0].start.col == 7 && m[0].end.col == 7);
        m = findAll(s, "\xc3\xa9", true);  // precomposed é only matches precomposed (no normalisation)
        CHECK(m.size() == 1 && m[0].start.col == 9);
        m = findAll(s, "\xc3\x89", false);  // É case-insensitive
        CHECK(m.size() == 1 && m[0].start.col == 9);
        SearchIndex idx;
        idx.update(s, true);
        CHECK(idx.find(s, ji).size() == 1 && idx.find(s, ji)[0] == findAll(s, ji, true)[0]);
        CHECK(idx.find(s, "e\xcc\x81").size() == 1);
    }
    if (failures == 0) std::printf("unicode_test: OK\n");
    return failures ? 1 : 0;
}
