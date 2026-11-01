# Lunduke Terminal — M5 notes

Branch `m5-core` on M4 (PR #5, master 27a9d46). Order from the editor (2026-11-01, via Ted) plus
the code-review addition (`/workspace/lterm-code-review.md`, items 1, 2, 3, 4, 5, 7, 8, 9, 10, 12,
14; 6, 11, 13 skipped). Every number is labelled REAL (measured here), DERIVED (computed from REAL
numbers) or NOT MEASURED. The build box has **no GPU**: every GL number is Mesa llvmpipe on Xvfb.
Times are CST.

## Order items

| Item | What changed | Evidence |
|---|---|---|
| 1a governor wait flaky under ASan | `tests/view_test.cpp`: the wait for the probe to bring the governor back to Normal is 30 s under ASan/UBSan (15 s until PR #6), 6 s in release (`ltermWaitMs()` in `tests/sanitizer.hpp`). Neighbouring wall-clock waits (demotion, probe abort, recovery, tick speed-up, child reaping, malloc_trim) are scaled the same way. The time to Normal is printed. | Serial ASan runs with and without load: `bench/m5/tests/asan-serial-*.txt` (pass counts below) |
| 1b replay-only seeds | `docs/FUZZING.md` and `tools/fuzz/seeds/README.md` list each F12–F17 seed: its finding, what it replays, and whether the meson test can see the bug without the fix. No meson test reads seed files. In the fuzz build, F12-csi/osc, F14 and F16 fail without their fix and run clean with it. F12-rep, F13-* and F15 run clean either way, so they replay nothing (said so). F17 is evidenced only by the fuzz build. | `bench/m5/item1b/seed-replay.txt`, `seed-trace.txt` (REAL) |
| 1c GL probe CPU cost | How I read the M4 note: the probe's cost was the 7 GL programs recompiled on every probe and re-demotion, plus probes running while nobody can see the window. Fix: `GlPipeline::setParams()` swaps uniforms and sized objects and keeps the programs; due probes wait while the window is hidden or (pause-when-unfocused) unfocused. | `bench/m5/item1c/` (REAL, llvmpipe, a 3.9 s frame cost injected like the TCG VM). Focused: 62.1 → 53.0 ms CPU per probe (median), 11.4 → 10.0 % of a core. Unfocused: 13.1 → 2.0 % (0 probes). Mesa shader cache off: 25.5–27.0 % → 9.8–10.2 %. Tests: view_test (no program compiled over demotion + 2 probes; deferral), gl_match (setParams matches the C++ path). |
| 2 retro fonts | Five profiles with neutral names on licence-clean Debian fonts: `pixel-8bit` (`"Atari800"` 32 px), `pixel-hires-mono` (`Atari` ST 32 px), `pixel-16bit` (`Amiga` 32 px), `pixel-40col` (`C-64` 32 px, added on the editor's decision in PR #6), `modern` (`Hermit` 20 px, then `ProggyTinyTT`). Each falls back to the Terminus chain and DejaVu Sans Mono. No font files shipped. | Table below; `docs/FONTS.md` "M5"; `bench/fonts-SHA256SUMS`; tests `fonts` (fallback) and `profiles` (no trademark names); screenshots `f-*.png` |
| 3 docs/tests | This file, README, FONTS.md, FUZZING.md; test outputs in `bench/m5/tests/`. | below |

## Review items

| # | What changed | Test (`review`, `tests/review_test.cpp`) | Numbers |
|---|---|---|---|
| 1 output flood | `readAvailable()` reads for at most ~10 ms per slice (16 KB reads). On a budget hit the view drops its fd watch and reads on from a DEFAULT_IDLE source, so redraws and keys run in between. | flood + Ctrl+C through the key handler. Release: longest main-loop gap 16–21 ms, child exits ~20 ms after Ctrl+C | `bench/m5/review/README.md` §1: `yes` (plain) was frozen and Ctrl+C was never delivered; after, the longest gap is 19 ms and the child exits 4 ms after Ctrl+C (REAL) |
| 2 input methods | A GtkIMMulticontext (C API; gtkmm 3 has no wrapper) sees key presses and releases first, except Ctrl/Alt chords and keypad keys. Committed UTF-8 goes to the PTY. Focus, client window and cursor location are passed on. | `testInputMethod` with gtk-im-context-simple: dead_acute e → é, Compose o c → ©, then x; bytes `c3 a9 c2 a9 78` arrive | — |
| 3 hidden-tab memory | After 3 s hidden (`effectsFreeHiddenMs`), a tab frees its GL frame-sized textures (programs kept) or the C++ path's buffers and threads, and rebuilds them on show. All float textures are RGBA16F instead of RGBA32F (gl-match min PSNR 60.68 dB, threshold 40). | `testHiddenFrees` gl/cpu (gl 5.9 MB → 2.0 MB hidden → 5.9 MB shown) | Per extra amber tab, 1280x800: GL 79.1 → **42.2 MB**, C++ path 69.4 → **34.4 MB** (DERIVED from REAL VmRSS at 1 and 8 tabs). Review target: well under 77 MB. |
| 4 scrollback (stopgap) | Maximum 1,000,000 → 100,000 lines. Settings shows the upper bound per tab when full ("Up to X MB per tab…", "Warning:" from 100 MB). No storage rewrite. | `testScrollbackMax` (63 columns: 2.4 MB at the default; 243 MB with a warning at the maximum) | — |
| 5 cursor blink | Blinking stops (cursor solid) after `gtk-cursor-blink-timeout` (10 s by default) without a key, output or focus-in; any of those restarts it. | `testBlinkTimeout` | Amber GL static, idle: 11.5–12.1 % → 0.0 % of a core after 10 s (REAL) |
| 7 hidden tabs | A hidden view only parses; its changed rows are painted once on show. | `testHiddenTab` (47 requests skipped in 1 s, 0 painted) | Plain, busy hidden tab: 5.2 % (475 frames painted) → 0.9 % (0). Amber static: 3.6 % (476) → 0.8 % (0) (REAL) |
| 8 selection | The selected text is kept per line; output re-reads only the selected lines still on screen. | 1019-line selection: 80 lines compared over 5 reads | — |
| 9 confirm close | Tab close button, Ctrl+Shift+W and the window's close ask in a dialog when a tab's PTY foreground process group is not the shell's (`tcgetpgrp`), naming the job. An idle shell and child-exit closes do not ask. | `testConfirmClose` (/bin/sh -i running sleep 30: asks for tab and window; idle shell: no question) | — |
| 10 lowering scrollback | A lower limit is held as pending and applied when new output arrives. Raising it applies at once. | `testScrollbackLazyTrim` (281 lines kept after lowering to 50, then 50 after output) | — |
| 12 default profile | `amber` is the default (`kDefaultProfile`). `plain` stays selectable. | `profiles`, `review` | screenshot `default-amber.png` |
| 14 frame stats | Frame-time samples are recorded only with `--stats`; the counters stay. | no samples without `--stats` | — |

Before/after details (configurations, raw files) are in [`bench/m5/review/README.md`](bench/m5/review/README.md).

## Font qualification (licence-clean only; evidence from each package's `debian/copyright`)

The `.deb`s came from deb.debian.org (trixie) and were extracted, not installed. SHA-256 sums are in
[`bench/fonts-SHA256SUMS`](bench/fonts-SHA256SUMS). For the full quotes and name tables, see
[`FONTS.md`](FONTS.md) "M5".

| Package | Copyright line (abridged) | Qualifies | Profile |
|---|---|---|---|
| fonts-pc 1.0-2 | `Copyright: 2015-2016 VileR` `License: CC-BY-SA-4.0-Int` | yes | `ibm-dos` (since M1) |
| fonts-3270 3.0.1-1 | `Copyright: 2011-2016, Ricardo Banffy` `License: BSD3\|SIL` (OFL, no Reserved Font Name) | yes | `ibm-3278` (since M1) |
| fonts-terminus-otb 4.48-3.1 | "SIL Open Font License, Version 1.1. Copyright (c) 2010-2014 Dimitar Toshkov Zhekov, with Reserved Font Name "Terminus Font"" | yes | tube profiles' fallback |
| fonts-terminus 1.2.0+ds2-4 | **gap:** no stanza for `TerminusTTF-4.46.0.ttf`, so formally `Files: *` GPL-3; the font's own name table says OFL-1.1 (Zhekov, Blumenbach) | yes, with the gap noted (not reported to Debian, project rule) | first choice of amber/green |
| fonts-atarist 0+git20230822+ds-2 | `Copyright: 2015 ntwk` / `2017 Andrew Goncharov` `License: BSD-3-clause` | yes | new `pixel-hires-mono` |
| fonts-atarismall 2.3-3 | `Copyright: 1999 Thomas A. Fine` / `2015-2016 ntwk` / `2007,2023 Alex Myczko` `License: atari800-license` ("copy, modify, and distribute … provided this notice is preserved") | yes (permissive, **not SPDX-listed: one legal read recommended**) | new `pixel-8bit` |
| fonts-amiga 1.0-3 | `Copyright: 2019 The Homecomputer Fonts Project Authors` / `2019 Jens Kutilek` `License: OFL-1.1` | yes (a redrawing; trademark note) | new `pixel-16bit` |
| fonts-sixtyfour 1.0-3 | `Files: *` `Copyright: 2019 The Homecomputer Fonts Project Authors` / `2019 Jens Kutilek` `License: OFL-1.1`, no Reserved Font Name; re-verified from a fresh download (`bench/m5/sixtyfour-reverify.txt`) | yes (an original redrawing, not the Style64/Kreative fonts) | new `pixel-40col` (editor decision) |
| fonts-hermit 1.2.0+ds2-4 | stanza `modern-hermit/*` `Copyright: 2013 Pablo Caro` `License: OFL-1.1` (RFN "Hermit") | yes | new `modern` |
| fonts-proggy 1.2.0+ds2-4 | stanza `modern-proggy-tiny/*` `Copyright: 2004, 2005 Tristan Grimmer` `License: MIT` | yes | `modern`'s 2nd choice |
| Apple II (PrintChar21/PRNumber3), PetMe (Kreative), C64 Pro Mono (Style64), AtariClassic | not in Debian (cool-retro-term's `Files-Excluded`) | **no** | none; `profiles_test` forbids them |

**fonts-sixtyfour (editor decision, PR #6).** Ted relayed the editor's yes: include it if the
licence is clean. I downloaded the .deb again over https. Its SHA-256 matches trixie's
`Packages.xz` and `bench/fonts-SHA256SUMS`. I read the copyright file and the name table again:
`Files: *`, OFL-1.1, no Reserved Font Name, family `C-64`. The new profile is `pixel-40col`
("Pixel 40-column"). `profiles_test` allows exactly the family `C-64`, and only there. Screenshot:
`f-pixel-40col.png`.

The profile ids and labels avoid Atari, Amiga, Commodore, C64, Apple and PET (checked by a test).
Only the font family names, which are the packages' own, carry those marks.

## Before/after (items 1, 3, 5, 7)

All on this box: Xvfb :44, Mesa llvmpipe (**not a GPU**), terminal process CPU. REAL unless
marked DERIVED. Configurations and raw files: [`bench/m5/review/README.md`](bench/m5/review/README.md).

**Item 1, output flood** (a 10 ms timer stands in for key events; its longest gap is the freeze):

| Run | Before (badd3f9) | After |
|---|---|---|
| `yes`, plain | frozen; the timer never ran again; Ctrl+C never sent (killed by `timeout 120`) | longest gap 19 ms; child exits 4 ms after Ctrl+C |
| `yes`, amber | one timer run in 49.5 s; Ctrl+C 47.5 s late; child still running | longest gap 103 ms; exits 3 ms after Ctrl+C |
| `seq 1 3000000`, plain, 15 s | longest gap 10 001 ms, 1 content frame | 18 ms, 485 frames |
| `seq 1 3000000`, amber, 15 s | longest gap 5 657 ms, 1 frame | 111 ms, 292 frames |

**Item 3, RSS** (1280x800, quality static):

| | 1 tab | 8 tabs | Per extra tab (DERIVED, (8-1)/7) |
|---|---|---|---|
| amber GL, before / after | 230.2 / 202.4 MB | 783.8 / 497.9 MB | **79.1 / 42.2 MB** |
| amber C++ path, before / after | 124.9 / 124.8 MB | 610.5 / 365.5 MB | **69.4 / 34.4 MB** |
| plain, before / after | 53.3 / 53.5 MB | 91.7 / 91.7 MB | 5.5 / 5.5 MB |

The review's "~77 MB per amber tab" comes out as 79.1 MB here before the fix and 42.2 MB after.
The GL textures of all tabs dropped from 449.1 MB to 43.9 MB at 8 tabs (REAL, our allocation count).

**Item 5, idle cursor blink** (CPU % of one core per 10 s window, focused idle prompt):

| Config | Before 0-10 / 10-20 / 20-30 s | After |
|---|---|---|
| amber GL static | 11.5 / 11.9 / 12.1 % | 9.0 / 0.0 / 0.0 % |
| amber C++ path static | 9.5 / 9.1 / 9.0 % | 6.1 / 0.0 / 0.0 % |
| amber GL auto (= reduced on llvmpipe) | 29.0 / 30.4 / 28.9 % | 28.4 / 17.6 / 17.8 % (animation, not blink) |
| plain | 0.1 / 0.1 / 0.1 % | 0.1 / 0.0 / 0.0 % |

**Item 7, busy hidden tab** (it prints 20 lines every 20 ms; CPU %, frames painted for it per 10 s):

| Config | Before | After |
|---|---|---|
| plain, busy tab hidden | 5.2 %, 475 | 0.9 %, 0 |
| amber static, busy tab hidden | 3.6 %, 476 | 0.8 %, 0 |
| amber auto, busy tab hidden | 21.6 %, 477 | 19.5 %, 0 (includes ~17.7 % animation of the shown tab) |
| plain / amber static, busy tab shown | 6.8 % / 161 % | 7.2 % / 170 % (unchanged; box noise ±10 %) |

## Skipped review items

6, 11 and 13 (editor's decision), and anything else the review lists but the order did not
(15-19). Item 3 asked for lighter texture formats, so the RGBA32F -> RGBA16F part of item 15 is in;
the rest of 15 (uniform lookups, full-frame bloom) is not. Item 4 is a stopgap only; compact scrollback storage was not written.

## Tests (REAL)

See `bench/m5/tests/`:
- `release.txt`
- `asan-serial-load.txt`, `asan-serial-noload.txt`
- `asan-parallel.txt`
- the fuzz run (FUZZING.md, run p)

| Suite | Runs | Result |
|---|---|---|
| release `build/`, `--num-processes 2`, head afbbe05 | 1 | **16/16 OK** (view 102.8 s, review 26.5 s) |
| ASan+UBSan+leaks, serial, fuzzer/other load running | 3 | **3/3 runs 16/16 OK** |
| ASan+UBSan+leaks, serial, no load of mine | 3 | **2/3 runs 16/16 OK**; run 1: 14/16 (view, review failed) |
| ASan+UBSan+leaks, `--num-processes 2` | 2 | **2/2 runs 16/16 OK** |
| Fuzzer run p (caaa69a, ASan+UBSan) | 661 s | 79 461 execs, 0 crashes; F1-F16 seeds replay clean |

So: 7 of 8 sanitizer runs passed completely, which is 126 of 128 test results (DERIVED).
Leak checking: meson's `ASAN_OPTIONS` (detect_leaks=1) plus `tests/lsan.supp`; no leak reports in any run.
Serial-load run 1 used review_test from 8505761 (before 6b5b2d2 relinked it); the other runs are on the final test code.

**The failed run, honestly:** serial no-load run 1 (12:46 CST, head 375f3d1) failed
`view_test.cpp:739` (`probesDeferred > d0`; 0 probes deferred in the hidden C++-path case) and
`review_test.cpp:227` (blink-restart wait). The run took 11.4 min against ~4.8 min for the others,
and review took 452 s against ~48 s, so I suspect the box or process stalled. That is NOT proven.
Neither failure came back in the 7 later runs.

## Limits and caveats

- **No GPU.** Every GL number is llvmpipe. RSS, CPU and frame times on a real GPU are NOT MEASURED.
  A GPU driver keeps textures in VRAM, so item 3's RSS will look different there.
- **Item 5 is partial.** During the first 10 s, each blink phase still runs the whole effects
  pass (~9–12 % of a core on llvmpipe with amber). Redrawing only the cursor cell, or caching the
  two blink frames, was not done. With `auto` quality on llvmpipe (= reduced), ~17.7 % stays: that
  is the reduced-quality animation, not the cursor.
- **Item 3.** Each hidden amber GL tab still costs ~37 MB more than plain (DERIVED): mostly its
  GtkGLArea's GL context and llvmpipe's per-context memory, plus a 2 MB noise texture. The GL
  context is not destroyed. The shown tab gets only the RGBA16F saving (1 tab: 230 → 202 MB).
  The C++ path's buffers are freed while hidden, not made lighter. Burn-in history restarts when
  a freed tab is shown again. VmHWM stays at the peak.
- **Item 2.** Only the gtk-im-context-simple paths (dead keys, Compose) are tested. ibus/fcitx
  CJK input could not be tested headless here (NOT MEASURED). The preedit is drawn by the IM's
  own window, not in place.
- **Item 9.** Only interactive shells use job control (a separate foreground process group). With
  `-e cmd`, the command is the child itself, so closing does not ask.
- **Item 4** is a stopgap. Scrollback storage is unchanged, and the shown size is an upper bound.
- **Item 1** caps reads, not rendering. One amber effects frame on llvmpipe can still take ~100 ms.
- **Test timing under ASan (scaled in PR #6).** Two waits were tight in the first M5 runs:
  (a) testProbeDeferred pumped a fixed 2.5 s (serial no-load run 1 failed with 0 polls
  deferred); (b) in serial no-load run 3, "probe reached Normal" took 17 694 ms against the 15 s
  limit and still passed. It passed because `waitFor()` counts 20 ms pump steps, not wall-clock
  time, and slow main-loop iterations overrun those steps. Now build-san waits up to 7.5 s more
  for the first deferred poll (release unchanged: it still checks right after 2.5 s), and up to
  30 s for Normal (release still 6 s), both through `ltermWaitMs()`. `waitFor()` itself still
  counts steps (unchanged). The review_test:227 blink-restart failure of that same run was not
  changed, and it has not come back.
- **Process note.** During earlier work I ran `pkill -u <me> -x yes` once to stop a flood
  benchmark. It is scoped to my user, but in principle it could have hit another agent's `yes`
  under the same uid. Since then I kill by PID only.
