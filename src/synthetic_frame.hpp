// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic stand-in terminal frame for benchmarks and golden comparisons.
#pragma once

#include "grid.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lterm {

constexpr int kMargin = 24;  // px between the tube edge and the text grid

// Fills `g` with a fake shell session (prompt, coloured ls, a progress bar, a cursor).
// `scroll` shifts the content up by that many lines (used to exercise burn-in).
void fillSyntheticGrid(Grid& g, int scroll);

// Renders the synthetic grid into a w x h RGBA8 frame using Pango/Cairo.
std::vector<uint8_t> renderSyntheticFrame(int w, int h, int scroll, const std::string& family,
                                          int pixelSize, std::string* resolvedFamily = nullptr);

}  // namespace lterm
