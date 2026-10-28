// SPDX-License-Identifier: GPL-3.0-or-later
#include "synthetic_frame.hpp"
#include "grid_painter.hpp"

#include <algorithm>

namespace lterm {

namespace {
const Color8 kFg{208, 208, 208}, kRed{205, 49, 49}, kGreen{13, 188, 121}, kYellow{229, 229, 16},
    kBlue{36, 114, 200}, kMagenta{188, 63, 188}, kCyan{17, 168, 205}, kWhite{255, 255, 255};

struct Line {
    std::vector<std::pair<std::string, Color8>> spans;
    bool bold = false;
};

std::vector<Line> script() {
    std::vector<Line> L;
    auto prompt = [](const std::string& cmd) {
        Line l;
        l.spans = {{"bryan@lcos", kGreen}, {":", kFg}, {"~/src/lunduke-term", kBlue}, {"$ ", kFg}, {cmd, kFg}};
        return l;
    };
    L.push_back({{{"Lunduke Terminal M0 - synthetic frame (stand-in for the libvterm renderer)", kWhite}}, true});
    L.push_back({{{"", kFg}}});
    L.push_back(prompt("uname -a"));
    L.push_back({{{"Linux lcos 6.12.0-lcos #1 SMP PREEMPT_DYNAMIC Devuan 6.12 x86_64 GNU/Linux", kFg}}});
    L.push_back(prompt("ls --color"));
    L.push_back({{{"build/  ", kBlue}, {"docs/  ", kBlue}, {"src/  ", kBlue}, {"tests/  ", kBlue},
                  {"LICENSE  meson.build  README.md  ", kFg}, {"run-bench.sh", kGreen}}});
    L.push_back(prompt("meson compile -C build"));
    L.push_back({{{"[1/12] Compiling C++ object src/cpu_effects.cpp.o", kFg}}});
    L.push_back({{{"[2/12] Compiling C++ object src/gl_effects.cpp.o", kFg}}});
    L.push_back({{{"src/gl_effects.cpp:42: ", kWhite}, {"warning: ", kMagenta}, {"unused variable 'n'", kFg}}});
    L.push_back({{{"[12/12] Linking target lunduke-terminal", kFg}}});
    L.push_back(prompt("./run-bench.sh --threads 4"));
    L.push_back({{{"  frame  ", kFg}, {"[##########################--------]", kYellow}, {"  76%  ", kFg}, {"OK", kGreen}}});
    L.push_back({{{"  bloom   ", kFg}, {"3.1 ms", kCyan}, {"   static  ", kFg}, {"4.4 ms", kCyan}, {"   dynamic  ", kFg}, {"6.0 ms", kCyan}}});
    L.push_back({{{"  ERROR: ", kRed}, {"none - all effects nominal", kFg}}});
    L.push_back({{{"ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789 !@#$%^&*()[]{}", kFg}}});
    L.push_back({{{"+------------------------------------------------------------------------------+", kFg}}});
    L.push_back({{{"| ", kFg}, {"READY.", kWhite}, {"  10 PRINT \"LUNDUKE\" : 20 GOTO 10", kGreen}}});
    L.push_back({{{"+------------------------------------------------------------------------------+", kFg}}});
    return L;
}
}  // namespace

void fillSyntheticGrid(Grid& g, int scroll) {
    for (auto& c : g.cells) c = Cell{};
    auto lines = script();
    int row = 0;
    for (size_t i = static_cast<size_t>(scroll); i < lines.size() + 40 && row < g.rows; ++i, ++row) {
        const Line& l = lines[i % lines.size()];
        int col = 0;
        for (const auto& span : l.spans) {
            for (char ch : span.first) {
                if (col >= g.cols) break;
                Cell& c = g.at(row, col++);
                c.text = std::string(1, ch);
                c.fg = span.second;
                c.bold = l.bold;
            }
        }
    }
    // A reverse-video status bar on the last row.
    if (g.rows > 0) {
        const std::string status = " NORMAL  synthetic.txt  80x25  utf-8  [M0] ";
        for (int c = 0; c < g.cols; ++c) {
            Cell& cell = g.at(g.rows - 1, c);
            cell.bg = Color8{36, 114, 200};
            cell.fg = kWhite;
            cell.text = c < static_cast<int>(status.size()) ? std::string(1, status[c]) : " ";
        }
    }
    g.cursorRow = std::min(g.rows - 2, 21);
    g.cursorCol = 40;
    g.cursorVisible = true;
}

std::vector<uint8_t> renderSyntheticFrame(int w, int h, int scroll, const std::string& family, int px,
                                          std::string* resolved, int scale) {
    GridPainter painter(family, px, scale);
    if (resolved) *resolved = painter.resolvedFamily();
    const int m = kMargin * painter.scale();
    Grid g;
    g.resize((h - 2 * m) / painter.cellHeight(), (w - 2 * m) / painter.cellWidth());
    fillSyntheticGrid(g, scroll);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(s);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    painter.paint(cr, g, m, m, Color8{208, 208, 208});
    cairo_destroy(cr);
    std::vector<uint8_t> out;
    surfaceToRgba(s, out);
    cairo_surface_destroy(s);
    return out;
}

}  // namespace lterm
