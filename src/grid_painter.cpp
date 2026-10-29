// SPDX-License-Identifier: GPL-3.0-or-later
#include "grid_painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lterm {

GridPainter::GridPainter(const std::string& family, int pixelSize, int scale) : scale_(std::max(1, scale)) {
    font_ = pango_font_description_new();
    pango_font_description_set_family(font_, family.c_str());
    pango_font_description_set_absolute_size(font_, pixelSize * PANGO_SCALE);
    bold_ = pango_font_description_copy(font_);
    pango_font_description_set_weight(bold_, PANGO_WEIGHT_BOLD);

    PangoFontMap* map = pango_cairo_font_map_get_default();
    ctx_ = pango_font_map_create_context(map);
    cairo_font_options_t* fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
    pango_cairo_context_set_font_options(ctx_, fo);
    cairo_font_options_destroy(fo);

    PangoFontMetrics* m = pango_context_get_metrics(ctx_, font_, nullptr);
    cellW_ = static_cast<int>(std::ceil(pango_font_metrics_get_approximate_digit_width(m) /
                                        static_cast<double>(PANGO_SCALE)));
    int asc = pango_font_metrics_get_ascent(m), desc = pango_font_metrics_get_descent(m);
    ascent_ = static_cast<int>(std::ceil(asc / static_cast<double>(PANGO_SCALE)));
    cellH_ = static_cast<int>(std::ceil((asc + desc) / static_cast<double>(PANGO_SCALE)));
    pango_font_metrics_unref(m);
    if (cellW_ < 1) cellW_ = 1;
    if (cellH_ < 1) cellH_ = 1;
    cellW_ *= scale_;
    cellH_ *= scale_;
    ascent_ *= scale_;
    if (scale_ > 1) {  // rasterise at device resolution
        pango_font_description_set_absolute_size(font_, double(pixelSize) * scale_ * PANGO_SCALE);
        pango_font_description_set_absolute_size(bold_, double(pixelSize) * scale_ * PANGO_SCALE);
    }
    padX_ = std::max(1, cellW_ / 2);
    glyphLayout_ = pango_layout_new(ctx_);
}

void GridPainter::clearCache() {
    for (auto& kv : cache_) cairo_surface_destroy(kv.second);
    cache_.clear();
}

GridPainter::~GridPainter() {
    clearCache();
    if (glyphLayout_) g_object_unref(glyphLayout_);
    pango_font_description_free(font_);
    pango_font_description_free(bold_);
    g_object_unref(ctx_);
}

std::string GridPainter::resolvedFamily() const {
    PangoFont* f = pango_context_load_font(ctx_, font_);
    if (!f) return "?";
    PangoFontDescription* d = pango_font_describe(f);
    std::string s = pango_font_description_get_family(d) ? pango_font_description_get_family(d) : "?";
    pango_font_description_free(d);
    g_object_unref(f);
    return s;
}

static void setColor(cairo_t* cr, Color8 c) {
    cairo_set_source_rgb(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0);
}

void GridPainter::paint(cairo_t* cr, const Grid& g, int ox, int oy, Color8 cursorColor) const {
    PangoLayout* layout = pango_layout_new(ctx_);
    for (int r = 0; r < g.rows; ++r) {
        const int y = oy + r * cellH_;
        // Backgrounds first (runs of equal colour).
        int c = 0;
        while (c < g.cols) {
            int e = c + 1;
            while (e < g.cols && g.at(r, e).bg == g.at(r, c).bg) ++e;
            if (g.at(r, c).bg != Color8{0, 0, 0}) {
                setColor(cr, g.at(r, c).bg);
                cairo_rectangle(cr, ox + c * cellW_, y, (e - c) * cellW_, cellH_);
                cairo_fill(cr);
            }
            c = e;
        }
        // Glyphs: each cell is placed on its own grid position so proportional fallback glyphs
        // cannot drift the rest of the row.
        for (c = 0; c < g.cols; ++c) {
            const Cell& cell = g.at(r, c);
            const bool isCursor = g.cursorVisible && r == g.cursorRow && c == g.cursorCol;
            if (isCursor) {
                setColor(cr, cursorColor);
                cairo_rectangle(cr, ox + c * cellW_, y, cellW_ * (cell.width == 2 ? 2 : 1), cellH_);
                cairo_fill(cr);
            }
            if (cell.width == 0 || cell.text.empty() || cell.text == " ") continue;
            pango_layout_set_font_description(layout, cell.bold ? bold_ : font_);
            PangoAttrList* attrs = nullptr;
            if (cell.underline || cell.italic) {
                attrs = pango_attr_list_new();
                if (cell.underline) pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
                if (cell.italic) pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
            }
            pango_layout_set_attributes(layout, attrs);
            if (attrs) pango_attr_list_unref(attrs);
            pango_layout_set_text(layout, cell.text.c_str(), -1);
            setColor(cr, isCursor ? cell.bg : cell.fg);
            const int base = pango_layout_get_baseline(layout) / PANGO_SCALE;
            cairo_move_to(cr, ox + c * cellW_, y + ascent_ - base);
            pango_cairo_show_layout(cr, layout);
        }
    }
    g_object_unref(layout);
}

cairo_surface_t* GridPainter::glyph(const Cell& cell) {
    GlyphKey key{cell.text,
                 static_cast<uint8_t>((cell.bold ? 1 : 0) | (cell.italic ? 2 : 0) | (cell.underline ? 4 : 0)),
                 static_cast<uint8_t>(cell.width == 2 ? 2 : 1)};
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_.size() >= 8192) clearCache();  // crude bound; real terminals rarely get here
    const int w = cellW_ * key.width + 2 * padX_;
    // Rendered in ARGB32 first: a colour glyph (Noto Color Emoji and other colour fonts) is kept
    // as a colour image and painted as-is; anything else becomes an A8 mask tinted with the cell
    // colour. (Before M4 item 5 everything was A8, so emoji came out as fg-coloured silhouettes.)
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, cellH_);
    cairo_t* cr = cairo_create(s);
    PangoLayout* layout = glyphLayout_;
    pango_layout_set_font_description(layout, cell.bold ? bold_ : font_);
    PangoAttrList* attrs = nullptr;
    if (cell.underline || cell.italic) {
        attrs = pango_attr_list_new();
        if (cell.underline) pango_attr_list_insert(attrs, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        if (cell.italic) pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
    }
    pango_layout_set_attributes(layout, attrs);
    if (attrs) pango_attr_list_unref(attrs);
    pango_layout_set_text(layout, cell.text.c_str(), -1);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    const int base = pango_layout_get_baseline(layout) / PANGO_SCALE;
    cairo_move_to(cr, padX_, ascent_ - base);
    pango_cairo_show_layout(cr, layout);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    const int st = cairo_image_surface_get_stride(s);
    const uint8_t* d = cairo_image_surface_get_data(s);
    bool colour = false;
    for (int y = 0; y < cellH_ && !colour; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(d + static_cast<size_t>(y) * st);
        for (int x = 0; x < w; ++x) {
            const uint32_t p = row[x], a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            // White drawn through a mask is premultiplied grey: r == g == b == a.
            if (r != a || g != a || b != a) {
                colour = true;
                break;
            }
        }
    }
    if (!colour) {
        cairo_surface_t* m = cairo_image_surface_create(CAIRO_FORMAT_A8, w, cellH_);
        cairo_surface_flush(m);
        uint8_t* md = cairo_image_surface_get_data(m);
        const int ms = cairo_image_surface_get_stride(m);
        for (int y = 0; y < cellH_; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(d + static_cast<size_t>(y) * st);
            for (int x = 0; x < w; ++x) md[static_cast<size_t>(y) * ms + x] = row[x] >> 24;
        }
        cairo_surface_mark_dirty(m);
        cairo_surface_destroy(s);
        s = m;
    }
    cache_.emplace(std::move(key), s);
    return s;
}

void GridPainter::paintRows(cairo_t* cr, const Grid& g, int ox, int oy, Color8 cursorColor, Color8 clear,
                            const std::vector<char>* rows, int surfaceWidth) {
    for (int r = 0; r < g.rows; ++r) {
        if (rows && (r >= static_cast<int>(rows->size()) || !(*rows)[r])) continue;
        const int y = oy + r * cellH_;
        cairo_save(cr);
        cairo_rectangle(cr, 0, y, surfaceWidth, cellH_);
        cairo_clip(cr);
        setColor(cr, clear);
        cairo_paint(cr);
        int c = 0;
        while (c < g.cols) {
            int e = c + 1;
            while (e < g.cols && g.at(r, e).bg == g.at(r, c).bg) ++e;
            if (g.at(r, c).bg != clear) {
                setColor(cr, g.at(r, c).bg);
                cairo_rectangle(cr, ox + c * cellW_, y, (e - c) * cellW_, cellH_);
                cairo_fill(cr);
            }
            c = e;
        }
        for (c = 0; c < g.cols; ++c) {
            const Cell& cell = g.at(r, c);
            const bool isCursor = g.cursorVisible && r == g.cursorRow && c == g.cursorCol;
            // Only the solid block inverts the glyph; the other shapes are drawn over it.
            const bool solid = isCursor && g.cursorShape == Grid::CursorShape::Block && !g.cursorHollow;
            if (solid) {
                setColor(cr, cursorColor);
                cairo_rectangle(cr, ox + c * cellW_, y, cellW_ * (cell.width == 2 ? 2 : 1), cellH_);
                cairo_fill(cr);
            }
            if (!(cell.width == 0 || cell.text.empty() || (cell.text == " " && !cell.underline))) {
                cairo_surface_t* m = glyph(cell);
                if (cairo_image_surface_get_format(m) == CAIRO_FORMAT_ARGB32) {  // colour glyph
                    cairo_set_source_surface(cr, m, ox + c * cellW_ - padX_, y);
                    cairo_paint(cr);
                } else {
                    setColor(cr, solid ? cell.bg : cell.fg);
                    cairo_mask_surface(cr, m, ox + c * cellW_ - padX_, y);
                }
            }
            if (isCursor && !solid) {
                const int w = cellW_ * (cell.width == 2 ? 2 : 1);
                const int x = ox + c * cellW_;
                setColor(cr, cursorColor);
                if (g.cursorHollow) {
                    const double lw = scale_;
                    cairo_set_line_width(cr, lw);
                    cairo_rectangle(cr, x + lw / 2, y + lw / 2, w - lw, cellH_ - lw);
                    cairo_stroke(cr);
                } else if (g.cursorShape == Grid::CursorShape::Underline) {
                    const int t = std::max(2 * scale_, cellH_ / 8);
                    cairo_rectangle(cr, x, y + cellH_ - t, w, t);
                    cairo_fill(cr);
                } else {
                    cairo_rectangle(cr, x, y, std::max(2 * scale_, cellW_ / 8), cellH_);
                    cairo_fill(cr);
                }
            }
        }
        cairo_restore(cr);
    }
}

void surfaceToRgba(cairo_surface_t* s, std::vector<uint8_t>& out) {
    cairo_surface_flush(s);
    const int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    const int stride = cairo_image_surface_get_stride(s);
    const uint8_t* d = cairo_image_surface_get_data(s);
    out.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(d + static_cast<size_t>(y) * stride);
        uint8_t* o = &out[static_cast<size_t>(y) * w * 4];
        for (int x = 0; x < w; ++x) {
            const uint32_t p = row[x];
            o[x * 4 + 0] = (p >> 16) & 0xff;
            o[x * 4 + 1] = (p >> 8) & 0xff;
            o[x * 4 + 2] = p & 0xff;
            o[x * 4 + 3] = 255;
        }
    }
}

void rgbaToSurface(const uint8_t* rgba, int w, int h, cairo_surface_t* s) {
    cairo_surface_flush(s);
    const int stride = cairo_image_surface_get_stride(s);
    uint8_t* d = cairo_image_surface_get_data(s);
    for (int y = 0; y < h; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(d + static_cast<size_t>(y) * stride);
        const uint8_t* i = rgba + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x)
            row[x] = 0xff000000u | (uint32_t(i[x * 4]) << 16) | (uint32_t(i[x * 4 + 1]) << 8) | i[x * 4 + 2];
    }
    cairo_surface_mark_dirty(s);
}

size_t GridPainter::cachedGlyphBytes() const {
    size_t b = 0;
    for (const auto& kv : cache_)
        if (kv.second)
            b += static_cast<size_t>(cairo_image_surface_get_stride(kv.second)) *
                 static_cast<size_t>(cairo_image_surface_get_height(kv.second));
    return b;
}

}  // namespace lterm
