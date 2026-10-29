#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# usage: m2-shot.sh QUALITY OUT.png
export DISPLAY=:43 LIBGL_ALWAYS_SOFTWARE=1
./build/lunduke-terminal ${EXTRA:-} --size 1280x800 --profile amber --effects-quality "$1" --quit-after 15000 -e /bin/bash --norc -c 'PS1="lcos\$ "; printf "Lunduke Terminal M2 - effects-quality %s\n\n" '"$1"'; ls -la /usr/share | head -20; echo; printf "\033[1mbold\033[0m  normal  \033[4munderline\033[0m\n"; exec bash --norc -i' >/dev/null 2>&1 &
p=$!
sleep 3
xdotool search --sync --pid $p >/dev/null
sleep 1
best=0; w=
for c in $(xdotool search --pid $p); do
  eval $(xdotool getwindowgeometry --shell $c)
  if [ $((WIDTH*HEIGHT)) -gt $best ]; then best=$((WIDTH*HEIGHT)); w=$c; fi
done
xdotool windowactivate $w; sleep 5; [ -n "${KEY:-}" ] && { xdotool type --window $w "x"; sleep 2; }
eval $(xdotool getwindowgeometry --shell $w)
import -window root -crop ${WIDTH}x${HEIGHT}+${X}+${Y} +repage "$2"
wait $p
