// SPDX-License-Identifier: GPL-3.0-or-later
#include "selection.hpp"
#include "effects.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace lterm {

int LineSource::cellChars(long line, int col, uint32_t* out, int max, int* width) const {
    *width = cellWidth(line, col);
    const std::string t = cellText(line, col);
    int n = 0;
    for (size_t i = 0; i < t.size() && n < max;) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = len == 1 ? c : c & (0x7f >> len);
        for (int k = 1; k < len && i + k < t.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(t[i + k]) & 0x3f);
        out[n++] = cp;
        i += static_cast<size_t>(len);
    }
    return n;
}

bool Selection::isWord(const std::string& ch) const {
    if (ch.empty()) return false;
    unsigned char c = static_cast<unsigned char>(ch[0]);
    if (c >= 0x80) return true;  // non-ASCII letters, CJK, ...
    if (std::isalnum(c)) return true;
    return ch.size() == 1 && std::strchr(wordChars_.c_str(), ch[0]) != nullptr;
}

// 0 blank, 1 word, 2 + char for other punctuation (runs of the same symbol group together).
int Selection::classOf(const LineSource& src, long line, int col) const {
    // The right half of a wide character belongs to it (it used to count as a blank, so a
    // double-click on CJK text selected one character, or the blanks around it).
    if (col > 0 && src.cellWidth(line, col) == 0) --col;
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

// The empty last column a wide character left when it did not fit and wrapped whole: not part
// of the text (copy and search join "abcd" + "漢" without a space).
bool wideJumpGap(const LineSource& src, long line, int col, long end) {
    return col == src.cols() - 1 && line + 1 < end && src.wrapped(line) && src.cellWidth(line + 1, 0) == 2;
}

std::string Selection::text(const LineSource& src) const { return text(src, start_.line, end_.line); }

bool Selection::hasText(const LineSource& src) const {
    if (!active_) return false;
    const int cols = src.cols();
    for (long l = start_.line; l <= end_.line; ++l) {
        const bool soft = src.wrapped(l) && l != end_.line;
        if (!soft && l != end_.line) return true;  // text() adds a newline after this line
        const int c0 = l == start_.line ? start_.col : 0;
        const int c1 = l == end_.line ? end_.col : cols - 1;
        for (int c = c0; c <= c1 && c < cols; ++c) {
            uint32_t cp[8];
            int w = 1;
            const int n = src.cellChars(l, c, cp, 8, &w);
            if (w == 0) continue;
            bool blank = true;
            for (int i = 0; i < n; ++i) blank = blank && cp[i] == ' ';
            if (!blank) return true;
            // Soft-wrapped rows keep their blanks, except the gap a wrapped wide character left;
            // the last row's trailing blanks are trimmed.
            if (soft && !(n == 0 && wideJumpGap(src, l, c, src.endLine()))) return true;
        }
    }
    return false;
}

std::string Selection::text(const LineSource& src, long from, long to) const {
    std::string out;
    if (!active_) return out;
    const int cols = src.cols();
    from = std::max(from, start_.line);
    to = std::min(to, end_.line);
    for (long l = from; l <= to; ++l) {
        int c0 = l == start_.line ? start_.col : 0;
        int c1 = l == end_.line ? end_.col : cols - 1;
        std::string row;
        const bool soft = src.wrapped(l) && l != end_.line;
        for (int c = c0; c <= c1 && c < cols; ++c) {
            if (src.cellWidth(l, c) == 0) continue;
            std::string t = src.cellText(l, c);
            // A wide character that did not fit in the last column wrapped whole and left that
            // column empty; it is not a space in the text ("abcd" + "漢", not "abcd 漢").
            if (t.empty() && soft && wideJumpGap(src, l, c, src.endLine())) continue;
            row += t.empty() ? std::string(" ") : t;
        }
        if (!soft) {
            while (!row.empty() && row.back() == ' ') row.pop_back();
        }
        out += row;
        if (l != to && !soft) out += '\n';
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

namespace {
// Smallest logical inner size (inside the bezel) that holds `cells` of `cell` logical px,
// using the same margin rule as computeGridLayout.
int tightInner(int cells, int cell, int marginMin, float frac) {
    auto count = [&](int inner) {
        if (inner < 1) return 0;
        const int mx = std::max(marginMin, static_cast<int>(inner * static_cast<double>(frac)));
        if (inner <= 2 * mx) return 0;
        return (inner - 2 * mx) / std::max(1, cell);
    };
    int inner = cells * std::max(1, cell) + 2 * std::max(0, marginMin);
    if (frac > 0.0f && frac < 0.49f) {
        const int guess = static_cast<int>(std::ceil((cells * std::max(1, cell)) / (1.0 - 2.0 * frac)));
        if (guess > inner) inner = guess;
    }
    for (int n = 0; n < 100000 && count(inner) < cells; ++n) ++inner;
    while (inner > 1 && count(inner - 1) >= cells) --inner;
    return std::max(1, inner);
}
}  // namespace

WindowSize windowSizeForGrid(int cols, int rows, int cellW, int cellH, int scale, int marginMinPx, float marginFrac,
                             int frameInset) {
    const int s = std::max(1, scale);
    const int cw = std::max(1, cellW / s), ch = std::max(1, cellH / s);
    const int fi = std::max(0, frameInset);
    WindowSize out;
    out.width = tightInner(std::max(1, cols), cw, marginMinPx, marginFrac) + 2 * fi;
    out.height = tightInner(std::max(1, rows), ch, marginMinPx, marginFrac) + 2 * fi;
    return out;
}

namespace {
// Signed distance from 4:3. Positive means the window is wider than a CRT.
long aspectMiss(const WindowSize& w) { return 3L * w.width - 4L * w.height; }
}  // namespace

CellBox fitCellBox(int natCellW, int natCellH, int scale, int cols, int rows, int marginMinPx, float marginFrac,
                   int frameInset, bool crtAspect, int maxLogicalW, int maxLogicalH) {
    const int s = std::max(1, scale);
    const int c = std::max(1, cols), r = std::max(1, rows);
    int cw = std::max(1, natCellW / s);
    int ch = std::max(1, natCellH / s);
    auto windowAt = [&](int lw, int lh) {
        return windowSizeForGrid(c, r, std::max(1, lw) * s, std::max(1, lh) * s, s, marginMinPx, marginFrac,
                                 frameInset);
    };
    auto closer = [&](int a, int b, bool widthAxis) {
        const long ea = std::labs(aspectMiss(widthAxis ? windowAt(a, ch) : windowAt(cw, a)));
        const long eb = std::labs(aspectMiss(widthAxis ? windowAt(b, ch) : windowAt(cw, b)));
        return ea <= eb ? a : b;
    };
    if (crtAspect) {
        const long miss = aspectMiss(windowAt(cw, ch));
        if (miss > 0) {
            // Too wide for 4:3: stretch the cell taller until the glass matches a CRT.
            int lo = ch, hi = std::max(ch + 1, ch * 4);
            while (aspectMiss(windowAt(cw, hi)) > 0 && hi < ch * 16) hi *= 2;
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (aspectMiss(windowAt(cw, mid)) > 0) lo = mid + 1;
                else hi = mid;
            }
            ch = lo > ch ? closer(lo, lo - 1, false) : lo;
        } else if (miss < 0) {
            int lo = cw, hi = std::max(cw + 1, cw * 4);
            while (aspectMiss(windowAt(hi, ch)) < 0 && hi < cw * 16) hi *= 2;
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (aspectMiss(windowAt(mid, ch)) < 0) lo = mid + 1;
                else hi = mid;
            }
            cw = lo > cw ? closer(lo, lo - 1, true) : lo;
        }
    }
    if (maxLogicalW > 0 && maxLogicalH > 0) {
        const WindowSize full = windowAt(cw, ch);
        if (full.width > maxLogicalW || full.height > maxLogicalH) {
            int lo = 1, hi = 1000, best = 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                const int tw = std::max(1, (cw * mid) / 1000);
                const int th = std::max(1, (ch * mid) / 1000);
                const WindowSize t = windowAt(tw, th);
                if (t.width <= maxLogicalW && t.height <= maxLogicalH) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            cw = std::max(1, (cw * best) / 1000);
            ch = std::max(1, (ch * best) / 1000);
            // Integer rounding can leave the window a pixel over the work area, or a little
            // off 4:3. Nudge while staying inside the work area.
            for (int n = 0; n < 64; ++n) {
                WindowSize t = windowAt(cw, ch);
                if (t.width > maxLogicalW || t.height > maxLogicalH) {
                    if (cw > 1 && (t.width > maxLogicalW || ch == 1)) --cw;
                    if (ch > 1 && (t.height > maxLogicalH || cw == 1)) --ch;
                    continue;
                }
                if (!crtAspect) break;
                const long e = aspectMiss(t);
                const long tol = std::max(2L, std::max<long>(t.width, t.height) / 80);
                if (std::labs(e) <= tol) break;
                if (e > 0 && ch + 1 < 4096 && windowAt(cw, ch + 1).height <= maxLogicalH) ++ch;
                else if (e < 0 && cw + 1 < 4096 && windowAt(cw + 1, ch).width <= maxLogicalW) ++cw;
                else break;
            }
        }
    }
    CellBox box;
    box.cellW = std::max(1, cw) * s;
    box.cellH = std::max(1, ch) * s;
    return box;
}

}  // namespace lterm
