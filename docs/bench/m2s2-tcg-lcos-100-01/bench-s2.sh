#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M2 stage 2 item 5 in the TCG guest: tick benches and idle CPU, bezel / rgb shift vs none.
set -u
OUT=$1; B=/t/bin/lterm-m1-bench; T=/t/bin/lunduke-terminal; SIZE=1280x800; mkdir -p $OUT
: > $OUT/bench.txt
for v in "" "--frame" "--profile ibm-dos --rgb-shift 0.3524"; do
  for q in full static; do
    p="--profile amber"; case "$v" in *ibm-dos*) p="";; esac
    echo "\$ --tick-gl $p $v --quality $q" >> $OUT/bench.txt
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4 $B --tick-gl --size $SIZE $p $v --quality $q --runs 5 >> $OUT/bench.txt 2>&1
    echo "\$ --tick --threads 4 $p $v --quality $q" >> $OUT/bench.txt
    $B --tick --threads 4 --size $SIZE $p $v --quality $q --runs 5 >> $OUT/bench.txt 2>&1
  done
done
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
idle() {
  label=$1; shift
  LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after 56000 "$@" > $OUT/idle-stats.tmp 2>&1 &
  pid=$!; sleep 3; xdotool search --sync --pid $pid | head -1 | xargs -r xdotool windowactivate >/dev/null 2>&1
  sleep 37; hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep 10; b=$(cpu_ticks $pid)
  wait $pid 2>/dev/null
  st=$(grep -E '^stats: quality' $OUT/idle-stats.tmp | tr '\n' ' ')
  echo "$label: cpu $(awk -v a=$a -v b=$b -v hz=$hz 'BEGIN{printf "%.1f",(b-a)*100/hz/10}')% [$st]" >> $OUT/idle.txt
}
echo "# window $SIZE, idle shell, cpu over 10 s after 40 s; 100% = one core" > $OUT/idle.txt
for r in gl cpu; do
  for q in auto static; do
    idle "amber $r $q" --profile amber --renderer $r --threads 4 --effects-quality $q
    idle "amber+bezel $r $q" --profile amber --frame --renderer $r --threads 4 --effects-quality $q
    idle "ibm-dos(rgb) $r $q" --profile ibm-dos --renderer $r --threads 4 --effects-quality $q
  done
done
rm -f $OUT/idle-stats.tmp
