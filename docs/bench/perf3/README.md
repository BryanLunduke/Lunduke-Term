# Perf step 3: less memory for the effects (REVIEW.md P2/P3)

Branch perf-step3 off master 6283674. Everything here was measured on the shared 8-core box
with Mesa llvmpipe (software GL, Mesa 25.0.7, LLVM 19.1.7). There is no GPU, and nothing here
says anything about GPU drivers. **REAL** = measured or counted here. **DERIVED** = computed
from sizes or from REAL numbers.

## What changed

| item | before (master) | after (perf-step3) | output |
|---|---|---|---|
| GL: bloom ping-pong | down, bloomA, bloomB (3 half-res RGBA16F) | down, bloomB; the vertical blur writes back into down | bit-identical |
| GL: texture formats | RGBA16F | RGBA16F (unchanged, see the experiments below) | - |
| CPU: warp table `warp_` | 4 floats per pixel | gone: one column table and one row table (pre-barrel coordinates are separable); the barrel is applied per pixel exactly as before | bit-identical |
| CPU: bloom buffers | down, bloomA, bloomB, RGBA float | down, bloomB, RGB float; the blur ping-pongs down -> bloomB -> down | bit-identical |
| CPU: static buffer `static_` | RGBA float | RGB float (nothing reads its alpha) | bit-identical |

In both paths the half-res burn-in, the only other reader of the downsample, now runs before
the blur, so the downsample can hold the bloom.

DERIVED buffer sizes at 1280x800 (half-res 640x400):

| buffer | before | after |
|---|---|---|
| GL half-res RGBA16F textures | 3 x 2.05 MB | 2 x 2.05 MB (-2.05 MB of texture storage) |
| CPU warp table | 16.38 MB | 1280 + 800 floats (8 KB) |
| CPU bloom buffers | 3 x 4.10 MB | 2 x 3.07 MB |
| CPU static buffer | 16.38 MB | 12.29 MB |
| CPU total for these | 45.06 MB | 18.44 MB (-26.6 MB) |

## Quality: GL-vs-CPU PSNR (REAL)

The gl-match test (76 configurations, fail bar 40 dB) gives a **minimum of 58.81 dB on master**
(paper-white with bezel, static quality, scale 1; glmatch-master.txt). On perf-step3 every
gl-match PSNR line is the same as master (glmatch-after.txt): **minimum 58.81 dB**.

A second check renders 48 configurations (8 profile/bezel variants x full/reduced/static x
scale 1 and 2, three synthetic frames at t = 1000/1100/1180 ms) on both back-ends with
master's and the branch's object files, and compares the raw frames (offline/: render.cpp,
build.sh, psnr.py, try.sh). For the branch head (experiments/psnr-cpu3.md), CPU-before vs
CPU-after and GL-before vs GL-after are byte-identical in all 48 configurations (shown as
99.00 dB, max diff 0). CPU-vs-GL is unchanged: minimum 58.81 dB.

### Experiments that were measured and not applied (REAL, same 48-config render)

"CPU vs GL" is the minimum over the 48 configurations (master: 58.81 dB). "vs master" is the
minimum PSNR of the changed back-end against master's output, with the largest 8-bit channel
difference.

| tag | change | CPU vs GL min | vs master (max diff) | worst config | verdict |
|---|---|---|---|---|---|
| gl1 | GL: down, bloomA, bloomB and static as R11F_G11F_B10F | 48.54 dB | GL 48.81 dB (9) | futuristic full s1 | fails bar |
| gl2 | GL: down + bloom as R11F_G11F_B10F | 52.07 dB | GL 52.42 dB (5) | futuristic reduced s1 | fails |
| gl3 | GL: bloomA/B as R11F_G11F_B10F | 53.77 dB | GL 54.26 dB (4) | futuristic full s1 | fails |
| gl4 | GL: down + bloom as RGB10_A2 | 56.63 dB | GL 57.97 dB (1) | paper-white bezel static s1 | fails |
| gl5 | GL: bloom as RGB10_A2 | 56.69 dB | GL 58.11 dB (1) | paper-white bezel static s1 | fails |
| gl6 | GL: down as RGB10_A2 | 58.66 dB | GL 69.76 dB (1) | paper-white bezel static s1 | fails (by 0.15 dB) |
| gl7 | GL: static as RGB10_A2, values scaled by 1/2.5 to fit 0..1 | 53.58 dB | GL 53.91 dB (1) | futuristic full s1 | fails |
| gl8 | GL: reuse down as the vertical-blur target (applied) | 58.81 dB | identical | - | **applied** |
| gl9 | gl8 + bloomB as RGB10_A2 | 58.15 dB | GL 63.69 dB (1) | paper-white bezel static s1 | fails |
| cpu1 | CPU: no warp table, no bloomA, RGB bloom (applied) | 58.81 dB | identical | - | **applied** |
| cpu2 | cpu1 + static as 16-bit fixed point (6 B/px, scale 16384) | 58.79 dB | CPU 72.53 dB (1) | paper-white bezel static s1 | fails (by 0.02 dB); patch in experiments/ |
| cpu3 | cpu1 + static as RGB float (applied) | 58.81 dB | identical | - | **applied** |

Every 4-byte-per-texel GL format lowers the minimum below 58.81 dB, because the CPU path
works in 32-bit float and any rounding on the GL side increases the distance. The
static values reach about 3.75 (screen brightness up to 1.5 plus bloom), which rules out the
unorm formats unless they are scaled, and scaling loses the precision (gl7). The CPU 16-bit
static moves the C++ output by at most 1 level in 8-bit (not visible), but it adds noise
that GL does not have, so CPU-vs-GL goes down in 47 of 48 configurations (0.01-0.21 dB).
Quarter-res bloom and 8-bit bloom were not tried: each loses far more precision than
the formats above, which already fail.

None of these lossy variants is in the branch, not even as an option. If a lower-memory mode
that is not bit-identical is wanted, gl6 (2 MB less) and cpu2 (6.1 MB less at 1280x800) have
the smallest PSNR cost. That is an editor's call.
