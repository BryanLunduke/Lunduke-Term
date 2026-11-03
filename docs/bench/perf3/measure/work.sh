#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# runs inside the terminal: waits for a go file, runs fixed outputs, logs ns timings
lab=$1; d=/tmp/perf3-run
while [ ! -e $d/$lab.go ]; do sleep 0.2; done
t(){ local s=$(date +%s%N); "$@"; local e=$(date +%s%N); echo "$1 $(( (e-s)/1000000 ))" >> $d/$lab.times; }
t seq 1 2000000
t cat /workspace/perf1-work/big.txt
sh -c 's=$(date +%s%N); yes | head -n 2000000; e=$(date +%s%N); echo "yes $(( (e-s)/1000000 ))" >> '$d/$lab.times
echo done >> $d/$lab.times
exec sleep 100000
