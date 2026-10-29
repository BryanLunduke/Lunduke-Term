// SPDX-License-Identifier: GPL-3.0-or-later
// M4 item 6: regression tests for what tools/fuzz/vterm_fuzz found (docs/FUZZING.md). Each case
// is the minimised trigger, written out by hand, plus a check that the terminal still works.
#include "vterm_session.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace lterm;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void feed(VtermSession& s, const std::string& str) { s.feed(str.data(), str.size()); }

int main() {
    // F1: a CSI with more than 16 arguments. libvterm 0.3.3 writes args[16+] past its fixed
    // array, over the parser's callbacks pointer: the system libvterm0 segfaults on
    // printf '\e[1;2;3;...;20m'. feed() now drops the arguments after the 16th.
    {
        VtermSession s(4, 40);
        std::string q = "\x1b[";
        for (int i = 1; i <= 40; ++i) q += std::to_string(i % 10) + ";";
        q += "1mA";
        feed(s, q);
        CHECK(s.rowText(0) == "A");
        // Split over many feed() calls (the PTY delivers arbitrary chunks).
        VtermSession t(4, 40);
        for (char c : "\x1b[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;1mB") if (c) feed(t, std::string(1, c));
        CHECK(t.rowText(0) == "B");
        // SGR 38:2:r:g:b subparameters count as arguments too.
        VtermSession u(4, 40);
        std::string sub = "\x1b[";
        for (int i = 0; i < 30; ++i) sub += "38:2:1:2:3;";
        sub += "mC\x1b[31mD";
        feed(u, sub);
        CHECK(u.rowText(0) == "CD");
        // 16 arguments (15 separators) still all arrive: the 16th sets bold.
        VtermSession v(4, 40);
        feed(v, "\x1b[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;1mE");
        Grid g;
        v.fillGrid(g);
        CHECK(g.at(0, 0).text == "E" && g.at(0, 0).bold);
        // S / M with too many arguments keep working (they take the wrap-snapshot path).
        VtermSession w(4, 10);
        feed(w, "a\r\nb\r\nc\r\nd\x1b[1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1S");
        CHECK(w.rowText(0) == "b");
    }
    if (failures == 0) std::printf("fuzz_regress_test: OK\n");
    return failures ? 1 : 0;
}
