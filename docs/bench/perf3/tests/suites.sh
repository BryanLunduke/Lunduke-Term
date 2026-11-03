#!/bin/bash
# perf3 copy of perf1-work/suites.sh: own Xvfb :94 (no WM needed by the tests), 27 tests with appstreamcli.
cd /workspace/lunduke-term-perf3
[ -e /tmp/.X11-unix/X94 ] && { echo "display :94 exists, not mine"; exit 1; }
Xvfb :94 -screen 0 1920x1200x24 +extension GLX +render -noreset > /workspace/perf3-work/xvfb94.log 2>&1 & XP=$!; sleep 2
export PATH=/workspace/lterm-tools/root/usr/bin:$PATH LD_LIBRARY_PATH=/workspace/lterm-tools/root/usr/lib/x86_64-linux-gnu LIBGL_ALWAYS_SOFTWARE=1 DISPLAY=:94
echo "appstreamcli: $(command -v appstreamcli)"
o=/workspace/perf3-work/suites; mkdir -p $o
ninja -C build -j2 >/dev/null && ninja -C build-san -j2 > $o/build-san.log 2>&1 || { echo BUILD_FAIL; kill $XP; exit 1; }
for cfg in "build 1 release-serial" "build 2 release-parallel" "build-san 1 asan-serial" "build-san 2 asan-parallel"; do
  set -- $cfg
  s=$(date +%s); meson test -C $1 --no-rebuild --num-processes $2 --timeout-multiplier 4 > $o/$3.txt 2>&1; echo "$3 rc=$? $(( $(date +%s)-s ))s"
  cp $1/meson-logs/testlog.txt $o/$3-testlog.txt
done
kill $XP; echo ALL_DONE
