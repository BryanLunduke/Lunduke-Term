# Lunduke Terminal — M6 notes (PR A, branch `m6-ux`)

Branch `m6-ux` on master dc3b544 (M5, PR #6). Order from the editor (2026-11-02), items 1–5 for
Ted; item 6 (RAM) is a separate PR B. Every number is REAL (measured here) or DERIVED. The build
box has **no GPU**: every GL result is Mesa llvmpipe on Xvfb. Times are EST.

| Item | What changed | Test / evidence |
|---|---|---|
| 1 right-click menu | `TerminalView` has a context menu: Copy (greyed out unless there is a non-empty selection), Paste (greyed out while the child has exited or input is held), separator, Settings. Right-click opens it at the pointer; under mouse reporting the right-click goes to the application and Shift+right-click opens the menu (same rule as Shift for selection). A right-click on the bezel opens it too. Settings opens the window's Settings dialog. | `ux_test` `testContextMenu`, `testContextMenuWindow`; `bench/m6/tests/item1-ux.txt` |
| 2 blank plain/Ctrl+Shift+E (bug) | Reproduced on the GL path only: after effects ran, plain or effects-off showed only the window background (256000 of 256000 pixels background-bright; the C++ path was fine). Root cause: once the `GtkGLArea` has been realized, GDK keeps a GL paint context on the toplevel and composites every later frame with GL, reading the xlib paint surface through `GLX_EXT_texture_from_pixmap` without flushing it, so the cairo blit from the `DrawingArea` never reached the screen. `GDK_GL=software-draw-surface` (which disables that read) also hides the bug. Fix: `cairo_surface_flush(cairo_get_target(c))` after the blit in `TerminalView::onDraw`. | `ux_test` `testPlainRenders` gl/cpu (fails without the fix: 2 failures). `bench/m6/tests/item2-*.txt`, screenshots `bench/m6/screenshots/plain-after-amber-gl{,-WITHOUT-fix}.png` |
| 3 Font size field (bug) | The field showed 0 ("profile's size") and never followed the profile. Now it shows the profile's size (22 for amber, 16 for plain); a typed size overrides until the profile changes, then the field resets to the new profile's size and the override to 0 (so it is not saved as an override). | `ux_test` `testSettingsFontPx` (4 failures without the fix); `bench/m6/tests/item3-*.txt` |
| 4 custom profiles | `src/custom_profiles.*`: files `$XDG_CONFIG_HOME/lunduke-terminal/profiles/NAME.ini` (`[profile]`, `[effects]`), id `custom:NAME`, atomic writes (`g_file_set_contents`), missing keys from plain, out-of-range values clamped, corrupt files skipped with a warning and left on disk. Built-ins are read-only (save/rename/delete refuse them; a file named `amber.ini` does not shadow the built-in). Name validation: not empty or blank, ≤ 64 bytes, valid UTF-8, no control characters, `/` or `\`, no leading `.`, no leading/trailing spaces, not a built-in id or label or another custom name (case-insensitive). `src/profile_editor.*`: an editor dialog (tabs Text, Palette, Effects, Bezel) for every profile field except the runtime `pixelScale`. Settings: New…, Copy to custom profile…, Edit…, Rename…, Delete (with confirmation); Edit/Rename/Delete are greyed out on built-ins and a note says where a custom profile is stored. Loaded at startup; `--list-profiles` and `--profile custom:NAME` include them. | `custom-profiles` test (round trip of all 14 built-ins + one with every field changed, corrupt files, rename incl. case-only, delete, read-only built-ins, name validation, reload as restart, unwritable dir): `bench/m6/tests/item4-custom-profiles-test.txt`. `ux_test` `testCustomProfilesUi` (buttons, bad names, copy+edit+save, restart via reload in a new window, rename, delete cancel/confirm, New): `item4-ux.txt`. Screenshots `bench/m6/screenshots/settings-*.png`, `profile-editor-*.png` |
| 5 "(default)" | The Amber entry is labelled "Amber" only. | `review_test` `testDefaultProfile` checks the label has no "default". No committed screenshots showed it; older docs' "plain (default)" refer to the M1 default and were left as history. |

## Tests (REAL)

- Release `meson test -C build`: **18/18 OK** at f01c307 (`bench/m6/tests/release-meson-test.txt`; new tests `custom-profiles` and `ux`).
- ASan/UBSan with leak checking (`build-san`, `tests/lsan.supp`): serial **2 × 18/18 OK**, parallel (2 processes) **1 × 18/18 OK** at f01c307 (`asan-serial.txt`, `asan-parallel.txt`). No leak or UBSan reports.
- Re-run on the pushed head 4ec60e4 (no source change since f01c307; only docs and outputs): release **18/18 OK**, ASan/UBSan serial **2 × 18/18 OK**, parallel **1 × 18/18 OK**, no leak or UBSan reports (`release-final-4ec60e4.txt`, `asan-serial-final-4ec60e4.txt`, `asan-parallel-final-4ec60e4.txt`, 21:30–21:48 EST).
- Timing and the frame governor were not touched, so the 10-run view loop was not repeated.
- `src/vterm_session.cpp` is unchanged, so no fuzz run.

## Caveats

- Item 2: that the GL composite reads the unflushed xlib surface is shown by the experiments and the GTK 3.24 source; why cairo-xlib holds back that particular blit is inferred, not traced inside cairo. Verified on llvmpipe/Xvfb only (no GPU, no Wayland session).
- Item 4: an effect switched off in the editor keeps its slider value only while the editor is open; the file stores the effective value (0 = off). Ctrl+Shift+P cycled the built-in profiles only (fixed in the PR #7 review, below). Custom profile labels are their names.
- Screenshots come from Xvfb without a window manager and without the SVG pixbuf loader in the tools prefix, so spin-button +/- icons are drawn as grey squares; a real desktop shows the icons.
- `review_test` failed once during development at its flood frame-cap check (line 161) and passed on 3 reruns and in every suite run above; it looks timing-sensitive under load.

## PR #7 review fixes (2026-11-02, CST)

Ted's review of 525f16c ("approve after one fix"), plus the editor's decision on `--list-profiles`.
Every result is REAL (measured here, Xvfb + llvmpipe, no GPU). "Fails without the fix" runs are
the new tests with the fix disabled in a scratch build, outputs committed.

| Item | Change | Evidence |
|---|---|---|
| MUST FIX: a profile file skipped as broken was overwritten | `validateProfileName(name, err, renaming, dir)` also refuses a name when `dir` holds `NAME.ini` (case-insensitive) that is not a loaded profile (`unloadedProfileFile`). `saveCustomProfile` (New, Copy, editor Save) and `renameCustomProfile` pass the directory; the name dialog's live check does too, so OK is greyed out with the reason: *A profile file named "broken.ini" already exists in DIR but could not be loaded; fix or remove it by hand, or pick another name*. The status line shows the same text if a save/rename is refused. | `custom-profiles` test: `badcolor.ini` (bad colour) and `broken.ini` (not a key file) stay byte-identical after New/Copy onto `badcolor`/`BadColor` and Rename onto `broken`; the name is free once the file is removed. Without the fix: **10 failures** (`bench/m6/tests/custom-profiles-WITHOUT-fix-review.txt`). `ux_test` `testBrokenProfileFileKept` (New, Copy, Rename through the dialog): **9 of the 11** failures in `ux-WITHOUT-fixes-review.txt` |
| Ctrl+Shift+P includes custom profiles | `nextProfileId()`: built-ins, then custom profiles, wrapping. | `ux_test` `testProfileCycle`: modern > custom:Cycle A > custom:Cycle B > plain > … > modern. Without the fix: 2 failures (it went modern > plain) |
| ux_test removes its `/tmp/lterm-ux-test-PID` | `removeTree(home)` at the end of `main` and in an `atexit` handler (covers `exit()` paths; a crash or signal still leaves it). Only paths under `/tmp/lterm-ux-test-` are removed. | prints "config folder … removed" and checks it; without it: 1 failure ("LEFT BEHIND") |
| Cheap Copy check | `Selection::hasText()` answers `!text().empty()` without building the text (stops at the first cell that would be copied); the context menu uses it. | `ux_test` `testSelectionHasText`: 8,000 random selections (blanks, literal spaces, wide chars, soft wraps) agree with `!text().empty()`, 0 mismatches; a 5,000 × 80 soft-wrapped selection: `hasText` read 1 cell, `text()` 400,000. The existing Copy menu tests pass unchanged. (No failing-without-fix test: the old code was correct, only slow.) |
| Deleting the saved default | `resetDeletedDefaultProfile()`: after a custom profile is deleted, if settings.ini's `[terminal] profile` is that id it becomes `amber`; other keys and groups are kept, the write is atomic, an unreadable settings.ini is left alone (status line says so). | `ux_test` `testDeleteSavedDefault`: deleting another custom profile leaves settings.ini byte-identical; deleting the saved one gives `profile=amber` and keeps `[other] keep=yes` and `scrollback-lines`. Without the fix: 1 failure |
| `--list-profiles` without "[default]" (editor's decision) | The list moved into `profileListText()` (used by `main`); no default marker. | `custom-profiles` test: no "default" in the output, one line per built-in and custom profile, `amber  Amber`. Before/after: `bench/m6/tests/list-profiles-no-default-review.txt`. (The captured output in `docs/TERMINUS-ON-ISO.md` is an older dated capture and was left as is.) |

Tests on head 86ee8ff (source as pushed; later commits are docs only):
- Release: **18/18 OK** (`release-86ee8ff-review.txt`).
- ASan/UBSan with leak checking: serial **2 × 18/18 OK**, parallel (2 processes) **1 × 18/18 OK**, no leak or UBSan reports (`asan-serial-86ee8ff-review.txt`, `asan-parallel-86ee8ff-review.txt`, 21:43–21:59 CST).
- `src/vterm_session.cpp` unchanged: no fuzz run. Governor and timing code untouched.

Left as the review asked: case-only renames on case-insensitive filesystems and the other nits.
15 older `/tmp/lterm-ux-test-*` folders from runs before this fix are still on the box (not repo files, not deleted).

## Default scrollback 5000 (PR #8 review, branch `m6-ram`, 2026-11-02, CST)

Editor's order via Ted: the default scrollback is 5000 lines (was 10,000). `kDefaultScrollbackLines`
in `src/vterm_session.hpp` feeds both `ViewOptions::scrollbackLines` and the session's initial limit;
built-in and custom profiles carry no scrollback setting. A `scrollback-lines` already saved in
settings.ini is read as before and never migrated or rewritten. Tests: `fixes` checks the constant
and a new session's limit; `scrollback-default` runs the binary with no settings, with settings
lacking the key (5000 both), and with saved 10000 and 7000 (kept; files byte-identical after the
run). RAM with the new default: `bench/m6/ram/summary-sb5000.txt` (REAL; -1.4 to -2.5 MB after
output against the same build at 10,000). Earlier M1–M6 notes and the RAM captures that say 10,000
or 10000 are dated measurements and are left as they were.

