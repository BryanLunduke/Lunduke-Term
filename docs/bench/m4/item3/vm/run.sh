#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the LCOS live guest (TCG). Payload disk mounted at /t. M4 item 3.
export LD_LIBRARY_PATH=/t/lib FONTCONFIG_FILE=/t/fonts.conf DISPLAY=:0 XAUTHORITY=/home/lcos/.Xauthority
export PATH=/t/bin:$PATH SHELL=/bin/bash
echo "START $(date)" > /t/out/status; sync
# Back to the X server's VT (the driver logged in on tty2).
x=$(pgrep -o -x Xorg || pgrep -o -f "Xorg|X :0")
vt=$(tr '\0' ' ' < /proc/$x/cmdline 2>/dev/null | grep -o 'vt[0-9]*' | tr -d v | tr -d t)
[ -n "$vt" ] || vt=$(sudo ls -l /proc/$x/fd 2>/dev/null | grep -o '/dev/tty[0-9]*' | head -1 | tr -dc 0-9)
echo "X pid $x vt ${vt:-7}" >> /t/out/status; sudo chvt ${vt:-7}; sleep 5
sh /t/m4-vm.sh /t/out > /t/out/m4-vm.log 2>&1
echo "DONE $(date)" >> /t/out/status; sync
echo "MARK DONE" | sudo tee /dev/ttyS0 >/dev/null
