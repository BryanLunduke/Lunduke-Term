// SPDX-License-Identifier: GPL-3.0-or-later
#include "search.hpp"

#include <glib.h>

#include <algorithm>
#include <cstring>

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

// ---- M3: SearchIndex ----

namespace {
constexpr size_t kChunkBytes = 256 * 1024;
constexpr uint16_t kNotStart = 0x8000, kWide = 0x4000, kColMask = 0x3fff;

void appendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) {
        s += static_cast<char>(0xC0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += static_cast<char>(0xE0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    }
}
}  // namespace

long appendLogicalLine(const LineSource& src, long line, long limit, bool caseSensitive, bool stopAtLimit,
                       SearchIndex::Chunk& c) {
    const int cols = std::min(src.cols(), static_cast<int>(kColMask));
    const size_t text0 = c.text.size(), rows0 = c.rows.size(), maps0 = c.maps.size();
    std::vector<uint16_t> map;
    std::string cell;
    long l = line;
    for (;;) {
        SearchIndex::Row row{l, static_cast<uint32_t>(c.text.size()), -1};
        bool simple = true;
        map.clear();
        for (int col = 0; col < cols; ++col) {
            uint32_t cp[8];
            int w = 1;
            const int n = src.cellChars(l, col, cp, 8, &w);
            if (w == 0) {  // right half of a wide character
                simple = false;
                continue;
            }
            const size_t before = c.text.size();
            if (n == 0) c.text += ' ';
            else if (n == 1 && cp[0] < 0x80) {
                c.text += static_cast<char>(caseSensitive ? cp[0] : static_cast<uint32_t>(g_ascii_tolower(static_cast<char>(cp[0]))));
            } else {
                cell.clear();
                for (int i = 0; i < n; ++i) appendUtf8(cell, cp[i]);
                if (!caseSensitive) {
                    gchar* f = g_utf8_casefold(cell.c_str(), static_cast<gssize>(cell.size()));
                    if (f) cell = f;
                    g_free(f);
                }
                c.text += cell;
            }
            const size_t len = c.text.size() - before;
            if (len != 1 || w != 1) simple = false;
            for (size_t i = 0; i < len; ++i)
                map.push_back(static_cast<uint16_t>(col | (i ? kNotStart : 0) | (w == 2 ? kWide : 0)));
        }
        if (!simple) {
            row.map = static_cast<int32_t>(c.maps.size());
            c.maps.push_back(map);
        }
        c.rows.push_back(row);
        const bool wraps = src.wrapped(l);
        if (!wraps) break;
        if (l + 1 >= limit) {
            if (stopAtLimit) {  // continues in lines that may still change: not cacheable yet
                c.text.resize(text0);
                c.rows.resize(rows0);
                c.maps.resize(maps0);
                return -1;
            }
            break;
        }
        ++l;
    }
    c.text += '\n';
    c.end = l + 1;
    return l + 1;
}

void SearchIndex::reset() {
    chunks_.clear();
    cols_ = -1;
    next_ = 0;
    frontier_ = false;
}

bool SearchIndex::complete(const LineSource& src, bool caseSensitive) const {
    if (caseSensitive != caseSensitive_ || cols_ != src.cols() || epoch_ != src.contentEpoch()) return false;
    return next_ >= src.stableEnd() || frontier_;
}

bool SearchIndex::update(const LineSource& src, bool caseSensitive, long long deadlineUs) {
    if (caseSensitive != caseSensitive_ || cols_ != src.cols() || epoch_ != src.contentEpoch()) {
        reset();
        caseSensitive_ = caseSensitive;
        cols_ = src.cols();
        epoch_ = src.contentEpoch();
        next_ = src.firstLine();
    }
    const long first = src.firstLine(), stable = src.stableEnd();
    // History that scrolled away: drop whole chunks (a partly expired one is filtered in find()).
    size_t drop = 0;
    while (drop < chunks_.size() && chunks_[drop].end <= first) ++drop;
    if (drop) chunks_.erase(chunks_.begin(), chunks_.begin() + static_cast<long>(drop));
    if (next_ < first) {
        next_ = first;
        frontier_ = false;
    }
    if (frontier_ && next_ < stable) frontier_ = false;  // more lines became stable: try again
    int n = 0;
    while (next_ < stable && !frontier_) {
        if (chunks_.empty() || chunks_.back().text.size() >= kChunkBytes) {
            if (!chunks_.empty()) {  // full: give back the growth slack
                chunks_.back().text.shrink_to_fit();
                chunks_.back().rows.shrink_to_fit();
            }
            chunks_.emplace_back();
            chunks_.back().text.reserve(kChunkBytes + 4096);
            chunks_.back().first = chunks_.back().end = next_;
        }
        const long e = appendLogicalLine(src, next_, stable, caseSensitive_, true, chunks_.back());
        if (e < 0) {
            frontier_ = true;  // the last scrollback line wraps onto the screen
            break;
        }
        next_ = e;
        if (deadlineUs > 0 && (++n & 63) == 0 && g_get_monotonic_time() >= deadlineUs) break;
    }
    if (!chunks_.empty() && chunks_.back().rows.empty()) chunks_.pop_back();
    return next_ >= stable || frontier_;
}

size_t SearchIndex::cachedLines() const {
    size_t n = 0;
    for (const auto& c : chunks_) n += static_cast<size_t>(std::count(c.text.begin(), c.text.end(), '\n'));
    return n;
}

size_t SearchIndex::memoryBytes() const {
    size_t b = 0;
    for (const auto& c : chunks_) {
        b += c.text.capacity() + c.rows.capacity() * sizeof(Row);
        for (const auto& m : c.maps) b += m.capacity() * sizeof(uint16_t);
    }
    return b;
}

namespace {
// Matches of q in one chunk, in order.
void findInChunk(const SearchIndex::Chunk& c, const std::string& q, int cols, long firstLine,
                 std::vector<SearchMatch>& out) {
    if (c.rows.empty() || c.text.size() < q.size()) return;
    auto locate = [&](size_t pos, bool* start, bool* wide) {
        auto it = std::upper_bound(c.rows.begin(), c.rows.end(), pos,
                                   [](size_t p, const SearchIndex::Row& r) { return p < r.off; });
        const SearchIndex::Row& r = *(it - 1);
        const size_t i = pos - r.off;
        if (r.map < 0) {
            *start = true;
            *wide = false;
            return SelPos{r.line, static_cast<int>(i)};
        }
        const uint16_t m = c.maps[static_cast<size_t>(r.map)][i];
        *start = !(m & kNotStart);
        *wide = (m & kWide) != 0;
        return SelPos{r.line, m & kColMask};
    };
    const char* base = c.text.data();
    const size_t len = c.text.size();
    size_t pos = 0;
    while (pos + q.size() <= len) {
        const void* hit = memmem(base + pos, len - pos, q.data(), q.size());
        if (!hit) break;
        pos = static_cast<size_t>(static_cast<const char*>(hit) - base);
        bool st, wide;
        SearchMatch m;
        m.start = locate(pos, &st, &wide);
        if (!st) {  // inside a cell (folding changed a multi-byte character)
            ++pos;
            continue;
        }
        m.end = locate(pos + q.size() - 1, &st, &wide);
        if (wide && m.end.col + 1 < cols) ++m.end.col;
        if (m.start.line >= firstLine) out.push_back(m);
        pos += q.size();
    }
}
}  // namespace

std::vector<SearchMatch> SearchIndex::find(const LineSource& src, const std::string& query, size_t maxMatches) const {
    std::vector<SearchMatch> out;
    if (query.empty() || maxMatches == 0 || query.find('\n') != std::string::npos ||
        !g_utf8_validate(query.c_str(), static_cast<gssize>(query.size()), nullptr))
        return out;
    const std::string q = fold(query, caseSensitive_);
    const int cols = src.cols();
    const long first = src.firstLine();
    // The part that is not cached (screen, the line wrapping onto it), read now.
    Chunk tail;
    long l = std::max(next_, first);
    tail.first = l;
    while (l < src.endLine()) l = appendLogicalLine(src, l, src.endLine(), caseSensitive_, false, tail);
    // Newest first, so the cap keeps the newest matches (the ones Enter/Shift+Enter start from).
    std::vector<std::vector<SearchMatch>> parts;
    size_t total = 0;
    std::vector<SearchMatch> part;
    findInChunk(tail, q, cols, first, part);
    total += part.size();
    parts.push_back(std::move(part));
    for (size_t i = chunks_.size(); i-- > 0 && total < maxMatches;) {
        part.clear();
        findInChunk(chunks_[i], q, cols, first, part);
        total += part.size();
        parts.push_back(std::move(part));
    }
    out.reserve(std::min(total, maxMatches));
    for (size_t i = parts.size(); i-- > 0;) out.insert(out.end(), parts[i].begin(), parts[i].end());
    if (out.size() > maxMatches) out.erase(out.begin(), out.begin() + static_cast<long>(out.size() - maxMatches));
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
