#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M3 item 1b: amber auto under a long burst of output (seq), then idle. Reports the governor's
# messages, the final quality and CPU over the last 10 s (idle). Usage: heavy-output.sh BINARY LABEL
T=$1; LABEL=$2; SIZE=1280x800; export DISPLAY=${DISPLAY:-:44} SHELL=/bin/sh
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
LOG=$(mktemp); DONE=$(mktemp); T0=$(date +%s.%N)
LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after 90000 --profile amber \
  -e sh -c "seq 1 ${SEQ:-15000000}; date +%s.%N > $DONE; sleep 60" > $LOG 2>&1 &
pid=$!; echo $pid >> /tmp/m3.pids; sleep 3
w=$(xdotool search --sync --pid $pid | head -1); xdotool windowactivate $w >/dev/null 2>&1
sleep 72; hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep 10; b=$(cpu_ticks $pid)
wait $pid 2>/dev/null
echo "== $LABEL"
echo "output burst took $(awk -v a=$T0 -v b=$(cat $DONE 2>/dev/null || echo 0) 'BEGIN{printf "%.1f", b-a}') s (idle cpu window: 75-85 s)"
grep -E 'auto' $LOG || echo "(no governor messages)"
grep -E '^stats: quality' $LOG | tail -1
echo "idle cpu (last 10 s): $(awk -v a=$a -v b=$b -v hz=$hz 'BEGIN{printf "%.1f",(b-a)*100/hz/10}')%"
rm -f $LOG $DONE
