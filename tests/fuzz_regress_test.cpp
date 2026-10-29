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
    // F2: an argument with more digits than a long holds (signed overflow in libvterm's
    // parser, UBSan) or >= 2^31 (collides with CSI_ARG_FLAG_MORE). At most 9 significant digits
    // are kept; leading zeros do not count.
    {
        VtermSession s(5, 20);
        feed(s, "\x1b[99999999999999999999999999;99999999999999999999999Hx");
        Grid g;
        s.fillGrid(g);
        CHECK(g.at(4, 19).text == "x");  // clamped to the bottom-right corner
        feed(s, "\x1b[00000000000000000000003;00000000000000000000000004Hy");
        s.fillGrid(g);
        CHECK(g.at(2, 3).text == "y");
        feed(s, "\x1b[2147483648Cz\x1b[4294967295;1Hw");
        s.fillGrid(g);
        CHECK(g.at(4, 0).text == "w");
    }
    // F3: libvterm 0.3.3 abort()s ("screen_resize failed to update cursor position") when the
    // cursor's wrapped line no longer fits after a resize and the cursor is on its last row.
    {
        struct Case { int r, c; const char* text; int nr, nc; };
        const Case cases[] = {{5, 3, "abcdefg", 1, 3}, {5, 3, "abcdefg", 2, 1}, {14, 1, "ab", 1, 1},
                              {3, 1, "ab", 1, 1},      {5, 2, "abcd", 1, 2},    {24, 80, "", 1, 1}};
        for (const Case& k : cases) {
            VtermSession s(k.r, k.c);
            feed(s, k.text);
            s.resize(k.nr, k.nc);  // used to abort() the process
            CHECK(s.rows() == k.nr && s.cols() == k.nc);
            feed(s, "\r\x1b[KZ");
            CHECK(s.rowText(s.rows() - 1).find('Z') != std::string::npos || s.rowText(0).find('Z') != std::string::npos);
            s.resize(k.r, k.c);
            CHECK(s.rows() == k.r);
        }
        // A long wrapped prompt line on the alternate screen, shrunk to one row.
        VtermSession a(6, 10);
        feed(a, "\x1b[?1049h");
        feed(a, std::string(35, 'q'));
        a.resize(1, 10);
        a.resize(6, 10);
        feed(a, "\x1b[?1049l");
        CHECK(a.reflowFallbacks() >= 1);
        // The usual case still reflows (no fallback): a wrapped line that fits.
        VtermSession n(5, 10);
        feed(n, "0123456789abcde\r\n$ ");
        n.resize(5, 20);
        CHECK(n.reflowFallbacks() == 0 && n.rowText(0) == "0123456789abcde");
    }
    // F4 (found reading the code for item 5, then confirmed): rowText() used a fixed 4 KiB
    // buffer, but a row can need more (250 cells of a letter + 5 four-byte combining marks =
    // 5250 bytes), and libvterm returns the length it needed: std::string(buf, n) read past it.
    {
        VtermSession s(2, 250);
        std::string cell = "a";
        for (int i = 0; i < 5; ++i) cell += "\xf0\x9d\x85\xa7";  // U+1D167 COMBINING TREMOLO-1
        std::string row;
        for (int i = 0; i < 250; ++i) row += cell;
        feed(s, row);
        const std::string t = s.rowText(0);
        CHECK(t.size() == row.size());
        CHECK(t == row);
    }
    // F5: feed()'s guard state machine must follow libvterm's. ESC P starts a DCS whose command
    // bytes are not a string yet, so ESC [ right after it is a CSI to libvterm; feed() thought
    // it was inside the string and let 19-digit / 17-argument CSIs through to the overflows.
    // Inside a real string, ESC [ is not a CSI (libvterm ends the string, no C1 hoisting).
    {
        VtermSession s(5, 20);
        std::string q = "\x1bP\x1b[";
        for (int i = 0; i < 30; ++i) q += "9;";
        q += "99999999999999999999Hx";
        feed(s, q);
        Grid g;
        s.fillGrid(g);
        CHECK(g.at(4, 8).text == "x");  // CSI 9;9 H (arguments 17+ dropped), clamped
        VtermSession t(5, 20);
        feed(t, "\x1b]2;abc\x1b[2Jy");  // the OSC ends; "[2Jy" is text after an unknown ESC [
        CHECK(t.rowText(0) == "2Jy");
        // ESC 7 right after a string's ESC is DECSC, as libvterm sees it.
        VtermSession u(5, 20);
        feed(u, "\x1b(0\x1b]2;t\x1b" "7\x1b(B\x1b" "8q");
        CHECK(u.rowText(0) == "\xe2\x94\x80");  // DEC line drawing restored with the cursor
    }
    // F6: OSC command numbers overflow libvterm's int (UB); feed() keeps 9 digits.
    {
        VtermSession s(3, 20);
        std::string seen;
        s.onTitle = [&] { seen = s.title(); };
        feed(s, "\x1b]99999999999999999999999;x\x07\x1b]00000000000000000002;ok\x07");
        CHECK(seen == "ok");
    }
    if (failures == 0) std::printf("fuzz_regress_test: OK\n");
    return failures ? 1 : 0;
}
