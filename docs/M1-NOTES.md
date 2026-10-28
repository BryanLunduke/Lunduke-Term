# M1 notes — checkpoints A, B and C

Checkpoint A: GtkGLArea, glyph cache, idle CPU, profiles. Checkpoint B (at the end): PTY,
scrollback, selection, clipboard, mouse reporting, bell, cursor shapes.

Branch `m1-core` from master `71de83e` (M0 spike merged in PR #1). Checkpoint A covers M1
items 1–3; checkpoints B and C are listed at the end.

## What changed

**GL is the main effects path (item 1).**
- The window now has a real `Gtk::GLArea` (GL 3.3 core). The GLSL pipeline moved out of the
  headless EGL wrapper into `GlPipeline` (`src/gl_pipeline.*`), which runs on whatever context
  is current: GtkGLArea in the window, or EGL in tests and benchmarks (`GlEffects`).
- It renders straight into GtkGLArea's framebuffer (a `flipY` uniform; no `glReadPixels`).
- The multithreaded C++ pipeline is the fallback. It is used when the GL context or shaders
  fail (`--renderer auto`), or when forced with `--renderer cpu`. It still matches the GL
  output to the PSNR tests (meson test `gl-match`).
- **M0 texture leak fixed.** `resize()` now frees the previous size's textures and FBOs.
  `GlPipeline` counts its live objects, `countGlTextures()` cross-checks them with
  `glIsTexture`, and `gl-match` runs a 50-resize loop that must keep both counts constant
  and still match the CPU output afterwards.
- Source rows upload as 8-bit BGRA (Cairo's own layout) and only the dirty rows go up. The
  CPU float conversion is gone.

**Rendering cost (item 2).**
- **Glyph cache.** `GridPainter::paintRows` rasterises each glyph once into an A8 mask. The
  cache key is text, bold, italic, underline and width. Masks are composited with
  `cairo_mask_surface`. The output is pixel-identical to the M0 per-cell Pango painter
  (test `painter`).
- **Changed rows only.** The grid is double-buffered, and `changedRows()` diffs cells and the
  cursor. Only dirty row bands are repainted into the source surface, and only they are
  uploaded to GL. Plain mode invalidates only that band (`queue_draw_area`).
- **Frame scheduler.**
  - Content frames are capped at `--max-fps` (60), or 30 with effects on.
  - The effects tick runs at `--effects-fps` (20). It only runs while the picture can still
    change on its own: noise, flicker, jitter, sync or the glowing line, or a burn-in ghost
    that is still fading.
  - Nothing redraws when nothing changes.
- **Pausing.** The tick stops when the window is unfocused (`--no-pause-unfocused` turns this
  off), minimized, unmapped or fully obscured.
- **Half-resolution buffers.** Burn-in runs at half resolution (as cool-retro-term's
  burnInQuality 0.5) from a shared 2×2 downsample, which the bloom blur also reads.
  `EffectParams::burnInHalfRes = false` restores the M0 full-res mode, which is still tested.

**Profiles (item 3).** `src/profiles.*`, `--profile ID`, `--list-profiles`, Ctrl+Shift+P cycles.

| id | look | effects |
|---|---|---|
| `plain` (**default**) | Tango palette, DejaVu Sans Mono 16 px | off (Ctrl+Shift+E / `--effects` turns on amber-tube effects) |
| `amber` | CRT 1.2.0 "Default Amber" (no scanlines), Terminus 22 px | on |
| `green` | "Monochrome Green" (#0ccc68, chroma 0) | on |
| `green-scanlines` | green with rasterization 1 | on |
| `ibm-dos` | "IBM Dos" values, full colour, CGA palette | on |
| `ibm-3278` | "IBM 3278" (no noise/flicker/jitter, long burn-in) | on, static: no idle ticks |

- **Per-profile palettes.**
  - Monochrome tubes get a phosphor palette: every ANSI colour is lifted towards white until
    its grey level is at least 0.50 (bright colours 0.68, colour 8 0.38). Black stays black.
    256-colour and truecolour foregrounds are lifted at mapping time (`minFgGrey`).
  - The dark-blue `ls` directory went from grey 0.12 (nearly black after the amber tint) to
    0.50. Compare screenshots 02 and 10.
  - `ibm-dos` keeps hues but lifts CGA blue (#0000aa) to grey 0.30.
- `plain` with effects switched on gets the same phosphor palette instead of Tango.

**Settings.**
- `$XDG_CONFIG_HOME/lunduke-terminal/settings.ini`, group `[terminal]`, with the keys
  `profile`, `effects`, `renderer`, `threads`, `effects-fps`, `max-fps`, `pause-unfocused`,
  `font` and `font-px`.
- The command line overrides it.

## Before / after

**Setup.**
- Xvfb 1280×800 with xfwm4, Mesa 25.0.7 llvmpipe (LLVM 19.1.7, AVX2), 4 threads unless noted.
- Idle CPU is `pidstat` over 10 s with an idle bash.
- The before builds are master 71de83e. Bench numbers come from `tools/m1_bench.cpp`; the M0
  leak numbers come from a small program linked against the M0 libraries.

| | M0 (71de83e) | M1 checkpoint A |
|---|---|---|
| Idle CPU, default launch | 149.8 % (effects on, CPU path) | **0.0 %** (plain default) |
| Idle CPU, effects off | 0.0 % | 0.0 % |
| Idle CPU, amber, focused | 149.8 % (CPU, 4 threads) | **94.7 %** GL at 20 fps (44.6 % at 10 fps), 132.7 % CPU fallback |
| Idle CPU, amber, unfocused | 145.8 % | **0.0 %** (GL and CPU) |
| Idle CPU, minimized | 0.2 % | 0.0 % |
| Idle CPU, ibm-3278 (static profile), focused | n/a | 0.0 % |
| Full-screen repaint, 112×32 Terminus 22 px | 10.7 ms (per-cell Pango) | **2.9 ms** warm glyph cache (3.5 ms cold) |
| Full-screen repaint, 123×39 DejaVu 16 px | 13.1 ms | **2.5 ms** |
| One changed row | full repaint | **0.06–0.11 ms** |
| In-app median paint (`--stats`) | ~47–55 ms (M0, 28 px, cell by cell) | 2.5–4.2 ms |
| CPU effects tick, 1/4/8 threads | 16.6 ms at 4 threads (M0 params) | 59.7 / 17.2 / 9.1 ms (amber) |
| GL tick, llvmpipe, LP_NUM_THREADS 1/4/8 | 6.7 ms at 4 threads (incl. readback) | 30.7 / 7.9 / 4.6 ms (no readback; draws into the window) |
| GL vs CPU PSNR, 1280×800 | 103.46 dB (max diff 1) | 103.97 dB amber (max 1), 109.99 ibm-dos, 113.01 green-scanlines |
| 60-resize loop, GL textures | **8 → 435** (leak) | 10 → 10, FBOs 7 → 7 |
| 60-resize loop, RSS | **236 MB → 4.6 GB** | 251 → 292 MB after the first cycle (llvmpipe high-water mark), then flat (285 MB at 60 resizes, 279 MB at 300) |
| PSNR after the resize loop | 103.46 dB | 103.97 dB |
| RSS in the window | 159.5 MB effects / 55.4 MB plain | 228 MB GL amber / 127 MB CPU amber / 51.7 MB plain |

**Reading the numbers.**
- The large wins are structural: the plain default, no ticks when unfocused, hidden or static,
  and cheap repaints.
- A focused, animated CRT profile still costs real CPU under llvmpipe. It costs about 4.6 %
  per effects fps, because every tick re-runs the full-screen dynamic shader and GTK
  composites the GL area. On a real GPU this would be near zero; checkpoint C measures it on
  TCG and limited-thread llvmpipe.
- The 20 fps default is a compromise; `--effects-fps 10` halves the cost.

## Problems and open points

- **GL memory.** GL RSS is higher than the CPU path (llvmpipe plus GtkGLArea buffers).
- **HiDPI.** The scale factor is not handled; scale 1 is assumed.
- **Unimplemented CRT effects.** `rbgShift` (IBM Dos) is still missing. Frame/border
  reflections are not done.
- **Plain margins.** `plain` keeps its 4 px margin when effects are toggled on, so the
  barrel warp clips the corners a little (screenshot 07).
- **The glowing line.** The sweeping brighter band in some screenshots is cool-retro-term's
  glowing-line effect, which has a sawtooth edge. It is not an artefact.
- **`--stats` timings.** GL timings call `glFinish` only when `--stats` is on.
- **`ibm-dos` font.** Resolved in checkpoint C: Debian's fonts-pc calls the family `Px IBM VGA8`.
  See [FONTS.md](FONTS.md).

## What is left

**Checkpoints B and C (items 4–6).** Done; see the sections below.

**Checkpoint C (item 5).**
- Benchmarks in a TCG (no-KVM) VM.
- A real-hardware-style llvmpipe path with limited `LP_NUM_THREADS` and no AVX
  (`LP_NATIVE_VECTOR_WIDTH=128`, CPU-feature masks).

**Checkpoint C (item 6).**
- Licence-clean fonts only: fonts-pc, fonts-3270, fonts-terminus, fonts-hermit, fonts-proggy.
- Verify the sixtyfour / Amiga / Atari licences in the Debian copyright files.
- Avoid the Apple II, PET and original C64 fonts.
- Prefer depending on the Debian packages over bundling.

## Reproduce

    meson setup build && ninja -C build -j2 && meson test -C build
    build/lterm-m1-bench --paint [--profile plain]
    build/lterm-m1-bench --tick --threads 1,4,8
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4 build/lterm-m1-bench --tick-gl
    LIBGL_ALWAYS_SOFTWARE=1 build/lterm-m1-bench --compare --size 1280x800
    LIBGL_ALWAYS_SOFTWARE=1 build/lterm-m1-bench --resize-loop 60
    build/lunduke-terminal --profile amber --stats --quit-after 8000

# Checkpoint B — PTY, scrollback, selection, clipboard, mouse, bell, cursor

## PTY (`src/vterm_session.*`)

- Own spawn, no `forkpty`, no utmp/wtmp:
  - `posix_openpt(O_CLOEXEC)`, `grantpt`, `unlockpt`, `ptsname_r`, `TIOCSWINSZ`.
  - The child calls `setsid`, opens the slave, `TIOCSCTTY`, sets termios (`IUTF8`, `VERASE`
    = DEL) and `dup2`s 0–2.
  - It also closes fds 3–1023 and resets the signal mask and dispositions.
- **Signal safety.** argv and env are built before `fork`, so the child calls only
  async-signal-safe functions up to `execvpe`.
- **When exec fails.** The child prints `lunduke-terminal: cannot run X: <error>` and exits 127.
- **Shell.** `$SHELL`, then the passwd entry, then `/bin/sh`. A login shell (`--login` /
  `login-shell=true`) gets `argv[0]` = `-bash`.
- **Working directory.** `cwd` setting/`--cwd`, falling back to `$HOME`.
- **Environment:**
  - `TERM=xterm-256color`, `COLORTERM=truecolor`, `TERM_PROGRAM=lunduke-terminal`,
    `TERM_PROGRAM_VERSION`.
  - The inherited `TERM*`, `COLUMNS` and `LINES` are removed.
  - `LANG=C.UTF-8` is added only when none of `LANG`, `LC_ALL` or `LC_CTYPE` is set.
- **Child exit** goes through GLib `g_child_watch_add` (SIGCHLD + waitpid status), not PTY
  EOF:
  - The PTY is drained first so the last output is still shown.
  - `on-exit` = `close` | `hold` | `hold-on-error` (the default).
  - **hold-on-error** closes on a clean exit 0 and holds otherwise. A held window shows
    `[process exited with status N]` or `[process killed by signal N (Name)]` and prefixes
    the title with `[exited: N]` / `[killed: signal N]`. Enter, Esc or q closes it; other
    keys are ignored.
  - Closing the window sends SIGHUP to the child's process group.
- **Writes.** PTY writes are queued and flushed non-blockingly from an `IO_OUT` watch, so a
  large paste never blocks the UI.

## Scrollback

- **Buffer.** libvterm `sb_pushline`/`sb_popline`/`sb_clear` feed a deque of trimmed cell rows
  with a soft-wrap flag:
  - Default size 10000 lines, set with `scrollback-lines` / `--scrollback N`; 0 disables it.
  - `ED 3` (`clear` / `\e[3J`) empties it.
  - Reflow is on.
- **Scrolling.** Mouse wheel (3 lines), Shift+PgUp/PgDn (a page minus one line),
  Shift+Home/End.
  - Typing snaps to the bottom.
  - While you are scrolled back, new output keeps the view anchored on the same text.
- **Alt screen.** Scrollback is not used in the alt screen (less, vim, top). The wheel sends
  3× Up/Down there instead, like xterm `alternateScroll`.
- **Limitation:**
  - The soft-wrap flag of a pushed line is read from libvterm's line info. That is exact
    for one-line scrolls, which is the normal case.
  - For a multi-line scroll in one operation (e.g. `CSI 5 S`) only the first line's flag is
    known, so the others count as hard breaks. The only effect is a newline in copied text.

## Selection (`src/selection.*`)

- **Coordinates.** Positions are absolute line numbers (scrollback + screen), so a selection
  survives scrolling and works across the scrollback/screen boundary.
- **Mouse gestures:**
  - Drag selects characters. Double-click selects a word; word characters are alnum plus
    `word-chars`, default `-_./~:@%+=?&#`, so paths and URLs select whole.
  - Triple-click selects a line, following soft wraps.
  - Shift+click extends. Dragging past the top or bottom edge auto-scrolls.
  - Wide characters are never split.
- **Copied text.** Trailing blanks are trimmed and soft wraps join without a newline.
- **Repaint.** Selected cells carry a `selected` flag that is part of `Cell::operator==`, so the
  changed-rows repaint redraws exactly the rows whose highlight changed.
- **When the selection is dropped:**
  - the selected text changes (snapshot compare on damage);
  - its history falls off the scrollback;
  - the alt screen switches;
  - the window is resized;
  - a selection that is only blanks is not kept.
- **CRT curvature.** The reverse warp is implemented in `pointToCell()`:
  - The GLSL/C++ effects sample the source at `barrel(p)` for output position `p`, with
    curvature = `screenCurvature × 0.4`.
  - So a pointer at `p` sits over source `barrel(p)`; no analytic inverse is needed.
  - The forward function is shared with `effects.hpp` and is unit-tested against it.
  - It applies only when effects are on (not `plain`). Jitter and horizontal sync are
    ignored because they are sub-cell and time-varying.
  - Screenshots b-10/b-10b show a double-click in amber near the top-left and bottom-right
    corners selecting the word under the pointer.

## Clipboard and paste

- **Copy and paste keys.** Ctrl+Shift+C copies to CLIPBOARD and Ctrl+Shift+V pastes from it.
- **PRIMARY.** Selecting sets PRIMARY; middle-click and Shift+Insert paste it.
- **Bracketed paste** (`?2004`) wraps the text in `ESC[200~ … ESC[201~` only when the
  application enabled it.
- **Sanitising.** Pasted text is always cleaned up:
  - CRLF/LF become CR;
  - C0 controls except TAB, and DEL, are dropped;
  - so an embedded `ESC[201~` cannot end the bracket early.

## Mouse reporting

- **Modes and encoding.** Modes `?9` (X10), `?1000`, `?1002` and `?1003`, with SGR `?1006`
  (and the default X10-style encoding).
  - libvterm ignores `?9`, so the session scans the output for it, including sequences split
    across reads. It then encodes press-only reports with no modifiers.
- **What is reported:** wheel (64/65), modifiers and motion.
- **Releases.** A release is always sent for a press that was reported, so the application
  never keeps a stuck button.
- **Shift bypass.** Holding Shift bypasses reporting, so you can select text inside mouse
  applications.
- **Focus events** (`?1004`) are sent on window focus changes.

## Bell

- **Setting.** `bell = visual,urgent` (default), plus `audible` or `none`; also `--bell LIST`.
- **Visual flash.** Inverts the screen for 120 ms (composed with DECSCNM reverse video).
- **Urgent.** Sets the window's urgency hint while unfocused and clears it on focus.
- **Audible.** `gdk_beep`.
- **Rate limit.** One bell per 200 ms.

## Cursor

- **Shapes.**
  - Block, underline and bar, from `cursor-shape` / `--cursor-shape`.
  - Applications can override it with DECSCUSR (`CSI n SP q`, 0–6) and `?12`.
  - Unfocused windows show a hollow block.
- **Blink** is off by default (`cursor-blink`, `--cursor-blink`).
  - When on, a 530 ms timer runs only while the window is focused, visible, not held and
    not scrolled back. Typing resets the phase.
  - DECSCUSR 0/1 turn it on, like xterm.

## Settings keys added (`[terminal]` in settings.ini)

`scrollback-lines`, `login-shell`, `cwd`, `on-exit`, `bell`, `cursor-shape`, `cursor-blink`,
`word-chars`.
CLI flags: `--scrollback N`, `--login`, `--cwd DIR`, `--on-exit close|hold|hold-on-error`,
`--hold`, `--bell LIST`, `--cursor-shape block|underline|bar`, `--cursor-blink`,
`--no-cursor-blink`.

## Tests

`meson test -C build` passes 6/6: profiles, vterm, **session (new)**, painter, effects and gl-match.

**session (headless).**
- **Scrollback:** limit, offset, colours, ED 3, limit 0, alt screen, wrap flag.
- **Selection text:** char, reversed, word, path, blank, line, multi-line from scrollback,
  soft wrap, wide characters, custom word chars.
- **`pointToCell`:** flat, and with curvature 0.12 against `barrel`.
- **Mouse encoding:**
  - none; X10, including a split `?9h`; 1000 in X10 encoding;
  - wheel; SGR 1006; Ctrl+right; 1002 drag; 1003 motion.
- **Paste:** unbracketed, bracketed, ESC stripped.
- **DECSCUSR and modes:** DECSCUSR 2/4/5, `?12`, `?25`, `?5`, focus `?1004`.
- **changedRows:** a change to the cursor shape or the `selected` flag.
- **Environment.**
- **Real PTY:**
  - TERM/COLORTERM, isatty, `stty size`, session leader, IUTF8;
  - exit status 3, cwd, SIGTERM, login shell `$0=-bash`;
  - a missing program exits 127 with a message.

**painter.** Gained pixel checks for the block, underline, bar and hollow cursors.

## Live checks (Xvfb, xfwm4, xdotool)

vim, less, htop, mc and vttest are not installed on the box, and big packages were not added.
The checks used bash, `cat -v`, a python3 curses mouse program, procps `top` and printf.

| Screenshot | Shows |
|---|---|
| b-01-env-pty | TERM/COLORTERM/LANG, `/dev/pts/0`, bash is the session leader |
| b-02-scrollback-wheel | 3 wheel clicks = 9 lines back |
| b-03-scrollback-shift-pgup | Shift+PgUp, a page back |
| b-04-selection-drag-in-scrollback | drag selection in scrollback, Ctrl+Shift+C → Ctrl+Shift+V into `cat` |
| b-04b-double-click-word | double-click selects `/usr/share/doc/lunduke-terminal/README.md` |
| b-04c-triple-click-line | triple-click line |
| b-04d-primary-middle-click-bracketed-paste | middle-click PRIMARY under `?2004` → `^[[200~…^[[201~` |
| b-05-mouse-sgr1006-shift-bypass | SGR reports `^[[<0;11;3M/m`, right = 2, wheel = 64; Shift-drag selects instead |
| b-05b-mouse-1003-1002-x10 | 1003 motion `<35;…`, 1002 drag, X10 press-only |
| b-06-mouse-python-curses | curses BUTTON1/3 CLICKED, BUTTON4/5, Ctrl modifier |
| b-07-cursor-shapes-decscusr | block / underline / bar / hollow (unfocused) via DECSCUSR |
| b-08-visual-bell-flash | visual bell (mean brightness 0.79 during, 0.008 after); urgency hint verified with xprop |
| b-09-exit-hold-status | `exit 3`: hold message and title `[exited: 3]`; Enter closes, other keys don't |
| b-09b-exit-signal | `kill -KILL $$`: `[process killed by signal 9 (Killed)]`; Esc closes |
| b-10 / b-10b | amber with curvature: double-click near the corners selects the word under the pointer (red ring) |
| b-11-top-procps | procps `top` running (it does not use the alt screen) |

## Idle CPU re-check (1000×600, pidstat over 10 s, terminal idle at a prompt)

| Case | CPU |
|---|---|
| plain, focused | 0.00 % |
| plain, `--cursor-blink` | 0.10 % (a 530 ms timer that repaints one row) |
| amber, unfocused | 0.00 % |

`--stats` with an idle plain window gives 4 content frames, 1 full repaint and 0 ticks after
both 4 s and 8 s, so nothing redraws when nothing changes. Pointer motion without mouse
reporting does not repaint either.

# Checkpoint C — benchmarks (item 5) and fonts (item 6)

## Benchmarks

**Where.** Everything is in [M1-BENCHMARKS.md](M1-BENCHMARKS.md), with raw output in `bench/`.
`tools/bench-m1.sh OUTDIR [BUILDDIR]` reruns the whole matrix:
- sizes 1280×800 and 1920×1080;
- plain vs amber repaint;
- CPU effects tick at 1/2/4 threads;
- llvmpipe at `LP_NUM_THREADS` 1/2/4;
- "no AVX" variants;
- idle CPU of the real terminal.

**Real runs.** Two:
- the box itself (8 vCPU Xeon with AVX-512);
- the LCOS 1.0 live ISO, booted read-only under `qemu-system-x86_64 -accel tcg` (no KVM, `-snapshot`).

**No AVX.**
- Our binaries are built for baseline x86-64 (no `-march`); objdump finds 0 ymm/zmm instructions.
- llvmpipe is restricted with `GALLIUM_OVERRIDE_CPU_CAPS=sse4.1 LP_NATIVE_VECTOR_WIDTH=128`. The
  renderer string then reports "128 bits", and `GALLIUM_DUMP_CPU` shows avx=0.
- For the CPU path, pixman and glibc AVX paths are masked with `PIXMAN_DISABLE` and
  `GLIBC_TUNABLES`.

## Fonts

**Where.** See [FONTS.md](FONTS.md) for the licence table and the profile → font mapping.

**New code.**
- `src/font_resolve.*` picks the first *installed* family through fontconfig. Each choice has its
  own pixel size, e.g. `Px IBM VGA8` at 32 px = 2× its 8×16 glyphs.
- A missing font falls back along the profile's list, then to `Monospace`, and never to the
  proportional sans that fontconfig would substitute.
- The terminal warns when a `--font` is missing or the pick is not monospaced.
- `--list-profiles` shows what each profile resolves to.
- New meson test `fonts`.
- Screenshots `c-01`…`c-08` are in `/workspace/lcos-app-reviews/terminal-m1/`.

## Tests

`meson test -C build` passes 7/7: profiles, vterm, **fonts (new)**, session, painter, effects and
gl-match.

## Problems during checkpoint C

**The box was reset mid-run (about 11:02 CDT).**
- Lost: the build directory, apt packages, the first TCG VM and some uncommitted files.
- Done again:
  - rebuilt the dev packages, fetching apt indexes and .debs over https with curl because apt's
    own https failed through the proxy;
  - rewrote `tests/fonts_test.cpp`, `tools/bench-m1.sh` and `docs/FONTS.md`;
  - rebooted the TCG VM from scratch.
- The host benchmarks in `bench/host/` were run after the reset. They differ a little from the
  checkpoint A numbers (it is a different box instance).

**fonts-terminus** has an incomplete Debian copyright file (see FONTS.md). It is not on the
LCOS 1.0 ISO, so amber/green fall back to DejaVu Sans Mono there until it is added.

# M1 summary

M1 turned the M0 spike into a usable terminal. Not part of LCOS 1.0.
- **Rendering:**
  - GtkGLArea GLSL effects, with a multithreaded C++ fallback;
  - glyph cache and changed-rows-only repaint;
  - idle-aware frame scheduler: no ticks when unfocused, hidden or static;
  - plain no-effects default (0 % idle CPU).
- **Profiles:** plain, amber, green, green-scanlines, ibm-dos and ibm-3278, with phosphor-safe
  palettes and licence-checked Debian fonts, plus a monospace-safe fallback.
- **Terminal core:**
  - own PTY spawn, scrollback, selection (curvature-aware), CLIPBOARD/PRIMARY, bracketed paste;
  - mouse reporting (X10/1000/1002/1003, SGR), bell and cursor shapes;
  - exit/hold handling and `settings.ini`.
- **Measured** on the AVX-512 box, with llvmpipe limited to 1/2/4 threads and no AVX, and in a
  no-KVM TCG VM of LCOS 1.0.
- **Blocking "M1 ready":** nothing functional. The animated CRT profiles are still expensive on
  pure software rendering (TCG numbers in M1-BENCHMARKS.md). That is why `plain` stays the
  default and the effects remain opt-in.

## M2 proposals (remaining work)

1. **Cheaper effects on software GL.**
   - Split the shader into a static pass (curvature, bloom, burn-in, rasterization), cached
     until the content changes, and a light per-tick dynamic pass (noise, flicker, glow line).
   - Lower the default effects fps on llvmpipe (10).
   - Auto-pick `--effects-fps` from a startup self-benchmark.
   - Optionally render the effects at half resolution and upscale.
2. **Detect software rendering** (llvmpipe/softpipe) and default an amber choice to "static" CRT
   (no animated noise or flicker) there.
3. **Packaging (separate task):**
   - Debian `Depends: fonts-dejavu-core`;
   - `Recommends: fonts-terminus | fonts-terminus-otb, fonts-pc, fonts-3270`;
   - a .desktop file, man page and AppStream metainfo.
   - Propose adding fonts-terminus to a later LCOS release.
   - Report the fonts-terminus copyright gap to Debian.
4. **New profiles** on licence-clean fonts: c64 (`C-64`), amiga (`Amiga`), atari-800, atari-st,
   modern (Hermit).
5. **Missing CRT effects:** `rbgShift` (IBM DOS), frame/border reflections, a margin fix for
   `plain` + effects.
6. **HiDPI:** scale factor, and font size in points vs pixels.
7. **Terminal features:**
   - tabs or multiple windows;
   - a search in scrollback;
   - URL detection and opening;
   - a settings dialog (GTK) instead of editing `settings.ini`;
   - drag-and-drop paste;
   - OSC 52 / OSC 8.
8. **Tests:** a vttest/esctest pass on a machine that has them, plus the large apps (vim, htop,
   mc). A11y via ATK was not looked at.
9. **CI:** a GitHub Actions job running `meson test` with Xvfb and llvmpipe.
