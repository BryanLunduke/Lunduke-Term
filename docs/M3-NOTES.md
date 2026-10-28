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

## Tests

`meson test` has 13 tests: desktop-file, metainfo, governor (new), profiles, painter, fonts, vterm,
effects, session, fixes, search, gl-match (GL vs CPU PSNR) and view. view gained testTabClose,
testTabIds, testCorruptSettings and testGovernor (auto, cpu). All pass in the release build and in
the ASan+UBSan build (`--timeout-multiplier 4`).

Tools: `tools/m3_close_bench.cpp` (`lterm-m3-close-bench`), `lterm-m3-bench --search`,
`docs/bench/m3-item1/*.sh`, and `tools/m3-shot.sh` for the screenshots.
