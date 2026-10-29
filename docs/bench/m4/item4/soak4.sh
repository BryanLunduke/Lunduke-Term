#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 4: the M3 soak (heavy output in tab 1, 3 tabs opened/typed/closed, 4 resizes per cycle)
# plus memory accounting: --stats-every 5 (glibc heap + per-view counters), smaps_rollup every
# 2 cycles, and Rss by mapping kind (smaps-group.sh) at ~8 s, ~30 s, ~60 s and the end.
# Usage: DISPLAY=:N DUR=S soak4.sh OUTDIR LABEL [terminal args]   (env passes through, e.g. MALLOC_ARENA_MAX)
D=$(cd "$(dirname "$0")" && pwd)
T=${T:-/workspace/lunduke-term-m4/build/lunduke-terminal}; OUT=$1; LABEL=$2; DUR=${DUR:-600}; shift 2
export LIBGL_ALWAYS_SOFTWARE=1; mkdir -p $OUT; CSV=$OUT/$LABEL.csv
CFG=$(mktemp -d); export XDG_CONFIG_HOME=$CFG
$T --size 1024x640 --stats --stats-every 5 --quit-after $(( (DUR+60)*1000 )) "$@" -e /bin/bash --norc -c \
  'n=0; while :; do n=$((n+1)); seq -f "line %g of a long build log: cc -O2 -Wall -c src/module.c -o build/module.o ... ok" 1 4000; printf "\e[1;3%dmcolour burst %d\e[0m\n" $((n%8)) $n; sleep 0.3; done' > $OUT/$LABEL.log 2>&1 &
pid=$!; sleep 8
win=$(xdotool search --sync --onlyvisible --pid $pid 2>/dev/null | tail -1)
echo "t_s,rss_kb,anon_kb,file_kb,fds,threads,tabs_opened,tabs_closed,resizes" > $CSV
start=$(date +%s); opened=0; closed=0; rs=0; i=0; g=0
sample() { t=$(( $(date +%s)-start )); r=$(awk '/^Rss:/{print $2}' /proc/$pid/smaps_rollup); an=$(awk '/^Anonymous:/{print $2}' /proc/$pid/smaps_rollup); fl=$(awk '/^Rss:/{r=$2} /^Anonymous:/{a=$2} END{print r-a}' /proc/$pid/smaps_rollup); f=$(ls /proc/$pid/fd | wc -l); th=$(awk '/Threads/{print $2}' /proc/$pid/status); echo "$t,$r,$an,$fl,$f,$th,$opened,$closed,$rs" >> $CSV; }
group() { echo "# t=$(( $(date +%s)-start )) s" >> $OUT/$LABEL-smaps.txt; $D/smaps-group.sh $pid >> $OUT/$LABEL-smaps.txt; }
sample; group
while [ $(( $(date +%s)-start )) -lt $DUR ] && kill -0 $pid 2>/dev/null; do
  i=$((i+1))
  xdotool windowfocus --sync $win 2>/dev/null; xdotool mousemove --window $win 300 200
  for k in 1 2 3; do xdotool key ctrl+shift+t; opened=$((opened+1)); sleep 0.6; xdotool type --delay 5 "echo soak $i $k; ls /usr/lib | head -50"; xdotool key Return; sleep 0.4; done
  for k in 1 2 3; do xdotool key ctrl+shift+w; closed=$((closed+1)); sleep 0.5; done
  for sz in "700 400" "1200 760" "500 300" "1024 640"; do xdotool windowsize $win $sz; rs=$((rs+1)); sleep 0.5; done
  [ $((i%2)) = 0 ] && sample
  el=$(( $(date +%s)-start ))
  if [ $g = 0 ] && [ $el -ge 30 ]; then group; g=1; fi
  if [ $g = 1 ] && [ $el -ge 60 ]; then group; g=2; fi
done
sample; group
kill $pid; wait $pid 2>/dev/null
grep -E "^stats-mem" $OUT/$LABEL.log > $OUT/$LABEL-mem.txt
grep -E "^stats:" $OUT/$LABEL.log | tail -3 > $OUT/$LABEL-stats.txt
echo "zombies: $(ps -eo stat,ppid | awk -v p=$pid '$1 ~ /Z/ && $2==p' | wc -l)" >> $OUT/$LABEL-stats.txt
rm -rf "$CFG"
