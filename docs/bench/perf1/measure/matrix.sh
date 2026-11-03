#!/bin/bash
# Perf step 1 before/after matrix: same Xvfb :71 + xfwm4, interleaved, 3 runs per profile.
# BEFORE = master 21eff10 release build, AFTER = perf-step1 head release build (copied binaries).
cd /workspace/perf1-work
M=./measure.sh
B=/workspace/perf1-work/bin-master/lunduke-terminal
A=/workspace/perf1-work/bin-after/lunduke-terminal
one() {  # tag build-binary args...
  local tag=$1 bin=$2; shift 2
  echo "$(date +%T) start $tag load=$(cut -d" " -f1-3 /proc/loadavg) nproc=$(nproc)"
  $M $tag :71 lt -- $bin "$@"
}
for r in 1 2 3; do
  for prof in amber-gl-1280 amber-gl-1024 amber-cpu-1024 plain-1024; do
    case $prof in
      amber-gl-1280) args=(--stats) ;;
      amber-gl-1024) args=(--size 1024x768 --stats) ;;
      amber-cpu-1024) args=(--renderer cpu --size 1024x768 --stats) ;;
      plain-1024) args=(--no-effects --size 1024x768 --stats) ;;
    esac
    if [ $((r % 2)) = 1 ]; then order="before after"; else order="after before"; fi
    for w in $order; do
      if [ $w = before ]; then bin=$B; else bin=$A; fi
      one p1-$w-$prof-r$r $bin "${args[@]}"
    done
  done
done
echo MATRIX_DONE $(date '+%T')
