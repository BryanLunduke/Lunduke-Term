# perf step 2 status

Done and committed: bundled libvterm 0.3.3 + patches 0001/0002, REP regression test, t/ suite,
differential tests (meson + long runs), feed/dump comparison, coverage, licensing/docs, test
suites round 1 and 2, libFuzzer 120 s (fuzz.txt), measurement (measure/TABLE.md).

scrollback-default with 0002: a timing race in the test (WM resize vs. faster output), fixed in
f37aba0 (the test waits 1 s for the window to settle); scrollback-default.txt has the probe and 4/4
passes (3 release, 1 ASan).

Running: round 3 at HEAD, full release (2 procs) + ASan/UBSan+LSan parallel (2 procs), own Xvfb :83
(/workspace/perf2-work/round3.sh -> /workspace/perf2-work/round3.log). When done: tests-round3.txt.
