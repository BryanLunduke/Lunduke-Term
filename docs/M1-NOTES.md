# M1 notes — checkpoint A (GtkGLArea, glyph cache, idle CPU, profiles)

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
- **`ibm-dos` font.** The profile's first-choice family, "Px437 IBM VGA8", is only referenced
  by name, nothing is bundled. Its licence review belongs to checkpoint C. Without it the
  profile falls back to Terminus.

## What is left

**Checkpoint B (item 4).**
- Own PTY handling (instead of `forkpty` defaults).
- Scrollback and selection.
- Clipboard, including PRIMARY.
- Mouse reporting, bell, cursor shapes.

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
