#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 item 6: diagnostic runs to attribute the GL path's memory (one run each).
out=$1; bin=$2; base=$3; here=$(dirname "$0"); mkdir -p "$out"
src=$(cd "$here/../../../.." && pwd)/src
job="sleep 10; seq 1 20000; cat $src/*.cpp; exec sleep 600"
m() { bash $here/measure.sh "$@"; }
m gtk-glarea-1280 "$out" -- "$base" glarea-1280
LP_NUM_THREADS=1 m amber-gl-lpthreads1 "$out" -- "$bin" --stats-every 5 --profile amber -e bash -c "$job"
GALLIUM_DRIVER=softpipe m amber-gl-softpipe "$out" -- "$bin" --stats-every 5 --profile amber -e bash -c "$job"
m amber-gl-640x400 "$out" -- "$bin" --stats-every 5 --profile amber --size 640x400 -e bash -c "$job"
m amber-gl-1024x768 "$out" -- "$bin" --stats-every 5 --profile amber --size 1024x768 -e bash -c "$job"
m plain-1024x768 "$out" -- "$bin" --stats-every 5 --profile plain --size 1024x768 -e bash -c "$job"
m amber-gl-static "$out" -- "$bin" --stats-every 5 --profile amber --effects-quality static -e bash -c "$job"
