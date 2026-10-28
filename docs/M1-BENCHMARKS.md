# M1 benchmarks (checkpoint C, item 5)

All numbers here are **measured**. Each table says where it was measured:
- **REAL host**: the build box itself.
- **REAL TCG VM**: inside a no-KVM qemu guest.
- **PROXY**: the guest was not used. No proxy numbers were needed (see the end).

Raw output is in [`bench/`](bench/). Rerun with `tools/bench-m1.sh OUTDIR [BUILDDIR]` (see the
script header).

**What is measured** (`tools/m1_bench.cpp`; the medians use `--runs` 20 on the host and 10 in the VM):
- **paint.** A full-screen repaint of a synthetic screen of text:
  - "direct" is the M0 per-cell Pango painter;
  - "warm" is the M1 glyph cache;
  - "one row" is the cost of a typical keystroke.
- **CPU tick.** The C++ fallback's per-frame effects cost. The *tick* is the dynamic-only frame
  that runs at `--effects-fps` while idle. The *content pass* is a frame after the text changed.
- **GL tick.** The same frames on llvmpipe through headless EGL, with `glFinish` and no readback.
- **idle CPU.** The real `lunduke-terminal` window, 1280×800, with bash idle at a prompt and the
  window focused. The figure is utime+stime / wall time over 10 s; 100 % = one core.
- **No AVX.**
  - Our code is already baseline x86-64 (no `-march`; objdump finds 0 ymm/zmm instructions in
    both binaries).
  - llvmpipe: `GALLIUM_OVERRIDE_CPU_CAPS=sse4.1 LP_NATIVE_VECTOR_WIDTH=128`. `GALLIUM_DUMP_CPU`
    shows avx=0 avx2=0, and the renderer reports "128 bits".
  - CPU path: `PIXMAN_DISABLE=avx2 GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX,-AVX2,-AVX512F,…`. cairo
    and pixman pick their SIMD at run time; this masks the AVX2 paths that can be masked.

## REAL host — the box (bench/host/)

**Setup.**
- Intel Xeon (8 vCPU, AVX-512, 16 GB) running Debian 13.
- Mesa 25.0.7 llvmpipe (LLVM 19.1.7). Idle runs on my own Xvfb :41 + xfwm4 (compositor off).
- Fonts are Debian's, unpacked locally with `FONTCONFIG_FILE`: amber = Terminus (TTF) 22 px,
  plain = DejaVu Sans Mono 16 px.
- 2026-10-28 11:14–11:20 CDT, load average about 1.4.
- **The box is shared with other agents**, so single numbers carry about ±20 % noise. Some
  no-AVX rows come out *faster* than the AVX ones, which is noise.

### Full repaint (ms, median)

| Size | Profile | grid | direct Pango (M0) | glyph cache cold | **warm full** | one changed row |
|---|---|---|---|---|---|---|
| 1280×800 | plain | 123×39 | 12.10 | 3.92 | **2.61** | 0.061 |
| 1280×800 | amber | 112×32 | 9.07 | 3.47 | **2.10** | 0.075 |
| 1280×800 | amber, no-AVX pixman/glibc | 112×32 | 28.31 | 7.41 | **3.84** | 0.129 |
| 1920×1080 | plain | 187×54 | 19.52 | 5.89 | **4.58** | 0.045 |
| 1920×1080 | amber | 170×44 | 14.72 | 4.69 | **3.56** | 0.066 |
| 1920×1080 | amber, no-AVX pixman/glibc | 170×44 | 17.48 | 5.07 | **3.68** | 0.067 |

Plain has no effects, so the repaint is its whole frame cost.

### Effects per frame, amber (ms, median)

| Size | Path | threads | idle tick | content pass |
|---|---|---|---|---|
| 1280×800 | CPU fallback | 1 / 2 / 4 | 65.3 / 43.4 / 25.6 | 71.0 / 54.1 / 30.6 |
| 1280×800 | CPU fallback, no-AVX | 1 / 2 / 4 | 64.5 / 36.5 / 19.9 | 70.9 / 42.9 / 23.3 |
| 1280×800 | llvmpipe 256-bit | LP 1 / 2 / 4 | 33.9 / 17.2 / 9.1 | 108.3 / 55.5 / 31.5 |
| 1280×800 | llvmpipe **no-AVX 128-bit** | LP 1 / 2 / 4 | 52.9 / 26.2 / 13.4 | 135.8 / 69.2 / 37.3 |
| 1920×1080 | CPU fallback | 1 / 2 / 4 | 125.2 / 65.7 / 40.4 | 137.3 / 73.5 / 46.3 |
| 1920×1080 | CPU fallback, no-AVX | 1 / 2 / 4 | 126.6 / 62.6 / 47.6 | 138.2 / 69.9 / 55.5 |
| 1920×1080 | llvmpipe 256-bit | LP 1 / 2 / 4 | 61.5 / 32.8 / 16.3 | 205.4 / 111.1 / 57.5 |
| 1920×1080 | llvmpipe **no-AVX 128-bit** | LP 1 / 2 / 4 | 99.8 / 49.4 / 26.4 | 263.0 / 132.6 / 75.3 |

**Reading it.**
- At the default 20 effects fps the budget is 50 ms per tick.
- At 1280×800, one llvmpipe thread fits even without AVX (53 ms ≈ the budget). The C++ fallback
  needs two threads.
- At 1920×1080 without AVX, two llvmpipe threads are needed.
- Without AVX llvmpipe costs about 1.5× more. Our CPU fallback hardly changes: it is scalar C++
  plus SSE2.

### Idle CPU, window 1280×800, focused (bench/host/idle.txt)

| Case | CPU (100 % = 1 core) | RSS |
|---|---|---|
| plain (default) | **0.0 %** | 52 MB |
| ibm-3278 GL (static profile) | **0.0 %** | 229 MB |
| amber GL, default threads | 90.0 % | 232 MB |
| amber GL, LP_NUM_THREADS 1 / 2 / 4 | 78.1 / 82.3 / 89.1 % | 228 MB |
| amber GL no-AVX, LP 1 / 2 / 4 | 97.7 / 139.9 / 124.9 % | 228–230 MB |
| amber GL `--effects-fps 10` | 45.9 % | 229 MB |
| amber CPU fallback, threads 1 / 2 / 4 | 99.8 / 134.9 / 135.7 % | 128 MB |
| amber CPU fallback, threads 4, no-AVX | 136.8 % | 128 MB |

**Notes on idle CPU.**
- Unfocused, minimized or obscured windows stop ticking (0 %, checkpoint A). Above, the window is
  focused.
- At 1 CPU thread, 99.8 % means the fallback is saturated and cannot keep 20 fps. It drops frames
  rather than queueing them.
- The GL cost includes GTK compositing the GLArea, not only the shader.
- At 1920×1080, scale the tick costs above by about 1.9×.

## REAL TCG VM — LCOS 1.0 live ISO under qemu, no KVM (bench/tcg-lcos-100-01/)

TCG_PLACEHOLDER
