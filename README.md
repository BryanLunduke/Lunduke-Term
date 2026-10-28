# Lunduke Terminal

A retro CRT-style terminal emulator for LCOS (C++17, gtkmm-3.0, libvterm), aiming to match the effects of Cool-Retro-Term without Qt.

Work in progress, not part of LCOS 1.0.

License: GPL-3.0-or-later (see `LICENSE`).

## Status: M1 in progress (branch `m1-core`)

M0 (the spike, PR #1) answered the feasibility questions; see
[`docs/M0-RECOMMENDATION.md`](docs/M0-RECOMMENDATION.md). M1 checkpoint A: the GLSL effects run on a
`Gtk::GLArea` (C++ fallback), glyph-cached changed-rows-only rendering, an idle-aware frame
scheduler, and profiles with a plain no-effects default. Checkpoint B: own PTY spawn,
scrollback, selection (with curvature-correct mouse mapping), CLIPBOARD/PRIMARY, bracketed
paste, mouse reporting (X10/1000/1002/1003, SGR), bell and cursor shapes. Checkpoint C: benchmarks
(host, limited/no-AVX llvmpipe, TCG VM) in [`docs/M1-BENCHMARKS.md`](docs/M1-BENCHMARKS.md), licence-checked
fonts with monospace-safe fallback in [`docs/FONTS.md`](docs/FONTS.md). See [`docs/M1-NOTES.md`](docs/M1-NOTES.md).

| Part | Where |
|---|---|
| libvterm + PTY session, scrollback, keyboard/mouse | `src/vterm_session.*`, `src/terminal_view.*` |
| Selection, pointer→cell (curvature) | `src/selection.*` |
| Glyph-cached grid painter, dirty rows | `src/grid_painter.*`, `src/grid.hpp` |
| GLSL pipeline (GtkGLArea or headless EGL) | `src/gl_pipeline.*`, `src/gl_shaders.hpp`, `src/gl_effects.*` |
| C++ fallback effects, row-parallel | `src/effects.*`, `src/cpu_effects.*`, `src/thread_pool.*` |
| Profiles and palettes | `src/profiles.*` |
| Font choice and fallback (fontconfig) | `src/font_resolve.*` |
| Benchmarks | `tools/m1_bench.cpp`, `tools/bench-m1.sh`, `tools/m0_bench.cpp` |

### Build

Needs `libgtkmm-3.0-dev`, `libvterm-dev` (0.3.3), `libepoxy-dev`, `libegl-dev`, `libfontconfig-dev`, Meson, Ninja, g++.
No fonts are bundled. For the retro profiles, install `fonts-terminus` (or `fonts-terminus-otb`), `fonts-pc` and `fonts-3270`.
Without them, the profiles fall back to DejaVu Sans Mono. See `docs/FONTS.md`.

    meson setup build
    ninja -C build -j2
    meson test -C build          # effects, vterm, session, profiles, fonts, painter and CPU-vs-GL (incl. resize-leak) tests

### Run

    build/lunduke-terminal                         # plain profile, no effects (default)
    build/lunduke-terminal --profile amber         # Default Amber CRT look (GLSL, C++ fallback)
    build/lunduke-terminal --list-profiles         # profiles and the font each resolves to here
    build/lunduke-terminal --effects               # plain colours with the CRT effects
    build/lunduke-terminal --profile green --renderer cpu --threads 4
    build/lunduke-terminal --effects-fps 10 --max-fps 60 --no-pause-unfocused --stats -e htop

Options: `--profile ID`, `--effects`/`--no-effects`, `--renderer auto|gl|cpu`, `--threads N`
(C++ fallback), `--effects-fps N` (default 20), `--max-fps N` (default 60), `--no-pause-unfocused`,
`--font FAMILY`, `--font-px N`, `--size WxH`, `--quit-after MS`, `--stats`, `--scrollback N`,
`--login`, `--cwd DIR`, `--on-exit close|hold|hold-on-error`, `--hold`, `--bell visual,urgent,audible|none`,
`--cursor-shape block|underline|bar`, `--cursor-blink`/`--no-cursor-blink`, `-e COMMAND...`.
Keys: Ctrl+Shift+E toggles effects, Ctrl+Shift+P cycles profiles, Ctrl+Shift+C/V copy/paste,
Shift+PgUp/PgDn/Home/End scroll back, Shift+Insert or middle-click pastes PRIMARY, Shift bypasses
mouse reporting. Defaults can be set in
`~/.config/lunduke-terminal/settings.ini` (group `[terminal]`, keys `profile`, `effects`,
`renderer`, `threads`, `effects-fps`, `max-fps`, `pause-unfocused`, `font`, `font-px`,
`scrollback-lines`, `login-shell`, `cwd`, `on-exit`, `bell`, `cursor-shape`, `cursor-blink`, `word-chars`).

    build/lterm-m1-bench --paint
    LIBGL_ALWAYS_SOFTWARE=1 build/lterm-m1-bench --compare --resize-loop 60
    build/lterm-m0-bench --bench --threads 1,4,8 --runs 5 --frames 30
    DISPLAY=:1 IDLE=1 tools/bench-m1.sh /tmp/bench build   # full M1 matrix (see docs/M1-BENCHMARKS.md)

The effect maths follows cool-retro-term 1.2.0 (GPL-3.0-or-later, Filippo Scognamiglio);
it was re-implemented here for C++ and GLSL 3.30 (both projects are GPL-3.0-or-later).
