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
- Timing and the frame governor were not touched, so the 10-run view loop was not repeated.
- `src/vterm_session.cpp` is unchanged, so no fuzz run.

## Caveats

- Item 2: that the GL composite reads the unflushed xlib surface is shown by the experiments and the GTK 3.24 source; why cairo-xlib holds back that particular blit is inferred, not traced inside cairo. Verified on llvmpipe/Xvfb only (no GPU, no Wayland session).
- Item 4: an effect switched off in the editor keeps its slider value only while the editor is open; the file stores the effective value (0 = off). Ctrl+Shift+P cycles the built-in profiles only. Custom profile labels are their names.
- Screenshots come from Xvfb without a window manager and without the SVG pixbuf loader in the tools prefix, so spin-button +/- icons are drawn as grey squares; a real desktop shows the icons.
- `review_test` failed once during development at its flood frame-cap check (line 161) and passed on 3 reruns and in every suite run above; it looks timing-sensitive under load.
