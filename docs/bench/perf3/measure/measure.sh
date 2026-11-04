#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# perf3 copy of perf1-work/measure.sh. Changes: paths; the terminal gets --quit-after so it exits
# cleanly and prints its --stats line (median tick / effects-source / paint ms); the process is
# only killed if it has not exited 60 s after the planned quit.
# measure.sh LABEL DISPLAY QUIT_MS -- CMD...
lab=$1; disp=$2; quit=$3; shift 4
d=/tmp/perf3-run; mkdir -p $d; rm -f $d/$lab.*
out=/workspace/perf3-work/measure/raw; mkdir -p $out
h=$(mktemp -d /tmp/perf3-home-XXXX)
export DISPLAY=$disp
cpu(){ awk '{print $14+$15}' /proc/$1/stat; }   # clock ticks
snap(){ { echo "# $lab $1 $(date '+%T')"; grep -E '^(Rss|Pss|Private_Dirty|Private_Clean|Shared_Clean|Anonymous):' /proc/$pid/smaps_rollup; echo "GLmaps $(grep -cE 'libLLVM|dri|mesa|libGLX|libEGL' /proc/$pid/maps)"; echo "Threads $(grep Threads /proc/$pid/status | awk '{print $2}')"; } > $out/$lab-$1.txt; }
t0=$(date +%s%N)
XDG_CONFIG_HOME=$h "$@" --quit-after $quit -e /workspace/perf3-work/measure/work.sh $lab > $out/$lab-stdout.txt 2>&1 &
pid=$!
wid=$(timeout 30 xdotool search --sync --onlyvisible --pid $pid 2>/dev/null | head -1)
t1=$(date +%s%N)
echo "startup_map_ms $(( (t1-t0)/1000000 ))" > $out/$lab-summary.txt
for i in $(seq 1 300); do pgrep -f "work.sh $lab" >/dev/null && break; sleep 0.01; done
[ -n "$wid" ] && xdotool windowactivate $wid 2>/dev/null; sleep 0.5
echo "geom $(xdotool getwindowgeometry $wid 2>/dev/null | tr '\n' ' ')" >> $out/$lab-summary.txt
sleep 7.5; snap fresh
c0=$(cpu $pid); sleep 30; c1=$(cpu $pid)
echo "idle_cpu_pct_30s $(echo "($c1-$c0)/30" | bc -l | xargs printf %.1f)" >> $out/$lab-summary.txt
c0=$(cpu $pid); touch $d/$lab.go
for i in $(seq 1 1800); do grep -q done $d/$lab.times 2>/dev/null && break; sleep 0.1; done
c1=$(cpu $pid)
echo "work_cpu_s $(echo "($c1-$c0)/100" | bc -l | xargs printf %.1f)" >> $out/$lab-summary.txt
cat $d/$lab.times >> $out/$lab-summary.txt 2>/dev/null
sleep 3; snap after
c0=$(cpu $pid); sleep 30; c1=$(cpu $pid)
if kill -0 $pid 2>/dev/null; then
  echo "idle_cpu_pct_after_30s $(echo "($c1-$c0)/30" | bc -l | xargs printf %.1f)" >> $out/$lab-summary.txt
else
  echo "idle_cpu_pct_after_30s INVALID_process_gone" >> $out/$lab-summary.txt
fi
echo "elapsed_before_quit_ms $(( ($(date +%s%N)-t0)/1000000 )) (quit-after $quit)" >> $out/$lab-summary.txt
for i in $(seq 1 $(( quit/1000 + 60 ))); do kill -0 $pid 2>/dev/null || break; sleep 1; done
if kill -0 $pid 2>/dev/null; then echo "clean_exit no (killed)" >> $out/$lab-summary.txt; kill $pid; sleep 1; kill -9 $pid 2>/dev/null; else echo "clean_exit yes" >> $out/$lab-summary.txt; fi
wait $pid 2>/dev/null
pkill -f "work.sh $lab"
grep '^stats:' $out/$lab-stdout.txt >> $out/$lab-summary.txt
rm -rf $h
