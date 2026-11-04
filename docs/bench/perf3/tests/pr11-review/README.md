# Test suites at the PR #11 review head (REAL)

Code: 66e05d1 (the C++ and meson.build are the same as at f1bdbb7; 66e05d1 only adds docs).
Own Xvfb :96, LIBGL_ALWAYS_SOFTWARE=1, appstreamcli on PATH (/workspace/lterm-tools), so there are
28 tests: the 27 from before plus `cpu-hash`. `--timeout-multiplier 4`, builds -j2.
build = release, build-san = debug with `-Db_sanitize=address,undefined` (ASan+UBSan, with LSan via
detect_leaks=1). Script: suites.sh. Run 2026-11-03, 18:23-18:54 CST.

| suite | result | wall |
|---|---|---|
| release, serial | 28 OK | 237 s |
| release, parallel (2) | 28 OK | 223 s |
| ASan/UBSan+LSan, serial | 28 OK | 635 s |
| ASan/UBSan+LSan, parallel (2) | 28 OK | 553 s |

`cpu-hash` passed in all four (12 of 12 cases; it compares, it does not skip, on this x86-64 build
without FMA). gl-match (release-serial-testlog.txt) gives the same 76 lines as ../../glmatch-after.txt
and ../../glmatch-rowprepass.txt; the minimum GL-vs-CPU PSNR is 58.81 dB, as on master.

## The review_test.cpp:161 flake did appear, in a run that was stopped

aborted-no-metainfo/ is an earlier attempt at the same code. After meson.build changed (cpu-hash),
the build directories had been reconfigured without appstreamcli on PATH, so that run had 27 tests:
cpu-hash in, metainfo out. Its release-serial suite had 26 OK and 1 FAIL: review_test.cpp:161
(testFloodFrameCap, the wall-clock check that at most 10 content frames per second are drawn during a
flood), the known timing flake (docs/bench/m6/ram/tests/review-flood-cap-note.txt; it also hit
release-serial at 2f13075, see ../README.md). I stopped that run, reconfigured both build directories
with appstreamcli on PATH, and ran all four suites again (above). After the suites, 3 re-runs of the
release review test all passed (review-reruns.txt: 19 content frames in 2.0 s each). The branch does
not touch the frame-cap code.
