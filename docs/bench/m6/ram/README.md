# M6 item 6: RAM measurements (raw)

Box: Debian 13 VM, Xvfb 1280x800 (:46 for Lunduke Terminal, :44 for cool-retro-term), **no GPU**:
every GL number is Mesa llvmpipe (LLVM 19), which keeps textures, framebuffers and JIT-compiled
shaders in the process, and maps libLLVM. All numbers REAL unless marked DERIVED. kB from
`/proc/PID/smaps_rollup` (Rss, Pss, Private_Dirty) and `/proc/PID/smaps` grouped by mapping
(`smaps-group.sh`). The terminal process only (not its shell). Lunduke Terminal settings: empty
`XDG_CONFIG_HOME` (defaults; window 1280x800, scrollback 10000).

- `measure.sh LABEL OUTDIR -- CMD`: snapshot "fresh" at t=8 s (no output yet), "after" at t=30 s
  (from t=10 s the shell prints `seq 1 20000` and `cat src/*.cpp`, ~25k lines).
- `run-all.sh`: the matrix (amber GL x3, plain x3, amber C++ path, scrollback 0, amber --no-effects).
- `gtk_baseline.cpp`: an empty gtkmm window (DrawingArea, or a GtkGLArea drawing one frame).
- `raw/`: `*-rollup.txt`, `*-groups.txt`, `*-stdout.txt` (`--stats-every 5` lines: malloc stats,
  scrollback, glyph cache, our GL texture bytes, Cairo surfaces); full smaps in `raw-smaps-*.tar.xz`.
- cool-retro-term 1.2.0+ds2-4 (Debian 13) and its Qt5/QML deps were unpacked with `dpkg-deb -x`
  into `/workspace/lterm-tools/crt/root` (no system install), run with LD_LIBRARY_PATH,
  QT_PLUGIN_PATH, QML2_IMPORT_PATH and an empty config; default profile, window 1024x768.
- `run-diag.sh`: one run each to attribute the GL path: window size (640x400, 1024x768),
  `LP_NUM_THREADS=1`, `GALLIUM_DRIVER=softpipe`, `--effects-quality static`, plain at 1024x768,
  and the GtkGLArea baseline at 1280x800. Outputs in `raw-diag/`, smaps in `raw-smaps-diag.tar.xz`.
