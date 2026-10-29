#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
cd /workspace/lunduke-term-m2; T=build/lunduke-terminal; OUT=/tmp/s3/hb; SIZE=1280x800
export DISPLAY=:43 SHELL=/bin/bash
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
idle() {
  label=$1; tabs=$2; shift 2
  LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after 34000 "$@" > $OUT/idle-stats.tmp 2>&1 &
  pid=$!; echo $pid >> /tmp/s3/term.pids; sleep 3
  w=$(xdotool search --sync --pid $pid | head -1); xdotool windowactivate $w >/dev/null 2>&1; sleep 0.5
  for ((i=1;i<tabs;i++)); do xdotool key --window $w ctrl+shift+t; sleep 1; done
  sleep 14; hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep 10; b=$(cpu_ticks $pid)
  wait $pid 2>/dev/null
  st=$(grep -E '^stats: quality' $OUT/idle-stats.tmp | tail -1)
  echo "$label: cpu $(awk -v a=$a -v b=$b -v hz=$hz 'BEGIN{printf "%.1f",(b-a)*100/hz/10}')% [$st]" >> $OUT/idle.txt
}
echo "# host, stage 3 (7a13b12), window $SIZE, idle shell, cpu over 10 s; 100% = one core" > $OUT/idle.txt
idle "plain 1 tab" 1
idle "plain 3 tabs" 3
idle "amber gl auto 1 tab" 1 --profile amber
idle "amber gl auto 3 tabs" 3 --profile amber
idle "amber gl static" 1 --profile amber --effects-quality static
idle "vintage gl auto" 1 --profile vintage
idle "vintage+bezel gl auto" 1 --profile vintage --frame
idle "futuristic gl auto" 1 --profile futuristic
idle "paper-white gl auto" 1 --profile paper-white
rm -f $OUT/idle-stats.tmp; echo IDLE-DONE
