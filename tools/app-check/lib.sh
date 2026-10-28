# helpers: start TERM_CMD in lunduke-terminal on :43, find its window
export DISPLAY=${DISPLAY:-:43} LIBGL_ALWAYS_SOFTWARE=1
[ -f "${LTERM_TOOLS_ENV:-/workspace/lterm-tools/env.sh}" ] && . "${LTERM_TOOLS_ENV:-/workspace/lterm-tools/env.sh}"
S2=${S2:-/tmp/s2}; mkdir -p "$S2"
LT=${LT:-$(dirname "${BASH_SOURCE[0]}")/../../build/lunduke-terminal}
start_term() { # args: lunduke-terminal args...
  $LT "$@" >$S2/term.out 2>$S2/term.err &
  TPID=$!; echo $TPID >> $S2/term.pids
  sleep 2; xdotool search --sync --pid $TPID >/dev/null
  sleep 0.5; best=0; WIN=
  for c in $(xdotool search --pid $TPID); do
    eval $(xdotool getwindowgeometry --shell $c)
    if [ $((WIDTH*HEIGHT)) -gt $best ]; then best=$((WIDTH*HEIGHT)); WIN=$c; fi
  done
  xdotool windowactivate --sync $WIN 2>/dev/null; sleep 0.5
}
shot() { import -window $WIN "$1"; }
k() { xdotool key --delay 60 "$@"; }
t() { xdotool type --delay 40 "$@"; }
stop_term() { kill $TPID 2>/dev/null; wait $TPID 2>/dev/null; }
# vttest: run menu item $1, take $2 screenshots pressing Return between them
vt_menu() {
  local item=$1 n=$2 pre=$3 i
  t "$item"; k Return; sleep 1.2
  for i in $(seq 1 $n); do shot $pre-$(printf %02d $i).png; k Return; sleep 1.2; done
}
