#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 PR B follow-up: RAM with the new default scrollback (5000), and the head at --scrollback 10000
# with the bezel. BIN = a release build of the head with the 5000 default. Outputs in raw-sb5000/.
cd "$(dirname "$0")/../../../.."
d=docs/bench/m6/ram; o=$d/raw-sb5000; mkdir -p $o; job="sleep 10; seq 1 20000; cat $PWD/src/*.cpp; exec sleep 600"
n=${BIN:-build/lunduke-terminal}
for r in 1 2; do bash $d/measure.sh head-sb10000-amber-cpu-bezel-r$r $o -- $n --scrollback 10000 --stats-every 5 --profile amber --renderer cpu --frame -e bash -c "$job"; done
bash $d/measure.sh sb5000-amber-gl $o -- $n --stats-every 5 --profile amber -e bash -c "$job"
bash $d/measure.sh sb5000-amber-cpu $o -- $n --stats-every 5 --profile amber --renderer cpu -e bash -c "$job"
bash $d/measure.sh sb5000-plain $o -- $n --stats-every 5 --profile plain -e bash -c "$job"
bash $d/measure.sh sb5000-amber-cpu-bezel $o -- $n --stats-every 5 --profile amber --renderer cpu --frame -e bash -c "$job"
