# Testing Lunduke Terminal

## Unit and integration tests (meson)

```sh
meson setup build && meson compile -C build
xvfb-run -a -s "-screen 0 1280x1024x24" meson test -C build --print-errorlogs
```

| Test | What it covers | Needs X |
|---|---|---|
| `profiles` | built-in profiles, settings keys | no |
| `painter` | glyph cache, cell painting, changed-row repaint | no |
| `fonts` | font resolution with private fontconfig configs (DejaVu only, no monospace) | no |
| `effects` | barrel warp, CPU effect passes, quality levels | no |
| `vterm` | libvterm session: SGR, UTF-8/wide, resize, title, palette, DECSC/DECRC charsets, mode 47, DECSCNM, resize pen | no |
| `session` | scrollback, selection, pointToCell, mouse reports, paste, cursor, environment, pty, DECREQTPARM/DA | no |
| `fixes` | regression tests for earlier review findings | no |
| `gl-match` | GLSL pipeline vs CPU fallback, PSNR ≥ 40 dB per quality level (EGL + llvmpipe, `LIBGL_ALWAYS_SOFTWARE=1`) | no (EGL surfaceless) |
| `view` | GTK `TerminalView` lifetime, built with AddressSanitizer; skips without `$DISPLAY` | yes |
| `review` | M5 code-review fixes: output flood + Ctrl+C, input method (gtk-im-context-simple), hidden-tab memory and painting, cursor blink timeout, confirm close, scrollback maximum/lazy trim, selection compare, `--stats`, default profile; not parallel; skips without `$DISPLAY` | yes |
| `scrollback-default` | runs `lunduke-terminal`: default scrollback is 5000 lines; a `scrollback-lines` saved in settings.ini (10000, 7000) is used as is and the file is not rewritten; not parallel; skips without `$DISPLAY` | yes |
| `libvterm-pristine` | `third_party/libvterm` is byte-identical to the 0.3.3 tarball (`libvterm-SHA256SUMS`) | no |
| `libvterm-rep` | patch 0001: REP of a wide glyph at the right margin (raw libvterm, a session, the fuzz reproducer `tests/data/fuzz/crash-46ca723d-libvterm-rep.bin`) | no |
| `libvterm-t-patched`, `libvterm-t-stock` | upstream's own `t/` suite (`make test`) on the patched and on the stock library | no |
| `libvterm-diff` | randomised escape-sequence streams through stock and patched libvterm (`tests/libvterm_diff.c`): every callback and full screen/scrollback dumps must be byte-identical; 100 seeds | no |
| `libvterm-diff-repfix` | the same with stock+0001 vs patched, including REP of wide glyphs and 1-column screens; 100 seeds | no |
| `libvterm-feed-diff` | the repo's sources and `seq 1 50000` through stock and patched at 4 window sizes, logs compared (`tools/libvterm_feed_diff.sh`) | no |

Sanitizer build (what the CI `sanitizers` job runs):

```sh
meson setup build-san --buildtype=debug -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-san
xvfb-run -a meson test -C build-san --print-errorlogs --timeout-multiplier 4
```

meson sets `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS` and
`LSAN_OPTIONS=suppressions=tests/lsan.supp` for every test (M4). Leak checking is on; the
suppressions cover only third-party caches that live until exit (fontconfig, Mesa GLX/llvmpipe,
one GDK X11 list). view_test also uses `fast_unwind_on_malloc=0`, so GDK frames are visible.

The libvterm tests exist only with the bundled libvterm (the default); with `-Dlibvterm=system`
only `libvterm-pristine` runs. Longer differential runs (thousands of seeds, 20 000-step
streams, `seq 1 2000000` and an 8 MiB file):

```sh
python3 tools/libvterm_diff_run.py build/libvterm_diff_stock build/libvterm_diff_patched a 1 3000 2000
python3 tools/libvterm_diff_run.py build/libvterm_diff_repfix build/libvterm_diff_patched b 1 3000 2000
tools/libvterm_feed_diff.sh build/libvterm_diff_stock build/libvterm_diff_patched . 2000000 FILE...
```

## CI

`docs/ci/ci.yml` (to be copied to `.github/workflows/ci.yml`; the box's GitHub token has no `workflow` scope, so it could not be pushed there) runs two jobs in a `debian:trixie` container (the LCOS base):
`build-test` (release build + `xvfb-run -a meson test`) and `sanitizers` (ASan + UBSan,
as above). The test log is uploaded when a job fails.

## Application checks (manual, scripted with xdotool)

`tools/app-check/` drives the real terminal on an X server of your own (default `:43`; set
`DISPLAY`) with XTEST key and mouse events, and takes screenshots with ImageMagick `import`:

- `lib.sh`: `start_term`, `shot`, `k` (keys), `t` (type), `stop_term`, `vt_menu`.
- `vimrun.sh`, `htoprun.sh`, `mcrun.sh`: `OUT=/tmp/x bash tools/app-check/vimrun.sh`.

Run a window manager (xfwm4 or similar) on that display so `windowactivate` and `windowsize`
work. `--size 808x464 --profile plain` gives exactly 80×24 cells with the default font.
vttest is interactive; the menus checked and the results are in `docs/M2-NOTES.md`.

The programs do not have to be installed: unpack the Debian packages into a scratch folder
(`dpkg-deb -x`) and set `PATH`, `LD_LIBRARY_PATH`, `VIMRUNTIME`, `MC_DATADIR`.

## Benchmarks

`tools/m2-shot.sh` (screenshots per quality level), `lterm-m1-bench` (per-pass timings) and the
idle-CPU procedure are described in `docs/M2-NOTES.md`.
