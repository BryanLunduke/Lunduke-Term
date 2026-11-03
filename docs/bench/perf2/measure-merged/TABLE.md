<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Flood throughput after merging origin/master f2b96d1 (perf step 2)

REAL measurements, 2026-11-03 11:14:13–11:15:40 CST, `matrix.sh` (raw log `matrix.log`, AT-SPI
warnings removed). Same tool and method as `../measure/` (`tools/m5_throughput_bench.cpp`, 1280x800
window, time until the sentinel is on screen, `flood8.txt` 8,388,608 bytes ending in `\n`).

- **BEFORE:** master f2b96d1 (perf step 1 merged), release build, Debian `libvterm.so.0`
  (0.3.3-3+b1). Bench sha256 ada00fd8d5e623f3.
- **AFTER:** perf-step2 at the merge 118ba52, release build, bundled libvterm 0001+0002 (static).
  Bench sha256 09dd47fb87ea533c.
- Own Xvfb :84 1280x1024x24 + `xfwm4 --compositor=off`. Both were alive before and after all 24
  runs.
- Interleaved, 3 runs each (order rotated in run 2). llvmpipe only.

| workload | profile | BEFORE master f2b96d1 (Debian libvterm.so.0) | AFTER merged perf-step2 (bundled 0001+0002) | AFTER vs BEFORE (DERIVED) |
|---|---|---|---|---|
| seq 1 2000000 | amber | median **6.71 s** (7.57 / 6.65 / 6.71) | median **3.23 s** (4.07 / 3.23 / 3.22) | 2.08x faster |
| seq 1 2000000 | plain | median **9.38 s** (9.92 / 9.38 / 9.22) | median **3.41 s** (3.41 / 3.61 / 3.31) | 2.75x faster |
| cat 8 MiB | amber | median **1.20 s**, 6.99 MB/s (1.11 / 1.41 / 1.20) | median **0.87 s**, 9.64 MB/s (0.87 / 1.12 / 0.78) | 1.38x faster |
| cat 8 MiB | plain | median **1.26 s**, 6.66 MB/s (1.22 / 1.26 / 1.43) | median **0.67 s**, 12.52 MB/s (0.72 / 0.67 / 0.65) | 1.88x faster |

24 runs; 1-minute load at run start 4.71-6.26.

The earlier comparison against 21eff10 (`../measure/TABLE.md`) gave 1.99x / 2.81x (seq) and
1.44x / 1.90x (cat). These ratios agree with it within run-to-run spread.

Caveats: 3 runs per cell on a shared box; ratios are DERIVED from the medians. The first run in each
of the two cells, before seq amber (7.57) and after seq amber (4.07), is slower than the other two.
Only 1280x800 was measured.
