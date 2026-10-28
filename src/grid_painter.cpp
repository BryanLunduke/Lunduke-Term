// SPDX-License-Identifier: GPL-3.0-or-later
#include "grid_painter.hpp"

#include <cmath>
#include <cstring>

namespace lterm {

GridPainter::GridPainter(const std::string& family, int pixelSize) {
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
}

GridPainter::~GridPainter() {
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

}  // namespace lterm
