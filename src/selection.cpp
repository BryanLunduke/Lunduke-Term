// SPDX-License-Identifier: GPL-3.0-or-later
#include "selection.hpp"
#include "effects.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace lterm {

bool Selection::isWord(const std::string& ch) const {
    if (ch.empty()) return false;
    unsigned char c = static_cast<unsigned char>(ch[0]);
    if (c >= 0x80) return true;  // non-ASCII letters, CJK, ...
    if (std::isalnum(c)) return true;
    return ch.size() == 1 && std::strchr(wordChars_.c_str(), ch[0]) != nullptr;
}

// 0 blank, 1 word, 2 + char for other punctuation (runs of the same symbol group together).
int Selection::classOf(const LineSource& src, long line, int col) const {
    std::string t = src.cellText(line, col);
    if (t.empty() || t == " ") return 0;
    if (isWord(t)) return 1;
    return 2 + static_cast<unsigned char>(t[0]);
}

void Selection::begin(const LineSource& src, SelPos p, SelectMode mode) {
    anchor_ = head_ = p;
    mode_ = mode;
    active_ = mode != SelectMode::Char;
    update(src);
}

void Selection::extend(const LineSource& src, SelPos p) {
    head_ = p;
    if (mode_ == SelectMode::Char && !(head_ == anchor_)) active_ = true;
    update(src);
}

void Selection::update(const LineSource& src) {
    SelPos a = anchor_, b = head_;
    if (b < a) std::swap(a, b);
    const int cols = src.cols();
    if (mode_ == SelectMode::Line) {
        // Whole logical lines: follow soft wraps in both directions.
        while (a.line > src.firstLine() && src.wrapped(a.line - 1)) --a.line;
        while (b.line + 1 < src.endLine() && src.wrapped(b.line)) ++b.line;
        a.col = 0;
        b.col = cols - 1;
    } else if (mode_ == SelectMode::Word) {
        int ca = classOf(src, a.line, a.col);
        while (a.col > 0 && classOf(src, a.line, a.col - 1) == ca) --a.col;
        int cb = classOf(src, b.line, b.col);
        while (b.col + 1 < cols && classOf(src, b.line, b.col + 1) == cb) ++b.col;
    }
    // Never split a wide character.
    if (a.col > 0 && src.cellWidth(a.line, a.col) == 0) --a.col;
    if (b.col + 1 < cols && src.cellWidth(b.line, b.col) == 2) ++b.col;
    start_ = a;
    end_ = b;
}

bool Selection::contains(long line, int col) const {
    if (!active_) return false;
    SelPos p{line, col};
    return !(p < start_) && !(end_ < p);
}

std::string Selection::text(const LineSource& src) const {
    std::string out;
    if (!active_) return out;
    const int cols = src.cols();
    for (long l = start_.line; l <= end_.line; ++l) {
        int c0 = l == start_.line ? start_.col : 0;
        int c1 = l == end_.line ? end_.col : cols - 1;
        std::string row;
        for (int c = c0; c <= c1 && c < cols; ++c) {
            if (src.cellWidth(l, c) == 0) continue;
            std::string t = src.cellText(l, c);
            row += t.empty() ? std::string(" ") : t;
        }
        const bool soft = src.wrapped(l) && l != end_.line;
        if (!soft) {
            while (!row.empty() && row.back() == ' ') row.pop_back();
        }
        out += row;
        if (l != end_.line && !soft) out += '\n';
    }
    return out;
}

bool pointToCell(double x, double y, int width, int height, float curvature, int marginX, int marginY, int cellW,
                 int cellH, int rows, int cols, int& row, int& col, const FrameGeometry& frame) {
    bool inside = true;
    if ((curvature > 0.0f || frame.inset > 0.0f) && width > 0 && height > 0) {
        float su, sv, lu, lv;
        screenCoords(static_cast<float>(x / width), static_cast<float>(y / height), frame, curvature, su, sv, lu, lv);
        if (lu < 0 || lu > 1 || lv < 0 || lv > 1) inside = false;
        if (frame.inset > 0.0f && screenSdf(frame, lu, lv) > 0.0f) inside = false;
        x = su * width;
        y = sv * height;
    }
    int c = static_cast<int>(std::floor((x - marginX) / std::max(1, cellW)));
    int r = static_cast<int>(std::floor((y - marginY) / std::max(1, cellH)));
    if (r < 0 || c < 0 || r >= rows || c >= cols) inside = false;
    row = std::clamp(r, 0, std::max(0, rows - 1));
    col = std::clamp(c, 0, std::max(0, cols - 1));
    return inside;
}

GridLayout computeGridLayout(int devW, int devH, int scale, int cellW, int cellH, int marginMinPx, float marginFrac,
                             int frameInset) {
    const int s = std::max(1, scale);
    const int fi = std::max(0, frameInset);
    const int w = std::max(1, devW / s - 2 * fi), h = std::max(1, devH / s - 2 * fi);
    const int cw = std::max(1, cellW / s), ch = std::max(1, cellH / s);
    GridLayout g;
    int mx = std::max(marginMinPx, static_cast<int>(w * marginFrac));
    int my = std::max(marginMinPx, static_cast<int>(h * marginFrac));
    g.cols = std::max(1, (w - 2 * mx) / cw);
    g.rows = std::max(1, (h - 2 * my) / ch);
    if (marginFrac > 0) {  // centre the grid inside the tube
        mx = (w - g.cols * cw) / 2;
        my = (h - g.rows * ch) / 2;
    }
    g.marginX = (mx + fi) * s;
    g.marginY = (my + fi) * s;
    return g;
}

}  // namespace lterm
