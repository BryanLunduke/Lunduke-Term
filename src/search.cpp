// SPDX-License-Identifier: GPL-3.0-or-later
#include "search.hpp"

#include <glib.h>

namespace lterm {

namespace {
std::string fold(const std::string& s, bool caseSensitive) {
    if (caseSensitive || s.empty()) return s;
    if (s.size() == 1 && static_cast<unsigned char>(s[0]) < 0x80)  // fast path: one ASCII cell
        return std::string(1, static_cast<char>(g_ascii_tolower(s[0])));
    gchar* f = g_utf8_casefold(s.c_str(), static_cast<gssize>(s.size()));
    std::string out = f ? f : s;
    g_free(f);
    return out;
}
}  // namespace

std::vector<SearchMatch> findAll(const LineSource& src, const std::string& query, bool caseSensitive,
                                 size_t maxMatches) {
    std::vector<SearchMatch> out;
    if (query.empty() || !g_utf8_validate(query.c_str(), static_cast<gssize>(query.size()), nullptr)) return out;
    const std::string q = fold(query, caseSensitive);
    const int cols = src.cols();
    std::string text;
    std::vector<SelPos> cellOf;  // per byte of `text`: the cell it came from
    std::vector<char> cellStart; // per byte: first byte of its cell
    long line = src.firstLine();
    const long end = src.endLine();
    while (line < end && out.size() < maxMatches) {
        // One logical line: rows joined while they are soft-wrapped.
        text.clear();
        cellOf.clear();
        cellStart.clear();
        long l = line;
        for (;;) {
            for (int c = 0; c < cols; ++c) {
                if (src.cellWidth(l, c) == 0) continue;  // right half of a wide character
                std::string t = src.cellText(l, c);
                if (t.empty()) t = " ";
                t = fold(t, caseSensitive);
                for (size_t i = 0; i < t.size(); ++i) {
                    cellOf.push_back(SelPos{l, c});
                    cellStart.push_back(i == 0);
                }
                text += t;
            }
            if (!src.wrapped(l) || l + 1 >= end) break;
            ++l;
        }
        size_t pos = 0;
        while (out.size() < maxMatches && (pos = text.find(q, pos)) != std::string::npos) {
            if (!cellStart[pos]) {  // inside a cell (folding changed a multi-byte character)
                ++pos;
                continue;
            }
            SearchMatch m;
            m.start = cellOf[pos];
            m.end = cellOf[pos + q.size() - 1];
            if (src.cellWidth(m.end.line, m.end.col) == 2 && m.end.col + 1 < cols) ++m.end.col;
            out.push_back(m);
            pos += q.size();
        }
        line = l + 1;
    }
    return out;
}

int stepMatch(const std::vector<SearchMatch>& m, SelPos from, bool backward) {
    if (m.empty()) return -1;
    const int n = static_cast<int>(m.size());
    if (backward) {
        for (int i = n - 1; i >= 0; --i)
            if (m[i].start < from) return i;
        return n - 1;
    }
    for (int i = 0; i < n; ++i)
        if (from < m[i].start) return i;
    return 0;
}

}  // namespace lterm
