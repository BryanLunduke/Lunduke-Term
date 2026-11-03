# Lunduke Terminal

A retro CRT-style terminal emulator for LCOS (C++17, gtkmm-3.0, libvterm), aiming to match the effects of Cool-Retro-Term without Qt.

Work in progress, not part of LCOS 1.0.

License: GPL-3.0-or-later (see `LICENSE`).

## Status: M6 in progress (branch `m6-ux`; M5 merged as PR #6)

M0 (the spike, PR #1) answered the feasibility questions; see
[`docs/M0-RECOMMENDATION.md`](docs/M0-RECOMMENDATION.md). M1 checkpoint A: the GLSL effects run on a
`Gtk::GLArea` (C++ fallback), glyph-cached changed-rows-only rendering, an idle-aware frame
scheduler, and profiles with a plain no-effects default. Checkpoint B: own PTY spawn,
scrollback, selection (with curvature-correct mouse mapping), CLIPBOARD/PRIMARY, bracketed
paste, mouse reporting (X10/1000/1002/1003, SGR), bell and cursor shapes. Checkpoint C: benchmarks
(host, limited/no-AVX llvmpipe, TCG VM) in [`docs/M1-BENCHMARKS.md`](docs/M1-BENCHMARKS.md), licence-checked
fonts with monospace-safe fallback in [`docs/FONTS.md`](docs/FONTS.md). See [`docs/M1-NOTES.md`](docs/M1-NOTES.md).

M2 (see [`docs/M2-NOTES.md`](docs/M2-NOTES.md), tests in [`docs/TESTING.md`](docs/TESTING.md)):
`--effects-quality auto|full|reduced|static` with a CPU-budget governor (auto drops to static on
slow software rendering), vttest/vim/htop/mc fixes, HiDPI (the GTK scale factor: device-pixel
glyphs and effects, font size stays logical), the CRT 1.2.0 RGB shift (`ibm-dos`) and an optional
monitor bezel for `amber`, `green` and `green-scanlines` (rounded glass, shaded frame, glass
highlight and the screen's glow reflected on the frame; pre-rendered, so it costs nothing when idle).
Stage 3: three more CRT 1.2.0-derived profiles (`vintage`, `futuristic`, `paper-white`, each with its
own palette and licence-clean fonts only), tabs, scrollback search with highlighting, a settings
dialog, and packaging prep (`.desktop`, AppStream metainfo, icon, `meson install`,
an unbuilt `debian/`; see [`docs/PACKAGING.md`](docs/PACKAGING.md)). The default was `plain` until M5; since M5 it is `amber` (see below).

M3 item 1 (see [`docs/M3-NOTES.md`](docs/M3-NOTES.md)): the search does not block typing (an idle-built
cache, 10000-match cap), the governor measures render cost only and recovers, closing a tab
does not wait for the shell (SIGHUP, SIGKILL after 2 s), tabs close by id, and a corrupt settings
file no longer blocks Save.

M4 (see [`docs/M4-NOTES.md`](docs/M4-NOTES.md)): Unicode (wide CJK, combining marks, colour emoji
in the glyph cache, invalid UTF-8, copy and search round-trips; the behaviour table there lists
what libvterm 0.3.3 cannot do: no ZWJ sequences, flags, variation-selector width or bidi), and a
libFuzzer harness (`tools/fuzz/`, [`docs/FUZZING.md`](docs/FUZZING.md)) whose findings in front
of libvterm are fixed with regression tests (`tests/fuzz_regress_test.cpp`). No GPU on the build
box: every GL number so far is llvmpipe; real-GPU numbers are not measured.

M5 (see [`docs/M5-NOTES.md`](docs/M5-NOTES.md)): amber is the default profile; five
retro-font profiles on licence-clean Debian fonts (`pixel-8bit`, `pixel-hires-mono`,
`pixel-16bit`, `pixel-40col`, `modern`); fixes from the code review: an output flood no longer freezes the
window (Ctrl+C gets through), input methods (dead keys, Compose, ibus/fcitx), hidden tabs neither
paint nor keep their effects buffers, the cursor stops blinking after the blink timeout, closing
a tab or window that runs a program asks first, and the scrollback maximum is 100,000 lines
(Settings shows what that can cost; lowering it trims only when new output arrives).

M6 (see [`docs/M6-NOTES.md`](docs/M6-NOTES.md)): a right-click menu (Copy, Paste, Settings); the
plain profile and Ctrl+Shift+E no longer leave the window blank once effects have run on the GL
path; the Settings "Font size (px)" field follows the chosen profile; custom profiles (create, copy
from a built-in, edit every option, rename, delete); the amber entry is no longer labelled "(default)".

| Part | Where |
|---|---|
| libvterm + PTY session, scrollback, keyboard/mouse | `src/vterm_session.*`, `src/terminal_view.*` |
| Selection, pointer→cell (curvature) | `src/selection.*` |
| Glyph-cached grid painter, dirty rows | `src/grid_painter.*`, `src/grid.hpp` |
| GLSL pipeline (GtkGLArea or headless EGL) | `src/gl_pipeline.*`, `src/gl_shaders.hpp`, `src/gl_effects.*` |
| C++ fallback effects, row-parallel | `src/effects.*`, `src/cpu_effects.*`, `src/thread_pool.*` |
| Profiles and palettes | `src/profiles.*` |
| Window: tabs, search bar, settings dialog | `src/terminal_window.*` |
| Scrollback search (wrap-joined, case-folded) | `src/search.*` |
| Desktop file, metainfo, icon; Debian packaging prep | `data/`, `debian/`, `docs/PACKAGING.md` |
| Font choice and fallback (fontconfig) | `src/font_resolve.*` |
| Benchmarks | `tools/m1_bench.cpp`, `tools/bench-m1.sh`, `tools/m0_bench.cpp` |
| Fuzzing (libFuzzer harness, seeds) | `tools/fuzz/`, `docs/FUZZING.md` |

### Build

Needs `libgtkmm-3.0-dev`, `libvterm-dev` (0.3.3), `libepoxy-dev`, `libegl-dev`, `libfontconfig-dev`, Meson, Ninja, g++.
No fonts are bundled. For the retro profiles, install `fonts-terminus` (or `fonts-terminus-otb`), `fonts-pc` and `fonts-3270`.
Without them, the profiles fall back to DejaVu Sans Mono. See `docs/FONTS.md`.

    meson setup build
    ninja -C build -j2
    meson test -C build          # governor, effects, vterm, session, profiles, fonts, painter, search, view and CPU-vs-GL (incl. resize-leak) tests
    meson install -C build --destdir /tmp/stage   # binary, .desktop, metainfo, icon, docs

### Run

    build/lunduke-terminal                         # amber CRT look (default since M5; GLSL, C++ fallback)
    build/lunduke-terminal --profile plain         # no effects at all
    build/lunduke-terminal --list-profiles         # profiles and the font each resolves to here
    build/lunduke-terminal --effects               # plain colours with the CRT effects
    build/lunduke-terminal --profile green --renderer cpu --threads 4
    build/lunduke-terminal --effects-fps 10 --max-fps 60 --no-pause-unfocused --stats -e htop
    build/lunduke-terminal --profile amber --frame                  # with the monitor bezel
    build/lunduke-terminal --profile green --frame --frame-size 40 --frame-color '#c8c0b0' --frame-shininess 1
    build/lunduke-terminal --profile ibm-dos --rgb-shift 0          # ibm-dos without its RGB shift
    GDK_SCALE=2 build/lunduke-terminal --profile amber              # HiDPI (follows the GTK scale)
    build/lunduke-terminal --profile vintage --frame                # green CRT 1.2.0 "Vintage", with bezel
    build/lunduke-terminal --profile futuristic                     # blue, chroma 1, no bezel
    build/lunduke-terminal --profile paper-white --frame -T notes   # white phosphor, own window title

Options: `--profile ID`, `--effects`/`--no-effects`, `--renderer auto|gl|cpu`, `--threads N`
(C++ fallback), `--effects-fps N` (default 20), `--max-fps N` (default 60), `--no-pause-unfocused`,
`--font FAMILY`, `--font-px N`, `--size WxH`, `--quit-after MS` (close the window after MS **milliseconds**), `--stats`, `--scrollback N` (default 5000 lines; a `scrollback-lines` already saved in settings.ini is used as is),
`--login`, `--cwd DIR`, `--on-exit close|hold|hold-on-error`, `--hold`, `--bell visual,urgent,audible|none`,
`--cursor-shape block|underline|bar`, `--cursor-blink`/`--no-cursor-blink`,
`--effects-quality auto|full|reduced|static`, `--cpu-budget PCT` (software rendering only, percent of one core, default 15, 0 = off), `--frame`/`--no-frame`
(bezel; amber/green/green-scanlines/vintage/paper-white only, ignored with a warning elsewhere), `--frame-size PX`
(logical px, default 28), `--frame-color #RRGGBB`, `--frame-shininess 0..1` (reflection and
highlights), `--rgb-shift X` (CRT `rbgShift`, 0 = off), `-T/--title TEXT`, `-e COMMAND...`, `-h/--help` (all options and keys), `--version`.
An unknown option exits with status 2, and so does a bad value for `--renderer`, `--on-exit`, `--cursor-shape`
or `--effects-quality`; other option values are not checked.
A press on the bezel neither selects nor is reported to the application; a drag that leaves the
glass clamps to the edge cells.
Right-click opens a menu with Copy (greyed out without a selection), Paste and Settings; when the
application uses the mouse, Shift+right-click opens it.
Keys: F11 toggles fullscreen (Shift/Ctrl/Alt/Super+F11 still reach the program). Font zoom: Ctrl+= or Ctrl++ (also keypad +)
makes the font 1 px bigger, Ctrl+- (keypad -) 1 px smaller, Ctrl+0 (keypad 0) goes back to the profile's size, or to the
`--font-px`/Settings size if one is set; the zoom is per tab, is not saved (the Settings font size field and "Save as default"
ignore it) and starts again at 0 when the profile or the font size is changed. Ctrl+Shift+E toggles effects, Ctrl+Shift+P cycles the profiles (built-in, then custom), Ctrl+Shift+C/V copy/paste,
Shift+PgUp/PgDn/Home/End scroll back, Shift+Insert or middle-click pastes PRIMARY, Shift bypasses
mouse reporting. Tabs: Ctrl+Shift+T opens a tab (in the current tab's working directory),
Ctrl+Shift+W closes it (asking first if a program other than the shell runs in it), Ctrl+PgUp/PgDn switch (wrapping); the tab bar shows only with two or more
tabs, and hidden tabs do not animate. Search: Ctrl+Shift+F opens the search bar over the scrollback
(matches highlighted, current one in orange, "N of M"); Enter goes to the next older match,
Shift+Enter to the newer one, Escape closes. Settings: Ctrl+Shift+S opens a dialog for profile,
font and size (the size field shows the profile's own size; typing another size overrides it until the profile changes), effects quality, scrollback lines and bezel; changes apply live to every tab in the window and to new tabs;
"Save as default" writes them to the settings file (other keys are kept; a corrupt file is kept as `settings.ini.corrupt` and replaced). Defaults can be set in
`~/.config/lunduke-terminal/settings.ini` (group `[terminal]`, keys `profile`, `effects`,
`renderer`, `threads`, `effects-fps`, `max-fps`, `pause-unfocused`, `font`, `font-px`,
`scrollback-lines`, `login-shell`, `cwd`, `on-exit`, `bell`, `cursor-shape`, `cursor-blink`, `word-chars`,
`effects-quality`, `cpu-budget`, `frame`, `frame-size`, `frame-color`, `frame-shininess`, `rgb-shift`).
Custom profiles: in Settings, "New..." starts from plain, "Copy to custom profile..." starts from the
selected profile, and "Edit...", "Rename..." and "Delete" work on custom profiles only (built-ins are
read-only). Each one is a file `~/.config/lunduke-terminal/profiles/NAME.ini` (`$XDG_CONFIG_HOME`;
groups `[profile]` and `[effects]`), written atomically; missing keys take plain's values, out-of-range
values are clamped, and a file that cannot be read is skipped with a warning and left on disk. They
are listed by `--list-profiles` and chosen with `--profile "custom:NAME"`. Names may not be empty,
start with '.', have leading or trailing spaces, contain `/`, `\` or control characters, be longer than
64 bytes, or match another profile (case-insensitive).

    build/lterm-m1-bench --paint
    LIBGL_ALWAYS_SOFTWARE=1 build/lterm-m1-bench --compare --resize-loop 60
    LIBGL_ALWAYS_SOFTWARE=1 build/lterm-m1-bench --tick-gl --profile amber --frame --quality static
    build/lterm-m0-bench --bench --threads 1,4,8 --runs 5 --frames 30
    DISPLAY=:1 IDLE=1 tools/bench-m1.sh /tmp/bench build   # full M1 matrix (see docs/M1-BENCHMARKS.md)

The effect maths follows cool-retro-term 1.2.0 (GPL-3.0-or-later, Filippo Scognamiglio);
it was re-implemented here for C++ and GLSL 3.30 (both projects are GPL-3.0-or-later).
