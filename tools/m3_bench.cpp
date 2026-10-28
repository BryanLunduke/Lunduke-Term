// SPDX-License-Identifier: GPL-3.0-or-later
// lterm-m3-bench: M3 item 1 measurements.
//
//   lterm-m3-bench --search [--lines 10000,100000]   per-keystroke search time, typing "needle"
#include "search.hpp"
#include "vterm_session.hpp"

#include <glib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace lterm;

static double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A build-log-like scrollback: `lines` lines, "needle" on every 1000th.
static void fill(VtermSession& s, long lines) {
    s.setScrollbackLimit(static_cast<size_t>(lines));
    std::string chunk;
    for (long i = 0; i < lines; ++i) {
        chunk += "[" + std::to_string(i) + "] g++ -O2 -c src/module_" + std::to_string(i % 97) +
                 ".cpp -o build/obj" + (i % 1000 == 500 ? " Needle found" : "") + "\r\n";
        if (chunk.size() > 65536) {
            s.feed(chunk);
            chunk.clear();
        }
    }
    s.feed(chunk);
}

static void benchSearch(long lines) {
    VtermSession s(24, 80);
    fill(s, lines);
    const std::string word = "needle";
    std::printf("scrollback %ld lines x 80 cols (firstLine %ld, endLine %ld)\n", lines, s.firstLine(), s.endLine());
    double total = 0, worst = 0;
    for (size_t k = 1; k <= word.size(); ++k) {
        const std::string q = word.substr(0, k);
        const auto t0 = std::chrono::steady_clock::now();
        const auto m = findAll(s, q, false);
        const double ms = msSince(t0);
        total += ms;
        worst = std::max(worst, ms);
        std::printf("  findAll (M2, per keystroke) '%s': %zu matches, %.2f ms\n", q.c_str(), m.size(), ms);
    }
    std::printf("  findAll: total %.1f ms, worst keystroke %.2f ms\n", total, worst);

    // M3: SearchIndex. Cold: the cache is built in 4 ms slices (as the view's idle worker does;
    // each slice is one main-loop turn), then each keystroke is one find() on the complete cache.
    SearchIndex idx;
    int slices = 0;
    double worstSlice = 0, build = 0;
    for (;;) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool done = idx.update(s, false, g_get_monotonic_time() + 4000);
        const double ms = msSince(t0);
        ++slices;
        build += ms;
        worstSlice = std::max(worstSlice, ms);
        if (done) break;
    }
    std::printf("  SearchIndex build: %d slices, %.1f ms total, worst slice %.2f ms, %zu lines, %.1f MB\n", slices,
                build, worstSlice, idx.cachedLines(), idx.memoryBytes() / 1048576.0);
    total = worst = 0;
    for (size_t k = 1; k <= word.size(); ++k) {
        const std::string q = word.substr(0, k);
        const auto t0 = std::chrono::steady_clock::now();
        idx.update(s, false);  // nothing new: returns at once
        const auto m = idx.find(s, q);
        const double ms = msSince(t0);
        total += ms;
        worst = std::max(worst, ms);
        std::printf("  SearchIndex (M3, per keystroke) '%s': %zu matches, %.2f ms\n", q.c_str(), m.size(), ms);
        if (m != findAll(s, q, false)) std::printf("  MISMATCH against findAll for '%s'\n", q.c_str());
    }
    std::printf("  SearchIndex: total %.1f ms, worst keystroke %.2f ms\n", total, worst);
    // A frequent letter hits the result cap (10000, the newest kept).
    for (const char* q : {"o", "g++"}) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto m = idx.find(s, q);
        std::printf("  SearchIndex '%s': %zu matches (cap 10000), %.2f ms\n", q, m.size(), msSince(t0));
        const auto t1 = std::chrono::steady_clock::now();
        const auto r = findAll(s, q, false);
        std::printf("  findAll     '%s': %zu matches (cap 10000), %.2f ms\n", q, r.size(), msSince(t1));
    }
    // 100 new lines arrive, then a keystroke: only the new lines are read.
    for (int i = 0; i < 100; ++i) s.feed("new output line " + std::to_string(i) + " needle\r\n");
    const auto t0 = std::chrono::steady_clock::now();
    idx.update(s, false);
    const auto m = idx.find(s, "needle");
    std::printf("  SearchIndex after 100 new lines: update + find %.2f ms, %zu matches\n", msSince(t0), m.size());
}

int main(int argc, char** argv) {
    std::vector<long> lines{10000, 100000};
    bool search = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--search")) search = true;
        else if (!std::strcmp(argv[i], "--lines") && i + 1 < argc) {
            lines.clear();
            std::stringstream ss(argv[++i]);
            std::string t;
            while (std::getline(ss, t, ',')) lines.push_back(std::atol(t.c_str()));
        }
    }
    if (!search) {
        std::fprintf(stderr, "usage: lterm-m3-bench --search [--lines N,N]\n");
        return 2;
    }
    for (long n : lines) benchSearch(n);
    return 0;
}
