#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 item 6: the measurement matrix. Usage: run-all.sh OUTDIR BINARY [REPEATS]
out=$1; bin=$2; rep=${3:-3}; here=$(dirname "$0"); mkdir -p "$out"
src=$(cd "$here/../../../.." && pwd)/src
job="sleep 10; seq 1 20000; cat $src/*.cpp; exec sleep 600"
for r in $(seq 1 $rep); do
  bash $here/measure.sh amber-gl-r$r  "$out" -- "$bin" --stats-every 5 --profile amber -e bash -c "$job"
  bash $here/measure.sh plain-r$r     "$out" -- "$bin" --stats-every 5 --profile plain -e bash -c "$job"
done
bash $here/measure.sh amber-cpu   "$out" -- "$bin" --stats-every 5 --profile amber --renderer cpu -e bash -c "$job"
bash $here/measure.sh amber-gl-sb0 "$out" -- "$bin" --stats-every 5 --profile amber --scrollback 0 -e bash -c "$job"
bash $here/measure.sh plain-sb0   "$out" -- "$bin" --stats-every 5 --profile plain --scrollback 0 -e bash -c "$job"
bash $here/measure.sh amber-gl-noeffects "$out" -- "$bin" --stats-every 5 --profile amber --no-effects -e bash -c "$job"
