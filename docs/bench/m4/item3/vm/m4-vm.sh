#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 3 in the LCOS live guest (TCG). Payload disk at /t. Same idle runs as M3 (m3-item2/vm-std-vga),
# plus a longer amber-GL run that samples process CPU every 10 s (probe cost) and RSS.
OUT=$1; T=/t/bin/lunduke-terminal; SIZE=1024x640
mark() { echo "MARK $*" | sudo tee /dev/ttyS0 >/dev/null; }
shot() { sync; echo "SHOT $1" | sudo tee /dev/ttyS0 >/dev/null; sleep 12; }
ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
rss() { awk '/^VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }
sys() { awk '/^cpu /{print $2+$3+$4+$6+$7+$8, $5}' /proc/stat; }
hz=$(getconf CLK_TCK); ncpu=$(nproc)
focus() { xdotool search --sync --onlyvisible --pid "$1" 2>/dev/null | tail -1 | xargs -r xdotool windowactivate >/dev/null 2>&1; }
{ uname -a; head -3 /etc/os-release; xdpyinfo 2>&1 | head -3; glxinfo -B 2>&1 | grep -E "renderer|version string"; nproc; free -m;
  cat /t/BUILD; $T --list-profiles; } > $OUT/env.txt 2>&1
s1=$(sys); sleep 20; s2=$(sys)
echo "$s1 $s2" | awk -v n=$ncpu '{b=$3-$1; i=$4-$2; printf "guest baseline (no terminal): busy %.1f%% of all %d vCPUs over 20 s\n", b*100/(b+i), n}' > $OUT/idle.txt
# idle LABEL WARMUP SECS args...: process CPU (100% = one vCPU), guest CPU, RSS, governor lines (with
# guest uptime stamps), and a per-10-s CPU trace in trace-LABEL.txt.
idle() {
  label=$1; warm=$2; secs=$3; shift 3; tag=$(echo $label | tr ' /()' '--__'); LOG=$OUT/idle-$tag.log
  ( LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after $(( (warm+secs+15)*1000 )) "$@" -e /bin/bash --norc -c 'export PS1="lcos\$ "; echo "M4 idle: '"$label"'"; exec bash --norc -i' 2>&1 \
    | while IFS= read -r l; do echo "[$(cut -d' ' -f1 /proc/uptime)] $l"; done > $LOG ) &
  sleep 2; pid=$(pgrep -n -f "^$T --size $SIZE"); t0=$(cut -d' ' -f1 /proc/uptime)
  echo "pid $pid start-uptime $t0" > $OUT/trace-$tag.txt
  sleep 3; focus $pid; sleep $((warm-5))
  a=$(ticks $pid); s1=$(sys); k=0; prev=$a
  while [ $k -lt $secs ]; do sleep 10; k=$((k+10)); c=$(ticks $pid)
    echo "t+$k s cpu-ms-in-10s $(( (c-prev)*1000/hz )) rss-kb $(rss $pid)" >> $OUT/trace-$tag.txt; prev=$c; done
  b=$(ticks $pid); s2=$(sys); r=$(rss $pid)
  shot "m4-vm-idle-$tag"
  while kill -0 $pid 2>/dev/null; do sleep 1; done; sleep 1
  echo "$s1 $s2" | awk -v l="$label" -v a=$a -v b=$b -v hz=$hz -v s=$secs -v n=$ncpu -v w=$warm -v r=$r '{bb=$3-$1; ii=$4-$2; printf "%s: process %.1f%% of one vCPU over %d s after %d s; guest busy %.1f%% of %d vCPUs; RSS %d kB\n", l, (b-a)*100/hz/s, s, w, bb*100/(bb+ii), n, r}' >> $OUT/idle.txt
  grep -E "stats: quality|governor|auto|probing" $LOG | sed "s/^/    /" >> $OUT/idle.txt
  sync
}
mark idle-start
idle "plain (default)" 40 20
idle "amber gl auto" 40 60 --profile amber
idle "amber cpu auto" 40 60 --profile amber --renderer cpu --threads 4
idle "amber gl auto long" 40 180 --profile amber
mark idle-done
ps -eo pid,stat,cmd | awk '$2 ~ /Z/' > $OUT/zombies.txt
sync
