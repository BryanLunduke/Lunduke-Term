<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Flood throughput, BEFORE vs AFTER (perf step 2)

REAL measurements, 2026-11-03 10:46:26–10:48:53 CST, `matrix.sh` (raw log `matrix.log`, the AT-SPI
warnings removed). Tool: `tools/m5_throughput_bench.cpp` (same as M5 N1): a 1280x800 window runs the
command, then prints a sentinel; the time is from start until the sentinel is on screen.

- **BEFORE:** master 21eff10, release build, linked with Debian's `libvterm.so.0` (0.3.3-3+b1).
- **MID:** the branch's objects relinked with the bundled *stock* libvterm (`libvterm-stock.a`,
  pristine 0.3.3, built by meson); separates patch 0002 from bundling, static linking and flags
  (`MID-LINK-COMMAND.txt`).
- **AFTER:** branch perf-step2, release build, bundled libvterm with 0001+0002.
- There is no `src/` difference between master and the branch; only libvterm differs.
- Own Xvfb :82 1280x1024x24 + `xfwm4 --compositor=off`. Xvfb and xfwm4 were checked (`kill -0`,
  `xdpyinfo`) before and after every run: alive for all 36 runs.
- Interleaved, 3 runs each, with the order rotated per repetition. The 1-minute load at run start
  was 3.77–5.73 (the box is shared; 8 CPUs). Rendering: llvmpipe (software); amber = GLSL effects
  on llvmpipe, plain = no effects.
- `flood8.txt`: 8,388,608 bytes, 196,204 lines of the repo's sources and docs, ending in `\n`
  (sha256 d1c4b195…be9e51). MB/s = 8.388608 MB / time.

| workload | profile | BEFORE | MID | AFTER | AFTER vs BEFORE (DERIVED) |
|---|---|---|---|---|---|
| seq 1 2000000 | amber | median **7.20 s** (7.20 / 6.91 / 7.35) | 7.24 s (7.26 / 6.85 / 7.24) | median **3.61 s** (3.44 / 3.61 / 3.67) | 1.99x faster |
| seq 1 2000000 | plain | median **9.67 s** (9.67 / 9.50 / 9.69) | 9.42 s (9.47 / 9.14 / 9.42) | median **3.44 s** (3.45 / 3.37 / 3.44) | 2.81x faster |
| cat 8 MiB | amber | median **1.31 s**, 6.40 MB/s (1.31 / 1.29 / 1.42) | 1.31 s, 6.40 MB/s (1.30 / 1.31 / 1.42) | median **0.91 s**, 9.22 MB/s (0.89 / 0.92 / 0.91) | 1.44x faster |
| cat 8 MiB | plain | median **1.29 s**, 6.50 MB/s (1.29 / 1.27 / 1.31) | 1.34 s, 6.26 MB/s (1.31 / 1.37 / 1.34) | median **0.68 s**, 12.34 MB/s (0.68 / 0.71 / 0.68) | 1.90x faster |

MID is within run-to-run spread of BEFORE: the gain comes from patch 0002, not from bundling,
static linking or compiler flags.

Caveats:
- 3 runs per cell on a shared box. The ratios are DERIVED from the medians.
- The window is 1280x800. 0002 removes a cost that grows with rows x cols, so other window sizes
  will give other ratios (not measured).
- Software rendering only (llvmpipe); no GPU was measured.
- A first matrix run (10:39–10:44 CST) is discarded. That `flood8.txt` had no final newline, so the
  sentinel was printed onto the file's last line, wrapped at the margin, and was never matched
  (all cat runs "sentinel not seen within 120 s"). Its seq rows (1 repetition) agree with the table:
  BEFORE 6.99 / 9.01 s, MID 6.59 / 8.99 s, AFTER 3.31 / 3.23 s (amber / plain).
