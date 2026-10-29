#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M3 item 1b: idle CPU, plain and amber auto (software GL), same method as docs/bench/m2s3-host.
# Usage: idle.sh BINARY LABEL
T=$1; LABEL=$2; SIZE=1280x800; export DISPLAY=${DISPLAY:-:44} SHELL=/bin/bash
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
idle() {
  name=$1; shift; LOG=$(mktemp)
  LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after 30000 "$@" > $LOG 2>&1 &
  pid=$!; echo $pid >> /tmp/m3.pids; sleep 3
  w=$(xdotool search --sync --pid $pid | head -1); xdotool windowactivate $w >/dev/null 2>&1
  sleep 14; hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep 10; b=$(cpu_ticks $pid)
  wait $pid 2>/dev/null
  echo "$LABEL $name: cpu $(awk -v a=$a -v b=$b -v hz=$hz 'BEGIN{printf "%.1f",(b-a)*100/hz/10}')% [$(grep -E '^stats: quality' $LOG | tail -1)]"
  rm -f $LOG
}
idle "plain" 
idle "amber gl auto" --profile amber
idle "amber cpu auto" --profile amber --renderer cpu
