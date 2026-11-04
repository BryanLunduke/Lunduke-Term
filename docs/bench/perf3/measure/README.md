# Perf step 3: window measurements (REAL unless marked DERIVED)

> CPU frame time after the PR #11 review fix (row prepass) is re-measured in ../measure-review/README.md; the CPU slowdown described below is for 2f13075.

Conditions: my own Xvfb :93 (1920x1200x24, GLX) with `xfwm4 --compositor=off`, on the shared 8-core box, 2026-11-03 15:15-15:53 CST. Mesa llvmpipe only (software GL), no GPU. Window 1280x800, `--profile amber`, GL (default renderer) and `--renderer cpu`; under llvmpipe the app picks quality "reduced" (stats line). There were 3 rounds: before/after on odd rounds, after/before on even rounds.
BEFORE = master 6283674 release build (sha256 42b7cbe0…). AFTER = perf-step3 release build of 995c820 (sha256 1e07f5ab…). The code is unchanged up to the head; only docs were added.
Script: measure.sh is a copy of perf1-work/measure.sh. The only change: the terminal runs with `--quit-after 170000`, so it exits cleanly and prints its `--stats` line (frame times). All 12 runs exited cleanly. matrix.sh checks before and after every run that Xvfb and the same xfwm4 PID are alive, and redoes a run if not. **The WM was alive for all 12 runs; none was discarded** (matrix.log; every window was decorated at 10,58).
Units: idle CPU = % of one core over 30 s (utime+stime), measured fresh (~8 s after map) and 3 s after the work load (seq 2M, cat 8 MB, yes 2M; work.sh). Private_Dirty = kB from smaps_rollup. Frame time = the app's own `--stats` medians over the whole run: "tick" = one effects frame (GL draw, or the C++ effects render), "effects-source" = building the source frame for the effects.
Oddities: the box had wall-clock stalls. after-amber-gl-1280-r2 took 306 s to its last reading (planned ~78 s), before-amber-gl-1280-r3 took 167 s and before-amber-cpu-1280-r2 took 156 s. In those runs the idle-after values (7.9 and 1.0) and tick counts (267 and 203) are low outliers. They are kept: the medians are shown, and dropping them does not change the direction.

| profile | build | run | idle CPU % fresh | idle CPU % after work | Private_Dirty fresh kB | Private_Dirty after kB | median tick ms | median effects-source ms | median paint ms | work CPU s | content frames/ticks | window pos | WM alive before+after |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| amber-gl-1280 | before | 1 | 12.6 | 12.4 | 85400 | 88640 | 3.53 | 11.02 | 0.52 | 10.3 | 55/684 | 10,58 | yes |
| amber-gl-1280 | before | 2 | 13.0 | 13.2 | 85208 | 89856 | 3.60 | 10.52 | 0.53 | 10.0 | 50/668 | 10,58 | yes |
| amber-gl-1280 | before | 3 | 8.7 | 11.2 | 85344 | 88576 | 3.37 | 10.69 | 0.39 | 10.1 | 61/568 | 10,58 | yes |
| amber-gl-1280 | after | 1 | 10.2 | 11.5 | 83216 | 86432 | 3.38 | 10.45 | 0.41 | 9.6 | 55/630 | 10,58 | yes |
| amber-gl-1280 | after | 2 | 13.3 | 7.9 | 83380 | 86616 | 3.72 | 11.20 | 0.69 | 10.0 | 53/267 | 10,58 | yes |
| amber-gl-1280 | after | 3 | 12.4 | 11.9 | 83204 | 86424 | 3.19 | 8.81 | 0.35 | 9.1 | 50/728 | 10,58 | yes |
| amber-cpu-1280 | before | 1 | 12.4 | 11.3 | 70532 | 74824 | 10.26 | 13.41 | 0.67 | 10.6 | 58/448 | 10,58 | yes |
| amber-cpu-1280 | before | 2 | 12.8 | 1.0 | 70528 | 74828 | 9.86 | 13.53 | 0.53 | 10.6 | 58/203 | 10,58 | yes |
| amber-cpu-1280 | before | 3 | 12.3 | 11.0 | 70528 | 74712 | 9.78 | 13.06 | 0.44 | 9.8 | 54/471 | 10,58 | yes |
| amber-cpu-1280 | after | 1 | 14.0 | 12.9 | 44528 | 48844 | 10.84 | 15.84 | 0.64 | 11.2 | 56/485 | 10,58 | yes |
| amber-cpu-1280 | after | 2 | 10.4 | 10.0 | 44528 | 48664 | 10.71 | 15.74 | 0.49 | 11.2 | 58/400 | 10,58 | yes |
| amber-cpu-1280 | after | 3 | 13.0 | 12.9 | 44516 | 48700 | 10.45 | 15.41 | 0.43 | 11.4 | 59/487 | 10,58 | yes |

| profile | idle CPU % fresh | idle CPU % after work | PD fresh kB (delta) | PD after kB (delta) | median tick ms | median effects-source ms |
|---|---|---|---|---|---|---|
| amber-gl-1280 | 12.6 -> 12.4 | 12.4 -> 11.5 | 85344 -> 83216 (-2128) | 88640 -> 86432 (-2208) | 3.53 -> 3.38 | 10.69 -> 10.45 |
| amber-cpu-1280 | 12.4 -> 13 | 11 -> 12.9 | 70528 -> 44528 (-26000) | 74824 -> 48700 (-26124) | 9.86 -> 10.71 | 13.41 -> 15.74 |

(The second table is DERIVED: medians of 3 REAL runs per cell.)

DERIVED reading: Private_Dirty drops by about 2.1 MB for GL, matching the one RGBA16F 640x400 texture removed (2.05 MB; llvmpipe keeps textures in process memory). It drops by about 26.0 MB for CPU, matching the 26.6 MB (= 25,980 kB) computed from the buffer sizes. Idle CPU is within noise. GL frame time is within noise. **CPU frame time is worse:** median tick goes 9.86 → 10.71 ms (+0.85 ms, about 9%), effects-source goes 13.41 → 15.74 ms, and work CPU goes 10.6 → 11.2 s. Every after run is slower than every before run. The likely cause is that the barrel warp is now computed per pixel instead of read from the 16 MB table; this was not profiled.
