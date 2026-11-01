<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Fuzz seed corpus

Inputs for `vterm_fuzz` (the libFuzzer harness, `tools/fuzz/build.sh`). **No meson test reads
these files.** The meson test `fuzz-regress` (`tests/fuzz_regress_test.cpp`) has its own small
inline case per finding against the system libvterm0. These seeds only feed the fuzzer: as a
starting corpus, and as a replay with `build-fuzz/vterm_fuzz -runs=0 tools/fuzz/seeds` (or one
file at a time).

The harness reads an input as a program: its size, scrollback and ops come from the end of the
input, the strings to feed from the front. A plain byte stream therefore does **not** reach
libvterm as written. A short one may feed nothing at all. `LTERM_FUZZ_TRACE=1 vterm_fuzz FILE`
prints what an input really does.

## F12–F17 manifest (replay only)

REAL, measured on 2026-11-01 (CST). Each seed was replayed once in fuzz builds of the commits
before and after its fix (`docs/bench/m5/item1b/seed-replay.txt`), and traced on 27a9d46
(`seed-trace.txt`).

| Seed | Finding | What the harness makes of it (trace) | Fuzz build without the fix | Fuzz build with it | Meson test sees the bug without the fix? |
|---|---|---|---|---|---|
| `regress-F12-csi` (85 B) | F12, high byte after ESC (CSI) | 6×110, byte-wise feed of `ESC \xc3 [` + ~40 args | **ASan heap-buffer-overflow** (8ddda4c) | clean (d0f1ae1 and later) | yes, by its inline case (segfault, exit 139; M4) |
| `regress-F12-osc` (90 B) | F12 after an OSC | 6×110, byte-wise `ESC ] 0 ; t ESC \xc3 [` + args | **ASan heap-buffer-overflow** (8ddda4c) | clean (d0f1ae1 and later) | yes, by its inline case (exit 139; M4) |
| `regress-F12-rep` (5 B) | F12, `ESC \xc3 [ b` hang | feeds an empty string: the 5 bytes all go to parameters | clean (does not replay) | clean | yes, by its inline case (hang, SIGALRM; M4) |
| `regress-F13-decaln` (7 B) | F13, `ESC # 8` + REP | no feed op at all | clean (does not replay; d0f1ae1 and 8ddda4c) | clean | yes, by its inline case (exit 142; M4) |
| `regress-F13-s7c1t` (7 B) | F13, `ESC SP F` + REP | no feed op | clean (does not replay) | clean | yes, by its inline case (exit 142; M4) |
| `regress-F13-scs-high` (8 B) | F13, high byte in SCS | no feed op | clean (does not replay) | clean | yes, by its inline case (M4) |
| `regress-F14-osc-number` (69 B) | F14, C0 inside the OSC number | 33×28, whole feed of the sequence | **UBSan signed overflow** (8ddda4c, bb7684e) | clean (32fb07c and later) | yes, by its inline case (title set, exit 1; M4) |
| `regress-F15-string-esc-c0` (8 B) | F15, ESC + C0 at the end of a read in a string | byte-wise feed of `ESC ] 2` only | clean (does not replay; 32fb07c) | clean | yes, by its inline case (exit 1; M4) |
| `regress-F16-string-esc-bel` (128 B) | F16, ESC BEL in a string | byte-wise feed of `ESC ] 2 ; x ESC BEL [` + 40 args `m` … | **guard/libvterm mismatch abort** (bb7684e, 32fb07c; the check exists only in this build); ASan heap-buffer-overflow on 8ddda4c | clean (5105912 and later) | yes, by its inline case (segfault, exit 139; M4) |
| `regress-F17-alt-reflow` (559 B, libFuzzer-minimized) | F17, alternate-screen reflow | 1×49, feeds, resizes, … | **ASan heap-use-after-free** (bb7684e, 32fb07c, 5105912); clean on 8ddda4c (older harness reads it differently) | clean (27a9d46) | **no**: the system libvterm0 does not crash; the inline case checks only the visible result |

So the corpus replays F12 (csi, osc), F14, F16 and F17 in the fuzz build. The F12-rep, F13 and
F15 files replay nothing; they stay as corpus seeds for mutation only. All of F12–F16 are covered
by the meson test's inline cases, which failed without each fix (M4, REAL). F17 is covered
only by the fuzz build. Those inline cases do not run the differential check (it needs libvterm's
internal parser struct, so it exists only in the fuzz build).
