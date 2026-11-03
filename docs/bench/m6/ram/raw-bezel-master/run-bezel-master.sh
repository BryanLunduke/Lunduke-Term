#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 PR B follow-up: master dc3b544, amber C++ path + bezel, 3 runs (r3 was started as a separate run).
cd /workspace/lunduke-term-m6-ram
export PATH=/workspace/lterm-tools/root/usr/bin:$PATH LD_LIBRARY_PATH=/workspace/lterm-tools/root/usr/lib/x86_64-linux-gnu DISPLAY=:46 LIBGL_ALWAYS_SOFTWARE=1
d=docs/bench/m6/ram; o=/workspace/m6-work/raw-bezel; mkdir -p $o; job="sleep 10; seq 1 20000; cat $PWD/src/*.cpp; exec sleep 600"
m=/workspace/m6-work/wt-master/build/lunduke-terminal
for r in 1 2 3; do bash $d/measure.sh master-amber-cpu-bezel-r$r $o -- $m --stats-every 5 --profile amber --renderer cpu --frame -e bash -c "$job"; done
echo BEZELDONE
