// SPDX-License-Identifier: GPL-3.0-or-later
// Scrollback search (M2 stage 3): matches in history and on screen, case folding, soft-wrapped
// lines, wide characters, next/previous with wrap-around, history that scrolled away.
#include "search.hpp"
#include "vterm_session.hpp"

#include <chrono>
#include <cstdio>
#include <string>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

int main() {
    {
        VtermSession s(5, 20);
        s.setScrollbackLimit(1000);
        for (int i = 0; i < 30; ++i) s.feed("line " + std::to_string(i) + (i % 10 == 3 ? " Needle" : "") + "\r\n");
        s.feed("needle on screen");
        const auto m = findAll(s, "needle", false);
        CHECK(m.size() == 4);  // lines 3, 13, 23 in history + the screen
        if (m.size() == 4) {
            CHECK(m[0].start.line < s.topLine());       // in history
            CHECK(m[3].start.line >= s.topLine());      // on screen
            CHECK(m[3].start.col == 0 && m[3].end.col == 5);
            CHECK(m[0].start.col == 7 && m[0].end.col == 12);  // "line 3 Needle"
            CHECK(s.rowText(m[3].start.line - s.topLine()).find("needle") == 0);
            for (size_t i = 1; i < m.size(); ++i) CHECK(m[i - 1].start < m[i].start);
        }
        CHECK(findAll(s, "needle", true).size() == 1);   // case-sensitive: only the screen one
        CHECK(findAll(s, "NEEDLE", false).size() == 4);
        CHECK(findAll(s, "", false).empty());
        CHECK(findAll(s, "absent", false).empty());
        CHECK(findAll(s, "line", false, 7).size() == 7);  // maxMatches
        // stepMatch: forward and backward with wrap-around.
        if (m.size() == 4) {
            CHECK(stepMatch(m, SelPos{s.endLine(), 0}, true) == 3);
            CHECK(stepMatch(m, m[3].start, true) == 2);
            CHECK(stepMatch(m, m[0].start, true) == 3);  // wraps to the newest
            CHECK(stepMatch(m, m[0].start, false) == 1);
            CHECK(stepMatch(m, m[3].start, false) == 0);  // wraps to the oldest
        }
        CHECK(stepMatch({}, SelPos{0, 0}, false) == -1);
    }
    {
        // A soft-wrapped line is searched as one: "wrapme" split over two rows of 10 columns.
        VtermSession s(4, 10);
        s.feed("abcdwrapmexyz\r\n");  // "abcdwrapme" | "xyz"
        s.feed("123456789wr");        // "123456789w" | "r"  -> "wr" across the wrap
        auto m = findAll(s, "mexy", false);
        CHECK(m.size() == 1);
        if (m.size() == 1) {
            CHECK(m[0].start.col == 8 && m[0].end.col == 1 && m[0].end.line == m[0].start.line + 1);
            CHECK(m[0].contains(m[0].start.line, 9) && m[0].contains(m[0].end.line, 0));
            CHECK(!m[0].contains(m[0].end.line, 2));
        }
        CHECK(findAll(s, "9wr", false).size() == 1);
        // Hard line break: no match across it.
        CHECK(findAll(s, "xyz1", false).empty());
    }
    {
        // Wide characters and non-ASCII case folding.
        VtermSession s(3, 20);
        s.feed("a\xe6\xbc\xa2\xe5\xad\x97z \xc3\x84rger");  // a漢字z Ärger
        auto m = findAll(s, "\xe6\xbc\xa2\xe5\xad\x97", true);
        CHECK(m.size() == 1);
        if (m.size() == 1) CHECK(m[0].start.col == 1 && m[0].end.col == 4);  // two wide cells
        CHECK(findAll(s, "\xc3\xa4rger", false).size() == 1);  // ärger matches Ärger
        CHECK(findAll(s, "\xc3\xa4rger", true).empty());
        CHECK(findAll(s, "\xff\xfe", false).empty());  // invalid UTF-8 query
    }
    {
        // History beyond the limit is gone and is not found; a full 10000-line search is fast.
        VtermSession s(24, 80);
        s.setScrollbackLimit(10000);
        s.feed("OLDEST\r\n");
        for (int i = 0; i < 10100; ++i) s.feed("filler line " + std::to_string(i) + " with some text to search through\r\n");
        CHECK(findAll(s, "oldest", false).empty());
        const auto t0 = std::chrono::steady_clock::now();
        const auto m = findAll(s, "filler", false);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("search 10000+24 lines x 80: %zu matches in %.1f ms\n", m.size(), ms);
        CHECK(m.size() >= 10000);
        CHECK(ms < 2000.0);  // generous: ASan/UBSan debug builds run this too
    }
    if (failures == 0) std::printf("search_test: OK\n");
    return failures ? 1 : 0;
}
