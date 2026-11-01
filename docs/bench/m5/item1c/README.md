# M5 item 1c: GL probe CPU cost (REAL, host box, llvmpipe on Xvfb :44, NOT a GPU)

Tool: `build/lterm-m5-probe-bench [auto|cpu] [seconds] [focused|unfocused]` (tools/m5_probe_bench.cpp).
Amber, 1024x768, a shell-like child (`printf '$ '; sleep`), probe delay pinned to 2 s, a
3900 ms per-frame cost *injected* (`setRenderCostForTest`, like GL in the TCG VM) so every probe
aborts after one frame. It reports process CPU for the whole run and per probe cycle (probe start
to 400 ms after re-demotion, net of the idle rate between probes). The injected cost is not
spent CPU; the numbers are what a probe really costs on this box.

Files:
- `before-*.txt`: 3bf5d97 (M4 src), `after-*.txt`: + item 1c. One 30 s run each.
- `ab-gl-focused.txt`: 3 interleaved rounds old/new, Mesa shader cache on and off
  (`MESA_SHADER_CACHE_DISABLE=true`, i.e. every program really compiled, as on a first boot).
- `view-test-on-M4-src.txt`: the new view_test checks run against the M4 source (they fail:
  28 programs compiled during demotion + 2 probes; probes run while unfocused/hidden).

Caveat: with the shader cache off, the old code's probe start and re-demotion happen inside one
main-loop pump of the bench, so it counts "0 probes"; the total process CPU (25.5-27.0 % of a
core vs 9.8-10.2 % new) is the number to compare there.
