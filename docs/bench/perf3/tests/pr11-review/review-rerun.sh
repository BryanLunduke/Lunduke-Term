#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
cd /workspace/lunduke-term-perf3
[ -e /tmp/.X11-unix/X96 ] && { echo ":96 busy"; exit 1; }
Xvfb :96 -screen 0 1920x1200x24 +extension GLX +render -noreset >/dev/null 2>&1 & XP=$!; sleep 2
export PATH=/workspace/lterm-tools/root/usr/bin:$PATH LD_LIBRARY_PATH=/workspace/lterm-tools/root/usr/lib/x86_64-linux-gnu LIBGL_ALWAYS_SOFTWARE=1 DISPLAY=:96
for i in 1 2 3; do echo "$(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"; meson test -C build --no-rebuild --timeout-multiplier 4 review 2>&1 | grep -E "review +(OK|FAIL)"; grep "flood amber" build/meson-logs/testlog.txt; done
kill $XP; echo DONE
