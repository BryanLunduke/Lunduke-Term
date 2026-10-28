// SPDX-License-Identifier: GPL-3.0-or-later
// A renderer-neutral character grid: filled from libvterm (or a synthetic test frame) and
// painted with Pango/Cairo by GridPainter.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lterm {

struct Color8 {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Color8& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color8& o) const { return !(*this == o); }
};

struct Cell {
    std::string text;  // UTF-8, empty = blank
    Color8 fg{208, 208, 208};
    Color8 bg{0, 0, 0};
    bool bold = false;
    bool italic = false;
    bool underline = false;
    int width = 1;     // 2 for wide characters; the following cell has width 0
};

struct Grid {
    int rows = 0, cols = 0;
    std::vector<Cell> cells;
    int cursorRow = 0, cursorCol = 0;
    bool cursorVisible = true;
    void resize(int r, int c) {
        rows = r;
        cols = c;
        cells.assign(static_cast<size_t>(r) * c, Cell{});
    }
    Cell& at(int r, int c) { return cells[static_cast<size_t>(r) * cols + c]; }
    const Cell& at(int r, int c) const { return cells[static_cast<size_t>(r) * cols + c]; }
};

}  // namespace lterm
