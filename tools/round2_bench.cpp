// SPDX-License-Identifier: GPL-3.0-or-later
// RSS around a scrollback search: after the lines are in, after the index is built,
// and after the index is dropped, the scrollback recompressed and malloc_trim(0).
#include "search.hpp"
#include "vterm_session.hpp"

#include <cstdio>
#include <fstream>
#include <malloc.h>
#include <string>

using namespace lterm;

static long rssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
    return -1;
}

int main() {
    const int rows = 24, cols = 80, lines = 100000;
    VtermSession s(rows, cols);
    s.setScrollbackLimit(lines);
    std::string chunk;
    chunk.reserve(static_cast<size_t>(lines) * 40);
    for (int i = 0; i < lines; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "line %d the quick brown fox\r\n", i);
        chunk += buf;
    }
    s.feed(chunk);
    s.compressScrollback(true);
    malloc_trim(0);
    const long base = rssKb();
    std::printf("rss base (100k lines, compressed): %ld kB, scrollback %zu bytes\n", base, s.scrollbackBytes());

    SearchIndex index;
    while (!index.update(s, false, 0)) {
    }
    const auto matches = index.find(s, "fox");
    const long searched = rssKb();
    std::printf("rss after search index (%zu matches, index %zu bytes): %ld kB, scrollback %zu bytes\n",
                matches.size(), index.memoryBytes(), searched, s.scrollbackBytes());

    index.reset();
    s.compressScrollback(true);
    malloc_trim(0);
    const long after = rssKb();
    std::printf("rss after close, recompress and malloc_trim: %ld kB, scrollback %zu bytes\n", after,
                s.scrollbackBytes());
    std::printf("delta search %+ld kB, returned %+ld kB\n", searched - base, searched - after);
    return matches.empty() ? 1 : 0;
}
