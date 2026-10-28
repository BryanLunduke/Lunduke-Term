# M2 notes, stage 1 (items 1 and 2)

Branch `m2-core` from master `170023d` (M1 merged in PR #2). This stage covers item 1 (bug
fixes, each with a test) and item 2 (cheaper effects on software rendering).

## Item 1: bug fixes

| # | Fix | Test (meson test name: function) |
|---|---|---|
| a | The window title (OSC 0/2) is capped at 4096 bytes. Bytes past the cap are dropped while the sequence is still arriving, and the stored title is cut back to a whole UTF-8 character. | `fixes`: `testTitleCap` (4 MiB title; multibyte title cut at the boundary) |
| b | `scrollback-lines` (settings key, `--scrollback`, `setScrollbackLimit`) is clamped to 0..1,000,000. The key is read as int64, and a warning is printed when the value is clamped. | `fixes`: `testScrollbackClamp`; `view`: `testScrollbackClampView` (2^40 and -3 through a real `TerminalView`) |
| c | `sanitizePaste` decodes UTF-8 and drops C1 controls U+0080–U+009F (encoded `C2 80`..`C2 9F`). It also drops invalid or overlong sequences, surrogates, C0 controls except TAB/CR/LF, and DEL. CRLF and LF become CR. | `fixes`: `testPasteC1` |
| d | Clipboard requests capture a `std::weak_ptr` to a token that the view's destructor resets. A reply that arrives after the window closed is ignored instead of touching freed memory. | `view`: `testClipboardAfterClose`. The test is built with AddressSanitizer. A separate X client owns CLIPBOARD and PRIMARY and answers each request 120 ms late, so the view is destroyed while both requests are pending. With the guard removed, ASan reports `heap-use-after-free` in the paste lambda (checked by hand). |
| e | Before exec, the child closes fds with `close_range(3, ~0U, 0)` (raw syscall). The fallback is an async-signal-safe `close()` loop up to `RLIMIT_NOFILE` (read before `fork`, capped at 2^20). The old loop stopped at 1024. | `fixes`: `testCloseFds` (fds 900 and 5000 left open without CLOEXEC must not reach the child; the fallback loop and `closeFrom3` are also tested on their own in forks) |
| f | The soft-wrap flag is no longer lost when several lines scroll out at once. Before a CSI `S` (SU) or CSI `M` (DL), and before a resize, the session snapshots libvterm's per-row continuation flags. `pushline` then takes the flag of the row actually pushed. libvterm 0.3.3 moves its lineinfo before it calls the scroll callback, so it used to read row 0 of the already-shifted array. The scan also handles a sequence split across reads. | `fixes`: `testWrapMultiScroll` (6 cases: CSI 3 S, a split `ESC [` + `3S`, CSI M at the top, a line-feed burst, resize 5→1 rows, and unwrapped lines staying unwrapped). Old code vs new: `CSI S: w0=0` → `1`; `resize: 0 0 0` → `1 1 0`. |
| g | `hangup()` sends SIGHUP to the process group and the child, then reaps with `waitpid(WNOHANG)` for up to 50 ms. If the child is still running, the view registers a GLib child watch that reaps it later. No zombie is left while the terminal process lives on. | `fixes`: `testHangupReaps` (`sleep 30` is reaped and `waitpid` gives ECHILD; a HUP-ignoring child returns its pid); `view`: `testReapAfterHangup` (closing the view leaves no `/proc/PID`; a child with `trap '' HUP; sleep 0.6` is reaped by the watch). With the old destructor, the view test fails at the zombie check (checked by hand). |

**Limit for (f), not fixed (libvterm behaviour).** If a single scroll covers the whole scroll
region, libvterm 0.3.3 (`vterm_scroll_rect`) erases the region and does not move it, so the
rows never reach `sb_pushline`. Examples are CSI `n` S with n ≥ rows, or DL of every line from
the top. Those lines are lost to scrollback, not just their wrap flag. Ordinary output, `clear`
and line-feed bursts are not affected. The test therefore scrolls less than the screen height.

**Also found and fixed (039690e).** The GL view read `GL_DRAW_FRAMEBUFFER_BINDING` *after*
`updateSource()`, which binds the pipeline's own FBOs. So every content frame was drawn into an
internal FBO, and only the next effects tick reached the window. Animated profiles hid this
(M1 kept a 100 ms tick tail after each content change). With `effects-quality static` (no
ticks) the window stayed black. It is now read first. Screenshot `s1-amber-static.png` shows the
fixed result.

**When there is no main loop.** If the view is destroyed after `app->run()` has returned, no
main loop is left to run the child watch. The process is exiting, so init reaps the child.

## Item 2: effects-quality and software rendering

- **Setting / flag:** `effects-quality = auto|full|reduced|static` and `--effects-quality`.
  `cpu-budget` / `--cpu-budget PERCENT` sets the governor target; the default is 25 and 0
  turns the governor off.
- **Detection (software rendering):**
  - `GL_RENDERER` contains `llvmpipe`, `softpipe` or `swrast` (case-insensitive, read when the
    GtkGLArea is realized); or
  - `LIBGL_ALWAYS_SOFTWARE` is set to anything but empty, `0` or `false`; or
  - the view runs the C++ fallback.
- **auto** gives `reduced` on software rendering and `full` otherwise. `--stats` and the
  backend string show the resolved level, e.g.
  `GLSL on llvmpipe (...), quality reduced (software rendering)`.
- **full** is the profile as designed. `applyEffectsQuality(p, Full)` returns `p` unchanged
  (tested), and on a GPU the tick rate and code path are exactly M1's, so `full` on real GPUs
  looks unchanged. The governor runs only on software rendering.
- **reduced:**
  - Static noise and jitter are dropped. That removes the per-pixel noise-texture fetch from
    every tick.
  - The bloom radius is halved; this is part of the content pass.
  - Burn-in stays at half resolution.
  - Flicker, horizontal sync, the glowing line, burn-in, scanlines, curvature, tint and ambient
    stay.
  - Ticks run at half rate (`effects-fps`/2, so 10 fps by default), and the governor applies on
    top.
- **static:**
  - Noise, jitter, flicker, sync, the glowing line and burn-in are dropped, so
    `animated() == false` and the burn-in fade is 0.
  - What is left (barrel warp, phosphor tint, bloom, scanlines, ambient and the tube mask) is a
    fixed per-pixel overlay of the source. It runs once per content change: the content pass
    plus one dynamic pass.
  - No tick timer is ever started, and the old 100 ms tick tail after content changes is gone
    when there is no burn-in.
  - The C++ fallback also skips re-rendering on plain exposes when nothing animates.
- **Governor (software rendering, not static):**
  - Every 1 s it compares the process CPU time (`CLOCK_PROCESS_CPUTIME_ID`, so llvmpipe's
    worker threads count) with wall time.
  - Above the budget, the tick interval grows by load/budget × 1.05, up to 1000 ms. Below 60 %
    of the budget, it shrinks by 15 % back toward the base rate.
  - If `auto` is still over budget at 1 fps, it switches to `static` and prints one line to
    stderr (`effects-quality auto -> static`). Explicit `full`/`reduced` stay at 1 fps.
  - Unfocused windows still stop ticking, as in M1.
- **GL = C++:** `gl-match` compares GL with C++ for amber and green-scanlines at each of full,
  reduced and static, in addition to the M1 cases. See the PSNR table.

### PSNR, GL (llvmpipe) vs C++ fallback, 640×400 (REAL, docs/bench/m2-host/psnr.txt)

| Parameters | PSNR | max diff (0–255) |
|---|---|---|
| default, half-res burn-in (M1 case) | 106.98 dB | 1 |
| default, full-res burn-in (M0 case) | 106.98 dB | 1 |
| amber full | identical (reported as 99.00) | 0 |
| amber reduced | 103.97 dB | 1 |
| amber static | 106.98 dB | 1 |
| green-scanlines full | identical | 0 |
| green-scanlines reduced | 106.98 dB | 1 |
| green-scanlines static | identical | 0 |
| after 50 resizes | 106.98 dB | 1 |

The test requires ≥ 40 dB. "99.00" is the test's value for an identical image (MSE 0).

## Benchmarks

Raw data:
- `docs/bench/m2-host/` (bench.txt, idle.txt, system.txt, psnr.txt);
- `docs/bench/m2-tcg-lcos-100-01/` (bench.txt, idle.txt, system.txt, env.txt, status, plus the
  `vm.sh` and `run.sh` that produced them).

Script: `tools/bench-m2.sh`.

- **Idle runs:** the real `lunduke-terminal`, 1280×800, amber, with bash idle at a prompt and
  the window focused. CPU is (utime+stime)/wall from `/proc` over 10 s after a warm-up (host
  30 s, TCG 40 s), and 100 % = one core.
- **BEFORE** is `--effects-quality full --cpu-budget 0`, which reproduces M1's behaviour (full
  effects at 20 fps, no governor).
- **REAL** means measured. **DERIVED** means computed from REAL numbers.

### Host: Debian 13, llvmpipe (Mesa 26.1), own Xvfb :43 + xfwm4 — REAL

**Idle CPU, amber focused**

| Case | GL | CPU fallback (4 threads) |
|---|---|---|
| BEFORE (M1: full, no governor) | 96.2 % | 133.0 % |
| auto (→ reduced on llvmpipe) | 16.2 % (re-run 16.7 %) | 20.0 % |
| full | 22.6 % | 21.5 % |
| reduced | 16.2 % | 20.5 % |
| static | 0.0 % | 0.0 % |
| plain, ibm-3278 | 0.0 % | 0.0 % |

The governor settled at a tick interval of about 230–240 ms (4–4.4 fps) for GL auto/reduced.

**Per frame, median ms (tick / content pass)**

| Quality | CPU 4 thr | GL LP 1 | GL LP 2 | GL LP 4 |
|---|---|---|---|---|
| full | 19.57 / 23.44 | 32.42 / 103.85 | 18.36 / 59.83 | 10.07 / 36.03 |
| reduced | 15.07 / 20.97 | 25.81 / 72.68 | 13.36 / 39.48 | 7.32 / 25.24 |
| static | 8.56 / 21.80 | 22.65 / 85.45 | 9.95 / 36.69 | 5.45 / 22.54 |

In static mode the "tick" is the single dynamic pass after a content change; nothing runs while
idle. Caveat: some host re-runs overlapped with the TCG VM, which used 4 host threads.

### TCG VM: LCOS 1.0 live ISO, `qemu-system-x86_64 -accel tcg -smp 4 -snapshot`, llvmpipe (Mesa 26.1.6), XLibre — REAL

The run was 17:43:48–17:57:36 UTC (12:43–12:57 CDT). The VM was stopped with QMP `quit` at
12:58:58 CDT.

**Idle CPU, amber focused (xdotool)**

| Case | GL | CPU fallback (4 threads) |
|---|---|---|
| BEFORE (M1: full, no governor) | 325.6 % | 290.2 % |
| auto | **0.0 %** (demoted: "needs 320 % of a core at 1 fps; auto -> static") | **0.0 %** (demoted: "needs 106 % at 1 fps") |
| full (explicit) | 327.4 % (governor at 1000 ms) | 155.7 % (1000 ms) |
| reduced (explicit) | 327.0 % (1000 ms) | 120.5 % (1000 ms) |
| static | 0.0 % | 0.0 % |
| plain, ibm-3278 | 0.0 % | 0.0 % |

For M1 comparison, M1-BENCHMARKS gives the M1 build in the same kind of VM at GL 395.7 % (LP 4)
and CPU 357.3 % (4 threads).

**Per frame, median ms (tick / content pass), 1280×800, 4 threads**

| Quality | CPU | GL |
|---|---|---|
| full | 646.57 / 1043.49 | 2584.65 / 3938.24 |
| reduced | 452.62 / 745.70 | 1485.22 / 2737.51 |
| static | 189.30 / 664.74 | 737.91 / 2365.94 |

On TCG, one GL tick of full or reduced amber takes 1.5–2.6 s of wall time on all 4 vCPUs. That
is longer than the 1000 ms maximum interval, so frames run back to back and an explicit
`full`/`reduced` cannot get under the budget. This is by design: explicit levels are honoured
and only `auto` demotes. The default `auto` reaches 0 % idle by switching to static. It costs
one 0.74 s (GL) or 0.19 s (CPU) dynamic pass per content change, on top of the content pass.

### DERIVED

| Metric | Host | TCG |
|---|---|---|
| Idle CPU, amber GL default (`auto`) vs BEFORE | 96.2 → 16.2 % (−83 %) | 325.6 → 0.0 % (−100 %) |
| Idle CPU, amber CPU fallback `auto` vs BEFORE | 133.0 → 20.0 % (−85 %) | 290.2 → 0.0 % (−100 %) |
| GL tick, reduced / full | 0.73 (LP 4) | 0.57 |
| GL tick, static / full | 0.54 (LP 4) | 0.29 |
| CPU tick, reduced / full | 0.77 | 0.70 |
| CPU tick, static / full | 0.44 | 0.29 |
| TCG slowdown vs host, full tick | — | GL ×257 (LP 4), CPU ×33 |

The goal of ≤ 25 % of one core at idle is met on the host (auto 16 %, full 23 %) and on TCG
(auto 0 %).

## Screenshots (host, fixed binaries, 1280×800, `tools/m2-shot.sh`)

All in `/workspace/lcos-app-reviews/terminal-m2/`:
- `s1-amber-full.png`, `s1-amber-reduced.png`, `s1-amber-static.png`;
- `s1-amber-full-reduced-static.png`: the three side by side, labelled;
- `s1-amber-zoom-corner-full-reduced-static.png`: a crop of the top-left corner (noise, bloom,
  warp).

## Problems / open points

- (f) above: a full-region scroll in libvterm erases without pushing to scrollback.
- In TCG screenshots taken during the run, the amber GL window sometimes showed only a painted
  top band. This is consistent with a 2.4–3.9 s content pass still in progress (llvmpipe tiles)
  when the screenshot was taken. It was not investigated further and the static path was not
  re-checked visually inside the VM. On the host, all three levels paint fully.
- The first TCG VM boot (12:33 CDT) was stopped with QMP quit before benchmarking, because the
  static-mode black-window bug (039690e) was found then. The payload was rebuilt and the VM
  rebooted (pid 131297, 12:38:40 CDT).
- Explicit `full`/`reduced` on very slow software rendering stay above the budget (see TCG).
  Only `auto` demotes.
