# perf step 2 status

Done and committed through 7a9e688: bundled libvterm 0.3.3 + patches 0001/0002, tests, differential
runs, fuzzing, measurement vs 21eff10 (measure/TABLE.md), test rounds 1-3.

118ba52: merged origin/master f2b96d1 (perf step 1, PR #9). Conflicts in meson.build, docs/TESTING.md
and tests/scrollback_default_test.sh; both sides were kept.

Running (/workspace/perf2-work/merged.sh): re-measure vs f2b96d1 (own Xvfb :84), then release,
ASan serial, ASan parallel, libFuzzer 60 s. Results go to tests-merged.txt and measure-merged/.
