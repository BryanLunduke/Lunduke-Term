# M6 item 6: where Lunduke Terminal's memory goes (findings)

Box: Debian 13 VM, Xvfb, **no GPU**, so every GL number is Mesa llvmpipe. One tab, window
1280x800 (the default) unless noted, empty config (scrollback 10000). kB from
`/proc/PID/smaps_rollup`. "fresh" = t=8 s, before output. "after" = t=30 s, after ~25k lines
(`seq 1 20000` + `cat src/*.cpp`). REAL = read from a file in this directory. DERIVED = computed
from REAL numbers. Raw files: `raw/` (master dc3b544), `raw-diag/`, `raw-fixed/` (after fixes).
Private_Dirty equals Anonymous in every run here, so only Private_Dirty is shown. It is also the
most stable column. Rss and Pss move with page-cache pressure and with what other processes
share. In raw-fixed `plain-sb0 after`, for example, the kernel reclaimed ~17 MB of clean file
pages while the fuzzer was running.

## Master dc3b544 (REAL, run 1 of 3; the other runs are within 0.3 MB on Private_Dirty)

| run | Rss fresh/after | Pss fresh/after | Private_Dirty fresh/after |
|---|---|---|---|
| amber, effects on, GL (llvmpipe) | 196,372 / 207,052 | 155,221 / 148,376 | 93,328 / 103,944 |
| amber, effects on, C++ (CPU) path | 125,184 / 136,820 | 108,125 / 119,745 | 82,532 / 94,104 |
| amber `--no-effects` | 52,708 / 64,384 | 36,163 / 47,839 | 10,364 / 22,040 |
| plain | 53,176 / 65,124 | 33,145 / 45,054 | 10,652 / 22,600 |
| plain, scrollback 0 | 53,124 / 53,708 | 36,244 / 36,827 | 10,656 / 11,228 |
| amber GL, scrollback 0 | 196,252 / 196,476 | 155,083 / 155,297 | 93,364 / 93,588 |
| bare gtkmm window (DrawingArea, 1024x640) | 78,780 | 29,242 | 14,380 |
| bare gtkmm GtkGLArea, one frame, 1280x800 | 128,216 | 91,510 | 38,720 |
| cool-retro-term 1.2.0 default, 1024x768 (llvmpipe) | 194,232 / 200,672 | 141,440 / 161,253 | 80,652 / 85,020 |
| Lunduke amber GL at 1024x768 (same size as CRT) | 186,492 / 196,952 | 145,762 / 156,222 | 84,288 / 94,748 |

## Per component (amber GL "after", master, from `raw/*-groups.txt` and `--stats-every`)

- **GL driver (llvmpipe) fixed cost**:
  - libLLVM maps 51.8 MB Rss (21 MB Pss).
  - Mesa maps 11.1 MB.
  - The bare GtkGLArea already costs 38.7 MB Private_Dirty, against 14.4 MB for a bare
    DrawingArea.
  - Window-size scaling (640x400, 1024x768, 1280x800) is about 42 B/px (DERIVED), so about
    150 MB of the amber GL Rss is fixed (DERIVED).
  - Settings that made no difference: `LP_NUM_THREADS=1`, and `--effects-quality static`.
  - softpipe is worse: 257/293 MB.
- **Our GL textures**: 9 textures, 32,817,152 bytes (REAL, stats line).
  - src RGBA16F 8.2 MB.
  - static RGBA16F 8.2 MB.
  - out RGBA8 4.1 MB.
  - down, bloomA and bloomB 2.05 MB each.
  - Two burn-in textures, 2.05 MB each.
  - llvmpipe keeps these in process memory, inside the anon>=1MiB (48.8 MB) and heap groups.
- **C++ effects path**: the float buffers at 1280x800 come to about 77 MB virtual (DERIVED,
  16 B/px each).
  - `src_`, `static_` and `warp_` at full resolution.
  - burn-in at full resolution.
  - `down_`, `bloomA_` and `bloomB_` at half resolution.
  - The anon>=1MiB group is 76.7 MB (REAL).
  - The bezel option (off by default) adds `frame_` plus a full copy, `base_`: 2 x 16.4 MB (DERIVED).
- **Scrollback**: 10,000 lines took `sb-bytes` 11,415,200 (REAL). That is 40 B per stored cell
  (`VTermScreenCell`). It is all of the fresh-to-after growth: with scrollback 0, plain grows only
  0.6 MB.
- **Glyph cache**: 56–82 glyphs, 31–43 KB. Negligible.
- **Cairo surfaces**: 8,192,000 bytes virtual (two 1280x800 ARGB32). Only one is resident in the
  GL and plain modes (anon>=1MiB in plain is 5.0 MB).
- **Fonts and pixbufs**:
  - The fontconfig cache is mmapped and shared: 8.0 MB Rss, 4.5 MB Pss, 12 kB dirty.
  - Font files total 0.3 MB.
  - No pixbuf copies show up.
- **GTK/Pango/Cairo/glib and other shared libraries**: about 31 MB Rss in plain (gtk3 6.6 MB
  plus other-libs 24 MB), mostly clean and shared.
- **malloc**: heap-free is 2.9 MB and heap-releasable 35 kB, so `malloc_trim` has almost nothing to
  give back. The llvmpipe threads did not change the totals (LP_NUM_THREADS=1), so
  `M_ARENA_MAX` is not a lever here.

## What this means for "47 MB vs 32 MB"

Those are the editor's numbers, from his machine with a real GPU. They cannot be reproduced here.
On this box, at the same window size (1024x768) and both on llvmpipe, Lunduke amber and
cool-retro-term are within about 3 MB of each other on Private_Dirty when fresh (84.3 vs 80.7 MB).
After output, Lunduke is 9.7 MB higher (94.7 vs 85.0 MB), and scrollback explains most of that gap.
Plain Lunduke (no GL) is Pss 33–36 MB fresh and 45–48 MB after output. Scrollback is the part
of plain that grows. What we control:
1. Scrollback cell size.
2. Our effects buffers: the GL textures, and the C++ path's float buffers.
3. Nothing in the driver or the shared libraries.

## Fixes on m6-ram

All four fixes keep the output the same. None of them changes timing or governor code.

1. **1481dd7, GL path: 8-bit `texSrc_`** (was RGBA16F). The Cairo source is 8-bit, so RGBA8 holds
   it exactly. This saves 4 B/px (4.1 MB at 1280x800). `texture-bytes` went from 32,817,152 to
   28,721,152 (REAL). The minimum PSNR in the gl-match test went from 60.68 to 58.81 dB (threshold
   40). The largest pixel difference is 2/255.
2. **dabebdf, scrollback: 16-byte packed cells** instead of libvterm's 40-byte `VTermScreenCell`:
   - Each cell keeps the codepoint, the 18 attribute bits, the width, and fg/bg.
   - The rare extra combining characters go in a per-line side array.
   - `sb-bytes` for 10,000 lines went from 11,415,200 to 5,032,848 (REAL).
   - The test is `session_test` `testScrollbackCells`. It checks that rich SGR, combining marks,
     wide characters and DECDWL/DECDHL survive history and sb_popline, and that a 100-column line
     costs 1600 bytes (REAL).
   - On the master scrollback code the test fails (2 failures: 4032 bytes per line, plus the
     raw-cell check). The raw-cell check also failed on master until two libvterm limits were
     excluded. libvterm does not restore DECDWL/DECDHL or the colours of a wide character's right
     half on popline, with or without this change.
   - A mutation run that dropped the font bits and the extra flag made the test fail 3 times (REAL).
   - Fuzz: 601 s, 75,659 runs, no crash or leak. The log is `fuzz/fuzz-dabebdf.txt`.
3. **5f95c13, C++ path: byte `src_`** (was float RGBA, 16 B/px, now 4 B/px). Reads convert
   with the same `k * (1/255)`, so the output is bit-identical. `exact/` shows identical output
   hashes for 6 parameter sets x 4 frames (bezel, rgbShift, full-res burn-in, bloom off).
4. **5f95c13, C++ path with the bezel**: no more full copy of the bezel layer (`base_`). The
   three base floats of each reflecting pixel now live in its `Refl` entry. This is bit-identical
   too (same `exact/` run).

## Before / after (REAL, kB, Private_Dirty fresh / after, 1280x800, one tab)

| profile | master dc3b544 | final (5f95c13 source) | change (DERIVED) |
|---|---|---|---|
| amber, effects on, GL (llvmpipe) | 93,328 / 103,944 | 89,356 / 93,656 | -4.0 / -10.3 MB |
| amber, effects on, C++ path | 82,532 / 94,104 | 70,532 / 75,940 | -12.0 / -18.2 MB |
| amber C++ path + bezel (`--frame`) | 117,168 / 122,556 * | 91,004 / 96,384 | -26.2 / -26.2 MB |
| plain | 10,652 / 22,600 | 10,664 / 16,316 | 0 / -6.3 MB |

| profile | Rss master | Rss final | Pss master | Pss final |
|---|---|---|---|---|
| amber GL | 196,372 / 207,052 | 198,184 / 202,484 | 155,221 / 148,376 | 147,518 / 151,804 |
| amber C++ | 125,184 / 136,820 | 112,660 / 118,068 | 108,125 / 119,745 | 86,720 / 92,132 |
| plain | 53,176 / 65,124 | 52,652 / 58,368 | 33,145 / 45,054 | 27,542 / 33,197 |

`*` The bezel "before" was run at 094697e. That commit already has fixes 1 and 2, but its C++ path
code is the same as master's, so its "after" column already includes the scrollback saving. The
final numbers are from `raw-fix34/` (one run each for GL and plain; the C++ path ran twice, 70,532
both times). The master numbers are run 1 of `raw/`.

Rss and Pss for GL did not improve, although Private_Dirty did. That is because they mostly
track shared, clean pages of libLLVM and Mesa, and those vary with other processes and page-cache
pressure.

## What remains vs the 32 MB target, and why

- **Plain**:
  - Pss is 27.5 MB fresh and 33.2 MB after output; Rss is 52.7 / 58.4 MB (REAL, final).
  - The remaining Rss is mostly shared libraries and the fontconfig cache: about 31 MB of
    GTK/glib/Pango/Cairo/other libraries, and the 8 MB fontconfig mmap.
  - Private_Dirty is 10.7 MB fresh. It is mostly the heap (8 MB), plus one 4 MB Cairo surface.
  - Growth after output is now 5.6 MB for 10,000 lines of history.
- **Amber GL here**: llvmpipe's own cost dominates (libLLVM, JIT shaders, and textures held in
  process memory). About 150 MB of Rss is fixed (DERIVED). This box cannot say what a GPU driver
  would cost.
- **What we still control, not changed** (each would change output or cost CPU; report only):
  - GL: `texStatic_` is RGBA16F, 8.2 MB (values exceed 1.0). down, bloomA, bloomB and the two
    burn textures are RGBA16F at half resolution, 2 MB each.
  - C++ path: `static_`, `warp_` and the full-res burn buffer are float, 16.4 MB each at
    1280x800 (DERIVED). `down_`, `bloomA_` and `bloomB_` are 4.1 MB each.
  - Scrollback default of 10,000 lines. This is a setting; it was not changed.
- **Not a lever**:
  - `malloc_trim`: heap-free is about 3 MB and releasable 35 kB.
  - `M_ARENA_MAX`: `LP_NUM_THREADS=1` made no difference.
  - Glyph cache: under 60 KB.
  - Pixbuf copies: none found.
