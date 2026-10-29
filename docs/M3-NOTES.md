# M3 notes (branch `m3-core`)

Item 1: the review bugs from M2, each with a test. Numbers come from this host (release build,
Xvfb + xfwm4, `LIBGL_ALWAYS_SOFTWARE=1` llvmpipe) unless they are marked ASan. Raw output is in
`docs/bench/m3-item1/`.

## (a) Scrollback search off the keystroke path

M2 ran `findAll` over the whole scrollback on every keystroke. M3 keeps a folded-text cache
(`SearchIndex`, `src/search.*`). It is built in idle slices of 4 ms and updated incrementally when
new lines arrive. A query typed before the cache is complete is *pending*: the count is 0 and the
search finishes from idle, so typing never blocks. Matches are capped at 10000, newest first, and
the bar then shows `N of 10000+`. Re-runs on new output are debounced. Behaviour is otherwise unchanged:
wrap-joined, case-folded, current match in orange, Enter/Shift+Enter, Escape.

`build/lterm-m3-bench --search` (`docs/bench/m3-item1/search.txt`), typing "needle" one key at a time:

| Scrollback | M2 worst keystroke | M3 worst keystroke | M3 cache build |
|---|---|---|---|
| 10k lines | 21.58 ms (127.6 ms total) | 0.61 ms (1.6 ms total) | 4 slices, 14.1 ms, worst slice 4.08 ms, 1.2 MB |
| 100k lines | 212.76 ms (1268.0 ms total) | 5.21 ms (14.4 ms total) | 37 slices, 149.3 ms, worst slice 4.21 ms, 9.3 MB |

Known limitation: while a reflow rebuilds the cache, the old highlights stay on screen until the
refresh lands.

## (b) Effects governor: render cost only, recovers upward

M2 measured whole-process CPU, so heavy output (parsing and the PTY) counted as "effects cost" and
could demote `auto` to static for good. M3 measures only the paint cycle when the view really drew:
process CPU from the frame clock's before-paint to after-paint, minus the source upload. This lives
in `src/effects_governor.*`, which is pure logic with its own test.

- Load is the frame cost divided by the tick interval, with hysteresis. The tick slows when load is
  above the budget (0.25) and speeds up (×0.85) when load is under 0.6 × budget.
- It demotes to static after 2 over-budget windows at the 1 fps floor.
- It probes again after 1.5 s in tests and 10 s by default, doubling the delay on failure up to
  160 s. A passing probe (3 windows) returns to normal.
- Messages: `animation needs N% of a core at 1 fps …; effects-quality auto -> static, retry in N s`
  and `effects-quality auto: probing the animation again`.

`docs/bench/m3-item1/governor.txt`, 10 s CPU at idle (1280x800, 100 % = one core):

| | plain | amber GL auto | amber CPU auto |
|---|---|---|---|
| before (d402d31) | 0.0 % | 19.4 % | 19.9 % |
| after | 0.0 % | 19.4 % | 19.2 % |

Heavy output (about 30 s of `seq`) followed by idle, two runs per build: neither build printed a
governor message this time. Idle afterwards was 16.9 % / 18.1 % before and 17.3 % / 19.4 % after.
The M2 demotion under output is real but flaky. One earlier base run (15M lines) printed
`animation needs 100% … auto -> static` and stayed static. The deterministic check is in the tests:
`governor_test`, and view_test `testGovernor`, which uses an injected 400 ms frame cost. With it,
auto demotes at about 7 s and is back to reduced at a 100 ms tick after the probe, for both the GL
and the CPU renderer.

## (c) Tab close does not block

M2's `~TerminalView` waited up to 50 ms for the child. M3 sends SIGHUP and returns. A static reaper
then does two things:
- A child watch reaps the child, so there are no zombies.
- A 2 s timer sends SIGKILL to the process group and the pid if the child is still unreaped. It
  checks first with `waitid(WNOWAIT)`, so a reused pid is never signalled.

`build/lterm-m3-close-bench` (`docs/bench/m3-item1/close.txt`), 19 closes:

| child | before median / total | after median / total |
|---|---|---|
| `sh` | 5.50 ms / 105.66 ms | 1.71 ms / 45.59 ms (another run: 0.35 ms / 7.25 ms) |
| `bash -i` | 5.62 ms / 107.82 ms | 0.40 ms / 8.04 ms |
| `trap '' HUP` | 51.77 ms / 984.62 ms, 19 left running | 0.38 ms / 7.74 ms, all gone 1997 ms later |

## (d) Deferred close by tab id

Tabs carry a `unsigned long` id. The close button and Ctrl+Shift+W queue `closeTabLater(id)`, and
the idle handler looks the id up again. A second click, or a close of a tab whose address was
reused, is a no-op. A close requested during a close is deferred. Pages map to tabs by widget, not
by notebook index, which also fixes a mix-up after tabs are reordered. view_test `testTabIds`
(ASan) covers a double click, a late close after reuse, and a nested close.

## (e) A corrupt settings.ini no longer blocks Save

If the file is not a valid key file, "Save as default" now does three things:
- warns,
- keeps the old text as `settings.ini.corrupt`,
- writes a fresh file atomically (`Glib::file_set_contents`).

The dialog shows `Warning: … was not valid (…); it was replaced, the old text is in ….corrupt.
Saved.` The status label wraps. view_test `testCorruptSettings` covers this.

## (f) Licensing

SPDX `GPL-3.0-or-later` headers were added to the build files, the scripts, the desktop file and
the Debian maintainer scripts. The commit message lists what was skipped and why:
- the licence text and `debian/copyright`,
- dpkg-parsed files,
- `docs/ci/ci.yml`, kept as is,
- prose and generated bench data.

The c64 row is gone from `docs/FONTS.md`.

# Item 2: measurements in a VM and on the host

Raw data is in `docs/bench/m3-item2/`. Labels: **REAL** means measured here and the raw output is
committed. **DERIVED** means computed from REAL data. **NOT MEASURED** means exactly that.

## No GPU on this box

REAL (`gpu-check.txt`): there is no `/dev/dri`, no `/sys/class/drm`, no `lspci`, and no PCI
display-class device. `glxinfo -B` reports llvmpipe (LLVM 19.1.7), Mesa 25.0.7, "Accelerated: no".
Every GL number below is llvmpipe software rendering. Nothing here was measured on a GPU, so these
notes make no GPU claims.

## qemu

REAL (`qemu-options.txt`): qemu 10.0.13.
- The only displays are `none` and `curses`.
- `egl-headless` and `sdl` are not available, and with no render node there is no virgl.
- `virtio-vga` in 2D mode starts.
- Both runs used `-accel tcg`. `/dev/kvm` exists but was not used.

The ISO `lcos-live-100-01.iso` was only booted, never modified. Its sha256 was
`33fc871b…4706e61` before and after both runs (`iso-sha256.txt`).

## VM run 1: LCOS 1.0.0, std VGA, TCG, 4 vCPU, 2 GB

Guest details (`vm-std-vga/env.txt`): kernel 7.2.6-lunduke, XLibre X server, bochs-drm. The build
was M3 at 69598c8, copied in and run as the live user. Script: `vm-std-vga/m3-vm.sh`.

REAL idle CPU (`vm-std-vga/idle.txt`). Measurement starts 40 s after launch. Process % is of one
vCPU; guest % is of all 4 vCPUs.

| Run | Process | Guest busy |
|---|---|---|
| No terminal (baseline, 20 s; it overlapped QMP typing on tty2) | n/a | 30.4 % |
| plain (default), 20 s | 0.0 % | 29.0 % |
| amber, GL, auto, 60 s | 72.2 % | 44.9 % |
| amber, CPU, auto, 60 s | 16.7 % | 14.0 % |

The guest baseline is noisy, which is why the plain run shows less guest load than the baseline.

Under TCG, the governor did what it should with both renderers.
- **amber GL:** each probe reported `animation needs 366–393% of a core at 1 fps (3656–3926 ms per
  frame)`.
- **amber CPU:** each probe reported 71–81 % (707–806 ms per frame).
- **Both:** auto dropped to static and backed off 10/20/40/80 s.

The GL probes are expensive: about 4 s of CPU each on llvmpipe under TCG. Most of the 72.2 % is
those probes, so on a machine like this the CPU renderer is the cheaper fallback (finding, no
change made).

REAL close bench in the guest (`vm-std-vga/close.txt`, 9 closes each):

| Child | Median | Max | Total | All children gone |
|---|---|---|---|---|
| `sh` | 6.63 ms | 126.55 ms | 183.49 ms | 1 ms after the last close |
| `bash -i` | 4.81 ms | 8.26 ms | 50.38 ms | 1 ms after |
| `trap '' HUP` | 5.17 ms | 10.54 ms | 56.01 ms | 2006 ms after (the 2 s SIGKILL) |

REAL interactive checks:
- **Tabs:** with 3 shells, one tab close left 2 shells (`children-before/after-close.txt`). There
  were no zombies (`zombies.txt` is empty).
- **Corrupt settings.ini:** "Save as default" replaced it and kept the old text as
  `settings.ini.corrupt`. The new file is `profile=plain`, `effects-quality=auto`,
  `scrollback-lines=10000` (`settings-files.txt`).

Screenshots, in `/workspace/lcos-app-reviews/terminal-m3/`:
- `m3-vm-idle-plain-default.png`
- `m3-vm-idle-amber-cpu-auto-warmup.png`
- `m3-vm-settings-open.png`
- `m3-vm-close-bench-tabs.png`

NOT CAPTURED: amber-GL idle, search, 3 tabs, tabs after close, settings saved. My screenshot
watcher held the single QMP socket and hung, so those moments were missed. The numbers above come
from the in-guest logs, not from screenshots.

## VM run 2: virtio-vga (2D)

REAL (`vm-virtio-vga/`): the guest booted to the desktop on `-vga none -device virtio-vga`. The
kernel showed `virtio_gpudrmfb`, `/dev/dri/card0` and `renderD128`. The guest's GL is still
software, because there is no virgl on the host.

Screenshots:
- `m3-vm-virtio-vga-desktop.png`
- `m3-vm-virtio-vga-tty-drm.png`

NOT MEASURED: the terminal runs in this VM.
- Writing the script into the image with `debugfs -w` produced inodes with bad metadata_csum, so
  the guest could not read them.
- QMP typing garbled the fallback command (a stuck shift).
- The retyped launch was rejected by the agent's safety review ("error occurred while
  classifying"), including on an approval retry.

The VM was shut down over QMP.

## Host profile matrix (llvmpipe)

REAL (`host/matrix-before-reflow-fix.txt`, `host/matrix.sh`):
- Xvfb :44 at 1024x640, `LIBGL_ALWAYS_SOFTWARE=1`, `effects-quality=auto`.
- 20 s warm-up, then 20 s of process CPU (% of one core).
- Build 69598c8, before the reflow fix below.

| Profile | GL | CPU renderer |
|---|---|---|
| plain | 0.0 % | 0.0 % |
| amber | 16.7 % | 19.2 % |
| green | 16.5 % | 21.2 % |
| green-scanlines | 18.2 % | 19.2 % |
| ibm-dos | 20.1 % | 16.4 % |
| ibm-3278 (static) | 0.0 % | 0.0 % |
| vintage | 20.6 % | 16.4 % |
| futuristic | 17.2 % | 22.4 % |
| paper-white | 17.6 % | 21.2 % |

All animated profiles stayed at `reduced`, with the tick between 120 and 214 ms. None demoted to
static on the host. The "badutf" column (21 and 9 for plain) led to the finding below.

## Soak: 600 s, amber GL, llvmpipe

REAL (`host/soak/soak-amber-gl.csv`, `host/soak.sh`): build f197952 (with the fix), Xvfb :44 with
no WM, heavy `seq` output the whole time.
- Each cycle opens 3 tabs (typing in them), closes 3, and resizes 700x400 → 1200x760 → 500x300 →
  1024x640.
- In total: 249 tab opens and 249 closes, plus 332 resizes. 250 backend lines in the log = the
  first window plus 249 tabs.

| t | RSS | fds | threads | children |
|---|---|---|---|---|
| 0 s | 221.7 MB | 12 | 23 | 1 |
| 29 s | 485.9 MB | 12 | 22 | 1 |
| 87 s | 489.8 MB | 12 | 22 | 1 |
| 290 s | 502.1 MB (max) | 12 | 22 | 1 |
| 602 s | 500.0 MB | 12 | 22 | 1 |

- Memory: RSS rises in the first half-minute (llvmpipe buffers, glyph cache, 10000-line
  scrollback under constant output), then levels off. From 87 s to the end it grew by 10.2 MB
  (DERIVED), and it did not grow over the last 300 s.
- fds stayed at 12.
- There were 0 zombies and 0 invalid-UTF-8 warnings.

NOT MEASURED: textures. `--stats` has no texture counter and llvmpipe has no per-process GPU memory
to read, so RSS, which includes llvmpipe's texture memory, is the only proxy.

## libvterm 0.3.3 reflow: out-of-bounds read, mitigated locally

The plain-profile warnings (`Invalid UTF-8 string passed to pango_layout_set_text()`) come from the
system libvterm 0.3.3, not from our code.

The bug: in `screen.c` `resize_buffer`, the reflow loop
`while (REFLOW && old_lineinfo && old_row >= 0 && old_lineinfo[old_row].continuation) old_row--;`
reaches −1 when row 0 is a continuation line. It then reads row −1, so garbage cells appear after
a resize. ASan cannot see this because the system library is not instrumented. It was confirmed
by reading the 0.3.3 source and with a small probe.

The mitigation, in f197952 (`VtermSession::clearTopContinuation()`): clear the continuation flag of
screen row 0 before `vterm_set_size`, and on entering the alternate screen. libvterm calls our
settermprop before it switches its lineinfo. The cost is that a line wrapped across the top edge
does not re-join on resize.

`tests/vterm_test.cpp` reproduces the bug on the primary and the alternate screen. It failed before
the fix and passes in the release and ASan builds. The live repro now prints 0 warnings.

This is a local workaround only. Nothing was reported to Debian or upstream.

## Not done in item 2

- Any GPU timing: there is no GPU.
- KVM runs.
- The terminal in the virtio-vga VM.
- The five missing VM screenshots.
- A texture count.

## Tests

`meson test` has 13 tests: desktop-file, metainfo, governor (new), profiles, painter, fonts, vterm,
effects, session, fixes, search, gl-match (GL vs CPU PSNR) and view. view gained testTabClose,
testTabIds, testCorruptSettings and testGovernor (auto, cpu). All pass in the release build and in
the ASan+UBSan build (`--timeout-multiplier 4`).

After item 2 (f197952 and later), all 13 pass again in both builds, with `meson test -j2`. The ASan run used the
documented `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`. A first run without it failed painter, effects
and gl-match on fontconfig cache "leaks". In that run, search also tripped `worstSlice < 60.0` under ASan with
two tests in parallel; that assertion depends on timing.

Tools: `tools/m3_close_bench.cpp` (`lterm-m3-close-bench`), `lterm-m3-bench --search`,
`docs/bench/m3-item1/*.sh`, and `tools/m3-shot.sh` for the screenshots.
