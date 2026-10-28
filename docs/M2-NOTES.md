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
