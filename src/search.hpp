// SPDX-License-Identifier: GPL-3.0-or-later
// Scrollback search over a LineSource (history + screen). Soft-wrapped lines are searched as one
// logical line, so a match may continue on the next row. Case-insensitive search uses Unicode
// case folding per cell (g_utf8_casefold).
#pragma once

#include "selection.hpp"

#include <string>
#include <vector>

namespace lterm {

struct SearchMatch {
    SelPos start, end;  // inclusive cell range, like Selection
    bool contains(long line, int col) const {
        const SelPos p{line, col};
        return !(p < start) && !(end < p);
    }
};

// All matches in line order (oldest first), at most maxMatches. Empty query = no matches.
std::vector<SearchMatch> findAll(const LineSource& src, const std::string& query, bool caseSensitive,
                                 size_t maxMatches = 10000);

// Index of the match to go to from `from` (exclusive): the next one after it (forward) or the
// previous one before it (backward), wrapping around. -1 when there are no matches.
int stepMatch(const std::vector<SearchMatch>& m, SelPos from, bool backward);

}  // namespace lterm
