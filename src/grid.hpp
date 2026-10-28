// SPDX-License-Identifier: GPL-3.0-or-later
// A renderer-neutral character grid: filled from libvterm (or a synthetic test frame) and
// painted with Pango/Cairo by GridPainter.
#pragma once

#include <algorithm>
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
    bool selected = false;  // painted with fg/bg swapped
    bool operator==(const Cell& o) const {
        return width == o.width && selected == o.selected && bold == o.bold && italic == o.italic && underline == o.underline &&
               fg == o.fg && bg == o.bg && text == o.text;
    }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

struct Grid {
    int rows = 0, cols = 0;
    std::vector<Cell> cells;
    int cursorRow = 0, cursorCol = 0;
    bool cursorVisible = true;
    enum class CursorShape : uint8_t { Block, Underline, Bar };
    CursorShape cursorShape = CursorShape::Block;
    bool cursorHollow = false;  // unfocused window: outline block
    void resize(int r, int c) {
        rows = r;
        cols = c;
        cells.assign(static_cast<size_t>(r) * c, Cell{});
    }
    Cell& at(int r, int c) { return cells[static_cast<size_t>(r) * cols + c]; }
    const Cell& at(int r, int c) const { return cells[static_cast<size_t>(r) * cols + c]; }
};

// Rows that must be repainted to turn `before` into `after` (cell changes and cursor moves).
// Everything is dirty when the sizes differ.
inline std::vector<char> changedRows(const Grid& before, const Grid& after) {
    std::vector<char> dirty(static_cast<size_t>(after.rows), 0);
    if (before.rows != after.rows || before.cols != after.cols) {
        std::fill(dirty.begin(), dirty.end(), 1);
        return dirty;
    }
    for (int r = 0; r < after.rows; ++r) {
        for (int c = 0; c < after.cols; ++c) {
            if (before.at(r, c) != after.at(r, c)) {
                dirty[r] = 1;
                break;
            }
        }
    }
    const bool cursorChanged = before.cursorVisible != after.cursorVisible || before.cursorRow != after.cursorRow ||
                               before.cursorCol != after.cursorCol || before.cursorShape != after.cursorShape ||
                               before.cursorHollow != after.cursorHollow;
    if (cursorChanged) {
        if (before.cursorRow >= 0 && before.cursorRow < after.rows) dirty[before.cursorRow] = 1;
        if (after.cursorRow >= 0 && after.cursorRow < after.rows) dirty[after.cursorRow] = 1;
    }
    return dirty;
}

}  // namespace lterm
