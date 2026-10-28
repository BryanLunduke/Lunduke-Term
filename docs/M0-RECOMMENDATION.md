# Lunduke Terminal M0 spike: findings and recommendation (draft)

Date: 2026-10-28 (America/Chicago). Branch `m0-spike`. Measured on the shared build box:
8 vCPUs "Intel Xeon Processor" (AVX2/AVX-512 capable, 1 thread per core), 15 GiB RAM (about 1-3 GiB free
during the runs, other agents active; load average 0.8-1.8), Debian 13, Mesa 25.0.7 llvmpipe
(LLVM 19.1.7, 256-bit), libvterm 0.3.3, gtkmm 3.24.10, Xvfb. **Not** measured on LCOS hardware or in a TCG VM.

## 1. What was built

- **(a) Terminal**: `lunduke-terminal` runs `$SHELL` (or `-e CMD`) on a PTY from `forkpty()`, feeds libvterm
  0.3.3, copies the libvterm screen into a renderer-neutral `Grid` and paints it with Pango/Cairo into an
  image, which goes through the CPU effects pass and is blitted to a gtkmm-3.0 `DrawingArea`.
  Works: 16/256/truecolour (libvterm palette), bold/italic/underline/reverse, block cursor, UTF-8 and wide
  characters, alternate screen (`top`), OSC 2 window title, keyboard (printables, Ctrl/Alt chords, Enter,
  Tab, Backspace, Esc, arrows, Home/End, PgUp/PgDn, Ins/Del, F1-F12 via `vterm_keyboard_*`), resize
  (`vterm_set_size` + `TIOCSWINSZ`: 1280x800 gave 32x107 cells, resizing to 960x600 gave 24x80 as reported by
  `stty size`), clean exit when the shell exits. Not in M0: scrollback, selection/clipboard, mouse, IME,
  bell, blinking, font zoom, settings, any input mapping through the warp.
- **(b) CPU effects** (`src/cpu_effects.cpp`): plain C++17, no intrinsics, row-parallel on a small persistent
  thread pool. Passes follow cool-retro-term 1.2.0 (`ShaderTerminal.qml`, `BurnInEffect.qml`,
  `ShaderLibrary.qml`, "Default Amber" profile values):
  1. bloom source: 2x2 box downsample + separable Gaussian, radius 20 at half resolution, transparent border;
  2. burn-in feedback buffer in source space: `max(old - decay, current)`, decay from elapsed ms x
     `1/lint(160,1600,burnIn)`;
  3. static pass (on content change): barrel warp `v - cc(1+d)d`, chroma/phosphor tint, bloom add (clamped 0.5),
     brightness;
  4. dynamic pass (every effects tick, 20 fps like CRT 1.2.0): horizontal sync, jitter, static noise with
     vignette, glowing line, burn-in combine, scanline rasterization (CRT mode 1), flicker, ambient light,
     outside-the-tube mask (black, stand-in for the bezel which is not in M0).
  Barrel coordinates, distance and in-screen mask are a per-pixel lookup table rebuilt on resize.
  Scanlines are on in the spike (Amber has them off) so their cost is included; period 3 output px.
- **(c) GL**: the same passes as GLSL 3.30 core fragment shaders (`src/gl_shaders.hpp`) on a headless EGL
  surfaceless context (`EGL_MESA_platform_surfaceless`, falls back to a pbuffer), float (RGBA32F)
  intermediate textures, `GL_LINEAR` + `CLAMP_TO_EDGE`/`REPEAT` sampling, exact `texelFetch` taps for the
  blur. The CPU sampler reproduces `GL_LINEAR` exactly (texel centres at +0.5, same clamp/repeat rules), all
  per-frame random values (flicker, h-sync) are computed once on the CPU and passed as uniforms, and the
  noise texture is the same deterministic 512x512 table on both sides. I used EGL rather than GtkGLArea so the
  comparison runs headless and in `meson test`; a GtkGLArea view is M4 work.

## 2. Performance (1280x800, medians of 5 runs x 30 frames)

"Full" = the terminal content changed (bytes to float, bloom, burn-in, static pass, dynamic pass).
"Tick" = an animated-effects frame with unchanged content (dynamic pass only). Excludes the X11 blit.

| Back-end | Threads | Full update | Tick | Bloom | Burn-in | Static |
|---|---|---|---|---|---|---|
| CPU C++ (`-O3`, x86-64 baseline) | 1 | 126.2 ms (7.9 fps) | 62.9 ms (15.9 fps) | 16.8 ms | 4.5 ms | 38.3 ms |
| CPU C++ | 4 | 32.2 ms (31.1 fps) | 16.6 ms (60.4 fps) | 4.5 ms | 1.0 ms | 9.9 ms |
| CPU C++ | 8 | 19.9 ms (50.2 fps) | 10.1 ms (99.0 fps) | 3.0 ms | 0.7 ms | 5.8 ms |
| CPU C++ built with `-march=x86-64-v3` (test only, 3 runs x 20; two sessions gave 101.4/47.6 and 97.7/48.3) | 1 | 97.7 ms | 48.3 ms | 14.8 ms | 3.6 ms | 29.7 ms |
| GL on llvmpipe, `LP_NUM_THREADS=1` | 1 | 138.3 ms (7.2 fps) | 26.1 ms (38.3 fps) | - | - | - |
| GL on llvmpipe | 4 | 39.2 ms (25.5 fps) | 6.7 ms (149 fps) | - | - | - |
| GL on llvmpipe | 8 | 22.7 ms (44.0 fps) | 3.6 ms (277 fps) | - | - | - |

GL numbers include the CPU byte-to-float conversion, the 16 MB float texture upload and `glReadPixels`.
Raw logs: `bench-cpu.txt`, `bench-gl.txt` next to the screenshots.

Other measurements:
- Synthetic frame (88x26 cells, Terminus 28 px) painted with one Pango layout per cell: **47-55 ms**. This,
  not the effects, is the most expensive part of a full update today.
- Live terminal at 1280x800, 4 effect threads, shell idle, effects ticking at 20 fps: **about 149% CPU**
  (pidstat, 10 s average). Same window with `--no-effects`, idle: **0.0%**.
  Resident memory about 159 MB with effects on.
- CPU 1-thread output equals 4-thread output byte for byte.

**Finding that changes the research recommendation**: llvmpipe ran the per-tick pass **2.4-2.8x faster than my
plain C++ code** at the same thread count, because it JIT-compiles the shader to 256-bit SIMD. The research
note assumed a hand-written CPU pipeline would be at least as fast as llvmpipe. That holds only if the CPU code is
vectorised (SIMD over x, 16-bit fixed point instead of float RGBA, half-resolution bloom/burn-in as CRT does,
nearest noise lookups). The full update is about equal on both because both are dominated by
the blur and data movement.

## 3. Software vs GL match

Golden sequence (same input on both): frame A at t=1000 ms, frame B (scrolled 3 lines) at t=1100 ms, tick at
t=1180 ms, so burn-in, bloom, noise, jitter, h-sync, flicker, glowing line and scanlines are all active.

| Size | Mean abs diff (0-255) | Max diff | PSNR | Pixels differing | Pixels with diff > 2 |
|---|---|---|---|---|---|
| 1280x800 (`tools/make-comparison.sh`) | 0.000003 (ImageMagick MAE 0.00075 of 65535) | 1 | 103.46 dB | 9 (ImageMagick AE) of 1,024,000 | 0 |
| 640x400 (`meson test gl-match`, threshold 40 dB) | - | 1 | 106.98 dB | - | 0 |

Visually identical. The remaining 1-level differences come from float rounding (`sin`, fused multiply-add)
at 8-bit quantisation boundaries. What made the match work: gamma-encoded values on both sides with no sRGB
textures or framebuffers (CRT 1.2.0 does no linearisation either), float intermediates on both sides (RGBA8
FBOs would add up to 1/255 per pass), a CPU sampler that copies `GL_LINEAR` texel-centre rules, `texelFetch`
for the blur taps, and per-frame random values computed only once.

## 4. Images (`/workspace/lcos-app-reviews/terminal-m0/`)

- `side-by-side-cpu-vs-gl.png`: CPU (left) and GL/llvmpipe (right) golden frame at 1280x800.
- `cpu.png`, `gl.png`, `source.png`, `source-b.png`, `zoom-corner-cpu-vs-gl.png` (2x crop), `diff-highlight.png`
  (ImageMagick highlight), `diff-x32.png` (difference x32).
- `terminal-window-effects.png`: live libvterm shell with the Amber effects (ls, stty size, colours, OSC title).
- `terminal-window-top-altscreen.png`: `top` on the alternate screen through the effects.
- `terminal-window-resized-960x600.png`: after a window resize (`stty size` = 24 80).
- `terminal-window-no-effects.png`: the plain renderer, full ANSI colours.

## 5. Problems and risks

1. **CPU cost of animated effects.** About 1.5 cores busy while the shell is idle at 1280x800 is too much for a
   default profile on older LCOS machines. Needed: SIMD/fixed-point kernels, half-resolution bloom and burn-in
   (CRT's `bloomQuality`/`burnInQuality` 0.5), render scale, effects FPS setting, pause when unfocused or
   occluded, and "reduce motion" (no noise, flicker, jitter) so the tick can stop entirely.
2. **Glyph rendering cost.** One Pango layout per cell is about 50 ms for a full screen. M1 needs a glyph cache
   (atlas of rasterised glyphs per font/attribute), damage-only row repaint and run-based shaping for
   ligature-free monospace text.
3. **Software GL may be the better software path.** Because llvmpipe is faster for the per-pixel pass, "CPU
   default, GL later" should become: one GLSL pipeline that runs on a GPU or on llvmpipe, plus a SIMD C++
   fallback for systems without usable EGL/GL 3.3. This needs an editor decision (see 7).
4. **Colour on monochrome profiles.** With Amber's chroma 0.25, libvterm's dark default blue (`ls` directory
   colour) turns almost black. CRT has the same problem in principle. We need a palette per profile or a minimum
   luminance for the tinted foreground.
5. **Bezel/frame, reflections and rounded corners** are not implemented (outside the tube is black). The
   reflection maths from the static pass is in place but masked.
6. **The warp breaks input mapping.** Mouse and selection need the inverse barrel map (Newton iterations or an
   inverse lookup table). Not done in M0.
7. **GtkGLArea on GTK3/X11** is untested; I used headless EGL. The M0 GL path reads back with `glReadPixels`; a real GtkGLArea view would avoid that copy. GL textures also leak on resize in the spike.
8. Measured on a shared, busy box: absolute numbers vary between runs (two 1-thread x86-64-v3 sessions differed by about 4%); treat them as relative.

## 6. Proposed M1 (plain working terminal), adjusted by M0

1. Keep libvterm + `forkpty` + `Grid` and replace per-cell Pango layouts with a glyph atlas and dirty-row
   rendering. Target: a full repaint under 5 ms at 1280x800, and a 50 MB `cat` limited by libvterm.
2. Effects stay opt-in in M1 (a "Plain" profile first), with the M0 pipeline behind a flag. Effects work moves
   to M3/M4 with the decision below.
3. Scrollback ring buffer, selection, PRIMARY/CLIPBOARD, bracketed paste, mouse reporting, bell, cursor shapes,
   blink, font zoom, `TERM=xterm-256color` terminfo check, vttest basics.
4. A test harness: libvterm byte-stream tests (as in `tests/vterm_test.cpp`), CPU/GL golden-image tests (as in
   `tests/gl_match_test.cpp`) and Xvfb UI tests like Paint/Edit.
5. Throughput and frame-time benchmarks on real LCOS hardware and in a TCG VM (no KVM).

Decision needed before M3: **(A)** GLSL as the single effects implementation (GPU or llvmpipe) with a SIMD C++
fallback, or **(B)** the current plan: a C++ default with GL later. M0 data favours (A) for speed and for keeping
software and GL identical by construction. The golden-image test then guards the C++ fallback.

## 7. Retro fonts (prefer Debian packages; licences from the CRT 1.2.0 tree and package metadata, to be confirmed against each `debian/copyright` before M5)

| Font (CRT profile use) | Debian package (trixie) | Licence | Notes |
|---|---|---|---|
| Terminus TTF 4.46 (Default Amber, Monochrome Green) | `fonts-terminus` 1.2.0+ds2-4 (built from the cool-retro-term source), `fonts-terminus-otb` 4.48 | SIL OFL 1.1 | Used for the M0 screenshots (extracted locally, not installed). `xfonts-terminus` is PCF, which Pango 1.44+ cannot use. |
| Hermit | `fonts-hermit` (cool-retro-term source) | OFL 1.1 | |
| ProggyTiny | `fonts-proggy` (cool-retro-term source) | MIT | |
| IBM 3270 (IBM 3278 profile) | `fonts-3270` 3.0.1 | BSD-3-Clause | |
| Inconsolata | `fonts-inconsolata` | OFL 1.1 | |
| PxPlus IBM VGA8 / IBM BIOS (IBM Dos, IBM VGA profiles) | `fonts-pc`, `fonts-pc-extra` 1.0 (int10h Ultimate Oldschool PC Font Pack) | CC BY-SA 4.0 | Depended on by Debian's cool-retro-term. |
| C64 look (Commodore 64 profile) | `fonts-sixtyfour` 1.0 (src `fonts-homecomputer`) | OFL 1.1 (to verify) | Free replacement for C64 Pro Mono, which is **non-free** (Style licence: no modification, no redistribution in collections). |
| Amiga Topaz look | `fonts-amiga` (src `fonts-homecomputer`) | OFL 1.1 (to verify) | |
| Atari | `fonts-atarist`, `fonts-atarismall` | to verify | CRT's AtariClassic licence is only in a PDF; unverified. |
| Apple II (PrintChar21, PRNumber3), Commodore PET (PetMe) | not in Debian | Kreative "Free Use License" 1.2f: **no selling copies** | Do not bundle on a sold ISO without the editor's/legal OK. Look for free alternatives. |
| Fixedsys Excelsior | not a separate Debian package (covered in the cool-retro-term `debian/copyright`) | "dfsg-compliant" public-domain-style text | Check before bundling. |
| Fallbacks | `fonts-unifont`, `fonts-spleen` (BSD-2-Clause), `fonts-hack` (MIT), `fonts-dejavu-core` | various free | |

Recommendation: depend on the Debian packages above (all already pulled in by Debian's `cool-retro-term`, so LCOS
loses nothing when it is replaced), and bundle nothing in M1. Revisit Apple II/PET/Atari in M5.

## 8. Part status

- (a) **done** (libvterm 0.3.3 was installed during the spike).
- (b) **done**, with numbers above.
- (c) **done** with headless EGL + llvmpipe; GtkGLArea view pending (M4).
