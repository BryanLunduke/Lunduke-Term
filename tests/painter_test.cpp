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
    cairo_surface_destroy(ref);
    cairo_surface_destroy(fast);
    cairo_surface_destroy(full);
    return fails ? 1 : 0;
}
