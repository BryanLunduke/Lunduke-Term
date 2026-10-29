// SPDX-License-Identifier: GPL-3.0-or-later
// Scrollback search over a LineSource (history + screen). Soft-wrapped lines are searched as one
// logical line, so a match may continue on the next row. Case-insensitive search uses Unicode
// case folding per cell (g_utf8_casefold).
#pragma once

#include "selection.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lterm {

struct SearchMatch {
    SelPos start, end;  // inclusive cell range, like Selection
    bool contains(long line, int col) const {
        const SelPos p{line, col};
        return !(p < start) && !(end < p);
    }
    bool operator==(const SearchMatch& o) const { return start == o.start && end == o.end; }
    bool operator!=(const SearchMatch& o) const { return !(*this == o); }
};

// All matches in line order (oldest first), at most maxMatches (the oldest ones). Empty query =
// no matches. The M2 reference implementation (reads every cell); the view uses SearchIndex.
std::vector<SearchMatch> findAll(const LineSource& src, const std::string& query, bool caseSensitive,
                                 size_t maxMatches = 10000);

// Incremental search cache (M3). The scrollback never changes once a line is in it, so its
// case-folded text is built once (in time-limited slices, see update()) and kept; only lines that
// scrolled in since, and the screen, are read again. A search is then one memmem() pass over the
// cached text. Results: the newest maxMatches matches, in line order (oldest first).
class SearchIndex {
public:
    // Extends the cache until it covers the scrollback or deadlineUs (g_get_monotonic_time)
    // passes; deadlineUs <= 0 means no limit. Returns true when the cache is complete. Starts
    // over by itself when the source's contentEpoch(), its width or the case mode changed.
    bool update(const LineSource& src, bool caseSensitive, long long deadlineUs = 0);
    bool complete(const LineSource& src, bool caseSensitive) const;
    // Lines the cache does not cover yet are read here (slow for a big uncached scrollback, so
    // call update() until it returns true first).
    std::vector<SearchMatch> find(const LineSource& src, const std::string& query, size_t maxMatches = 10000) const;
    void reset();
    size_t cachedLines() const;   // logical lines in the cache
    size_t memoryBytes() const;

    struct Row {
        long line;
        uint32_t off;  // byte offset of the row's text in Chunk::text
        int32_t map;   // -1: one byte per cell (byte i = column i), else an index into Chunk::maps
    };
    struct Chunk {
        std::string text;  // logical lines, each followed by '\n'
        std::vector<Row> rows;
        std::vector<std::vector<uint16_t>> maps;  // per byte: column | 0x8000 not a cell start | 0x4000 wide
        long first = 0, end = 0;                  // line range [first, end)
    };

private:
    bool caseSensitive_ = false;
    int cols_ = -1;
    unsigned long epoch_ = 0;
    long next_ = 0;         // first line not cached yet
    bool frontier_ = false; // the remaining lines are not stable yet (screen / wrapped into it)
    std::vector<Chunk> chunks_;
};

// Appends the logical line that starts at `line` (rows joined while soft-wrapped, never past
// `limit`) to `c`. Returns one past its last row, or -1 if it would continue at or past `limit`
// while `stopAtLimit` is set.
long appendLogicalLine(const LineSource& src, long line, long limit, bool caseSensitive, bool stopAtLimit,
                       SearchIndex::Chunk& c);

// Index of the match to go to from `from` (exclusive): the next one after it (forward) or the
// previous one before it (backward), wrapping around. -1 when there are no matches.
int stepMatch(const std::vector<SearchMatch>& m, SelPos from, bool backward);

}  // namespace lterm
