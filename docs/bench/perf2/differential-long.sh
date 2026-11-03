#!/bin/bash
# Long verification batch for perf-step2 (docs/bench/perf2/differential-long.txt).
export PATH=/workspace/lterm-tools/root/usr/bin:$PATH LD_LIBRARY_PATH=/workspace/lterm-tools/root/usr/lib/x86_64-linux-gnu
R=/workspace/lunduke-term-perf2; W=/workspace/perf2-work/verify; cd $R
B=$R/build; S=$R/build-san; RUN="python3 tools/libvterm_diff_run.py"
ninja -C build-san -j2 > $W/san-build.log 2>&1 || { echo SAN BUILD FAILED; exit 1; }
hdr() { echo; echo "== $(date '+%F %T %Z') load $(cut -d' ' -f1-3 /proc/loadavg): $*"; }
hdr "release, mode a (stock vs patched), seeds 1..3000 x 2000 steps"
$RUN $B/libvterm_diff_stock $B/libvterm_diff_patched a 1 3000 2000 -j1 --timeout 120
hdr "release, mode b (stock+0001 vs patched), seeds 1..3000 x 2000 steps"
$RUN $B/libvterm_diff_repfix $B/libvterm_diff_patched b 1 3000 2000 -j1 --timeout 120
hdr "release, mode a, seeds 100001..100300 x 20000 steps (long streams)"
$RUN $B/libvterm_diff_stock $B/libvterm_diff_patched a 100001 300 20000 -j1 --timeout 300
hdr "release, mode b, seeds 100001..100300 x 20000 steps (long streams)"
$RUN $B/libvterm_diff_repfix $B/libvterm_diff_patched b 100001 300 20000 -j1 --timeout 300
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
hdr "ASan/UBSan + leaks (build-san), mode a, seeds 1..300 x 2000 steps"
$RUN $S/libvterm_diff_stock $S/libvterm_diff_patched a 1 300 2000 -j1 --timeout 300
hdr "ASan/UBSan + leaks (build-san), mode b, seeds 1..300 x 2000 steps"
$RUN $S/libvterm_diff_repfix $S/libvterm_diff_patched b 1 300 2000 -j1 --timeout 300
unset ASAN_OPTIONS UBSAN_OPTIONS
hdr "mutation check: stock vs the first 0002 (commit f40898d, without the vacated-row copy), seeds 1..20"
$RUN $B/libvterm_diff_stock /tmp/lvold/diff a 1 20 2000 -j1 --timeout 120 | grep -a -v "^---\|^[^sl]" | grep -a "^seed\|^libvterm\|^different"
hdr "feed comparison: sources, seq 1 2000000, flood8.txt (8 MiB, the measurement file)"
tools/libvterm_feed_diff.sh $B/libvterm_diff_stock $B/libvterm_diff_patched $R 2000000 /workspace/perf2-work/flood8.txt
hdr "done"
