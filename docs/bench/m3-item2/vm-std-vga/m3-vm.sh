#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M3 item 2 in the TCG guest. "SHOT name" on ttyS0 asks the host to take a QMP screendump.
OUT=$1; T=/t/bin/lunduke-terminal; SIZE=1024x640
mark() { echo "MARK $*" | sudo tee /dev/ttyS0 >/dev/null; }
shot() { sync; echo "SHOT $1" | sudo tee /dev/ttyS0 >/dev/null; sleep 12; }
ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
sys() { awk '/^cpu /{print $2+$3+$4+$6+$7+$8, $5}' /proc/stat; }   # busy idle (jiffies, all vCPUs)
hz=$(getconf CLK_TCK); ncpu=$(nproc)
focus() { xdotool search --sync --onlyvisible --pid "$1" 2>/dev/null | tail -1 | xargs -r xdotool windowactivate >/dev/null 2>&1; }
{ uname -a; head -3 /etc/os-release; xdpyinfo 2>&1 | head -3; glxinfo -B 2>&1 | grep -E "renderer|version string" ; nproc; free -m;
  cat /t/BUILD; $T --list-profiles; } > $OUT/env.txt 2>&1
# Whole-guest CPU with nothing of ours running (baseline).
s1=$(sys); sleep 20; s2=$(sys)
echo "$s1 $s2" | awk -v n=$ncpu '{b=$3-$1; i=$4-$2; printf "guest baseline (no terminal): busy %.1f%% of all %d vCPUs over 20 s\n", b*100/(b+i), n}' > $OUT/idle.txt
# idle LABEL WARMUP SECS args...: process CPU (100% = one vCPU), guest CPU, governor stats
idle() {
  label=$1; warm=$2; secs=$3; shift 3; LOG=$OUT/idle-$(echo $label | tr ' /' '__').log
  LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --stats --quit-after $(( (warm+secs+15)*1000 )) "$@" -e /bin/bash --norc -c 'export PS1="lcos\$ "; echo "M3 idle: '"$label"'"; exec bash --norc -i' > $LOG 2>&1 &
  pid=$!; sleep 5; focus $pid; sleep $((warm-5))
  a=$(ticks $pid); s1=$(sys); sleep $secs; b=$(ticks $pid); s2=$(sys)
  shot "idle-$(echo $label | tr ' /' '--')"
  wait $pid 2>/dev/null
  echo "$s1 $s2" | awk -v l="$label" -v a=$a -v b=$b -v hz=$hz -v s=$secs -v n=$ncpu -v w=$warm '{bb=$3-$1; ii=$4-$2; printf "%s: process %.1f%% of one vCPU over %d s after %d s; guest busy %.1f%% of %d vCPUs\n", l, (b-a)*100/hz/s, s, w, bb*100/(bb+ii), n}' >> $OUT/idle.txt
  grep -E "^stats: quality|governor|auto|probing" $LOG | sed "s/^/    /" >> $OUT/idle.txt
  sync
}
mark idle-start
idle "plain (default)" 40 20
idle "amber gl auto" 40 60 --profile amber
idle "amber cpu auto" 40 60 --profile amber --renderer cpu --threads 4
mark idle-done
# Interactive: tabs, search, settings (corrupt file), close.
CFG=$(mktemp -d); mkdir -p $CFG/lunduke-terminal
printf '[terminal\nprofile=amber\n\377\376 garbage\n' > $CFG/lunduke-terminal/settings.ini
XDG_CONFIG_HOME=$CFG LIBGL_ALWAYS_SOFTWARE=1 $T --size $SIZE --quit-after 400000 -e /bin/bash --norc -c \
  'for i in $(seq 1 3000); do echo "[$i] cc -O2 -c module_$((i%97)).c ... ok"; [ $((i%250)) = 0 ] && echo "[$i] warning: needle in module_$((i%97)).c"; done; export PS1="lcos\$ "; exec bash --norc -i' > $OUT/interactive.log 2>&1 &
pid=$!; sleep 15; focus $pid; sleep 2
xdotool key ctrl+shift+f; sleep 3; xdotool type --delay 300 needle; sleep 6
xdotool key Return; sleep 2; xdotool key Return; sleep 4
shot search
xdotool key Escape; sleep 2
xdotool key ctrl+shift+t; sleep 8; xdotool type --delay 100 "echo tab two"; xdotool key Return; sleep 2
xdotool key ctrl+shift+t; sleep 8; xdotool type --delay 100 "echo tab three"; xdotool key Return; sleep 3
shot tabs-3
ps -o pid,stat,cmd --ppid $pid > $OUT/children-before-close.txt
xdotool key ctrl+shift+w; sleep 4
shot tabs-after-close
sleep 3; ps -o pid,stat,cmd --ppid $pid > $OUT/children-after-close.txt
xdotool key ctrl+shift+s; sleep 6
w=$(xdotool getactivewindow); xdotool getwindowgeometry $w > $OUT/settings-geometry.txt 2>&1
shot settings-open
xdotool mousemove --window $w 76 271 click 1; sleep 5
shot settings-saved
ls -la $CFG/lunduke-terminal > $OUT/settings-files.txt; cat $CFG/lunduke-terminal/settings.ini >> $OUT/settings-files.txt
xdotool key Escape; sleep 2
kill $pid 2>/dev/null; wait $pid 2>/dev/null; sleep 3
ps -eo pid,stat,cmd | awk '$2 ~ /Z/' > $OUT/zombies.txt
mark close-bench
LIBGL_ALWAYS_SOFTWARE=1 /t/bin/lterm-m3-close-bench 10 > $OUT/close.txt 2>&1
mark close-bench-done
sync
