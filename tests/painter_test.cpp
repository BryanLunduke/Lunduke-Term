// SPDX-License-Identifier: GPL-3.0-or-later
// Glyph cache renderer vs the M0 direct Pango renderer, and changed-rows-only repaint.
#include "grid_painter.hpp"
#include "synthetic_frame.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace lterm;

namespace {
std::vector<uint8_t> pixels(cairo_surface_t* s) {
    cairo_surface_flush(s);
    const int h = cairo_image_surface_get_height(s), stride = cairo_image_surface_get_stride(s);
    const uint8_t* d = cairo_image_surface_get_data(s);
    return std::vector<uint8_t>(d, d + static_cast<size_t>(h) * stride);
}
void stats(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int* maxd, double* mean) {
    long sum = 0;
    *maxd = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if ((i & 3) == 3) continue;
        const int d = std::abs(int(a[i]) - int(b[i]));
        sum += d;
        if (d > *maxd) *maxd = d;
    }
    *mean = double(sum) / (a.size() * 3 / 4);
}
}  // namespace

int main() {
    const int W = 800, H = 480;
    GridPainter painter("DejaVu Sans Mono", 14);
    Grid g;
    g.resize((H - 2 * kMargin) / painter.cellHeight(), (W - 2 * kMargin) / painter.cellWidth());
    fillSyntheticGrid(g, 0);
    const Color8 cur{200, 200, 200}, bg{0, 0, 0};

    cairo_surface_t* ref = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t* cr = cairo_create(ref);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    painter.paint(cr, g, kMargin, kMargin, cur);
    cairo_destroy(cr);

    cairo_surface_t* fast = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cr = cairo_create(fast);
    painter.paintRows(cr, g, kMargin, kMargin, cur, bg, nullptr, W);
    int mx;
    double mean;
    stats(pixels(ref), pixels(fast), &mx, &mean);
    std::printf("glyph cache vs direct Pango: mean abs diff %.4f, max diff %d, %zu glyphs cached\n", mean, mx,
                painter.cachedGlyphs());
    int fails = 0;
    if (mean > 0.5) {
        std::fprintf(stderr, "FAIL: glyph cache differs too much from the reference\n");
        ++fails;
    }

    // Change one row; a dirty-rows repaint must equal a full repaint of the new grid.
    Grid g2 = g;
    const char* msg = "changed row";
    for (int c = 0; c < (int)std::strlen(msg); ++c) g2.at(5, c).text = std::string(1, msg[c]);
    const auto dirty = changedRows(g, g2);
    int ndirty = 0;
    for (char d : dirty) ndirty += d ? 1 : 0;
    if (ndirty != 1 || !dirty[5]) {
        std::fprintf(stderr, "FAIL: changedRows found %d rows\n", ndirty);
        ++fails;
    }
    painter.paintRows(cr, g2, kMargin, kMargin, cur, bg, &dirty, W);
    cairo_destroy(cr);
    cairo_surface_t* full = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cr = cairo_create(full);
    painter.paintRows(cr, g2, kMargin, kMargin, cur, bg, nullptr, W);
    cairo_destroy(cr);
    stats(pixels(fast), pixels(full), &mx, &mean);
    std::printf("dirty-row repaint vs full repaint: max diff %d\n", mx);
    if (mx != 0) {
        std::fprintf(stderr, "FAIL: partial repaint differs from full repaint\n");
        ++fails;
    }
    // Cursor shapes on a blank cell: sample the cell's top-left, top-right and bottom-right pixels.
    {
        Grid cg;
        cg.resize(3, 6);
        cg.cursorRow = 1;
        cg.cursorCol = 2;
        const int cw = painter.cellWidth(), ch = painter.cellHeight();
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 6 * cw, 3 * ch);
        auto px = [&](int x, int y) {
            cairo_surface_flush(s);
            const uint8_t* d = cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s) + x * 4;
            return d[1] > 100;  // green channel of the cursor colour
        };
        struct Case { Grid::CursorShape shape; bool hollow; bool tl, tr, br, mid; const char* name; } cases[] = {
            {Grid::CursorShape::Block, false, true, true, true, true, "block"},
            {Grid::CursorShape::Underline, false, false, false, true, false, "underline"},
            {Grid::CursorShape::Bar, false, true, false, false, false, "bar"},
            {Grid::CursorShape::Block, true, true, true, true, false, "hollow"},
        };
        for (const Case& k : cases) {
            cg.cursorShape = k.shape;
            cg.cursorHollow = k.hollow;
            cairo_t* c = cairo_create(s);
            painter.paintRows(c, cg, 0, 0, cur, bg, nullptr, 6 * cw);
            cairo_destroy(c);
            const int x0 = 2 * cw, y0 = ch;
            const bool tl = px(x0, y0), tr = px(x0 + cw - 1, y0), br = px(x0 + cw - 1, y0 + ch - 1),
                       mid = px(x0 + cw / 2, y0 + ch / 2);
            if (tl != k.tl || tr != k.tr || br != k.br || mid != k.mid) {
                std::fprintf(stderr, "FAIL: cursor %s: tl=%d tr=%d br=%d mid=%d\n", k.name, tl, tr, br, mid);
                ++fails;
            }
        }
        std::printf("cursor shapes block/underline/bar/hollow: ok\n");
        cairo_surface_destroy(s);
    }
    // M2 HiDPI: a scale-2 painter has exactly twice the cell size, and its picture is the
    // scale-1 picture at twice the resolution (downsampled 2x2 it matches closely).
    {
        GridPainter p2("DejaVu Sans Mono", 14, 2);
        std::printf("scale 1 cell %dx%d, scale 2 cell %dx%d\n", painter.cellWidth(), painter.cellHeight(),
                    p2.cellWidth(), p2.cellHeight());
        if (p2.cellWidth() != 2 * painter.cellWidth() || p2.cellHeight() != 2 * painter.cellHeight() ||
            p2.scale() != 2) {
            std::fprintf(stderr, "FAIL: scale-2 cell size is not twice the scale-1 size\n");
            ++fails;
        }
        cairo_surface_t* s2 = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2 * W, 2 * H);
        cairo_t* c2 = cairo_create(s2);
        p2.paintRows(c2, g, 2 * kMargin, 2 * kMargin, cur, bg, nullptr, 2 * W);
        cairo_destroy(c2);
        // 2x2 box average vs the scale-1 frame; text positions must agree, glyph shapes differ a bit
        // (hinting at 28 px vs 14 px).
        cairo_surface_flush(s2);
        const uint8_t* d2 = cairo_image_surface_get_data(s2);
        const int st2 = cairo_image_surface_get_stride(s2);
        const auto f1 = pixels(fast);
        const int st1 = cairo_image_surface_get_stride(fast);
        long sum = 0, n = 0, ink1 = 0, ink2 = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) {
                    int v = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) v += d2[(2 * y + dy) * st2 + (2 * x + dx) * 4 + c];
                    v /= 4;
                    const int a = f1[static_cast<size_t>(y) * st1 + x * 4 + c];
                    sum += std::abs(v - a);
                    ink1 += a;
                    ink2 += v;
                    ++n;
                }
        const double meanAbs = double(sum) / n, inkRatio = double(ink2) / std::max(1L, ink1);
        std::printf("scale 2 downsampled vs scale 1: mean abs diff %.2f, ink ratio %.3f\n", meanAbs, inkRatio);
        if (meanAbs > 6.0 || inkRatio < 0.85 || inkRatio > 1.15) {
            std::fprintf(stderr, "FAIL: scale-2 picture does not match the scale-1 layout\n");
            ++fails;
        }
        // Cursor shapes keep their logical thickness: the bar is 2*scale device px at least.
        Grid cg;
        cg.resize(1, 3);
        cg.cursorRow = 0;
        cg.cursorCol = 1;
        cg.cursorShape = Grid::CursorShape::Bar;
        const int cw = p2.cellWidth(), ch = p2.cellHeight();
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 3 * cw, ch);
        cairo_t* c = cairo_create(s);
        p2.paintRows(c, cg, 0, 0, cur, bg, nullptr, 3 * cw);
        cairo_destroy(c);
        cairo_surface_flush(s);
        int lit = 0;
        for (int x = cw; x < 2 * cw; ++x) lit += cairo_image_surface_get_data(s)[(ch / 2) * cairo_image_surface_get_stride(s) + x * 4 + 1] > 100;
        std::printf("scale 2 bar cursor: %d px wide\n", lit);
        if (lit < 4) {
            std::fprintf(stderr, "FAIL: scale-2 bar cursor is %d px wide\n", lit);
            ++fails;
        }
        cairo_surface_destroy(s);
        cairo_surface_destroy(s2);
    }
    // M4 item 5: wide glyphs span their 2 cells in both renderers (direct Pango and the glyph
    // cache the view uses), combining marks stay in their cell, and the two agree. The GL and CPU
    // effect back-ends both start from this painted frame (gl_match_test compares them).
    {
        GridPainter wp("DejaVu Sans Mono", 16);
        const int cw = wp.cellWidth(), ch = wp.cellHeight(), cols = 12;
        Grid wg;
        wg.resize(1, cols);
        struct C { const char* t; int w; };
        const C row[cols] = {{"\xe6\xbc\xa2", 2}, {"", 0}, {"\xe5\xad\x97", 2}, {"", 0}, {"x", 1},
                             {"e\xcc\x81", 1}, {"\xea\xb8\x80", 2}, {"", 0}, {"\xf0\x9f\x98\x80", 2}, {"", 0},
                             {"a\xcc\x88\xcc\xa3", 1}, {"", 1}};  // 漢 字 x é 글 😀 ạ̈
        for (int c = 0; c < cols; ++c) {
            wg.at(0, c).text = row[c].t;
            wg.at(0, c).width = row[c].w;
        }
        wg.cursorVisible = false;
        auto render = [&](bool cached) {
            cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, cols * cw, ch);
            cairo_t* c = cairo_create(s);
            cairo_set_source_rgb(c, 0, 0, 0);
            cairo_paint(c);
            if (cached) wp.paintRows(c, wg, 0, 0, cur, bg, nullptr, cols * cw);
            else wp.paint(c, wg, 0, 0, cur);
            cairo_destroy(c);
            return s;
        };
        auto ink = [&](cairo_surface_t* s, int x0, int x1) {
            cairo_surface_flush(s);
            const uint8_t* d = cairo_image_surface_get_data(s);
            const int st = cairo_image_surface_get_stride(s);
            int n = 0;
            for (int y = 0; y < ch; ++y)
                for (int x = std::max(0, x0); x < x1; ++x) {
                    const uint8_t* p = d + y * st + x * 4;
                    n += (p[0] + p[1] + p[2]) > 120;
                }
            return n;
        };
        cairo_surface_t* direct = render(false);
        cairo_surface_t* cached = render(true);
        for (cairo_surface_t* s : {direct, cached}) {
            const char* name = s == direct ? "direct" : "glyph cache";
            for (int c : {0, 2, 6, 8}) {  // each wide glyph has ink in both of its cells
                const int l = ink(s, c * cw, (c + 1) * cw), r = ink(s, (c + 1) * cw, (c + 2) * cw);
                std::printf("%s: wide cell %d ink left %d right %d\n", name, c, l, r);
                if (l < 5 || r < 5) {
                    std::fprintf(stderr, "FAIL: %s: wide glyph in cell %d does not span 2 cells\n", name, c);
                    ++fails;
                }
            }
            // The combining marks are drawn (more ink than the bare letter), inside their cell.
            std::printf("%s: ink x %d, e+acute %d, a+2 marks %d, blank after it %d\n", name, ink(s, 4 * cw, 5 * cw),
                        ink(s, 5 * cw, 6 * cw), ink(s, 10 * cw, 11 * cw), ink(s, 11 * cw, 12 * cw));
            // (a + U+0308 + U+0323: HarfBuzz composes U+1EA1 and DejaVu Sans Mono has no mark
            // anchor for U+0308 on it, so the diaeresis lands at the right edge of the cell and
            // spills into the next one; the glyph cache clips it at its padding. Not checked.)
            if (ink(s, 5 * cw, 6 * cw) <= ink(s, 4 * cw, 5 * cw) / 2) {
                std::fprintf(stderr, "FAIL: %s: combining mark missing\n", name);
                ++fails;
            }
        }
        // Pixel match between the two, including the colour emoji (the glyph cache keeps colour
        // glyphs in colour since M4 item 5; before, it drew a silhouette in the fg colour).
        auto crop = [&](cairo_surface_t* s) {
            std::vector<uint8_t> v = pixels(s);
            std::vector<uint8_t> o;
            const int st = cairo_image_surface_get_stride(s);
            for (int y = 0; y < ch; ++y) o.insert(o.end(), v.begin() + y * st, v.begin() + y * st + 10 * cw * 4);
            return o;
        };
        int wmx;
        double wmean;
        stats(crop(direct), crop(cached), &wmx, &wmean);
        std::printf("wide/combining text, glyph cache vs direct Pango: mean abs diff %.4f, max diff %d\n", wmean, wmx);
        if (wmean > 0.5) {
            std::fprintf(stderr, "FAIL: wide text differs between the renderers\n");
            ++fails;
        }
        {  // the emoji cell has colour in the glyph cache output (not grey)
            cairo_surface_flush(cached);
            const uint8_t* d = cairo_image_surface_get_data(cached);
            const int st = cairo_image_surface_get_stride(cached);
            int coloured = 0;
            for (int y = 0; y < ch; ++y)
                for (int x = 8 * cw; x < 10 * cw; ++x) {
                    const uint8_t* p = d + y * st + x * 4;
                    coloured += std::abs(int(p[2]) - int(p[0])) > 40;  // R vs B
                }
            std::printf("glyph cache emoji: %d coloured px\n", coloured);
            if (coloured < 20) {
                std::fprintf(stderr, "FAIL: emoji drawn without colour by the glyph cache\n");
                ++fails;
            }
        }
        if (const char* dir = std::getenv("LTERM_PAINTER_DUMP")) {
            cairo_surface_write_to_png(direct, (std::string(dir) + "/painter-wide-direct.png").c_str());
            cairo_surface_write_to_png(cached, (std::string(dir) + "/painter-wide-cached.png").c_str());
        }
        cairo_surface_destroy(direct);
        cairo_surface_destroy(cached);
    }
    cairo_surface_destroy(ref);
    cairo_surface_destroy(fast);
    cairo_surface_destroy(full);
    return fails ? 1 : 0;
}
