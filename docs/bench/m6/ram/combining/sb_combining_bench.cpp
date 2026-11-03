// SPDX-License-Identifier: GPL-3.0-or-later
// M6 item 6 follow-up: scrolled-back redraw cost with combining marks. Public VtermSession API
// only, so it builds against any version (link with that build's liblterm-vterm.a/liblterm-effects.a).
// 50x200 window, 1000 history lines; fillGrid() of a fully scrolled-back screen, median of 15
// batches of 200 calls. Variants: every cell "e" + U+0301, and plain ASCII.
#include "vterm_session.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
using namespace lterm;
static double run(const std::string& cell) {
    const int rows = 50, cols = 200;
    VtermSession s(rows, cols);
    std::string line;
    for (int c = 0; c < cols; ++c) line += cell;
    for (int i = 0; i < 1000; ++i) s.feed(line + "\r\n");
    Grid g;
    s.fillGrid(g, 500);
    std::vector<double> ms;
    for (int b = 0; b < 15; ++b) {
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 200; ++k) s.fillGrid(g, 500 + (k & 1));
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 200);
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}
int main() {
    const double comb = run("e\xcc\x81"), ascii = run("x");
    std::printf("fillGrid 50x200 scrolled back: combining-every-cell %.3f ms, ascii %.3f ms (median of 15x200)\n", comb, ascii);
}
