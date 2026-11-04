# PR #11 review: CPU renderer re-measure after the row prepass (REAL unless marked DERIVED)

Why: in the first matrix (../measure/) the CPU renderer at 2f13075 was slower than master in every
run (median tick 9.86 -> 10.71 ms, effects-source 13.41 -> 15.74 ms). Ted's review traced that to
the per-pixel barrel and proposed the row prepass (applied in e3570b7). This is the re-measure.

Conditions: my own Xvfb :97 (1920x1200x24, GLX) with `xfwm4 --compositor=off`, on the shared
8-core box (other agents' Xvfb displays were running), 2026-11-03 18:01-18:19 CST. Mesa llvmpipe,
no GPU. Window 1280x800, `--profile amber --renderer cpu --stats`; the app picks quality "reduced"
(stats line). 3 interleaved rounds: before/after on odd rounds, after/before on round 2.
BEFORE = master 6283674 release build (sha256 42b7cbe0…, the same binary as ../measure/).
AFTER = perf-step3 f1bdbb7 release build (sha256 1e45d110…). The C++ is the same from 30479da
on (f1bdbb7 only adds the cpu-hash test).
Scripts: matrix-review.sh is ../measure/matrix.sh with display :97, the CPU profile only and the
p3r- label prefix; it uses ../measure/measure.sh and ../measure/work.sh unchanged (paths in the
scripts are the scratch paths they ran from). tab-review.py made TABLE.md from raw/.
**xfwm4 (same PID) and Xvfb were alive before and after all 6 runs; none was discarded**
(matrix-review.log; every window was decorated at 10,58). All 6 runs exited cleanly, and each run
took about 170 s, so this matrix had no wall-clock stalls like the first one.
Units are as in ../measure/README.md.

| profile | build | run | idle CPU % fresh | idle CPU % after work | Private_Dirty fresh kB | Private_Dirty after kB | median tick ms | median effects-source ms | median paint ms | work CPU s | content frames/ticks | window pos | WM alive before+after |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| amber-cpu-1280 | before | 1 | 12.5 | 10.7 | 70536 | 75104 | 11.02 | 15.53 | 0.70 | 13.5 | 72/434 | 10,58 | yes |
| amber-cpu-1280 | before | 2 | 11.7 | 10.6 | 70528 | 74696 | 10.35 | 14.50 | 0.78 | 12.1 | 67/444 | 10,58 | yes |
| amber-cpu-1280 | before | 3 | 11.0 | 11.0 | 70532 | 74652 | 10.48 | 13.88 | 0.75 | 11.1 | 61/449 | 10,58 | yes |
| amber-cpu-1280 | after | 1 | 11.7 | 10.0 | 44588 | 48748 | 10.98 | 14.11 | 0.57 | 11.7 | 65/424 | 10,58 | yes |
| amber-cpu-1280 | after | 2 | 12.7 | 11.7 | 44592 | 48764 | 10.66 | 13.80 | 0.63 | 11.3 | 61/456 | 10,58 | yes |
| amber-cpu-1280 | after | 3 | 11.2 | 10.9 | 44604 | 48736 | 11.33 | 14.05 | 0.77 | 11.4 | 62/419 | 10,58 | yes |

| profile | idle CPU % fresh | idle CPU % after work | PD fresh kB (delta) | PD after kB (delta) | median tick ms | median effects-source ms |
|---|---|---|---|---|---|---|
| amber-cpu-1280 | 11.7 -> 11.7 | 10.7 -> 10.9 | 70532 -> 44592 (-25940) | 74696 -> 48748 (-25948) | 10.48 -> 10.98 | 14.5 -> 14.05 |

Spread (REAL, min-max of 3): tick before 10.35-11.02, after 10.66-11.33 ms; effects-source before
13.88-15.53, after 13.80-14.11 ms. Work CPU medians 12.1 -> 11.4 s.

DERIVED reading: memory is unchanged by the prepass: Private_Dirty is about 25.9 MB lower than
master, fresh and after work, as in the first matrix. Idle CPU is within noise. Effects-source is
no longer slower: median 14.50 -> 14.05 ms (-0.45 ms, -3%), and every after run (13.80-14.11)
is below the before median (before runs 13.88-15.53). Median tick is 10.48 -> 10.98 ms (+0.50 ms, +5%), but the ranges overlap (one before
run is slower than two after runs), so with 3 runs I cannot call it slower or equal; the step
is under the run-to-run spread seen here. In the first matrix every after run was slower than every
before run on both numbers; that is no longer true. Master's own median tick moved 9.86 -> 10.48 ms
between the two matrices (same binary, different time on the shared box), so numbers should only be
compared within one matrix. Ted's window runs (his box) had master / PR+fix at tick 10.31 / 10.16 ms
and effects-source 13.66 / 13.89 ms. Not profiled.
