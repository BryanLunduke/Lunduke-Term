// SPDX-License-Identifier: GPL-3.0-or-later
#include "profiles.hpp"
#include "vterm_session.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void feed(VtermSession& s, const char* str) { s.feed(str, std::strlen(str)); }

int main() {
    VtermSession s(5, 20);
    feed(s, "hello\r\n\x1b[31mred\x1b[0m \x1b[1;44mbold\x1b[0m");
    CHECK(s.rowText(0) == "hello");
    CHECK(s.rowText(1) == "red bold");
    Grid g;
    s.fillGrid(g);
    CHECK(g.rows == 5 && g.cols == 20);
    CHECK(g.at(0, 0).text == "h");
    CHECK(g.at(1, 0).fg.r > 150 && g.at(1, 0).fg.g < 100);  // ANSI red
    CHECK(g.at(1, 4).bold);
    CHECK(g.at(1, 4).bg.b > 100);                            // blue background
    CHECK(g.cursorRow == 1 && g.cursorCol == 8);

    // UTF-8 and wide characters.
    feed(s, "\r\n\xc3\xa9\xe6\xbc\xa2x");
    s.fillGrid(g);
    CHECK(g.at(2, 0).text == "\xc3\xa9");
    CHECK(g.at(2, 1).width == 2);
    CHECK(g.at(2, 2).width == 0);
    CHECK(g.at(2, 3).text == "x");

    // Resize.
    s.resize(10, 40);
    s.fillGrid(g);
    CHECK(g.rows == 10 && g.cols == 40);
    CHECK(s.rowText(0) == "hello");

    // Title (OSC 2).
    std::string seen;
    s.onTitle = [&] { seen = s.title(); };
    feed(s, "\x1b]2;M0 title\x07");
    CHECK(seen == "M0 title");

    // Profile palette mapping: ANSI 0-15 come from the palette, default colours from fg/bg,
    // 256-colour/truecolor foregrounds are lifted to the minimum grey.
    {
        VtermSession p(3, 20);
        std::array<Color8, 16> pal{};
        for (int i = 0; i < 16; ++i) pal[i] = Color8{uint8_t(i * 10), uint8_t(i * 10 + 1), uint8_t(i * 10 + 2)};
        p.setColors(Color8{1, 2, 3}, Color8{4, 5, 6}, pal, 0.5f);
        feed(p, "a\x1b[34mb\x1b[94mc\x1b[38;2;0;0;80md\x1b[0;45me");
        Grid h;
        p.fillGrid(h);
        CHECK(h.at(0, 0).fg.r == 1 && h.at(0, 0).fg.b == 3);
        CHECK(h.at(0, 0).bg.r == 4 && h.at(0, 0).bg.b == 6);
        CHECK(h.at(0, 1).fg.r == 40 && h.at(0, 1).fg.g == 41);    // palette[4]
        CHECK(h.at(0, 2).fg.r == 120 && h.at(0, 2).fg.b == 122);  // palette[12]
        CHECK(greyOf(h.at(0, 3).fg) >= 0.49f);                     // truecolor dark blue lifted
        CHECK(h.at(0, 4).bg.r == 50);                              // palette[5] background
    }

    // M2 stage 2 (vttest 2 "save/restore cursor"): DECSC/DECRC save the character sets too.
    {
        VtermSession q(3, 20);
        // G0 = DEC graphics, save, back to ASCII, restore (the cursor too): the 'q' written
        // over the ASCII one must be a line again.
        feed(q, "\x1b(0q\x1b" "7\x1b(Bq\x1b" "8q");
        CHECK(q.rowText(0) == "\xe2\x94\x80\xe2\x94\x80");
        // Split over chunk boundaries, and the CSI ?1048 form, and SO/SI (G1).
        feed(q, "\r\n\x1b)0\x0e");
        feed(q, "\x1b[?10");
        feed(q, "48h\x0f\x1b");
        feed(q, "(Bx\x1b[?1048l" "x");
        CHECK(q.rowText(1) == "\xe2\x94\x82");
        // RIS resets the tracked state: no stale restore afterwards.
        VtermSession u(3, 20);
        feed(u, "\x1b(0q\x1b" "7\x1b" "c\x1b" "8q");
        CHECK(u.rowText(0) == " q");
    }
    // vttest 11.8.7.3: private mode 47 switches to/from the alternate screen.
    {
        VtermSession a(3, 10);
        feed(a, "main\x1b[?4");
        feed(a, "7hALT");
        CHECK(a.altScreen() && a.rowText(0) == "    ALT");
        feed(a, "\x1b[?47l");
        CHECK(!a.altScreen() && a.rowText(0) == "main");
    }
    // vim: resizing while a full-screen program has a reverse/colour pen must not paint the
    // new and reflowed blanks (also those of the main screen behind it) with that pen.
    {
        VtermSession v(3, 10);
        v.setColors(Color8{200, 200, 200}, Color8{0, 0, 0}, std::array<Color8, 16>{}, 0.f);
        feed(v, "main\x1b[?1049h\x1b[7;44mstatus");
        v.resize(5, 20);
        feed(v, "X\x1b[?1049l");
        Grid h;
        v.fillGrid(h);
        CHECK(h.at(0, 8).bg.r == 0 && h.at(0, 8).bg.b == 0);   // was the old width
        CHECK(h.at(0, 15).bg.r == 0 && h.at(0, 15).bg.b == 0);  // new column
        CHECK(h.at(4, 3).bg.r == 0);                            // new row
        feed(v, "\x1b[?1049h");
        v.resize(4, 12);
        feed(v, "\x1b[HY");
        v.fillGrid(h);
        CHECK(h.at(0, 0).text == "Y" && h.at(0, 0).fg.b > 100);  // pen (reverse on blue) kept
        CHECK(h.at(3, 11).bg.r == 0 && h.at(3, 11).bg.b == 0);
    }
    // DECSCNM: libvterm reverses the cells itself; the grid must come out light exactly once.
    {
        VtermSession r(2, 10);
        r.setColors(Color8{200, 200, 200}, Color8{0, 0, 0}, std::array<Color8, 16>{}, 0.f);
        feed(r, "a\x1b[?5h");
        Grid h;
        r.fillGrid(h);
        CHECK(r.reverseVideo());
        CHECK(h.at(0, 0).bg.r == 200 && h.at(0, 0).fg.r == 0);
        CHECK(h.at(1, 5).bg.r == 200);
        feed(r, "\x1b[?5l");
        r.fillGrid(h);
        CHECK(!r.reverseVideo() && h.at(0, 0).bg.r == 0);
    }

    // libvterm 0.3.3 reflow reads row -1 when the top row continues a line that has already
    // scrolled out (M3 item 2: garbage cells and invalid UTF-8 after growing the window).
    for (int alt = 0; alt < 2; ++alt) {
        VtermSession w(3, 10);
        feed(w, "1234567890abcdefghij\r\nX\r\nY");  // row 0 is now "abcdefghij", a continuation
        if (alt) feed(w, "\x1b[?1049h");
        w.resize(6, 12);
        if (alt) feed(w, "\x1b[?1049l");
        Grid rg;
        w.fillGrid(rg);
        int bad = 0;
        std::string all;
        for (int r = 0; r < rg.rows; ++r) {
            for (int c = 0; c < rg.cols; ++c) {
                const std::string& t = rg.at(r, c).text;
                for (unsigned char ch : t)
                    if (ch < 0x20 || ch >= 0x80) ++bad;  // only ASCII was fed
            }
            all += w.rowText(r) + "\n";
        }
        CHECK(bad == 0);
        // The top row must come back intact: exactly "abcdefghij" on one row (it does not re-join
        // with the line that scrolled away). Without clearTopContinuation() both halves fail here.
        bool exact = false;
        for (int r = 0; r < rg.rows; ++r) exact = exact || w.rowText(r) == "abcdefghij";
        CHECK(exact);
        CHECK(all.find("X") != std::string::npos && all.find("Y") != std::string::npos);
        if (bad || !exact) std::fprintf(stderr, "alt=%d rows:\n%s", alt, all.c_str());
    }

    if (failures == 0) std::printf("vterm_test: OK\n");
    return failures ? 1 : 0;
}
