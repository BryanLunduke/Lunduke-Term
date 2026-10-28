# SPDX-License-Identifier: GPL-3.0-or-later
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
start_term --size 808x464 --profile ${PROF:-plain} -e bash --norc --noprofile
echo "WIN=$WIN TPID=$TPID" > $S2/cur.env
sleep 1
t "PS1='$ '; export HOME=$S2/hhome; mkdir -p \$HOME; clear; mc -u /workspace/lunduke-term-m2 /workspace/lunduke-term-m2/src"; k Return; sleep 3
shot $OUT-mc.png
# click a file in the right panel (row 6), then the left panel row 8
xdotool mousemove --window $WIN 560 118 click 1; sleep 0.8
xdotool mousemove --window $WIN 100 156 click 1; sleep 0.8; shot $OUT-mc-click.png
# wheel in right panel
xdotool mousemove --window $WIN 560 200 click 5 click 5; sleep 0.8; shot $OUT-mc-wheel.png
# open the "Left" menu by clicking the menubar
xdotool mousemove --window $WIN 30 13 click 1; sleep 1; shot $OUT-mc-menu.png; k Escape; sleep 0.3; k Escape; sleep 0.6
xdotool windowsize $WIN 1008 600; sleep 2; shot $OUT-mc-resized.png
# click F10 Quit in the button bar (last row), confirm
eval $(xdotool getwindowgeometry --shell $WIN)
xdotool mousemove --window $WIN $((WIDTH-40)) $((HEIGHT-12)) click 1; sleep 1; shot $OUT-mc-quit.png
k Return; sleep 1; shot $OUT-mc-after.png
stop_term
