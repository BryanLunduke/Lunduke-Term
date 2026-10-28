// SPDX-License-Identifier: GPL-3.0-or-later
// Text selection over screen + scrollback (global line numbers), word/line expansion, text
// extraction, and the pixel -> cell mapping (including the CRT curvature warp).
#pragma once

#include "effects.hpp"

#include <cstdint>
#include <string>

namespace lterm {

// Anything that can provide cells by global line number (VtermSession, test doubles).
class LineSource {
public:
    virtual ~LineSource() = default;
    virtual int cols() const = 0;
    virtual long firstLine() const = 0;  // oldest line still available
    virtual long endLine() const = 0;    // one past the last screen line
    virtual std::string cellText(long line, int col) const = 0;  // "" = blank
    virtual int cellWidth(long line, int col) const = 0;         // 0 = right half of a wide char
    virtual bool wrapped(long line) const = 0;                   // soft-wrapped into line + 1
    // For caches (search): lines in [firstLine(), stableEnd()) do not change while they exist
    // (scrollback), until contentEpoch() changes (history cleared, lines popped back, resize).
    virtual long stableEnd() const { return firstLine(); }
    virtual unsigned long contentEpoch() const { return 0; }
    // Code points of a cell (at most max) and its width (0 = right half of a wide character).
    // The default goes through cellText()/cellWidth(); sources may do it without allocating.
    virtual int cellChars(long line, int col, uint32_t* out, int max, int* width) const;
};

struct SelPos {
    long line = 0;
    int col = 0;
    bool operator<(const SelPos& o) const { return line < o.line || (line == o.line && col < o.col); }
    bool operator==(const SelPos& o) const { return line == o.line && col == o.col; }
};

enum class SelectMode { Char, Word, Line };

class Selection {
public:
    static const char* defaultWordChars() { return "-_./~:@%+=?&#"; }
    void setWordChars(std::string w) { wordChars_ = std::move(w); }

    // Press: remembers the anchor. A Char selection only becomes visible once the head moves to
    // another cell (a plain click clears); Word and Line are visible immediately.
    void begin(const LineSource& src, SelPos p, SelectMode mode);
    void extend(const LineSource& src, SelPos p);
    void clear() { active_ = false; }
    bool active() const { return active_; }
    SelectMode mode() const { return mode_; }

    // Normalised inclusive range.
    SelPos start() const { return start_; }
    SelPos end() const { return end_; }
    bool contains(long line, int col) const;
    std::string text(const LineSource& src) const;

private:
    bool isWord(const std::string& ch) const;
    int classOf(const LineSource& src, long line, int col) const;
    void update(const LineSource& src);

    std::string wordChars_ = defaultWordChars();
    SelPos anchor_, head_, start_, end_;
    SelectMode mode_ = SelectMode::Char;
    bool active_ = false;
};

// Maps a pointer position in the widget to a grid cell. With curvature > 0 (the shader's
// screenCurvature * 0.4) the barrel warp is applied first: the shader shows, at output point p,
// the source pixel barrel(p), so the same forward function maps the mouse to the right cell.
// Returns false when the point falls outside the (warped) screen. row/col are clamped.
// With a bezel (frame.inset > 0) the warp works inside the glass and a point on the bezel,
// including the glass's rounded corners, is outside.
bool pointToCell(double x, double y, int width, int height, float curvature, int marginX, int marginY, int cellW,
                 int cellH, int rows, int cols, int& row, int& col, const FrameGeometry& frame = FrameGeometry{});

// Where the grid goes inside a widget of devW x devH device pixels (HiDPI: logical size times
// `scale`). Margins and the grid size are worked out in logical pixels and then multiplied by
// the scale, so the same logical window gets the same rows/cols at every scale. cellW/cellH are
// device pixels (GridPainter at that scale), marginMinPx is logical.
struct GridLayout {
    int marginX = 0, marginY = 0;  // device pixels
    int rows = 1, cols = 1;
};
// frameInset: logical px taken by a bezel on every side (the margins are then inside the glass).
GridLayout computeGridLayout(int devW, int devH, int scale, int cellW, int cellH, int marginMinPx, float marginFrac,
                             int frameInset = 0);

}  // namespace lterm
