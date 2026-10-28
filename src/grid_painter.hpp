// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "grid.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <string>

namespace lterm {

class GridPainter {
public:
    GridPainter(const std::string& fontFamily, int pixelSize);
    ~GridPainter();
    GridPainter(const GridPainter&) = delete;
    GridPainter& operator=(const GridPainter&) = delete;

    int cellWidth() const { return cellW_; }
    int cellHeight() const { return cellH_; }
    std::string resolvedFamily() const;

    // Paints the grid at (originX, originY) on an already cleared surface.
    void paint(cairo_t* cr, const Grid& g, int originX, int originY, Color8 cursorColor) const;

private:
    PangoFontDescription* font_ = nullptr;
    PangoFontDescription* bold_ = nullptr;
    PangoContext* ctx_ = nullptr;
    int cellW_ = 8, cellH_ = 16, ascent_ = 12;
};

// Converts a Cairo ARGB32 surface (premultiplied, opaque here) to tightly packed RGBA8.
void surfaceToRgba(cairo_surface_t* s, std::vector<uint8_t>& out);
// Converts RGBA8 to an opaque Cairo RGB24 surface's data.
void rgbaToSurface(const uint8_t* rgba, int w, int h, cairo_surface_t* s);

}  // namespace lterm
