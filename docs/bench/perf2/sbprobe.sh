#!/bin/bash
# own Xvfb :83 + xfwm4. sb-lines trajectory (every 1 s) for scrollback-default's command, stock vs patched,
# without and with `sleep 1` before seq (window settled before the output).
export PATH=/workspace/lterm-tools/root/usr/bin:$PATH LD_LIBRARY_PATH=/workspace/lterm-tools/root/usr/lib/x86_64-linux-gnu
[ -e /tmp/.X11-unix/X83 ] && { echo ":83 exists, abort"; exit 1; }
Xvfb :83 -screen 0 1280x1024x24 -nolisten tcp >/dev/null 2>&1 & X=$!
sleep 2; DISPLAY=:83 xfwm4 --compositor=off >/dev/null 2>&1 & W=$!
sleep 3
T=$(mktemp -d /tmp/lterm-sbprobe.XXXXXX)
for i in 1 2 3; do
 for pre in "" "sleep 1; "; do
  for v in stock patched; do
   case $v in stock) b=/workspace/perf2-work/bin/variants/lterm-stock;; patched) b=/workspace/lunduke-term-perf2/build/lunduke-terminal;; esac
   traj=$(DISPLAY=:83 XDG_CONFIG_HOME=$T/c$i$v$RANDOM timeout 60 $b --profile plain --renderer cpu --stats-every 1 --on-exit close \
     -e sh -c "${pre}seq 1 12000; sleep 2" 2>&1 | sed -n 's/^stats-mem t=\([0-9]*\) .* sb-lines=\([0-9]*\) .*/\1:\2/p' | tr '\n' ' ')
   echo "$i | pre='${pre}' | $v | t:sb-lines $traj| load $(cut -d' ' -f1 /proc/loadavg) | wm $(kill -0 $W 2>/dev/null && echo alive || echo DEAD)"
  done
 done
done
rm -rf "$T"; kill $W; sleep 1; kill $X; echo "stopped own xfwm4 $W Xvfb $X"
