. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
H=$(dirname "${BASH_SOURCE[0]}"); cp "$H/vimrc" "$S2/vimrc"; cp "$H/../../src/selection.cpp" "$S2/sample.cpp"
start_term --size 808x464 --profile ${PROF:-plain} -e bash --norc --noprofile
echo "WIN=$WIN TPID=$TPID" > $S2/cur.env
sleep 1
t "PS1='$ '; cd $S2; clear; echo main-screen-marker; printf '    alpha {\n        beta;\n    }\n'"; k Return; sleep 1
xdotool mousemove --window $WIN 5 32 mousedown 1; sleep 0.2; xdotool mousemove --window $WIN 40 50; sleep 0.2; xdotool mousemove --window $WIN 75 70; sleep 0.2; xdotool mouseup 1; sleep 0.4
t "vim.basic -u vimrc sample.cpp"; k Return; sleep 2
xdotool mousemove --window $WIN 95 279 mousedown 1; sleep 0.2; xdotool mousemove --window $WIN 200 290; sleep 0.2; xdotool mousemove --window $WIN 305 298; sleep 0.3; xdotool mouseup 1; sleep 0.6
shot $OUT-vim-visual.png; k Escape
xdotool windowsize $WIN 1008 600; sleep 1.5
k G o; sleep 0.3; k shift+Insert; sleep 0.8; k Escape; sleep 0.3
t ":echo &columns &lines"; k Return; sleep 0.6
shot $OUT-vim-resized-paste.png
t ":q!"; k Return; sleep 1
shot $OUT-vim-after-quit.png
stop_term
