#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M3 item 2: ~10 min soak. Heavy output in tab 1, tabs opened/closed, window resizes.
# Samples VmRSS, fd count, threads, child count every 10 s to soak-<label>.csv.
T=${T:-/workspace/lunduke-term-m3/build/lunduke-terminal}; OUT=$1; LABEL=$2; DUR=${DUR:-600}; shift 2
export LIBGL_ALWAYS_SOFTWARE=1; mkdir -p $OUT; CSV=$OUT/soak-$LABEL.csv
CFG=$(mktemp -d); export XDG_CONFIG_HOME=$CFG
XDG_CONFIG_HOME=$CFG $T --size 1024x640 --stats --quit-after $(( (DUR+60)*1000 )) "$@" -e /bin/bash --norc -c \
  'n=0; while :; do n=$((n+1)); seq -f "line %g of a long build log: cc -O2 -Wall -c src/module.c -o build/module.o ... ok" 1 4000; printf "\e[1;3%dmcolour burst %d\e[0m\n" $((n%8)) $n; sleep 0.3; done' > $OUT/soak-$LABEL.log 2>&1 &
pid=$!; sleep 8
win=$(xdotool search --sync --onlyvisible --pid $pid 2>/dev/null | tail -1)
echo "t_s,rss_kb,fds,threads,children,tabs_opened,tabs_closed,resizes" > $CSV
start=$(date +%s); opened=0; closed=0; rs=0; i=0
sample() { t=$(( $(date +%s)-start )); r=$(awk '/VmRSS/{print $2}' /proc/$pid/status); f=$(ls /proc/$pid/fd | wc -l); th=$(awk '/Threads/{print $2}' /proc/$pid/status); c=$(pgrep -P $pid | wc -l); echo "$t,$r,$f,$th,$c,$opened,$closed,$rs" >> $CSV; }
sample
while [ $(( $(date +%s)-start )) -lt $DUR ] && kill -0 $pid 2>/dev/null; do
  i=$((i+1))
  xdotool windowfocus --sync $win 2>/dev/null; xdotool mousemove --window $win 300 200
  # open 3 tabs, type in them, close them (back to tab 1 with the output loop)
  for k in 1 2 3; do xdotool key ctrl+shift+t; opened=$((opened+1)); sleep 0.6; xdotool type --delay 5 "echo soak $i $k; ls /usr/lib | head -50"; xdotool key Return; sleep 0.4; done
  for k in 1 2 3; do xdotool key ctrl+shift+w; closed=$((closed+1)); sleep 0.5; done
  # resize a few times
  for sz in "700 400" "1200 760" "500 300" "1024 640"; do xdotool windowsize $win $sz; rs=$((rs+1)); sleep 0.5; done
  [ $((i%2)) = 0 ] && sample
done
sample
kill $pid; wait $pid 2>/dev/null
grep -E "^stats" $OUT/soak-$LABEL.log | tail -3 > $OUT/soak-$LABEL-stats.txt
echo "zombies: $(ps -eo stat,ppid | awk -v p=$pid '$1 ~ /Z/ && $2==p' | wc -l)" >> $OUT/soak-$LABEL-stats.txt
rm -rf "$CFG"
