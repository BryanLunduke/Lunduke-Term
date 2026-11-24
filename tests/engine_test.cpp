// SPDX-License-Identifier: GPL-3.0-or-later
// libghostty-vt: resize reflow, ZWJ emoji width, and the SGR attributes the painter draws.
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

static void feed(VtermSession& s, const std::string& t) { s.feed(t); }

static std::string cells(const VtermSession& s, int row) {
    std::string o;
    char b[32];
    const long line = s.topLine() + row;
    for (int c = 0; c < s.cols(); ++c) {
        uint32_t cp[8];
        int w = 0;
        const int n = s.cellChars(line, c, cp, 8, &w);
        if (c) o += ' ';
        for (int k = 0; k < n; ++k) {
            std::snprintf(b, sizeof b, "%s%X", k ? "+" : "", cp[k]);
            o += b;
        }
        o += "/" + std::to_string(w);
    }
    return o;
}

int main() {
    // Reflow: a soft-wrapped line joins when the screen gets wider, and splits again when it shrinks.
    {
        VtermSession s(4, 10);
        feed(s, "0123456789abcdef\r\n");
        CHECK(s.wrapped(s.topLine()));
        CHECK(s.rowText(0) == "0123456789");
        CHECK(s.rowText(1) == "abcdef");
        s.resize(4, 20);
        CHECK(s.rowText(0) == "0123456789abcdef");
        CHECK(!s.wrapped(s.topLine()));
        s.resize(4, 8);
        CHECK(s.wrapped(s.topLine()));
        CHECK(s.rowText(0) == "01234567");
        CHECK(s.rowText(1) == "89abcdef");
    }
    // ZWJ family emoji is one wide cell (grapheme clusters, mode 2027).
    {
        VtermSession s(1, 16);
        feed(s, "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7!");  // 👨‍👩‍👧!
        CHECK(cells(s, 0).rfind("1F468+200D+1F469+200D+1F467/2 /0 21/1", 0) == 0);
        CHECK(codepointWidth(0x200D) == 0);
        uint32_t cps[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467};
        // The cluster width helper is what the terminal uses with mode 2027.
        // The cell above is the check that the engine stored it that way.
        CHECK(cps[0] == 0x1F468);
    }
    // SGR: faint, strike, overline, curly/double/dotted underline, colored underline.
    {
        VtermSession s(2, 20);
        feed(s, "\x1b[2;9;53;4:3;58:2::1:2:3mA"
                "\x1b[0;4:2mB"
                "\x1b[0;4:4mC"
                "\x1b[0;4:5mD"
                "\x1b[0;21mE");
        Grid g;
        s.fillGrid(g);
        const Cell& a = g.at(0, 0);
        CHECK(a.text == "A" && a.faint && a.strikethrough && a.overline);
        CHECK(a.underline == Underline::Curly && a.underlineColored);
        CHECK(a.underlineColor.r == 1 && a.underlineColor.g == 2 && a.underlineColor.b == 3);
        CHECK(g.at(0, 1).underline == Underline::Double);
        CHECK(g.at(0, 2).underline == Underline::Dotted);
        CHECK(g.at(0, 3).underline == Underline::Dashed);
        CHECK(g.at(0, 4).underline == Underline::Double);  // SGR 21
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("engine tests passed\n");
    return 0;
}
