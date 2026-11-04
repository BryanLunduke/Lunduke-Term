#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# perf3 copy of tools/m2-shot.sh: BIN DISPLAY OUT.png ARGS...  (fixed text, static quality, own display)
bin=$1; disp=$2; out=$3; shift 3
export DISPLAY=$disp LIBGL_ALWAYS_SOFTWARE=1
h=$(mktemp -d /tmp/perf3-shot-XXXX)
XDG_CONFIG_HOME=$h $bin "$@" --size 1280x800 --quit-after 15000 -e /bin/bash --norc -c 'PS1="lcos\$ "; printf "Lunduke Terminal perf3 screenshot\n\n"; printf "%s\n" alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu; seq 1 12 | paste -sd" "; echo; printf "\033[1mbold\033[0m  normal  \033[4munderline\033[0m  \033[31mred\033[32mgreen\033[34mblue\033[0m\n"; exec bash --norc -i' >/dev/null 2>&1 &
p=$!
sleep 3; xdotool search --sync --pid $p >/dev/null; sleep 1
best=0; w=
for c in $(xdotool search --pid $p); do eval $(xdotool getwindowgeometry --shell $c); if [ $((WIDTH*HEIGHT)) -gt $best ]; then best=$((WIDTH*HEIGHT)); w=$c; fi; done
xdotool windowactivate $w; sleep 5
eval $(xdotool getwindowgeometry --shell $w)
python3 -c "from PIL import ImageGrab; ImageGrab.grab(bbox=($X,$Y,$X+$WIDTH,$Y+$HEIGHT), xdisplay=\"$disp\").save(\"$out\")"
wait $p; rm -rf $h
