// SPDX-License-Identifier: GPL-3.0-or-later
// M4 item 6: regression tests for what tools/fuzz/vterm_fuzz found (docs/FUZZING.md). Each case
// is the minimised trigger, written out by hand, plus a check that the terminal still works.
#include "vterm_session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

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
    // The F7/F12/F13 cases hang libvterm without their fix: fail (SIGALRM) instead of hanging.
    alarm(120);
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
                              {3, 1, "ab", 1, 1},      {5, 2, "abcd", 1, 2},    {24, 80, "", 1, 1},
                              {14, 2, "abcdef", 1, 2}, {5, 3, "abcdefgh", 2, 2}};  // (1 column is 2 since F8)
        for (const Case& k : cases) {
            VtermSession s(k.r, k.c);
            feed(s, k.text);
            s.resize(k.nr, k.nc);  // used to abort() the process
            CHECK(s.rows() == k.nr && s.cols() == std::max(2, k.nc));
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
    // F7: REP (CSI Ps b) repeats the last printed glyph by its width; libvterm 0.3.3 loops forever
    // when that width is 0 (nothing printed yet, or a lone combining mark). feed() drops REP then.
    {
        VtermSession s(3, 20);
        feed(s, "\x1b[b");  // nothing printed yet (used to hang)
        feed(s, "\x1b[H\xcc\x81\x1b[5b");  // a lone U+0301 (used to hang)
        feed(s, "ok");
        CHECK(s.rowText(0) == "ok");
        // REP still works after a normal, wide or combined glyph, also split over chunks.
        VtermSession t(3, 20);
        feed(t, "x\x1b[3b");
        feed(t, "\r\n\xe6\xbc\xa2\x1b[");
        feed(t, "2b\r\ne\xcc\x81\x1b[1b");
        CHECK(t.rowText(0) == "xxxx");
        // libvterm counts REP in cells (xterm: characters): 2 more cells = one more 漢.
        CHECK(t.rowText(1) == "\xe6\xbc\xa2\xe6\xbc\xa2");
        CHECK(t.rowText(2) == "e\xcc\x81" "e\xcc\x81" || t.rowText(2) == "e\xcc\x81");  // dropped = conservative
        // A REP whose start arrived in an earlier chunk is aborted cleanly.
        VtermSession u(3, 20);
        feed(u, "\x1b[H\xcc\x81\x1b[9");
        feed(u, "9bz");
        CHECK(u.rowText(0) == "z");
    }
    // F8: a wide character on a 1-column screen: libvterm writes the (missing) second cell
    // through a NULL pointer. Sessions are at least 2 columns wide now.
    {
        VtermSession s(3, 1);
        CHECK(s.cols() == 2);
        feed(s, "\xe6\xbc\xa2\xe5\xad\x97x");
        CHECK(s.rowText(0) == "\xe6\xbc\xa2" && s.rowText(1) == "\xe5\xad\x97");
        VtermSession t(3, 10);
        t.resize(3, 1);
        CHECK(t.cols() == 2);
        feed(t, "\r\n\xe6\xbc\xa2");
        t.resize(0, 0);
        feed(t, "\xe6\xbc\xa2");
        CHECK(t.rows() == 1 && t.cols() == 2);
    }
    // F9: DECSC / ?1049h save the cursor; libvterm does not clamp the saved position when the
    // screen shrinks, so DECRC put the cursor below the screen and the next character read past
    // libvterm's lineinfo[] (heap overflow). feed() now clamps the cursor after each restore.
    {
        VtermSession s(37, 100);
        feed(s, "\x1b[30;90H\x1b" "7");
        s.resize(6, 50);
        feed(s, "\x1b" "8x");
        Grid g;
        s.fillGrid(g);
        CHECK(g.at(5, 49).text == "x");
        // The same through the alternate screen (?1049h saves, ?1049l restores).
        VtermSession t(30, 80);
        feed(t, "\x1b[25;70H\x1b[?1049h");
        t.resize(5, 40);
        feed(t, "\x1b[?1049ly");
        t.fillGrid(g);
        CHECK(g.at(4, 39).text == "y");
        // In origin mode with a scroll region the cursor ends up inside the region.
        VtermSession u(30, 80);
        feed(u, "\x1b[28;70H\x1b" "7\x1b[2;4r\x1b[?6h");
        u.resize(10, 40);
        feed(u, "\x1b" "8z");
        u.fillGrid(g);
        int zr = -1;
        for (int r = 0; r < 10; ++r)
            if (u.rowText(r).find('z') != std::string::npos) zr = r;
        CHECK(zr >= 0 && zr < 10);
    }
    // F10: a scroll region below the new bottom (vttest's "[12..13]" then a shrink to 5 rows):
    // libvterm clamped only the bottom margin, top > bottom, and the next LF memmove()d a
    // negative size. resize() resets the margins when a region was set and the screen shrinks.
    {
        VtermSession s(24, 80);
        feed(s, "\x1b[12;13r\x1b[13;5H");
        s.resize(5, 40);
        feed(s, "a\nb\nc\nd\ne\nf");
        CHECK(s.rowText(4).find('f') != std::string::npos);  // full-screen scrolling again
        // A shrink in the middle of a sequence: it is cancelled, the guard stays in step.
        VtermSession t(24, 80);
        feed(t, "\x1b[12;13r\x1b[1;3");
        t.resize(5, 40);
        feed(t, "1mX\n\n\n\n\nY");
        CHECK(t.rowText(4).find('Y') != std::string::npos);
        // Left/right margins (DECLRMM) past the new width.
        VtermSession u(10, 80);
        feed(u, "\x1b[?69h\x1b[60;70s\x1b[1;65H");
        u.resize(10, 30);
        feed(u, "\x1b[?69hz\r\n");
        Grid g;
        u.fillGrid(g);
        CHECK(u.rowText(0).find('z') != std::string::npos);
        // Without a region set, a shrink does not touch the cursor or anything else.
        VtermSession v(24, 80);
        feed(v, "\x1b[10;20H");
        v.resize(20, 70);
        feed(v, "q");
        v.fillGrid(g);
        CHECK(g.at(9, 19).text == "q");
    }
    // F11: resize() gives the application its pen back with penSgr(), fed past the guard. A pen
    // with every attribute and two RGB colours was one SGR of 20 arguments, over libvterm's 16.
    {
        VtermSession s(10, 60);
        feed(s, "\x1b[1;3;5;7;8;9;4:3;11;73m\x1b[38;2;1;2;3m\x1b[48;2;4;5;6m");
        s.resize(9, 23);
        feed(s, "X");
        Grid g;
        s.fillGrid(g);
        const Cell& x = g.at(0, 0);
        CHECK(x.text == "X");
        CHECK(x.bold && x.italic);
        // Reverse is on: the background shows the foreground colour.
        CHECK(x.bg.r == 1 && x.bg.g == 2 && x.bg.b == 3);
    }
    // F12 (review): libvterm ignores a byte >= 0x80 after ESC and stays in the escape, so
    // ESC 0xC3 [ is still a CSI. The guard went back to plain text there: F1 (40 arguments,
    // segfault) and F7 (REP with nothing printed, hang) came back, also inside a title string.
    {
        std::string args;
        for (int i = 0; i < 40; ++i) args += "1;";
        const std::string cases[] = {
            "\x1b\xc3[" + args + "mA",                 // probe case 1
            "\x1b]0;t\x1b\xc3[" + args + "mA",         // probe case 2: ESC inside an OSC
            "\x1bPq\x1b\xc3[" + args + "mA",           // ... and inside a DCS string
            "\x1b\xc3\xa9\xff[" + args + "mA",         // several high bytes
            "\x1b\xc3[99999999999999999999mA",         // F2 through the same gap
        };
        for (const std::string& q : cases) {
            VtermSession s(4, 40);
            feed(s, q);
            CHECK(s.rowText(0) == "A");
            VtermSession t(4, 40);  // byte at a time
            for (char c : q) feed(t, std::string(1, c));
            CHECK(t.rowText(0) == "A");
        }
        VtermSession u(4, 40);
        feed(u, "\x1b\xc3[bZ");  // probe case 5 (used to hang)
        CHECK(u.rowText(0) == "Z");
        // The CSI after the high byte is a real CSI (libvterm runs it): bold A.
        VtermSession v(4, 40);
        feed(v, "\x1b\xc3[1mA");
        Grid g;
        v.fillGrid(g);
        CHECK(g.at(0, 0).text == "A" && g.at(0, 0).bold);
    }
    // F13 (review): escapes with intermediates. The guard ended ESC SP F and ESC # 8 at the
    // second byte and took F / 8 for printed text, so a REP right after (nothing printed in
    // libvterm's eyes) hung. libvterm collects 0x20-0x2F, then the final byte ends the escape.
    {
        const char* cases[] = {
            "\x1b F\x1b[bZ",         // probe case 6: S7C1T
            "\x1b#8\x1b[bZ",         // probe case 7: DECALN (fills with E, but is not "printed")
            "\x1b(\xc3" "B\x1b[bZ",  // probe case 8: high byte inside ESC ( (F12 + F13)
            "\x1b( \x01" "B\x1b[bZ", // C0 inside, two intermediates
            "\x1b#\x7f" "8\x1b[bZ",  // DEL inside
        };
        for (const char* q : cases) {
            VtermSession s(4, 40);
            feed(s, q);
            CHECK(s.rowText(0).find('Z') != std::string::npos);
            VtermSession t(4, 40);
            for (const char* p = q; *p; ++p) feed(t, std::string(1, *p));
            CHECK(t.rowText(0).find('Z') != std::string::npos);
        }
        // DECALN still fills the screen; designations still work, also split.
        VtermSession u(2, 4);
        feed(u, "\x1b#8");
        CHECK(u.rowText(0) == "EEEE");
        VtermSession v(2, 10);
        feed(v, "\x1b(");
        feed(v, "0q\x1b(Bq");
        CHECK(v.rowText(0) == "\xe2\x94\x80q");  // DEC graphics q = U+2500, then ASCII again
        // Two intermediates are not an SCS for libvterm, so the set is not changed.
        VtermSession w(2, 10);
        feed(w, "\x1b(!0q");
        CHECK(w.rowText(0) == "q");
    }
    // F14 (found by the fuzzer's differential check): a C0 control inside the OSC command number
    // made the guard take the rest for string text, while libvterm stays in its command state
    // and keeps multiplying an int (signed overflow, UBSan in the fuzz build). With wrap-around,
    // 4294967298 = 2^32 + 2 is OSC 2, the title, so the overflow is visible here too.
    {
        const std::string q = std::string("\x1b]\x01") + "4294967298;T\x07" "A";
        VtermSession s(4, 20);
        feed(s, q);
        CHECK(s.title().empty() && s.rowText(0) == "A");
        VtermSession t(4, 20);
        for (char c : q) {
            feed(t, std::string(1, c));
            if (c == '\x01') CHECK(t.guardState() == 7);  // still the command number
        }
        CHECK(t.title().empty() && t.rowText(0) == "A");
        // A real OSC 2 after a C0 control still sets the title; NUL / DEL are ignored there too.
        VtermSession u(4, 20);
        feed(u, std::string("\x1b]\x01\x7f") + std::string(1, '\0') + "2;T\x07");
        CHECK(u.title() == "T");
    }
    if (failures == 0) std::printf("fuzz_regress_test: OK\n");
    return failures ? 1 : 0;
}
