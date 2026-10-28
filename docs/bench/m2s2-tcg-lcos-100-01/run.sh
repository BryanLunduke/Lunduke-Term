#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the LCOS 1.0 live guest (TCG). Payload disk mounted at /t. M2 stage 2 item 5.
export LD_LIBRARY_PATH=/t/lib FONTCONFIG_FILE=/t/fonts.conf DISPLAY=:0 XAUTHORITY=/home/lcos/.Xauthority
export PATH=/t/bin:$PATH
echo "START $(date)" > /t/out/status; sync
sh /t/bench-s2.sh /t/out/s2 > /t/out/bench-s2.log 2>&1
echo "DONE $(date)" >> /t/out/status
sync
