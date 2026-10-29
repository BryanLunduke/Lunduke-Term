// SPDX-License-Identifier: GPL-3.0-or-later
// Scrollback search (M2 stage 3): matches in history and on screen, case folding, soft-wrapped
// lines, wide characters, next/previous with wrap-around, history that scrolled away.
#include "sanitizer.hpp"
#include "search.hpp"
#include "vterm_session.hpp"

#include <glib.h>

#include <algorithm>
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
        CHECK(ms < (LTERM_ASAN ? 6000.0 : 2000.0));  // ASan+UBSan: 1.5-2.0 s measured in M4
    }
    // ---- M3 item 1(a): SearchIndex (cached, sliced) gives the same results as findAll ----
    auto sameAs = [](const LineSource& src, SearchIndex& idx, const std::string& q, bool cs) {
        idx.update(src, cs);
        const auto a = idx.find(src, q);
        const auto b = findAll(src, q, cs);
        if (a != b) std::fprintf(stderr, "  differs for '%s' (cs %d): index %zu, findAll %zu\n", q.c_str(), cs, a.size(), b.size());
        return a == b;
    };
    {
        // Mixed content: wide characters, folding that changes length, soft-wrapped lines, a
        // line that wraps from the scrollback onto the screen.
        VtermSession s(6, 12);
        s.setScrollbackLimit(500);
        unsigned seed = 12345;
        auto rnd = [&seed](unsigned n) {
            seed = seed * 1103515245u + 12345u;
            return (seed >> 16) % n;
        };
        const char* words[] = {"needle", "NEEDLE", "Ne", "\xe6\xbc\xa2\xe5\xad\x97", "\xc3\x84rger", "stra\xc3\x9f" "e",
                               "x", "  ", "haystack", "\xef\xac\x81" "le"};  // last: U+FB01 "fi" ligature
        for (int i = 0; i < 400; ++i) {
            std::string line;
            const unsigned n = 1 + rnd(5);
            for (unsigned k = 0; k < n; ++k) line += std::string(words[rnd(10)]) + (rnd(3) ? " " : "");
            s.feed(line + (rnd(4) ? "\r\n" : ""));  // some lines run on into the next
        }
        s.feed("tail needle that wraps onto the screen needle");
        SearchIndex idx;
        for (const char* q : {"needle", "ne", "e n", "\xe6\xbc\xa2", "\xc3\xa4rger", "STRASSE", "strasse", "fi", "file",
                              "x x", "  ", "haystackneedle", "absent"}) {
            CHECK(sameAs(s, idx, q, false));
            CHECK(sameAs(s, idx, q, true));
        }
        CHECK(idx.find(s, "").empty());
        CHECK(idx.find(s, "a\nb").empty());
        CHECK(idx.find(s, "\xff\xfe").empty());
        // New output: only the new lines are read, results still match.
        idx.update(s, false);
        const size_t before = idx.cachedLines();
        for (int i = 0; i < 50; ++i) s.feed(" more needle " + std::to_string(i) + "\r\n");
        CHECK(sameAs(s, idx, "needle", false));
        CHECK(idx.cachedLines() > before);
        // History beyond the limit: dropped from the cache too.
        for (int i = 0; i < 600; ++i) s.feed("filler " + std::to_string(i) + "\r\n");
        CHECK(sameAs(s, idx, "needle", false));
        CHECK(idx.find(s, "haystack").empty());
        CHECK(idx.cachedLines() <= 500 + 6);
        // Erase the scrollback (CSI 3 J) and a resize (reflow) start the cache over.
        s.feed("needle A\r\n");
        CHECK(sameAs(s, idx, "needle", false));
        s.feed("\x1b[3J");
        CHECK(sameAs(s, idx, "needle", false));
        for (int i = 0; i < 30; ++i) s.feed("after clear needle " + std::to_string(i) + "\r\n");
        CHECK(sameAs(s, idx, "needle", false));
        s.resize(5, 7);
        CHECK(sameAs(s, idx, "needle", false));
        CHECK(sameAs(s, idx, "clear ne", false));  // now wrapped across rows
        s.resize(8, 30);
        CHECK(sameAs(s, idx, "clear ne", false));
    }
    {
        // Sliced build: a tiny deadline needs several calls, the result is the same; the cap keeps
        // the newest matches (where Enter / Shift+Enter start), findAll kept the oldest.
        VtermSession s(24, 80);
        s.setScrollbackLimit(30000);
        std::string chunk;
        for (int i = 0; i < 30000; ++i) chunk += "log " + std::to_string(i) + ": nothing to see here\r\n";
        s.feed(chunk);
        SearchIndex idx;
        int calls = 0;
        while (!idx.update(s, false, g_get_monotonic_time() + 500)) ++calls;
        std::printf("sliced build of 30000 lines: %d extra calls\n", calls);
        CHECK(calls > 1);
        CHECK(idx.complete(s, false));
        CHECK(!idx.complete(s, true));  // another case mode is another cache
        const auto all = findAll(s, "log", false, 100000);
        const auto m = idx.find(s, "log", 100);
        CHECK(m.size() == 100 && all.size() == 30000);
        if (m.size() == 100 && all.size() == 30000) {
            CHECK(m.back() == all.back());       // the newest "log" line (on the screen)
            CHECK(m.front() == all[all.size() - 100]);
            for (size_t i = 1; i < m.size(); ++i) CHECK(m[i - 1].start < m[i].start);
        }
        const auto mine = idx.find(s, "log", 100000);
        CHECK(all == mine);
        CHECK(idx.find(s, "29999:").size() == 1);
    }
    {
        // Per-keystroke cost, 100 000 lines (printed; the bound is generous for ASan builds).
        VtermSession s(24, 80);
        s.setScrollbackLimit(100000);
        std::string chunk;
        for (int i = 0; i < 100000; ++i) {
            chunk += "[" + std::to_string(i) + "] g++ -O2 -c src/m" + std::to_string(i % 97) + ".cpp" +
                     (i % 1000 == 500 ? " Needle" : "") + "\r\n";
            if (chunk.size() > 65536) {
                s.feed(chunk);
                chunk.clear();
            }
        }
        s.feed(chunk);
        SearchIndex idx;
        double worstSlice = 0;
        int slices = 0;
        for (;;) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool done = idx.update(s, false, g_get_monotonic_time() + 4000);
            worstSlice = std::max(worstSlice, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            ++slices;
            if (done) break;
        }
        double worstKey = 0;
        size_t n = 0;
        for (const char* q : {"n", "ne", "nee", "need", "needl", "needle"}) {
            const auto t0 = std::chrono::steady_clock::now();
            idx.update(s, false);
            n = idx.find(s, q).size();
            worstKey = std::max(worstKey, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("100000 lines: build %d slices (worst %.2f ms), worst keystroke %.2f ms, %zu matches\n", slices,
                    worstSlice, worstKey, n);
        CHECK(n == 100);
        // A slice is 4 ms nominal; the bound is for a loaded box. ASan+UBSan is 3-10x slower
        // (M3: 75.8 ms once), so it gets its own bound.
        CHECK(worstSlice < (LTERM_ASAN ? 250.0 : 60.0));
        CHECK(worstKey < 200.0);
    }
    if (failures == 0) std::printf("search_test: OK\n");
    return failures ? 1 : 0;
}
