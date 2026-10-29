#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# host item-5 bench: tick benches + idle CPU (own Xvfb :43)
cd /workspace/lunduke-term-m2; B=build/lterm-m1-bench; T=build/lunduke-terminal; OUT=/tmp/s2/hb; SIZE=1280x800
export DISPLAY=:43
: > $OUT/bench.txt
for v in "--profile amber" "--profile amber --frame" "--profile ibm-dos" "--profile ibm-dos --rgb-shift 0" ; do
  for q in full reduced static; do
    echo "\$ --tick-gl $v --quality $q" >> $OUT/bench.txt
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4 $B --tick-gl --size $SIZE $v --quality $q --runs 20 >> $OUT/bench.txt 2>&1
    echo "\$ --tick --threads 4 $v --quality $q" >> $OUT/bench.txt
    $B --tick --threads 4 --size $SIZE $v --quality $q --runs 20 >> $OUT/bench.txt 2>&1
  done
done
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
idle() {
  label=$1; shift
  LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after 32000 "$@" > $OUT/idle-stats.tmp 2>&1 &
  pid=$!; echo $pid >> /tmp/s2/term.pids; sleep 3; xdotool search --sync --pid $pid | head -1 | xargs -r xdotool windowactivate >/dev/null 2>&1
  sleep 15; hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep 10; b=$(cpu_ticks $pid)
  wait $pid 2>/dev/null
  st=$(grep -E '^stats: quality' $OUT/idle-stats.tmp | tr '\n' ' ')
  echo "$label: cpu $(awk -v a=$a -v b=$b -v hz=$hz 'BEGIN{printf "%.1f",(b-a)*100/hz/10}')% [$st]" >> $OUT/idle.txt
}
echo "# host, window $SIZE, idle shell, cpu over 10 s after 18 s; 100% = one core" > $OUT/idle.txt
for r in gl cpu; do
  for q in auto full reduced static; do
    idle "amber $r $q" --profile amber --renderer $r --threads 4 --effects-quality $q
    idle "amber+bezel $r $q" --profile amber --frame --renderer $r --threads 4 --effects-quality $q
    idle "ibm-dos(rgb .3524) $r $q" --profile ibm-dos --renderer $r --threads 4 --effects-quality $q
    idle "ibm-dos rgb 0 $r $q" --profile ibm-dos --rgb-shift 0 --renderer $r --threads 4 --effects-quality $q
  done
done
rm -f $OUT/idle-stats.tmp; echo HOST-DONE
