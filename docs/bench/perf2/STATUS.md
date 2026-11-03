# perf step 2 status: complete

All slices are done and committed:
- bundled libvterm 0.3.3 + patches 0001/0002;
- tests, differential runs, fuzzing;
- measurement vs 21eff10 (measure/TABLE.md);
- merge of origin/master f2b96d1 (118ba52);
- tests at the merge (tests-merged.txt: release, ASan/UBSan+LSan serial and parallel 27/27, libFuzzer 60 s clean);
- measurement vs f2b96d1 (measure-merged/TABLE.md).

Nothing is open. The PR body is at /workspace/lunduke-term-perf2-PR-BODY.md (outside the repo).
