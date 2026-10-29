<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Fuzzing the terminal input paths (M4 item 6)

## What is fuzzed

`tools/fuzz/vterm_fuzz.cpp` is a libFuzzer harness. It drives `VtermSession` the way the view
and the PTY do. Most work is decided by a `FuzzedDataProvider`: integers come from the end of the
input and byte strings from the front, so the seed files (raw terminal output) stay readable.

The harness makes one session with 1–60 rows, 1–250 columns and 0–200 lines of scrollback.
`onTitle` reads the title back. It then runs up to 400 operations:

| Op | What it does |
|---|---|
| feed (default) | `feed()` a chunk of up to 512 bytes, whole or (half of the time) one byte per `feed()` call. This is the escape-sequence parser, OSC title, DCS/APC/PM/SOS strings, mouse/focus mode changes and charsets. |
| resize | `resize()` to 0–70 × 0–260, between feeds, so it lands in the middle of sequences. |
| paste | `paste()` with up to 256 bytes (bracketed-paste sanitising). |
| key | `sendKey()` for any `VTermKey` plus `sendChar()` for any code point up to U+10FFFF. |
| mouse | `mouseMove()` / `mouseButton()` (buttons 0–8) and focus in/out, in whatever mouse mode the fed bytes selected (X10, 1000, 1002, 1003, SGR 1006, 1015). |
| read-back | `fillGrid`, `rowText`, `cellText` / `cellChars` / `wrapped`, and a `Selection` begin/extend/text in every mode. |
| search | `SearchIndex` update/find and `findAll`. |
| scrollback | `setScrollbackLimit()`. |

`takeOutput()` and `takeDamage()` run after every operation. Set `LTERM_FUZZ_TRACE=1` to print
the operations of one input. That is how the findings below were minimised; the box has no `xxd`.

After every operation, and after every byte of a byte-at-a-time feed, the harness runs the
differential check described below.

libvterm itself is compiled into the fuzzer from the Debian 0.3.3 source, with the same ASan,
UBSan and coverage instrumentation, so bugs inside libvterm show up as well. Debian's
0.3.3-3 has no patches, so this is the code the system `libvterm0` runs.

## Differential check: the guard against libvterm's parser

`feed()` keeps its own copy of libvterm's escape-parser state (`csiState_`) so that it can stop
the inputs that crash or hang libvterm (F1, F2, F6, F7). If the copy and libvterm disagree, a
sequence the guard thinks is over (or plain text) is still open in libvterm, and the guard can be
walked around. That is how F5, F12, F13 and F16 worked.

The fuzz build has libvterm's source, so the harness reads libvterm's parser struct directly
(`vterm_internal.h`: `parser.state`, `in_esc`, `intermedlen`). `VtermSession` exposes its side
read-only (`guardState()`, `guardIntermediates()`, `guardHoldsEsc()`, `vtermForCheck()`).
`checkGuard()` maps libvterm's state to the guard's numbering and aborts on a mismatch:

| libvterm | guard (`csiState_`) |
|---|---|
| `NORMAL`, not `in_esc` | 0 ground |
| `in_esc`, not a string state, `intermedlen == 0` | 1 after ESC |
| `in_esc`, not a string state, `intermedlen > 0` | 3 ESC + intermediates (count compared too, up to libvterm's cap of 3) |
| `CSI_LEADER`, `CSI_ARGS`, `CSI_INTERMED` | 2 CSI |
| `OSC`, `DCS`, `APC`, `PM`, `SOS` | 4 control string |
| `in_esc` in a string state | 5 ESC in a string (or, since F15, libvterm still in the plain string while the guard holds the ESC back) |
| `DCS_COMMAND` | 6 |
| `OSC_COMMAND` | 7 |

It runs after every operation (so also after `resize()`, which feeds CAN and margin resets) and,
in the byte-at-a-time feeds, after every single byte.

What it does not cover:

- Only the state class is compared, not the contents: CSI argument values, the leader, the
  OSC/DCS command and the string text are not compared. The guard's own counters (separators,
  digits) are checked only by the regression tests.
- The UTF-8 decoder state (a partial character between reads) lives in libvterm's state layer,
  not the parser, and is not compared. The guard's `lastCp_` for REP is not compared either.
- It needs libvterm's internal header, so it runs only in the fuzz build. The meson tests use the
  system libvterm0; there, `tests/fuzz_regress_test.cpp` checks `guardState()` at fixed points
  and the visible effect of each finding instead.
- It catches a disagreement only on inputs the fuzzer produces. It is a check, not a proof.

## Tools (nothing installed system-wide)

All of these were downloaded from deb.debian.org into `/workspace/lterm-tools`. SHA256 sums are in
`debs/clang19/SHA256SUMS` and `src/SHA256SUMS-libvterm`.

| File | Purpose |
|---|---|
| `clang-19_19.1.7-3+b1_amd64.deb` | clang 19, extracted to `clang19root` |
| `libclang-cpp19_19.1.7-3+b1_amd64.deb` | clang 19 runtime library |
| `libclang-common-19-dev_19.1.7-3+b1_amd64.deb` | clang headers |
| `libclang-rt-19-dev_19.1.7-3+b1_amd64.deb` | libFuzzer, ASan and UBSan runtimes |
| `libvterm_0.3.3.orig.tar.gz` | libvterm source |
| `libvterm_0.3.3-3.debian.tar.xz` | Debian packaging. Checked to contain no patches. |

`/workspace/lterm-tools/clang19.sh` runs the extracted clang with the right paths. The box has
no `llvm-symbolizer`, so stacks are symbolised with `ASAN_OPTIONS=external_symbolizer_path=/usr/bin/addr2line`.

## Build and run

```sh
tools/fuzz/build.sh                       # -> build-fuzz/vterm_fuzz (build-* is gitignored)
mkdir -p corpus crashes
build-fuzz/vterm_fuzz -max_len=8192 -timeout=10 -max_total_time=600 -print_final_stats=1 \
    -artifact_prefix=crashes/ corpus tools/fuzz/seeds
LTERM_FUZZ_TRACE=1 build-fuzz/vterm_fuzz crashes/crash-...   # replay one input, print the ops
```

The build flags are:

- compile: `-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -O1 -g`
- link: `-fsanitize=fuzzer,address,undefined`

Leak checking is on. Use `-timeout=10`: libFuzzer's default of 1200 s hid the F7 hang for over five minutes.

## Seeds (`tools/fuzz/seeds/`, about 170 KB)

Regenerate them with `tools/fuzz/make-seeds.py`.

- `hand-*`: hand-written nasty sequences. These cover wide characters at the right edge,
  combining marks, emoji/ZWJ/flags, RTL, broken UTF-8, OSC titles (terminated and not), DCS,
  long and huge CSI parameters, every mouse mode, scroll regions, charsets/DEC graphics,
  DECALN, SGR 38;2 colours, tabs, and resize while wrapped.
- `vttest-menu{1,2,3,6,8,9,11}-N`: vttest output in 4 KB chunks. It was captured through a pty by a
  small driver that answers DA/DSR/CPR (not in the repo).
- `regress-F2`, `F3`, `F5`, `F7`, `F8`, `F9`, `F10`, `F11`: the inputs that found the bugs below. Each now runs clean.
- `regress-F12-*` … `regress-F16-*`: the review and differential-check findings (raw byte streams).
- `regress-F17-alt-reflow`: the minimized F17 input (559 bytes, libFuzzer format; crashes the fuzz build without the fix).

`tests/fuzz_regress_test.cpp` (meson test `fuzz-regress`) has a small, readable case for every
finding, so the normal test runs cover them without the fuzzer.

## Findings (all REAL, all fixed in `VtermSession`, all with a regression test)

F12 and F13 came from the code review of PR #5 (`/workspace/lterm-review-m4/probe/probe.cpp`, which links the system libvterm0). F14–F16 came from the differential check; F17 came from the fuzz runs on the review head (runs l and m). Every regression test was run without its fix and failed: F12 segfault (exit 139), F13 SIGALRM after the 120 s `alarm()` (exit 142, a hang), F14 title set (exit 1), F15 title/guard checks (exit 1), F16 segfault (exit 139). F17 is the exception: the system libvterm0 in the meson build does not crash on it, so its meson test only checks the scenario's visible result and passes with or without the fix; the evidence that the fix works is the fuzz build (ASan heap-use-after-free on the seed without the fix, clean with it, REAL). Note on the probe: its case 8 (`"\x1b(\xc3B…"`) is one out-of-range hex escape (`\xc3B`), so it does not test what it says; the regression test splits the literal.


The system libvterm0 has the same code, so each fix is a guard in front of it. Nothing was
reported upstream or to Debian (project rule).

| # | Found by | What | Fix |
|---|---|---|---|
| F1 | ASan heap-buffer-overflow, parser.c:235 | More than 16 CSI arguments are written past libvterm's fixed `args[16]`, over the parser's callbacks pointer. The system libvterm0 segfaults on `printf '\e[1;2;…;20m'` (checked against `/usr/lib/x86_64-linux-gnu/libvterm.so.0`). | `feed()` drops parameter bytes after the 15th separator. |
| F2 | UBSan signed overflow, parser.c:225 | A CSI argument with more than 9 digits overflows libvterm's `int` accumulator. | Keep 9 significant digits; leading zeros don't count. |
| F3 | `abort()` "screen_resize failed to update cursor position" | When the cursor is on the last row of a wrapped line that no longer fits after a resize, libvterm aborts the process. Release builds too, e.g. 5×3 `abcdefg` resized to 1×3. | `reflowWouldAbort()` simulates libvterm's bottom-up reflow. In that case the session resizes once without reflow (counted in `reflowFallbacks()`). |
| F4 | ASan stack-buffer-overflow | `rowText()` read past its fixed 4 KB stack buffer on long rows (many code points per cell). | `rowText()` is built from the (normalised) cells. |
| F5 | ASan (F1 through a gap in the guard) | The guard did not model libvterm's DCS-command state or ESC inside a string, so `ESC P ESC [ 999…` reached the parser unguarded. | The guard follows libvterm's DCS-command and ESC-in-string states (`csiState_` 6/5). |
| F6 | code review while fixing F5 | The OSC command number has the same `int` overflow. | Capped at 9 significant digits. |
| F7 | hang (no new coverage for 5+ min; SIGSEGV to my own fuzzer PID gave the stack) | REP (`CSI Ps b`) repeats the last glyph by its width. libvterm loops forever when that width is 0: nothing printed yet, or a lone combining mark. `printf '\e[b'` hangs the system libvterm0 too (checked). | `feed()` tracks the last printed code point. It replaces REP's final byte with CAN when that code point is width 0 or unknown. |
| F8 | UBSan null member access, screen.c:190 | A wide character on a 1-column screen: libvterm writes the second cell through a NULL pointer. The system libvterm0 segfaults (checked). | Sessions are at least 2 columns wide (constructor and `resize`). |
| F9 | ASan heap-buffer-overflow, state.c:387 | libvterm does not clamp the saved cursor (DECSC, `?1048h`, `?1049h`) when the screen shrinks. After DECRC the cursor is below the screen and the next character reads past `lineinfo[]`. | `clampCursorAfterRestore()` after ESC 8 / `?1048l` / `?1049l`. It works in origin mode too. |
| F10 | ASan negative-size-param, state.c:141 | libvterm clamps only the bottom/right margin on resize. A scroll region whose top is below the new bottom (vttest's `[12..13]`, then a shrink to 5 rows) leaves top > bottom, and the next LF `memmove()`s a negative size. The system libvterm0 segfaults (checked: `ESC[12;13r`, resize 24×80 to 5×40, LFs). | When a DECSTBM/DECSLRM with parameters was seen and the screen shrinks, `resize()` resets the margins (cancelling a sequence still open with CAN so the guard stays in step) and puts the cursor back. xterm resets margins on resize too. |
| F11 | UBSan misaligned callbacks pointer, parser.c:35 (our bug) | `resize()` gives the application its pen back with `penSgr()`, fed past the guard. A pen with every attribute and two RGB colours was one SGR with 20 arguments, so F1 overwrote the parser's callbacks. | Attributes and colours go in two SGRs of at most 10 arguments. |
| F12 | code review (probe cases 1, 2, 5) | A byte ≥ 0x80 right after ESC put the guard back to plain text, but libvterm ignores it and stays in the escape. `ESC \xc3 [` + 40 args + `m` segfaults the system libvterm0 (F1 again), whole or byte by byte, also after an OSC; `ESC \xc3 [ b` hangs it (F7 again). REAL: probe exit 139 / 124 before, 0 after (`docs/bench/m4/tests/review-probe.txt`). | In states 1 and 5 such a byte keeps the guard after ESC (`csiState_ = 1`). |
| F13 | code review (probe cases 6, 7) | The guard ended `ESC SP F` and `ESC # 8` at the second byte and took `F` / `8` as printed text, so a following `ESC [ b` with nothing printed hung (F7). libvterm collects 0x20–0x2F as intermediates and ends the escape only at a final byte 0x30–0x7E; C0 is executed, DEL and ≥ 0x80 ignored inside. DECALN's E fill is not "printed" for REP. | State 3 now collects intermediates like libvterm (SCS only with exactly one `(` / `)` and a final byte); the final byte is not recorded as printed. |
| F14 | differential check (first run) | A C0 control inside the OSC command number (`ESC ] \x01 999…`): libvterm executes it and stays in `OSC_COMMAND`, still multiplying an `int`; the guard took the rest as string text, so its 9-digit cap did not apply. UBSan signed overflow (parser.c:267) in the fuzz build; with wrap-around, 4294967298 = 2³² + 2 sets the title (OSC 2) in the system libvterm0. | The guard stays in state 7 on C0 controls, NUL and DEL. |
| F15 | differential check (ASan stack-buffer-overflow read in `on_dcs`, state.c:1823; libvterm bug) | When a `vterm_input_write()` ends inside a control string after ESC and one or more C0 controls (or NUL/DEL), libvterm computes the last fragment's length as `(end - string_start) - 1` = `(size_t)-1`. DCS handlers and our title callback then read past the buffer (the title got up to `kMaxTitleBytes` of garbage, and the ESC itself went into the title). It needs no split: `ESC ] 2 ; ab ESC \x01` as one read is enough. | In state 5, `feed()` holds the ESC back and gives it to libvterm only right before the byte that ends the escape; C0 controls are executed the same way without it. |
| F16 | found while fixing F15 | After ESC in a string, BEL is not the terminator for libvterm: the string ends and the escape stays open, so `ESC ] … ESC BEL [` is a real CSI. The guard thought it was still "ESC in a string" (no CSI there), so F1 and F7 got through again. The system libvterm0 segfaults on it (REAL, exit 139). | BEL in state 5 goes to state 1. |
| F17 | fuzz runs l and m on the review head (ASan heap-use-after-free in libvterm `resize_buffer`, fuzz build) | A line that wrapped and then scrolled on the alternate screen leaves its row 0 marked as a continuation. After `?1049l`, a resize makes libvterm reflow the hidden alternate buffer, and that continuation makes it read row -1 of a buffer it already freed. Minimal shape: 3×10, `?1049h`, 35 characters, `?1049l`, resize (559-byte minimized input). | Leaving the alternate screen also clears the alternate buffer's row-0 continuation (`clearTopContinuation()` in the `ALTSCREEN` callback), like entering it already did. |

## Runs (REAL; from each run's `-print_final_stats`, logs not in the repo; DERIVED times are execs ÷ exec/s)

| Run | Code | Execs (REAL) | Avg exec/s (REAL) | Time | Ended with |
|---|---|---|---|---|---|
| a | harness + F1 fix | 333 | 333 | ~1 s (DERIVED) | F2 |
| b | + F2 | 2 316 | 330 | ~7 s (DERIVED) | F3 |
| c | + F3 | 2 631 | 187 | ~14 s (DERIVED) | F5 |
| d | + F4, F5, F6 | 5 223 | 10 | ~9 min (DERIVED); about 5.5 min of it stuck in one input (default `-timeout` 1200 s) | F7 (hang) |
| e | + F7 | 2 761 | 153 | ~18 s (DERIVED) | F8 |
| f | + F8 | 21 230 | 134 | ~2.6 min (DERIVED) | F9 |
| g | + F9 | 14 842 | 127 | ~2 min (DERIVED) | F10 |
| h | + F10 | 15 670 | 123 | ~2.1 min (DERIVED) | F11 |
| i | + F11 (final code) | 30 850 | 45 | 676 s wall (REAL, 21:38:28–21:49:44 CDT) | libFuzzer timeout (397 s), does not reproduce |
| j | final code | 32 959 | 97 | 338 s wall (REAL, 21:52:06–21:57:44 CDT) | libFuzzer timeout (36 s), does not reproduce |
| k | final code, `-timeout=60` | 51 173 | 89 | 574 s wall (REAL, 22:04:23–22:13:57 CDT) | libFuzzer timeout (84 s), does not reproduce |
| l | review head + F12–F16 + differential check, `-timeout=60` | 33 341 | 121 | 275 s wall (REAL, 00:31:21–00:35:56 EDT) | F17 |
| m | same code (F17 not yet fixed) | 16 225 | 77 | 210 s wall (REAL, 00:35:56–00:39:26 EDT) | F17 again; also a 65 s `slow-unit` that re-runs in 10/16 ms (a stall, not a finding) |
| n | final code c0323c9 (+ F17), `-timeout=60` | 66 473 | 110 | 601 s wall (REAL, 00:47:15–00:57:16 EDT) | clean `Done 66473 runs in 601 second(s)`, slowest unit 0 s |
| o | final code c0323c9, `-timeout=60` | 65 035 | 108 | 601 s wall (REAL, 00:57:16–01:07:17 EDT) | clean `Done 65035 runs in 601 second(s)`, slowest unit 0 s |

Totals: 361 062 execs (REAL) over runs a–o. Runs i–k (the M4 code before the review) are
described below. Runs l–o ran with the differential check on. On the final head c0323c9 (runs n
and o): 131 508 execs (REAL) in 1 202 s ≈ 20 min of wall time (REAL), both runs ended with
libFuzzer's clean "Done", no crash, no timeout, no slow unit, no guard/libvterm mismatch.
Coverage reached cov 3417 (run o). Before runs n/o the corpus (seeds + corpus + the diff
inputs) was also replayed once with `-runs=0` after the F15/F16 fixes: 5 084 inputs, clean in 42 s
(REAL). The box was shared with other agents' work during all runs (one other process at ~100 %
of a core during n/o).

The three timeouts on the pre-review code (runs i–k: `timeout-74696bbf…`, `timeout-d5592b5c…`,
`timeout-b038575d…`, plus a `slow-unit` of 25 s) re-run in 6–418 ms each (REAL, with the same
binary and `-timeout=60`). Their stacks are in ordinary code (search, `cellText`, libvterm
`scroll`). They are not findings: the process was stalled, and the box was shared with other
agents' work and my own ASan test runs at the time. Because of them, no single run on the final
code reached libFuzzer's clean "Done … runs" end. The longest uninterrupted stretch was run i,
about 275 s of fuzzing (DERIVED) before its stall.

## Limits

- Only the session/parser layer is fuzzed. The GL path, GTK widgets and the PTY/fork code are
  not fuzzed; the view tests and vttest cover those.
- libvterm's own bugs are mitigated in front of it, not fixed in it. A program linking the system
  libvterm0 directly still has F1, F3, F7, F8, F9, F10, F15 and F17 (F12, F13, F14 and F16 are guard bugs; the libvterm behaviour behind them is F1/F7 and the OSC-number overflow).
- The run lengths are short: about 20 minutes of fuzzing on the final head, at about 110 exec/s with ASan+UBSan. Longer runs could well find more.
