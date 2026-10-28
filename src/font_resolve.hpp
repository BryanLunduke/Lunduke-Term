// SPDX-License-Identifier: GPL-3.0-or-later
// Picks the first *installed* family from a Pango-style family list ("A, B, Monospace").
// Pango/fontconfig would otherwise silently substitute a missing family with the default
// sans font, which is proportional and breaks the cell grid.
#pragma once

#include <string>
#include <vector>

namespace lterm {

struct FontPick {
    std::string family;     // the family to hand to Pango (always non-empty)
    int px = 0;             // pixel size for that family (0: caller's default)
    bool fallback = false;  // true when the first choice was not installed
    bool monospace = true;  // fontconfig reports the picked font as mono/charcell
};

struct FontChoice {
    std::string family;
    int px = 0;  // 0: the profile's default size
};

// Splits "A, B ,C" into {"A","B","C"} (empty items dropped).
std::vector<std::string> splitFamilies(const std::string& list);
// True when fontconfig has a font whose family (any of its names) equals `family`,
// case-insensitively. Generic aliases (monospace, sans, serif...) count as installed.
bool familyInstalled(const std::string& family);
// First installed entry wins; if none is installed the result is "Monospace".
FontPick pickFont(const std::vector<FontChoice>& choices);
FontPick pickFont(const std::string& familyList, int px = 0);

}  // namespace lterm
