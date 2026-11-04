# Test suites at 995c820 (REAL)

Own Xvfb :94, LIBGL_ALWAYS_SOFTWARE=1, appstreamcli on PATH (/workspace/lterm-tools), so there are 27 tests. `--timeout-multiplier 4`, builds -j2. build = release, build-san = debug with `-Db_sanitize=address,undefined` (ASan+UBSan, with LSan via detect_leaks=1).

| suite | result | wall |
|---|---|---|
| release, serial | 26 OK, 1 FAIL (review) | 235 s |
| release, parallel (2) | 27 OK | 222 s |
| ASan/UBSan+LSan, serial | 27 OK | 622 s |
| ASan/UBSan+LSan, parallel (2) | 27 OK | 546 s |

The one failure is review_test.cpp:161 (testFloodFrameCap), a wall-clock check that at most 10 content frames per second are drawn during a flood. This is the known timing flake recorded in docs/bench/m6/ram/tests/review-flood-cap-note.txt. The box had wall-clock stalls this afternoon (see measure/README.md). The same test passed in the other three suites, and 3 re-runs of the release review test all passed (review-reruns.txt: 20, 19 and 19 frames in 2.0 s). This branch does not touch the frame-cap code.

gl-match (release-serial-testlog.txt) gives the same 74 PSNR lines as master (../glmatch-master.txt vs ../glmatch-after.txt). The only differing lines are the two GL object counts, each one texture and one FBO lower: "textures 12 -> 12, fbos 10 -> 10" became "11 -> 11, 9 -> 9". In the review test the app's own texture counter for the amber GL static view reads "textures 4.6 MB shown" (the value recorded on master in docs/bench was 5.9 MB, at the review test's window size).

PR #11 review head: see pr11-review/README.md (28 tests, all four suites 28/28; the review flood-cap flake appeared once in a stopped run and is documented there).
