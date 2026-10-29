// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "grid.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace lterm {

class GridPainter {
public:
    // pixelSize is in logical pixels. scale is the GTK scale factor (HiDPI): the cell metrics
    // are measured at pixelSize and multiplied by scale, the glyphs are rasterised at
    // pixelSize * scale, so a scale-2 grid is exactly the scale-1 grid at twice the resolution.
    GridPainter(const std::string& fontFamily, int pixelSize, int scale = 1);
    ~GridPainter();
    GridPainter(const GridPainter&) = delete;
    GridPainter& operator=(const GridPainter&) = delete;

    int cellWidth() const { return cellW_; }    // device pixels
    int cellHeight() const { return cellH_; }   // device pixels
    int scale() const { return scale_; }
    std::string resolvedFamily() const;

    // M0 reference renderer: one Pango layout per cell, straight onto an already cleared
    // surface. Slow (~50 ms per 1280x800 screen); kept for the golden frames and as the
    // reference the glyph cache is tested against.
    void paint(cairo_t* cr, const Grid& g, int originX, int originY, Color8 cursorColor) const;

    // M1 renderer: glyphs are rasterised once into A8 masks (key: text, bold, italic,
    // underline, width) and composited with cairo_mask_surface. Only rows with rows[r] != 0 are
    // repainted (rows == nullptr: all). Each painted row band is first cleared to `clear`
    // across the whole surface width so margins stay clean.
    void paintRows(cairo_t* cr, const Grid& g, int originX, int originY, Color8 cursorColor, Color8 clear,
                   const std::vector<char>* rows, int surfaceWidth);
    size_t cachedGlyphs() const { return cache_.size(); }
    size_t cachedGlyphBytes() const;  // A8 masks + ARGB32 colour glyphs (stride x height), M4 item 4
    void clearCache();

private:
    struct GlyphKey {
        std::string text;
        uint8_t flags;  // 1 bold, 2 italic, 4 underline
        uint8_t width;
        bool operator==(const GlyphKey& o) const { return flags == o.flags && width == o.width && text == o.text; }
    };
    struct GlyphKeyHash {
        size_t operator()(const GlyphKey& k) const {
            return std::hash<std::string>()(k.text) ^ (static_cast<size_t>(k.flags) << 20) ^
                   (static_cast<size_t>(k.width) << 24);
        }
    };
    cairo_surface_t* glyph(const Cell& cell);
    std::unordered_map<GlyphKey, cairo_surface_t*, GlyphKeyHash> cache_;
    PangoLayout* glyphLayout_ = nullptr;
    int padX_ = 0;

    PangoFontDescription* font_ = nullptr;
    PangoFontDescription* bold_ = nullptr;
    PangoContext* ctx_ = nullptr;
    int cellW_ = 8, cellH_ = 16, ascent_ = 12;
    int scale_ = 1;
};

// Converts a Cairo ARGB32 surface (premultiplied, opaque here) to tightly packed RGBA8.
void surfaceToRgba(cairo_surface_t* s, std::vector<uint8_t>& out);
// Converts RGBA8 to an opaque Cairo RGB24 surface's data.
void rgbaToSurface(const uint8_t* rgba, int w, int h, cairo_surface_t* s);

}  // namespace lterm
