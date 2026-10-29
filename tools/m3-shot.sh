#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M3 item 1 screenshots. usage: m3-shot.sh BINARY search|tabclose|settings OUT.png [CLICK_X CLICK_Y]
# Runs on its own display (default :45). settings: XDG_CONFIG_HOME points at a temporary dir
# with a corrupt settings.ini; CLICK_X/Y (window-relative) is the dialog's "Save as default".
T=$1; MODE=$2; OUT=$3
export DISPLAY=${DISPLAY:-:45} LIBGL_ALWAYS_SOFTWARE=1 SHELL=/bin/bash
CFG=$(mktemp -d); export XDG_CONFIG_HOME=$CFG
mkdir -p $CFG/lunduke-terminal
[ "$MODE" = settings ] && printf '[terminal\nprofile=amber\n\377\376 garbage\n' > $CFG/lunduke-terminal/settings.ini
case $MODE in
search) CMD='for i in $(seq 1 3000); do echo "[$i] cc -O2 -c module_$((i%97)).c ... ok"; [ $((i%250)) = 0 ] && echo "[$i] warning: needle in module_$((i%97)).c"; done; export PS1="lcos\$ "; exec bash --norc -i' ;;
*) CMD='export PS1="lcos\$ "; echo "Lunduke Terminal M3"; exec bash --norc -i' ;;
esac
$T --size 1024x600 --profile plain --quit-after 25000 -e /bin/bash --norc -c "$CMD" >/dev/null 2>&1 &
p=$!; echo $p >> /tmp/m3.pids
sleep 3; xdotool search --sync --pid $p >/dev/null; sleep 1
big() { best=0; w=; for c in $(xdotool search --onlyvisible --pid $p); do eval $(xdotool getwindowgeometry --shell $c); if [ $((WIDTH*HEIGHT)) -gt $best ]; then best=$((WIDTH*HEIGHT)); w=$c; fi; done; echo $w; }
w=$(big); xdotool windowactivate $w; sleep 1
case $MODE in
search)
  xdotool key ctrl+shift+f; sleep 0.5
  xdotool type --delay 120 "needle"; sleep 1.5
  xdotool key Return; sleep 0.3; xdotool key Return; sleep 0.3; xdotool key Return; sleep 1 ;;
tabclose)
  xdotool type --delay 30 "echo tab one"; xdotool key Return
  xdotool key ctrl+shift+t; sleep 1; xdotool type --delay 30 "echo tab two"; xdotool key Return
  xdotool key ctrl+shift+t; sleep 1; xdotool type --delay 30 "echo tab three, closing"; xdotool key Return; sleep 0.5
  xdotool key ctrl+shift+w; sleep 1.5 ;;
settings)
  xdotool key ctrl+shift+s; sleep 1.5
  w=$(xdotool getactivewindow)
  if [ -n "$4" ]; then xdotool mousemove --window $w $4 $5 click 1; sleep 1; fi
  SHOT=$w ;;
esac
import -window "${SHOT:-$(xdotool getactivewindow)}" "$OUT"
kill $p 2>/dev/null; wait $p 2>/dev/null
rm -rf $CFG
