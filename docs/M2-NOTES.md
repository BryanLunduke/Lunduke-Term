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

## Tests

`meson test -C build` (DISPLAY = own Xvfb :43): 9/9 OK (profiles, painter, fonts, effects, vterm, session, fixes, gl-match, view).

# M2 stage 2

## Item 3: vttest, vim, htop, mc (own Xvfb :43 + xfwm4, XTEST via xdotool)

Terminal: `build/lunduke-terminal --size 808x464 --profile plain` (80×24, TERM=xterm-256color).
Programs from Debian trixie .debs unpacked into `/workspace/lterm-tools/root` (not installed):
vttest 2.7 (20241208), vim 9.1.1230, htop 3.4.1, mc 4.8.33. Scripts: `tools/app-check/`.

| Program | What was exercised | Result | Bug found |
|---|---|---|---|
| vttest 1 | box / E-frame (CUP, HVP, CUF, CUB, CUU, CUD, IND, RI, NEL, ED, EL, DECALN), DECAWM autowrap with BS/TAB/LF at the margin, controls inside CSI, leading zeros — 80 columns | pass | — |
| vttest 1, 2, 8 | the 132-column passes (DECCOLM) | not supported: the window is not resized, so the 132-column screens draw wrapped in 80 columns. Same as xterm's default (`allowC132` off) and VTE | none fixed (documented) |
| vttest 2 | DECAWM, TBC/HTS tab stops, DECSCNM light/dark background, DECSTBM soft and jump scroll (region 12–13 and 1–24, up/down), DECOM, SGR pattern (bold/underline/blink/negative), DECSC/DECRC | pass after fixes | **DECSCNM had no visible effect**: libvterm already reverses every cell for `?5h` and the view swapped them back. Fixed: the view now only inverts the margins (and the cursor colour) (3e7f6cb). **DECRC did not restore the character sets** (libvterm saves only position/pen/cursor modes): `line:`/`diamonds:` rows showed `qqqqq` / `` ````` ``. Fixed: `feed()` tracks G0/G1/SO-SI, saves them on ESC 7 / `?1048h` / `?1049h`, restores on ESC 8 / `?1048l` / `?1049l` (3e7f6cb). Open: SGR 5 blink is drawn as plain text |
| vttest 3 | character sets B, A (£), 0 (DEC graphics), 1, 2 as G0 (SI) and G1 (SO) | pass | — |
| vttest 6 | ENQ (empty answerback, as xterm), LNM set/reset (CR LF / CR), DSR 5 and 6 (incl. origin mode), DA (`?1;2c`), DA2 (`>0;100;0c`), DA3, DECREQTPARM 0 and 1 | pass after fix; DA3 not answered (VT420) | **DECREQTPARM (CSI x) was not answered** ("Bad format"). Fixed: CSI fallback answers `2;1;1;128;128;1;0x` / `3;…` like xterm (0426251) |
| vttest 8 | IL/DL accordion, IRM insert mode, DCH, ICH, staggered DL — 80 columns | pass (132-column pass: see DECCOLM) | — |
| vttest 11.5 | ISO 6429 HPA, CBT, CHA, CHT, HPR, VPA, CNL, CPL, VPR | pass | — |
| vttest 11.6 | 8×8 colour matrix (bright off/on), SGR 0 reset, BCE with ED/EL and with ECH/indexing | pass (palette black is the profile's dark grey; margins keep the default background, as xterm's internal border) | — |
| vttest 11.8 | set window title (OSC), mouse: X10, normal tracking, button-event tracking (press/release, cell coordinates checked), alternate screen 47 / 1047 / 1049 | 1049 pass; 47 pass after fix; 1047 "cursor save/restore failed" | **private mode 47 was ignored** (libvterm knows 1047/1049 only). Fixed: `?47h/l` also feeds `?1047h/l` (2ff2c87). Open: libvterm has one DECSC slot shared by both screens (xterm keeps one per screen), so the 47/1047 tests that DECSC on both screens report the alternate screen's position |
| vim 9.1 | `syntax on` colours (C++), alt screen enter/leave, `mouse=a` click (cursor to 13,16 = screen col 20 − 4 gutter), drag → visual mode, wheel scroll, window resize 80×24 → 100×31 (`&columns &lines`), bracketed paste (Shift+Insert, `autoindent`, no staircase) | pass after fix | **After a resize inside vim, the shell screen came back with the blanks in reverse video** (`s2-vim-after-quit-before-fix.png`): libvterm fills cells created by resize/reflow with the *current* pen (vim's reverse status line), on the main screen too. Fixed: resize with the default pen, then restore the application's pen (only between escape sequences) (241ffd7) |
| htop 3.4.1 | colours/meters, mouse click on a process row, wheel, resize 80×24 → 100×31, F1 help, quit (alt screen restored) | pass | — |
| mc 4.8.33 | two panels, line drawing (frames, tees, corners), colours, mouse click in both panels, wheel, menubar click, resize, F10 click in the button bar | pass | — (only mc's own "sfs.ini not found" warnings from the unpacked, non-installed data dir) |

New tests: `vterm` (DECSC/DECRC charsets incl. split chunks and `?1048`, RIS, mode 47, DECSCNM
cell colours, resize pen on main/alt screen) and `session` (`testReports`: DECREQTPARM, DA). The
resize-pen test fails without the fix (checked by stashing `src/`).

Screenshots (`/workspace/lcos-app-reviews/terminal-m2/`): `s2-vttest-1-border.png`,
`s2-vttest-1-autowrap.png`, `s2-vttest-1-132col.png`, `s2-vttest-2-sgr-light{,-before}.png`,
`s2-vttest-2-decsc{,-before}.png`, `s2-vttest-3-charsets.png`, `s2-vttest-6-decreqtparm.png`,
`s2-vttest-8-accordion.png`, `s2-vttest-11-hpa.png`, `s2-vttest-11-colors.png`,
`s2-vttest-11-mouse.png`, `s2-vttest-11-altscreen-1049.png`, `s2-vim.png`,
`s2-vim-resize-paste.png`, `s2-vim-after-quit{,-before-fix}.png`, `s2-htop.png`,
`s2-htop-mouse.png`, `s2-htop-resized.png`, `s2-mc.png`, `s2-mc-menu.png`, `s2-mc-resized.png`.

### CI

`docs/ci/ci.yml` (to be copied to `.github/workflows/ci.yml`; the box's GitHub token has no `workflow` scope, so it could not be pushed there): jobs `build-test` (debian:trixie container, apt build deps, meson
setup/compile, `xvfb-run -a meson test`) and `sanitizers` (`-Db_sanitize=address,undefined`,
`--timeout-multiplier 4`). Validated with yamllint 1.37.1 (default rules, line length 120) and a
PyYAML load; every apt package name was checked against the trixie `Packages` index. The same
steps were run locally: release `xvfb-run -a meson test` 9/9 OK, ASan+UBSan build 9/9 OK, no
`runtime error` in the log. **The workflow has not run on GitHub yet.** See `docs/TESTING.md`.

## Item 4: HiDPI (6ffd50f)

The view follows the GTK scale factor (`GDK_SCALE`, `notify::scale-factor` at run time). The
font size stays in logical px; the surface, the glyph cache, the GL textures and the C++
effects buffers are in device px.

- `GridPainter(family, px, scale)`: metrics measured at `px` and multiplied, glyphs rasterised
  at `px*scale` (sharp, not upscaled); cursor line widths scale too.
- `computeGridLayout()`: margins, rows and columns in logical px, then times the scale, so a
  window has the same rows × columns at scale 1 and 2.
- 1:1 blit with `cairo_scale(1/scale)`; event coordinates are multiplied before `pointToCell`.
- `scaleEffectParams()`: scanline period and bloom radius times the scale (bloom capped at 64);
  noise is sampled per logical pixel (`EffectParams::pixelScale`) in GLSL and C++, so the look
  matches scale 1.
- Tests: painter cell size and picture at scale 2; layout and mouse/selection mapping at scale 2
  (`session`: `testScale2Mapping`); GL vs CPU PSNR at scale 2 for every quality level
  (`gl-match`); scale-2 picture vs scale 1; run-time scale change in a real view, plain and GL
  (`view`: `testScaleChange`).
- PSNR GL vs C++ at scale 2 (1280×800 device px), full / reduced / static: amber 103.97 /
  105.22 / 106.98 dB, green-scanlines 106.98 / 106.98 / 109.99 dB (max diff ≤ 1).
- Screenshots: `s2-hidpi-plain-scale{1,2}.png`, `s2-hidpi-plain-scale2-crop-1to1.png`,
  `s2-hidpi-amber-gl-scale{1,2}.png`, `s2-hidpi-amber-cpu-scale2.png`,
  `s2-hidpi-amber-static-{gl,cpu}-scale2.png`, `s2-hidpi-selection-scale2.png`,
  `s2-hidpi-xftdpi192{,-scale2}.png`.

## Item 5: RGB shift and bezel (d0c4079)

### RGB shift

CRT 1.2.0 `rbgShift`: displacement `12 · rbgShift · 0.75` px (CRT's `totalFontScaling`), times
`pixelScale` at HiDPI; for 0.3524 that is 3.17 device px at scale 1. Each channel is mixed from
the left (L), right (R) and centre (C) sample with CRT's weights: r = .1L + .3R + .6C,
g = .2L + .2R + .6C, b = .3L + .1R + .6C. It runs in the static pass (once per content
change) in both back-ends, so it adds no per-tick work. Profile `ibm-dos` uses 0.3524 (CRT's
"IBM DOS" value); every profile accepts `--rgb-shift X` / `rgb-shift=` in settings.ini.

### Bezel

- Geometry (`frameGeometry()`): inset = min(frameSize · pixelScale, ¼ · min(w, h)); the glass
  is a rounded rectangle (radius min(0.6 · inset, ½ · min(inner))), the screen content and the
  barrel warp are mapped inside it (`screenCoords()`). With the frame off the mapping is
  bit-identical to M1.
- Static layer, rendered once per size (GLSL `kFrame`, C++ `frameLayerRows()`): frame colour
  with a vertical shade and a darker rim, a lit inner bevel (light from the top-left, specular
  scaled by shininess), an inner shadow on the glass edge and a soft glass highlight.
  Premultiplied RGBA.
- Reflection: the phosphor glow (bloom) mirrored at the glass edge onto the bevel, weighted by
  shininess and distance; recomputed per content change only, never per tick.
- Tick: the dynamic pass composites the premultiplied layer (GL: one texture fetch; C++: only
  on each row's non-empty spans of the layer, using unquantised floats so it matches GL).
- Grid layout: the inset is added to the margins, so the text sits inside the glass.
- Mouse: `pointToCell()` takes the geometry. A press on the bezel or in a rounded corner selects
  nothing and is not reported to the application (middle-click paste still works); a drag that
  leaves the glass clamps to the edge cells; edge-scroll still works.
- Profiles: `amber`, `green`, `green-scanlines` offer it (`--frame`, off by default; frame
  colours #2a2622 / #22262a). `plain`, `ibm-dos`, `ibm-3278` ignore `--frame` with a warning.
- Options / settings.ini: `--frame`/`--no-frame` (`frame`), `--frame-size PX` (`frame-size`,
  logical px, default 28), `--frame-color #RRGGBB` (`frame-color`), `--frame-shininess 0..1`
  (`frame-shininess`, default .5), `--rgb-shift X` (`rgb-shift`).

### PSNR, GL (llvmpipe) vs C++, 640×400 logical, `gl-match` test — REAL

All max diff ≤ 1 (8-bit).

| Variant | scale 1: full / reduced / static | scale 2: full / reduced / static |
|---|---|---|
| ibm-dos RGB shift | 106.98 / 103.97 / 103.97 | 103.97 / 106.98 / 109.99 |
| amber bezel | 99.00 / 106.98 / 106.98 | 103.46 / 104.55 / 103.97 |
| green-scanlines bezel | 106.98 / 102.21 / 103.97 | 105.22 / 105.22 / 106.02 |
| amber bezel + RGB shift | 99.00 / 106.98 / 106.98 | 103.46 / 104.55 / 104.55 |

Resize loop (50 resizes, bezel + RGB shift on): textures 12 → 12, FBOs 10 → 10, PSNR after
102.21 dB. Screenshots of the real window, GL vs C++ static (deterministic): RGB shift
107.95 dB, amber bezel 106.19 dB (ImageMagick `compare -metric PSNR`).

The first C++ bezel version composited after the 8-bit quantisation and only reached 61 dB;
compositing on the unquantised row floats fixed it.

### Tests added

- `effects`: geometry (inset, radius, SDF), static layer (frame colour, bevel, transparent glass
  centre), the reflection (the bezel pixel next to a lit column rises 29 → 40), the RGB fringes
  (left 63/42/21, right 21/42/63 on a white column).
- `gl-match`: the table above, both scales, plus the bezel resize-leak loop.
- `session`: `testBezelMapping` (bezel and rounded corner → outside, clamping, scale 2).
- `view`: `testBezelPointer` for `auto` (GL) and `cpu` in a real 640×400 window: grid 42×12,
  margins 47/38 (they include the bezel); bezel, top edge and corner map to no cell; a press and
  drag on the bezel selects nothing; a drag from row 0 of the glass onto the bezel corner clamps
  to column 0 and copies the start of "alpha beta gamma". (That a bezel press is not reported to
  a mouse-tracking application follows from the same early return in `onButtonPress`, which
  runs before the reporting branch; it is not covered by a test.)
- `effects_test` printed "OK" half-way, before the M2 blocks, which hid two wrong expectations
  in the first item-5 version (3198a9c): the glass-centre SDF bound (the value is −180 px, the
  smaller half extent) and a reflection point that assumed a flat glass edge (the default
  curvature pulls the edge in to x≈29 at mid-height). The checks now find the warped edge, and
  the flat case is checked against the exact value; "OK" is printed once at the end.

### Item 5 cost: idle CPU and per-frame time

Raw data: `docs/bench/m2s2-host/` (bench-s2-host.sh, bench.txt, idle.txt) and
`docs/bench/m2s2-tcg-lcos-100-01/` (vm-s2.sh, run.sh, bench-s2.sh, bench.txt, idle.txt, status).
Idle = the real `lunduke-terminal`, 1280×800, bash idle at a prompt, window focused, CPU =
(utime+stime)/wall over 10 s (host after 18 s, TCG after 40 s), 100 % = one core. Bezel =
`--profile amber --frame`; RGB shift = `--profile ibm-dos` (0.3524) vs `--rgb-shift 0`.

**Idle CPU, host (Debian 13, llvmpipe, own Xvfb :43) — REAL** (governor tick interval in brackets)

| Quality | amber | amber + bezel | ibm-dos RGB .3524 | ibm-dos RGB 0 |
|---|---|---|---|---|
| GL auto (→ reduced) | 18.6 % (207 ms) | 16.2 % (283 ms) | 17.5 % (206 ms) | 18.3 % (214 ms) |
| GL full | 21.4 % (212 ms) | 21.2 % (247 ms) | 22.6 % (210 ms) | 21.6 % (206 ms) |
| GL reduced | 16.7 % (225 ms) | 15.8 % (276 ms) | 17.2 % (231 ms) | 19.2 % (171 ms) |
| GL static | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| CPU 4 thr auto (→ reduced) | 16.4 % (265 ms) | 17.7 % (274 ms) | 20.5 % (244 ms) | 17.6 % (249 ms) |
| CPU 4 thr full | 17.5 % (387 ms) | 20.5 % (388 ms) | 20.4 % (417 ms) | 20.3 % (302 ms) |
| CPU 4 thr reduced | 16.6 % (289 ms) | 17.7 % (263 ms) | 20.7 % (279 ms) | 14.9 % (287 ms) |
| CPU 4 thr static | 0.0 % | 0.0 % | 0.0 % | 0.0 % |

The governor holds the budget, so the bezel shows up as a longer tick interval (GL 207 → 283 ms
under auto) rather than as more CPU. The host was shared with other agents' work; the
differences of 1–3 points between the RGB-shift columns are within run-to-run noise.

**Idle CPU, TCG VM (LCOS 1.0 live ISO, `-accel tcg -smp 4 -snapshot`, llvmpipe) — REAL**

| Quality | amber | amber + bezel | ibm-dos RGB .3524 |
|---|---|---|---|
| GL auto (→ static) | 0.0 % | 0.0 % | 0.0 % |
| GL static | 0.0 % | 0.0 % | 0.0 % |
| CPU 4 thr auto (→ static) | 0.0 % | 0.0 % | 0.0 % |
| CPU 4 thr static | 0.0 % | 0.0 % | 0.0 % |

The run took 19:00:43–19:16:19 UTC (14:00–14:16 CDT); the VM was stopped with QMP `quit` at
14:17:56 CDT. Explicit full/reduced were not re-run on TCG (they were > 100 % already without
the new effects, see stage 1).

**Per frame, median ms (tick / content pass), 1280×800, 4 threads — REAL**

| Case | host GL (LP 4) | host CPU | TCG GL | TCG CPU |
|---|---|---|---|---|
| amber full | 7.80 / 27.56 | 14.84 / 16.58 | 2247.39 / 3372.53 | 410.40 / 701.34 |
| amber + bezel full | 9.06 / 29.72 | 15.36 / 17.75 | 2412.06 / 3958.36 | 455.74 / 791.30 |
| amber reduced | 6.71 / 20.80 | 10.60 / 13.94 | — | — |
| amber + bezel reduced | 7.78 / 24.27 | 13.51 / 17.85 | — | — |
| amber static | 4.76 / 19.42 | 7.01 / 16.96 | 750.29 / 2310.22 | 215.32 / 625.87 |
| amber + bezel static | 5.51 / 22.78 | 6.80 / 14.94 | 816.40 / 2839.83 | 233.01 / 624.67 |
| ibm-dos RGB .3524 full | 8.08 / 29.52 | 15.52 / 22.04 | 2248.08 / 4225.19 | 427.92 / 931.33 |
| ibm-dos RGB 0 full | 7.26 / 25.88 | 16.12 / 19.62 | — | — |
| ibm-dos RGB .3524 static | 4.64 / 20.63 | 6.28 / 19.13 | 750.23 / 2819.93 | 201.97 / 800.36 |
| ibm-dos RGB 0 static | 4.56 / 17.82 | 5.84 / 13.69 | — | — |

In static mode the tick is the one dynamic pass after a content change; nothing runs while idle.

**DERIVED** (ratios of the REAL medians above)

| Metric | host GL | host CPU | TCG GL | TCG CPU |
|---|---|---|---|---|
| bezel / no bezel, tick, full | 1.16 | 1.03 | 1.07 | 1.11 |
| bezel / no bezel, tick, static | 1.16 | 0.97 | 1.09 | 1.08 |
| bezel / no bezel, content pass, full | 1.08 | 1.07 | 1.17 | 1.13 |
| RGB .3524 / RGB 0, content pass, full | 1.14 | 1.12 | — | — |
| RGB .3524 / RGB 0, content pass, static | 1.16 | 1.40 | — | — |
| RGB .3524 / RGB 0, tick, static | 1.02 | 1.08 | — | — |

The bezel costs about 10 % per tick (one premultiplied composite) and nothing when idle in
static. The RGB shift costs 12–40 % of the content pass (three samples instead of one in the
static pass) and nothing per tick.

### Screenshots (`/workspace/lcos-app-reviews/terminal-m2/`, host, 1000×640 logical)

- RGB shift: `s2-rgbshift-ibmdos-gl.png`, `s2-rgbshift-ibmdos-cpu.png` (full),
  `s2-rgbshift-ibmdos-static-{gl,cpu}.png`, `s2-rgbshift-ibmdos-off.png` (`--rgb-shift 0`),
  `s2-rgbshift-ibmdos-crop-x4.png` / `s2-rgbshift-ibmdos-off-crop-x4.png` (4× crops, fringes),
  `s2-rgbshift-amber-static.png` (`--rgb-shift 0.3524` on amber), `s2-rgbshift-ibmdos-scale2.png`.
- Bezel: `s2-bezel-amber-{gl,cpu}-full.png`, `s2-bezel-amber-gl-reduced.png`,
  `s2-bezel-amber-{gl,cpu}-static.png`, `s2-bezel-green-gl-static.png`,
  `s2-bezel-green-scanlines-gl-full.png`, `s2-bezel-amber-glossy-light.png` (size 44, #c8c0b0,
  shininess 1), `s2-bezel-amber-matte.png` (shininess 0), `s2-bezel-amber-scale2-gl.png`,
  `s2-bezel-amber-scale2-cpu-static.png`, `s2-bezel-plain-ignored.png`,
  `s2-bezel-ibmdos-ignored.png` (`--frame` ignored with a warning).
- Bezel mouse (xdotool): a drag that starts on the left bezel selects nothing, also when it
  ends in the far corner (`s2-bezel-drag-from-bezel.png`, `s2-bezel-drag-clamped-corner.png`),
  nor does a click / double-click on the top bezel (`s2-bezel-click-on-bezel.png`); a
  double-click inside selects a word (`s2-bezel-doubleclick-inside.png`); a drag from the glass
  out to the bezel corner clamps to the last cell (`s2-bezel-drag-inside-to-bezel-corner.png`),
  and a later click on the bezel leaves that selection alone
  (`s2-bezel-click-on-bezel-keeps-selection.png`).
