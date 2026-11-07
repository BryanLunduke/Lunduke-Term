# Memory review after the 80×24 default

The two cuts the editor approved are done: the noise table is bytes (item 1) and the CPU output surface is not allocated on the GL path (item 2). The rest of this file is still a note, not a patch. The UX work in PR #12 (80×24 window, 16 px font, “color” spelling, slightly tighter IBM DOS bloom) is unchanged. The smaller window is called out below because it changes the RAM picture, and it should be remeasured before any further cut.

Compared with the review from 2026-11-03 (master `21eff10`, before PRs #7–#11). That review’s completed items are listed so they are not redone. Numbers below that are not from a process listing are **estimates from the allocation sizes in the code**, not new RSS measurements. The editor’s LCOS 1.0.1 figure (Lunduke Terminal about 42 MB, Cool Retro Term about 31 MB, default launch) was taken on the previous default window and is not remeasured here.

## What already shipped (do not redo)

- **PR #7** UX menu, custom profiles, plain/no-effects blank window.
- **PR #8** Scrollback cells packed to 16 bytes (`SbCell` in `vterm_session.hpp`). Source texture and the CPU source buffer are 8-bit. The bezel no longer keeps a second full copy of the frame.
- **PR #9** `texOut_` / `fboOut_` created only in `renderToMemory` (`gl_pipeline.cpp`). `--help` / `--version`, F11, font zoom. CPU budget default 0.15. Uniform locations cached once (`lookUpUniforms`).
- **PR #10** Bundled libvterm 0.3.3, REP crash fix, full-screen scroll by ring rotation (`third_party/libvterm-patches`).
- **PR #11** CPU path: no per-pixel warp table, bloom and static buffers are RGB floats (no alpha), blur no longer needs a third half-res image. `GL_R11F_G11F_B10F` / `RGB10_A2` for the GL float targets, and 16-bit fixed point for the CPU static buffer, were measured and missed the GL-vs-CPU PSNR bar (`gl_pipeline.cpp` comment, `docs/bench/perf3`). Leave those formats as they are.
- **Noise table (done).** `noiseTexture()` in `effects.cpp` is a process-lifetime `vector<uint8_t>` of 512×512×4 (**1,048,576 bytes**, was 4,194,304). Each byte is `xorshift >> 24`, the same 8-bit quantity the old float table stored as `(byte / 255)`. `texNoise_` is `GL_RGBA8` (**1,048,576 bytes**, was 2,097,152 as RGBA16F), uploaded with `GL_UNSIGNED_BYTE`. The CPU sampler and `frameUniforms()` divide by 255 before the lerp, so flicker (one sample per frame) and the grain match the old generator. The 512 tile is unchanged. About **4 MB** less on every effects launch, independent of window size.
- **CPU output surface (done).** `TerminalView::allocateSurfaces` and `syncCpuOutput` create `out_` (Cairo RGB24, 4 × W × H) only while the C++ renderer is selected, and drop it when a resize or a mode change stays on GL or plain. `src_` stays: it is the glyph target and the GL upload source. About **1.7 MB** at 868×494 on the GL path, which is the default when a GPU is present. The 2026-11-03 review called this saving nil; that was wrong for an image surface, which is allocated up front.

## What the 80×24 default changes for RAM

The old default window was a fixed 1280×800. Effects storage scales with pixels. A first launch with no `--size` and no window-manager restore is now an 80×24 grid:

| Face (measured with GridPainter’s Pango options) | Cell | Logical window | Where |
|---|---|---|---|
| DejaVu Sans Mono 16 px | 10×19 | **868×494** | LCOS 1.0, which does not ship Terminus (`docs/FONTS.md`) |
| Terminus (TTF) 16 px | 8×17 | **694×442** | This box, where fonts-terminus is installed |
| xfce4-terminal 1.1.3, Monospace 12, 80×24 | 10×19 text | **818×485** client | Measured here, empty config |

868×494 is the LCOS case: about 6% wider and 2% taller than xfce’s client window. The extra pixels are amber’s 4% margin. 1280×800 was 1,024,000 pixels; 868×494 is 428,792 (42% of that).

GL textures that scale with the window, full quality, bezel off (`GlPipeline::resize`):

| Texture | Bytes |
|---|---|
| `texSrc_` RGBA8 | 4 × W × H |
| `texDown_`, `texBloomB_` RGBA16F, half res | 2 × W × H each |
| two burn targets, RGBA16F, half res | 2 × W × H each |
| `texStatic_` RGBA16F, full res | 8 × W × H |
| **scaled total** | **20 × W × H** |
| `texNoise_` 512² RGBA8 | 1,048,576, fixed |

That is about **21.5 MB** at 1280×800 and about **9.6 MB** at 868×494 (8.6 MB scaled + 1.0 MB noise). The noise texture does not shrink with the window. Estimated saving from the smaller window: **about 12 MB of the scaled textures**, plus `src_` shrinking with them. On the GL path there is one Cairo RGB24 image (`src_`, 4 × W × H, about **1.7 MB** at 868×494); `out_` is the same size and exists only for the CPU renderer. Driver overhead on a real GPU moves with the textures but was not measured. **Remeasure default launch on LCOS 1.0.1 before writing more memory patches.** The 42 vs 31 MB gap may already be much smaller, and CRT’s own window is an 80×24, so the comparison becomes the same kind of window.

## Ranked remaining cuts

### 1. The noise image is 8-bit data kept as floats, plus a half-float texture — done

Shipped. See “Noise table” above. Do not shrink the 512 tile or drop the grain.

### 2. The CPU output surface is allocated on the GL path — done

Shipped. See “CPU output surface” above. `src_` stays.

### 3. Leave the window-sized float targets alone

`texStatic_` is the largest scaled texture (8 × W × H: about **3.4 MB** at 868×494, about **8.2 MB** at 1280×800). Replacing RGBA16F with `GL_R11F_G11F_B10F` or `RGB10_A2` already failed the PSNR bar in perf step 3. Do not repeat that experiment unless the bar itself is being reconsidered.

The half-res chain (`texDown_`, `texBloomB_`, two burn targets) is about **0.86 MB** each at 868×494, about **3.4 MB** together. The CPU side already dropped bloom alpha. A desktop GL `RGB16F` image is not reliably 6 bytes per texel (drivers pad). Two burn images are the ping-pong. Neither is worth the risk at this window size.

On the CPU fallback, `static_` is 12 bytes per pixel (about **5.1 MB** at 868×494). Perf step 3 already removed the alpha and the warp LUT, and rejected 16-bit fixed point for the same PSNR reason. The default launch on LCOS uses GL when a GPU is present, so this buffer is not in the 42 MB figure. Leave it unless the no-GPU path becomes the target.

Turning the bezel on allocates three more full-res RGBA16F images (`texFrameBase_`, `texFrameRefl_`, `texFrame_`): about **10 MB** at 868×494. They are created only when `fx.frame` is set. The default profile leaves the bezel off. Keep that laziness; do not drop the bezel.

### 4. Not the default-launch gap

- **Scrollback.** Empty at startup, so it is not part of 42 vs 31. A full 5,000 lines at 80 columns is on the order of 5000 × 80 × 16 bytes of `SbCell` (about 6.4 MB) plus the line vectors, and trailing blanks are already trimmed. Do not lower the 5,000 line default to chase startup RAM.
- **Glyph cache.** Keyed by text plus a few attribute bits, cleared at 8,192 entries. A shell screen is a few dozen glyphs. Irrelevant to the gap. The old suggestion to rekey it is a paint-time cleanup, not a memory win at 80×24.
- **One process per window** (`APPLICATION_NON_UNIQUE` in `main.cpp`). A second window pays GTK again. The comparison is one window. Still true, still not the next step for the 42 vs 31 number.
- **libvterm screen and the ring scroll.** Already shipped. An 80×24 screen is small. Throughput work is out of scope for this note.

## What to leave alone

Keep the effects pipeline. The CRT look is the product. Plain mode was already the cheap path in the earlier measurements (on the order of 10 MB, idle CPU at 0); it is not the configuration being compared with Cool Retro Term.

Do not retry the packed-float and fixed-point cuts that missed the PSNR bar. Do not drop bloom, noise, curvature, or the burn-in buffers to “get to 31 MB.” The noise representation (about 4 MB, lossless against the generator that stored `(xorshift >> 24) / 255`) and not allocating `out_` on the GL path (about 1.7 MB at the LCOS default size) are done. What is left above is not a clean cut at this window size. The window shrink in PR #12 is larger than both, and it needs a real RSS reading on LCOS before anything else is scheduled.
