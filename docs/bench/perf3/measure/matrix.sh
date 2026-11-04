#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Perf step 3 before/after: own Xvfb :93 + xfwm4, 1280x800, amber GL and amber CPU,
# 3 interleaved rounds (before/after on odd rounds, after/before on even rounds).
# BEFORE = master 6283674 release build, AFTER = perf-step3 release build (copied binaries).
# The WM (and Xvfb) must be alive before and after every run; otherwise the run is discarded
# (moved to raw/discarded/), the WM is restarted, and the run is redone (up to 2 more tries).
W=/workspace/perf3-work; M=$W/measure; D=:93; Q=170000
B=$W/bin-master/lunduke-terminal; A=$W/bin-after/lunduke-terminal
[ -e /tmp/.X11-unix/X93 ] && { echo "display :93 exists, not mine; stop"; exit 1; }
Xvfb $D -screen 0 1920x1200x24 +extension GLX +render -noreset > $M/xvfb.log 2>&1 &
XPID=$!; sleep 2
startwm() { DISPLAY=$D xfwm4 --compositor=off >> $M/xfwm4.log 2>&1 & WPID=$!; sleep 2; }
startwm
echo "own Xvfb $D pid $XPID, xfwm4 pid $WPID; $(date '+%F %T %Z'); nproc $(nproc)"
echo "before $B sha256 $(sha256sum < $B | cut -c1-16); after $A sha256 $(sha256sum < $A | cut -c1-16)"
alive() { kill -0 $XPID 2>/dev/null && xdpyinfo -display $D >/dev/null 2>&1 && kill -0 $WPID 2>/dev/null && pgrep -x xfwm4 | grep -qx $WPID; }
one() {  # tag bin args...
  local tag=$1 bin=$2; shift 2
  for try in 1 2 3; do
    local pre=dead post=dead
    alive && pre=alive
    if [ $pre != alive ]; then echo "$(date +%T) WM/X dead before $tag; restarting xfwm4"; startwm; alive && pre=alive; fi
    echo "$(date +%T) start $tag try $try load=$(cut -d' ' -f1-3 /proc/loadavg) wm=$pre"
    $M/measure.sh $tag $D $Q -- $bin "$@"
    alive && post=alive
    echo "$(date +%T) end $tag wm=$post"
    if [ $pre = alive ] && [ $post = alive ]; then echo "wm_alive_before_after yes (xfwm4 pid $WPID)" >> $M/raw/$tag-summary.txt; return; fi
    mkdir -p $M/raw/discarded; for f in $M/raw/$tag-*; do mv $f $M/raw/discarded/$(basename $f).try$try; done
    echo "$(date +%T) DISCARDED $tag try $try (wm before=$pre after=$post)"
  done
}
for r in 1 2 3; do
  for prof in amber-gl-1280 amber-cpu-1280; do
    case $prof in
      amber-gl-1280) args=(--profile amber --size 1280x800 --stats) ;;
      amber-cpu-1280) args=(--profile amber --renderer cpu --size 1280x800 --stats) ;;
    esac
    if [ $((r % 2)) = 1 ]; then order="before after"; else order="after before"; fi
    for w in $order; do
      if [ $w = before ]; then bin=$B; else bin=$A; fi
      one p3-$w-$prof-r$r $bin "${args[@]}"
    done
  done
done
echo "MATRIX_DONE $(date '+%F %T %Z')"
kill $WPID; sleep 1; kill $XPID; wait 2>/dev/null
echo "stopped own xfwm4 $WPID and Xvfb $XPID"
