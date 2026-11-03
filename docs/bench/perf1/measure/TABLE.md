# Perf step 1: before/after measurements

Conditions: Xvfb :71 (1920x1200x24, GLX) with xfwm4 --compositor=off, on the shared 8-core box. Mesa llvmpipe (software GL) only; there is no GPU. The GL profiles print "GLSL on llvmpipe (LLVM 19.1.7, 256 bits)" for both builds. Both binaries ran with system libraries (LD_LIBRARY_PATH and LIBGL_ALWAYS_SOFTWARE unset).
BEFORE = master 21eff10 release build (separate clone, sha256 902b1fa6…). AFTER = perf-step1 release build of 8a9fef7 (sha256 fdd99f20…); the code is unchanged up to the head, only tests and docs were added.
Script: a copy of /workspace/lterm-review2/out/measure.sh and work.sh with only the paths changed (measure-sh-path-changes.diff). The driver is matrix.sh: 3 rounds; inside each round the profiles run in order, with before/after on odd rounds and after/before on even rounds. Load average at each start is in matrix.log (mostly 0.0–1.1 1-min; 3.88 at the start of before-amber-gl-1280-r2, right after the stall).
Units: idle CPU = % of one core over 30 s (from /proc/PID/stat utime+stime), measured fresh (about 8 s after map) and again 3 s after the work load (seq/cat/yes). Private_Dirty is in kB from /proc/PID/smaps_rollup at the fresh and after snapshots.
--stats texture bytes are not available: measure.sh stops the process with SIGTERM, so it prints no --stats output.

**Window manager:** xfwm4 (PID 2346967) was running for round 1 (all 8 r1 runs) and for after-amber-gl-1280-r2. It died during the box stall (02:53–07:00 CST), so the other 15 runs (the rest of rounds 2 and 3) ran on Xvfb :71 **without a window manager**. You can see this in each summary's `geom` line: window at 10,58 (decorated) for the first group and 0,0 for the second. Before and after are still paired under the same conditions within each round, and the round-1 runs alone (WM running) show the same direction and size: idle CPU fresh 17.5→12.3 (gl-1280), 17.6→12.0 (gl-1024), 20.9→12.2 (cpu-1024); Private_Dirty fresh −4140 kB (gl-1280), −2864 kB (gl-1024) (REAL r1 values; the differences are DERIVED).

## REAL: every run

| profile | build | run | idle CPU % fresh | idle CPU % after work | Private_Dirty fresh kB | Private_Dirty after kB | note |
|---|---|---|---|---|---|---|---|
| amber-gl-1280 | before | 1 | 17.5 | 19.2 | 89388 | 92664 |  |
| amber-gl-1280 | before | 2 | 19.1 | 18.7 | 89340 | 92576 |  |
| amber-gl-1280 | before | 3 | 18.6 | 18.6 | 89400 | 92652 |  |
| amber-gl-1280 | after | 1 | 12.3 | 12.1 | 85248 | 88516 |  |
| amber-gl-1280 | after | 2 | 10.4 | -95.0 (invalid) | 85480 | 90080 | idle-after INVALID (box stalled ~4 h 02:53→07:00; process gone before the 2nd CPU read; excluded) |
| amber-gl-1280 | after | 3 | 10.7 | 11.2 | 85284 | 88544 |  |
| amber-gl-1024 | before | 1 | 17.6 | 18.1 | 81160 | 84212 |  |
| amber-gl-1024 | before | 2 | 20.3 | 19.8 | 81244 | 84080 |  |
| amber-gl-1024 | before | 3 | 18.3 | 19.1 | 81064 | 84088 |  |
| amber-gl-1024 | after | 1 | 12.0 | 11.9 | 78296 | 81132 |  |
| amber-gl-1024 | after | 2 | 13.2 | 13.4 | 78204 | 81036 |  |
| amber-gl-1024 | after | 3 | 10.4 | 1.5 | 78344 | 81192 | run took ~4.3 min wall vs ~1.4 normal (box stall?); idle-after 1.5 is an outlier, kept |
| amber-cpu-1024 | before | 1 | 20.9 | 10.8 | 56620 | 60428 | idle-after 10.8 is an outlier vs 18.5/18.9, cause unknown; kept |
| amber-cpu-1024 | before | 2 | 20.3 | 18.9 | 56580 | 60348 |  |
| amber-cpu-1024 | before | 3 | 19.3 | 18.5 | 56572 | 60460 |  |
| amber-cpu-1024 | after | 1 | 12.2 | 10.1 | 56620 | 60560 | run took ~2.0 min wall vs ~1.4 normal; kept |
| amber-cpu-1024 | after | 2 | 11.8 | 12.2 | 56572 | 60340 |  |
| amber-cpu-1024 | after | 3 | 12.4 | 12.5 | 56572 | 60312 |  |
| plain-1024 | before | 1 | 0.0 | 0.0 | 9452 | 16256 |  |
| plain-1024 | before | 2 | 0.0 | 0.0 | 9408 | 14652 |  |
| plain-1024 | before | 3 | 0.0 | 0.0 | 9404 | 13188 |  |
| plain-1024 | after | 1 | 0.0 | 0.0 | 9448 | 13280 |  |
| plain-1024 | after | 2 | 0.0 | 0.0 | 9412 | 13196 |  |
| plain-1024 | after | 3 | 0.0 | 0.0 | 9408 | 13204 |  |

## DERIVED: medians of the runs above (the invalid value is excluded; n = number of runs)

| profile | idle CPU % fresh: before → after | idle CPU % after work: before → after | PD fresh kB: before → after (Δ) | PD after kB: before → after (Δ) |
|---|---|---|---|---|
| amber-gl-1280 | 18.6 → 10.7 (n 3/3) | 18.7 → 11.65 (n 3/2) | 89388 → 85284 (-4104) | 92652 → 88544 (-4108) |
| amber-gl-1024 | 18.3 → 12 (n 3/3) | 19.1 → 11.9 (n 3/3) | 81160 → 78296 (-2864) | 84088 → 81132 (-2956) |
| amber-cpu-1024 | 20.3 → 12.2 (n 3/3) | 18.5 → 12.2 (n 3/3) | 56580 → 56572 (-8) | 60428 → 60340 (-88) |
| plain-1024 | 0 → 0 (n 3/3) | 0 → 0 (n 3/3) | 9408 → 9412 (+4) | 14652 → 13204 (-1448) |
