// SPDX-License-Identifier: GPL-3.0-or-later
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

    if (failures == 0) std::printf("vterm_test: OK\n");
    return failures ? 1 : 0;
}
