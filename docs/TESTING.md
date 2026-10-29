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
