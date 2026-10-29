// SPDX-License-Identifier: GPL-3.0-or-later
// M4 item 6: libFuzzer harness for the terminal's input side. One input is a small program:
// the first bytes pick the screen size and scrollback limit, then each op either feeds a chunk
// of terminal output (VtermSession::feed, split at arbitrary points), resizes in the middle of
// it, sends keys/characters/pastes/mouse/focus (the input paths that produce bytes for the
// child), or reads everything back the way the view does (fillGrid, cellText/cellChars, rowText,
// selection text, search). Built with clang -fsanitize=fuzzer,address,undefined against
// libvterm 0.3.3 compiled from source with the same flags (tools/fuzz/build.sh).
//
// Differential check (review of M4): feed() keeps its own copy of libvterm's escape-parser state
// to stop the inputs that crash or hang libvterm (F1, F7, F12, F13). If the two disagree, a
// sequence the guard thinks is over (or is plain text) is still open in libvterm, and the guard's
// checks can be walked around. After every feed (and, for half of the feed ops, after every
// single byte) the harness compares feed()'s state with libvterm's own parser struct (read
// through vterm_internal.h, which is why this works only in this build) and aborts on a
// mismatch. See docs/FUZZING.md for what it does and does not cover.
#include "search.hpp"
#include "selection.hpp"
#include "vterm_session.hpp"

#include <fuzzer/FuzzedDataProvider.h>

extern "C" {
#include "vterm_internal.h"  // libvterm 0.3.3 source tree (tools/fuzz/build.sh: -I$VT/src)
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace lterm;

// LTERM_FUZZ_TRACE=1: print the ops of an input (to turn a crash into a regression test).
static const bool kTrace = std::getenv("LTERM_FUZZ_TRACE") != nullptr;
static void traceStr(const char* what, const std::string& s) {
    if (!kTrace) return;
    std::fprintf(stderr, "%s \"", what);
    for (unsigned char c : s) std::fprintf(stderr, c >= 0x20 && c < 0x7f && c != '"' && c != '\\' ? "%c" : "\\x%02x", c);
    std::fprintf(stderr, "\"\n");
}

// The class of libvterm's parser state in feed()'s numbering (csiState_; 1 and 3 differ in
// whether intermediates were collected).
static int parserClass(const VTerm* vt, int* inters) {
    const auto& p = vt->parser;
    using PS = decltype(p.state);  // enum nested in the C struct
    const bool str = p.state >= PS::OSC_COMMAND;
    *inters = 0;
    if (p.in_esc && str) return 5;
    if (p.in_esc) {
        *inters = p.intermedlen;
        return p.intermedlen ? 3 : 1;
    }
    switch (p.state) {
    case PS::NORMAL: return 0;
    case PS::CSI_LEADER:
    case PS::CSI_ARGS:
    case PS::CSI_INTERMED: return 2;
    case PS::DCS_COMMAND: return 6;
    case PS::OSC_COMMAND: return 7;
    default: return 4;  // OSC, DCS, APC, PM, SOS strings
    }
}

static void checkGuard(const VtermSession& s, const char* where) {
    int inters = 0;
    const int lv = parserClass(s.vtermForCheck(), &inters);
    const int g = s.guardState();
    // libvterm stops counting intermediates at INTERMED_MAX - 1.
    const int gi = std::min(s.guardIntermediates(), INTERMED_MAX - 1);
    if (lv == g && (g != 3 || gi == inters)) return;
    // F15: in state 5 feed() keeps the ESC back, so libvterm is still in the plain string.
    if (g == 5 && s.guardHoldsEsc() && (lv == 4 || lv == 7)) return;
    std::fprintf(stderr, "guard/libvterm parser mismatch after %s: guard state %d (intermediates %d), libvterm %d "
                 "(state %d, in_esc %d, intermedlen %d)\n", where, g, gi, lv,
                 static_cast<int>(s.vtermForCheck()->parser.state), static_cast<int>(s.vtermForCheck()->parser.in_esc),
                 s.vtermForCheck()->parser.intermedlen);
    std::abort();
}

static void readBack(const VtermSession& s, FuzzedDataProvider& fdp) {
    Grid g;
    s.fillGrid(g, fdp.ConsumeIntegralInRange<int>(0, 3000));
    for (int r = 0; r < s.rows(); ++r) (void)s.rowText(r);
    const long a = s.firstLine(), b = s.endLine();
    const long l0 = a + static_cast<long>(fdp.ConsumeIntegralInRange<unsigned>(0, 1u << 16) % static_cast<unsigned long>(b - a));
    for (long l = l0; l < std::min(b, l0 + 4); ++l)
        for (int c = 0; c < s.cols(); ++c) {
            uint32_t cp[8];
            int w = 0;
            (void)s.cellText(l, c);
            (void)s.cellChars(l, c, cp, 8, &w);
            (void)s.wrapped(l);
        }
    Selection sel;
    const SelectMode modes[3] = {SelectMode::Char, SelectMode::Word, SelectMode::Line};
    sel.begin(s, SelPos{l0, fdp.ConsumeIntegralInRange<int>(-2, s.cols() + 2)}, modes[fdp.ConsumeIntegralInRange<int>(0, 2)]);
    sel.extend(s, SelPos{l0 + fdp.ConsumeIntegralInRange<int>(-3, 3), fdp.ConsumeIntegralInRange<int>(-2, s.cols() + 2)});
    (void)sel.text(s);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzedDataProvider fdp(data, size);
    const int rows = fdp.ConsumeIntegralInRange<int>(1, 60);
    const int cols = fdp.ConsumeIntegralInRange<int>(1, 250);
    VtermSession s(rows, cols);
    const size_t sb = fdp.ConsumeIntegralInRange<size_t>(0, 200);
    s.setScrollbackLimit(sb);
    if (kTrace) std::fprintf(stderr, "session %dx%d scrollback %zu\n", rows, cols, sb);
    s.onTitle = [&s] { (void)s.title().size(); };
    SearchIndex idx;
    int ops = 0;
    while (fdp.remaining_bytes() > 0 && ops++ < 400) {
        switch (fdp.ConsumeIntegralInRange<int>(0, 15)) {
        default: {  // most ops: terminal output
            const bool bytewise = fdp.ConsumeBool();
            const std::string chunk = fdp.ConsumeRandomLengthString(512);
            traceStr(bytewise ? "feed bytewise" : "feed", chunk);
            if (bytewise) {
                for (char c : chunk) {
                    s.feed(&c, 1);
                    checkGuard(s, "a byte");
                }
            } else {
                s.feed(chunk);
                checkGuard(s, "a chunk");
            }
            break;
        }
        case 9: {
            const int r = fdp.ConsumeIntegralInRange<int>(0, 70), c = fdp.ConsumeIntegralInRange<int>(0, 260);
            if (kTrace) std::fprintf(stderr, "resize %d %d\n", r, c);
            s.resize(r, c);
            break;
        }
        case 10: {
            const std::string p = fdp.ConsumeRandomLengthString(256);
            traceStr("paste", p);
            s.paste(p);
            break;
        }
        case 11:
            s.sendKey(static_cast<VTermKey>(fdp.ConsumeIntegralInRange<int>(0, VTERM_KEY_MAX - 1)),
                      static_cast<VTermModifier>(fdp.ConsumeIntegralInRange<int>(0, 7)));
            s.sendChar(fdp.ConsumeIntegralInRange<uint32_t>(0, 0x10FFFF),
                       static_cast<VTermModifier>(fdp.ConsumeIntegralInRange<int>(0, 7)));
            break;
        case 12: {
            const int r = fdp.ConsumeIntegralInRange<int>(-2, s.rows() + 2), c = fdp.ConsumeIntegralInRange<int>(-2, s.cols() + 2);
            const VTermModifier m = static_cast<VTermModifier>(fdp.ConsumeIntegralInRange<int>(0, 7));
            if (fdp.ConsumeBool()) s.mouseMove(std::max(r, 0), std::max(c, 0), m);
            else s.mouseButton(std::max(r, 0), std::max(c, 0), fdp.ConsumeIntegralInRange<int>(0, 8), fdp.ConsumeBool(), m);
            s.focus(fdp.ConsumeBool());
            break;
        }
        case 13:
            if (kTrace) std::fprintf(stderr, "readback\n");
            readBack(s, fdp);
            break;
        case 14: {
            const bool cs = fdp.ConsumeBool();
            idx.update(s, cs);
            const std::string q = fdp.ConsumeRandomLengthString(16);
            (void)idx.find(s, q, 64);
            (void)findAll(s, q, cs, 64);
            break;
        }
        case 15:
        {
            const size_t lim = fdp.ConsumeIntegralInRange<size_t>(0, 50);
            if (kTrace) std::fprintf(stderr, "scrollback limit %zu\n", lim);
            s.setScrollbackLimit(lim);
        }
            break;
        }
        (void)s.takeOutput();
        (void)s.takeDamage();
        checkGuard(s, "an op");
    }
    readBack(s, fdp);
    return 0;
}
