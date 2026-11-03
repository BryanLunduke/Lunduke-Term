# perf step 2 status

Done and committed: bundled libvterm 0.3.3 + patches 0001/0002, REP regression test, t/ suite,
differential tests (meson + long runs), feed/dump comparison, coverage, licensing/docs,
test suites round 1 (tests-round1.txt) and round 2 (tests-round2.txt), libFuzzer 120 s (fuzz.txt).

OPEN: scrollback-default fails with patch 0002 (not a load flake; reproducible at load 4-5, the
WM alive; stock bundled and 0001-only builds pass). sb-lines ends 17 short = a 17-row window
growth after the last scrollback push pops 17 lines that the 5000-line cap had already dropped
at the front. Hypothesis: timing (0002 finishes the 12000 lines before the WM's late resize),
not a content difference (differential tests clean). Probe: /workspace/perf2-work/sbprobe.sh.

Running: measurement matrix (/workspace/perf2-work/measure/matrix.sh, own Xvfb :82 + xfwm4).
Left: probe the scrollback-default failure, measurement table, PR body.
