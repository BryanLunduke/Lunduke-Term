#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the LCOS 1.0 live guest (TCG). Payload disk mounted at /t. M2 stage 1.
export LD_LIBRARY_PATH=/t/lib FONTCONFIG_FILE=/t/fonts.conf DISPLAY=:0 XAUTHORITY=/home/lcos/.Xauthority
export PATH=/t/bin:$PATH
echo "START $(date)" > /t/out/status
{ uname -a; head -3 /etc/os-release; xdpyinfo 2>&1 | head -3; /t/bin/lunduke-terminal --list-profiles; } > /t/out/env.txt 2>&1
sync
RUNS=5 LPS="4" CPUTHREADS="4" IDLE=1 IDLE_SECS=10 IDLE_WARMUP=40 \
IDLE_FOCUS='xdotool search --sync --pid "$1" | head -1 | xargs -r xdotool windowactivate' \
  sh /t/bench-m2.sh /t/out/tcg /t/bin > /t/out/bench.log 2>&1
echo "DONE $(date)" >> /t/out/status
sync
