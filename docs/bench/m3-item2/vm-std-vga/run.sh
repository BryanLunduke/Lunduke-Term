#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs inside the LCOS 1.0 live guest (TCG). Payload disk mounted at /t. M3 item 2.
export LD_LIBRARY_PATH=/t/lib FONTCONFIG_FILE=/t/fonts.conf DISPLAY=:0 XAUTHORITY=/home/lcos/.Xauthority
export PATH=/t/bin:$PATH SHELL=/bin/bash
echo "START $(date)" > /t/out/status; sync
sh /t/m3-vm.sh /t/out > /t/out/m3-vm.log 2>&1
echo "DONE $(date)" >> /t/out/status; sync
