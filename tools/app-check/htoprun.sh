. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
start_term --size 808x464 --profile ${PROF:-plain} -e bash --norc --noprofile
echo "WIN=$WIN TPID=$TPID" > $S2/cur.env
sleep 1
t "PS1='$ '; export HOME=$S2/hhome; mkdir -p \$HOME; clear; htop -d 50"; k Return; sleep 3
shot $OUT-htop.png
# click the CPU% column header (sort), then a process row, then F2 setup via mouse on the function bar
xdotool mousemove --window $WIN 500 400 click 1; sleep 1; shot $OUT-htop-click-row.png
xdotool click 5 click 5; sleep 1; shot $OUT-htop-wheel.png
xdotool windowsize $WIN 1008 600; sleep 2; shot $OUT-htop-resized.png
eval $(xdotool getwindowgeometry --shell $WIN)
# function bar F10 Quit is the last row; click "Quit"
k F1; sleep 1; shot $OUT-htop-help.png; k Escape; sleep 0.5
k q; sleep 1; shot $OUT-htop-after.png
stop_term
