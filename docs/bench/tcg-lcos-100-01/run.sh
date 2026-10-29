#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the LCOS 1.0 live guest (TCG). Payload disk mounted at /t.
export LD_LIBRARY_PATH=/t/lib FONTCONFIG_FILE=/t/fonts.conf DISPLAY=:0 XAUTHORITY=/home/lcos/.Xauthority
export PATH=/t/bin:$PATH
echo "START $(date)" > /t/out/status
{ uname -a; cat /etc/os-release | head -3; xdpyinfo 2>&1 | head -3; xdotool version 2>&1; /t/bin/lunduke-terminal --list-profiles; } > /t/out/env.txt 2>&1
sync
RUNS=10 IDLE=1 NOAVX=0 IDLE_SECS=10 IDLE_WARMUP=25 \
IDLE_FOCUS='xdotool search --sync --pid "$1" | head -1 | xargs -r xdotool windowactivate' \
  sh /t/bench-m1.sh /t/out/tcg /t/bin > /t/out/bench.log 2>&1
echo "DONE $(date)" >> /t/out/status
sync
