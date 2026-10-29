#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 3: host llvmpipe matrix (copy of m3-item2/host/matrix.sh, M4 build). Every profile x {gl, cpu}, effects-quality auto.
# Process CPU (100% = one core) over SECS after WARM, plus the --stats summary at exit.
T=${T:-/workspace/lunduke-term-m4/build/lunduke-terminal}; OUT=$1; WARM=${WARM:-20}; SECS=${SECS:-20}
export DISPLAY=${DISPLAY:-:44} LIBGL_ALWAYS_SOFTWARE=1
hz=$(getconf CLK_TCK); mkdir -p $OUT
ticks() { awk '{print $14+$15}' /proc/$1/stat 2>/dev/null || echo 0; }
{ date; uname -r; glxinfo -B 2>&1 | grep -E "renderer|version string|Accelerated"; nproc; $T --list-profiles; } > $OUT/env.txt 2>&1
printf "%-16s %-4s %8s %6s  %s\n" profile rend "cpu%" "badutf" "final quality / backend" > $OUT/matrix.txt
for p in plain amber green green-scanlines ibm-dos ibm-3278 vintage futuristic paper-white; do
  for r in gl cpu; do
    log=$OUT/$p-$r.log
    $T --profile $p --renderer $r --size 1024x640 --stats --quit-after $(( (WARM+SECS+5)*1000 )) \
       -e /bin/bash --norc -c "echo 'M4 matrix: $p $r'; ls -la /usr/bin | head -30; exec sleep 600" > $log 2>&1 &
    pid=$!; sleep $WARM; a=$(ticks $pid); sleep $SECS; b=$(ticks $pid); wait $pid
    q=$(grep -E "^stats: quality" $log | tail -1); be=$(grep -oE 'backend="[^"]*"' $log | tail -1)
    u=$(grep -c "Invalid UTF-8" $log); printf "%-16s %-4s %7.1f%% %6s  %s | %s\n" $p $r $(awk -v a=$a -v b=$b -v hz=$hz -v s=$SECS 'BEGIN{print (b-a)*100/hz/s}') "$u" "$q" "$be" >> $OUT/matrix.txt
  done
done
