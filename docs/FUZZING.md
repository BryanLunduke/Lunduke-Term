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
| feed (default) | `feed()` a chunk of up to 512 bytes. This is the escape-sequence parser, OSC title, DCS/APC/PM/SOS strings, mouse/focus mode changes and charsets. |
| resize | `resize()` to 0–70 × 0–260, between feeds, so it lands in the middle of sequences. |
| paste | `paste()` with up to 256 bytes (bracketed-paste sanitising). |
| key | `sendKey()` for any `VTermKey` plus `sendChar()` for any code point up to U+10FFFF. |
| mouse | `mouseMove()` / `mouseButton()` (buttons 0–8) and focus in/out, in whatever mouse mode the fed bytes selected (X10, 1000, 1002, 1003, SGR 1006, 1015). |
| read-back | `fillGrid`, `rowText`, `cellText` / `cellChars` / `wrapped`, and a `Selection` begin/extend/text in every mode. |
| search | `SearchIndex` update/find and `findAll`. |
| scrollback | `setScrollbackLimit()`. |

`takeOutput()` and `takeDamage()` run after every operation. Set `LTERM_FUZZ_TRACE=1` to print
the operations of one input. That is how the findings below were minimised; the box has no `xxd`.

libvterm itself is compiled into the fuzzer from the Debian 0.3.3 source, with the same ASan,
UBSan and coverage instrumentation, so bugs inside libvterm show up as well. Debian's
0.3.3-3 has no patches, so this is the code the system `libvterm0` runs.

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

`tests/fuzz_regress_test.cpp` (meson test `fuzz-regress`) has a small, readable case for every
finding, so the normal test runs cover them without the fuzzer.

## Findings (all REAL, all fixed in `VtermSession`, all with a regression test)

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

Totals: 179 988 execs (REAL) over runs a–k. On the final code (runs i–k): 114 982 execs (REAL) and
1 588 s ≈ 26.5 min of wall time (REAL). At least 517 s of that was spent inside the three stalled
inputs, which leaves ≈ 17.9 min of actual fuzzing (DERIVED). Coverage went from cov 2236 (run a) to
cov 3272 (run k).

The three timeouts on the final code (`timeout-74696bbf…`, `timeout-d5592b5c…`,
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
  libvterm0 directly still has F1, F3, F7, F8, F9 and F10.
- The run lengths are short: about 18 minutes of fuzzing on the final code, at about 90–330 exec/s with ASan+UBSan. Longer runs would
  very likely find more.
