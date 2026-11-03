# perf step 2 status (updated as slices finish)

Done and committed: bundled libvterm 0.3.3 + patches 0001/0002, REP regression test, upstream
t/ suite (stock and patched), randomised differential test (meson + long runs), source/seq dump
comparison, coverage, fuzz build on the bundled libvterm, docs/licensing, LSan suppression for
the t/ harness.

In progress (pipeline /workspace/perf2-work/rest.sh, nohup, log /workspace/perf2-work/rest.log):
1. test suites first round (09:27-10:20 CST, HEAD 74e1a8f) -> tests-round1.txt
2. rerun release + ASan serial at f8430ac (after the t/ suppression)
3. fuzz: REP reproducer replay (stock vs patched), libFuzzer 120 s on the patched build
4. measurement matrix (/workspace/perf2-work/measure/matrix.sh, own Xvfb :82 + xfwm4)
Left after that: copy results here, PR body /workspace/lunduke-term-perf2-PR-BODY.md.
