// SPDX-License-Identifier: GPL-3.0-or-later
#include "grid_painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lterm {

GridPainter::GridPainter(const std::string& family, int pixelSize, int scale, int cellBoxW, int cellBoxH)
    : scale_(std::max(1, scale)) {
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
    natW_ = cellW_;
    natH_ = cellH_;
    if (cellBoxW > 0) cellW_ = cellBoxW;
    if (cellBoxH > 0) cellH_ = cellBoxH;
    if (scale_ > 1) {  // rasterise at device resolution
        pango_font_description_set_absolute_size(font_, double(pixelSize) * scale_ * PANGO_SCALE);
        pango_font_description_set_absolute_size(bold_, double(pixelSize) * scale_ * PANGO_SCALE);
    }
    padX_ = std::max(1, natW_ / 2);
    prescale_ = cellW_ != natW_ || cellH_ != natH_;
    if (!prescale_) blitOriginX_ = -padX_;
    else blitOriginX_ = static_cast<int>(std::floor(-padX_ * static_cast<double>(cellW_) / natW_));
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

static Color8 dimmed(Color8 c) {
    return Color8{static_cast<uint8_t>(c.r * 3 / 5), static_cast<uint8_t>(c.g * 3 / 5), static_cast<uint8_t>(c.b * 3 / 5)};
}

// Masks are already in the cell's pixel grid (a stretched CRT bakes the scale once, into the
// cache). The blit is an integer copy, so a 4:3 cell does not resample the glyph on every paint.
static void paintMask(cairo_t* cr, cairo_surface_t* m, bool colour, int x, int y) {
    if (colour) {
        cairo_set_source_surface(cr, m, x, y);
        cairo_paint(cr);
    } else {
        cairo_mask_surface(cr, m, x, y);
    }
}

// Rasterise the natural-size mask through the same translate/scale/filter a per-paint scale used,
// into an image whose origin is blitOriginX_ relative to the cell. An integer blit of that image
// lands on the same pixels.
static cairo_surface_t* prescaleGlyph(cairo_surface_t* src, double sx, double sy, double fracX) {
    const int nw = cairo_image_surface_get_width(src);
    const int nh = cairo_image_surface_get_height(src);
    const int sw = std::max(1, static_cast<int>(std::ceil(nw * sx + fracX)));
    const int sh = std::max(1, static_cast<int>(std::ceil(nh * sy)));
    const bool mask = cairo_image_surface_get_format(src) == CAIRO_FORMAT_A8;
    // A8 destinations ignore cairo_mask. Draw into ARGB32 (white through the mask, or the
    // colour glyph with the same GOOD filter the per-paint scale used) and, for a mask,
    // keep only the alpha.
    cairo_surface_t* argb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    cairo_t* cr = cairo_create(argb);
    cairo_translate(cr, fracX, 0);
    cairo_scale(cr, sx, sy);
    if (mask) {
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_mask_surface(cr, src, 0, 0);
    } else {
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(src);
    if (!mask) return argb;
    cairo_surface_t* dst = cairo_image_surface_create(CAIRO_FORMAT_A8, sw, sh);
    cairo_surface_flush(argb);
    cairo_surface_flush(dst);
    const int as = cairo_image_surface_get_stride(argb);
    const int ds = cairo_image_surface_get_stride(dst);
    const uint8_t* ad = cairo_image_surface_get_data(argb);
    uint8_t* dd = cairo_image_surface_get_data(dst);
    for (int y = 0; y < sh; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(ad + static_cast<size_t>(y) * as);
        for (int x = 0; x < sw; ++x) dd[static_cast<size_t>(y) * ds + x] = static_cast<uint8_t>(row[x] >> 24);
    }
    cairo_surface_mark_dirty(dst);
    cairo_surface_destroy(argb);
    return dst;
}

// Lines the glyph mask does not carry: faint is a tint, the rest are stroked so a colored
// underline (SGR 58) can differ from the text.
static void drawDecorations(cairo_t* cr, const Cell& cell, int x, int y, int w, int h, int scale, Color8 textColor) {
    const bool ul = cell.underline != Underline::None;
    if (!ul && !cell.strikethrough && !cell.overline) return;
    const int t = std::max(scale, std::max(1, h / 14));
    auto bar = [&](int yy, int hh, Color8 c) {
        setColor(cr, c);
        cairo_rectangle(cr, x, yy, w, std::max(1, hh));
        cairo_fill(cr);
    };
    if (cell.overline) bar(y, t, textColor);
    if (cell.strikethrough) bar(y + h / 2 - t / 2, t, textColor);
    if (!ul) return;
    const Color8 c = cell.underlineColored ? cell.underlineColor : textColor;
    const int base = y + h - 2 * t;
    if (cell.underline == Underline::Double) {
        bar(base - t - std::max(1, t / 2), t, c);
        bar(base, t, c);
        return;
    }
    if (cell.underline == Underline::Curly) {
        setColor(cr, c);
        cairo_set_line_width(cr, std::max(1.0, t * 0.8));
        cairo_new_path(cr);
        const int amp = std::max(1, t);
        const int period = std::max(4 * scale, 4);
        cairo_move_to(cr, x, base + amp);
        for (int i = 0; i <= w; i += std::max(1, period / 4)) {
            const double phase = (i % period) / static_cast<double>(period);
            const double yy = base + amp * (phase < 0.5 ? 1.0 - 4 * phase : 4 * phase - 3.0);
            cairo_line_to(cr, x + i, yy);
        }
        cairo_stroke(cr);
        return;
    }
    if (cell.underline == Underline::Dotted || cell.underline == Underline::Dashed) {
        setColor(cr, c);
        cairo_set_line_width(cr, t);
        const double dash[2] = {cell.underline == Underline::Dotted ? static_cast<double>(t) : static_cast<double>(3 * t),
                                static_cast<double>(2 * t)};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, x, base + t / 2.0);
        cairo_line_to(cr, x + w, base + t / 2.0);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        return;
    }
    bar(base, t, c);
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
            const Color8 tint = cell.faint ? dimmed(isCursor ? cell.bg : cell.fg) : (isCursor ? cell.bg : cell.fg);
            const int gw = cellW_ * (cell.width == 2 ? 2 : 1);
            if (!(cell.conceal || cell.width == 0 || cell.text.empty() || cell.text == " ")) {
                pango_layout_set_font_description(layout, cell.bold ? bold_ : font_);
                PangoAttrList* attrs = nullptr;
                if (cell.italic) {
                    attrs = pango_attr_list_new();
                    pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
                }
                pango_layout_set_attributes(layout, attrs);
                if (attrs) pango_attr_list_unref(attrs);
                pango_layout_set_text(layout, cell.text.c_str(), -1);
                setColor(cr, tint);
                const int base = pango_layout_get_baseline(layout) / PANGO_SCALE;
                if (cellW_ == natW_ && cellH_ == natH_) {
                    cairo_move_to(cr, ox + c * cellW_, y + ascent_ - base);
                    pango_cairo_show_layout(cr, layout);
                } else {
                    cairo_save(cr);
                    cairo_translate(cr, ox + c * cellW_, y);
                    cairo_scale(cr, static_cast<double>(cellW_) / natW_, static_cast<double>(cellH_) / natH_);
                    cairo_move_to(cr, 0, ascent_ - base);
                    pango_cairo_show_layout(cr, layout);
                    cairo_restore(cr);
                }
            }
            if (cell.width != 0) drawDecorations(cr, cell, ox + c * cellW_, y, gw, cellH_, scale_, tint);
        }
    }
    g_object_unref(layout);
}

cairo_surface_t* GridPainter::glyph(const Cell& cell) {
    GlyphKey key{cell.text, static_cast<uint8_t>((cell.bold ? 1 : 0) | (cell.italic ? 2 : 0)),
                 static_cast<uint8_t>(cell.width == 2 ? 2 : 1)};
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_.size() >= 8192) clearCache();  // crude bound; real terminals rarely get here
    const int w = natW_ * key.width + 2 * padX_;
    // Rendered in ARGB32 first: a colour glyph (Noto Color Emoji and other colour fonts) is kept
    // as a colour image and painted as-is; anything else becomes an A8 mask tinted with the cell
    // colour. (Before M4 item 5 everything was A8, so emoji came out as fg-coloured silhouettes.)
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, natH_);
    cairo_t* cr = cairo_create(s);
    PangoLayout* layout = glyphLayout_;
    pango_layout_set_font_description(layout, cell.bold ? bold_ : font_);
    PangoAttrList* attrs = nullptr;
    if (cell.italic) {
        attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
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
    for (int y = 0; y < natH_ && !colour; ++y) {
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
        cairo_surface_t* m = cairo_image_surface_create(CAIRO_FORMAT_A8, w, natH_);
        cairo_surface_flush(m);
        uint8_t* md = cairo_image_surface_get_data(m);
        const int ms = cairo_image_surface_get_stride(m);
        for (int y = 0; y < natH_; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(d + static_cast<size_t>(y) * st);
            for (int x = 0; x < w; ++x) md[static_cast<size_t>(y) * ms + x] = row[x] >> 24;
        }
        cairo_surface_mark_dirty(m);
        cairo_surface_destroy(s);
        s = m;
    }
    if (prescale_) {
        const double sx = static_cast<double>(cellW_) / std::max(1, natW_);
        const double sy = static_cast<double>(cellH_) / std::max(1, natH_);
        const double x0 = -static_cast<double>(padX_) * sx;
        s = prescaleGlyph(s, sx, sy, x0 - std::floor(x0));
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
            const Color8 tint = cell.faint ? dimmed(solid ? cell.bg : cell.fg) : (solid ? cell.bg : cell.fg);
            const int gw = cellW_ * (cell.width == 2 ? 2 : 1);
            if (!(cell.conceal || cell.width == 0 || cell.text.empty() || cell.text == " ")) {
                cairo_surface_t* m = glyph(cell);
                const bool colour = cairo_image_surface_get_format(m) == CAIRO_FORMAT_ARGB32;
                if (!colour) setColor(cr, tint);
                paintMask(cr, m, colour, ox + c * cellW_ + blitOriginX_, y);
            }
            if (cell.width != 0) drawDecorations(cr, cell, ox + c * cellW_, y, gw, cellH_, scale_, tint);
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
