// SPDX-License-Identifier: GPL-3.0-or-later
// Glyph cache renderer vs the M0 direct Pango renderer, and changed-rows-only repaint.
#include "grid_painter.hpp"
#include "synthetic_frame.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    cairo_surface_destroy(ref);
    cairo_surface_destroy(fast);
    cairo_surface_destroy(full);
    return fails ? 1 : 0;
}
