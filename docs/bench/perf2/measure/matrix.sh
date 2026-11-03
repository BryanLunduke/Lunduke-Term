#!/bin/bash
# perf step 2: flood throughput BEFORE (master 21eff10, Debian libvterm.so.0) vs AFTER
# (perf-step2, bundled patched libvterm), plus MID (perf-step2 code linked with the bundled
# *stock* libvterm, to separate patch 0002 from bundling/compiler flags). Same tool as M5 N1:
# tools/m5_throughput_bench.cpp. Own Xvfb :82 + xfwm4, interleaved, 3 runs each.
export LD_LIBRARY_PATH=/workspace/lterm-tools/root/usr/lib/x86_64-linux-gnu
W=/workspace/perf2-work; M=$W/measure; D=:82
BIN_before=$W/bin/before/lterm-m5-throughput-bench
BIN_mid=$W/bin/mid/lterm-m5-throughput-bench
BIN_after=$W/bin/after/lterm-m5-throughput-bench
FILE=$W/flood8.txt
[ -e /tmp/.X11-unix/X82 ] && { echo "display :82 exists, not mine; stop"; exit 1; }
Xvfb $D -screen 0 1280x1024x24 +extension GLX +render -noreset > $M/xvfb.log 2>&1 &
XPID=$!
sleep 2
DISPLAY=$D xfwm4 --compositor=off > $M/xfwm4.log 2>&1 &
WPID=$!
sleep 2
echo "own Xvfb $D pid $XPID, xfwm4 pid $WPID; $(date '+%F %T %Z'); nproc $(nproc)"
alive() { local x=dead w=dead; kill -0 $XPID 2>/dev/null && xdpyinfo -display $D >/dev/null 2>&1 && x=alive
          kill -0 $WPID 2>/dev/null && w=alive; echo "Xvfb $x, xfwm4 $w"; }
for b in before mid after; do eval "f=\$BIN_$b"; echo "$b: $f sha256 $(sha256sum < $f | cut -c1-16) libvterm: $(LD_LIBRARY_PATH=$LD_LIBRARY_PATH ldd $f | grep -o 'libvterm[^ ]* => [^ ]*' || echo static)"; done
one() {  # build workload profile rep
  local b=$1 w=$2 p=$3 r=$4 f; eval "f=\$BIN_$b"
  local arg=2000000; [ $w = cat ] && arg=$FILE
  echo "== $(date +%T) $b $w $p rep $r | load $(cut -d' ' -f1-3 /proc/loadavg) | before: $(alive)"
  DISPLAY=$D LIBGL_ALWAYS_SOFTWARE=1 setsid timeout 180 $f $w $p $arg &
  local pid=$!; wait $pid; local rc=$?
  pkill -g $pid 2>/dev/null   # the bench's leftover `sleep 600` (its own process group only)
  echo "exit $rc | after: $(alive)"
}
for r in 1 2 3; do
  for w in seq cat; do
    for p in amber plain; do
      case $r in 1) o="before mid after" ;; 2) o="after before mid" ;; 3) o="mid after before" ;; esac
      for b in $o; do one $b $w $p $r; done
    done
  done
done
echo "== $(date '+%F %T %Z') done | $(alive) | load $(cut -d' ' -f1-3 /proc/loadavg)"
kill $WPID; sleep 1; kill $XPID; wait 2>/dev/null
echo "stopped own xfwm4 $WPID and Xvfb $XPID"
