# Lunduke Terminal

A retro CRT-style terminal emulator for LCOS (C++17, gtkmm-3.0, libvterm), aiming to match the effects of Cool-Retro-Term without Qt.

Work in progress, not part of LCOS 1.0.

License: GPL-3.0-or-later (see `LICENSE`).

## Status: M0 spike (branch `m0-spike`)

This branch is a throw-away-quality spike that answers three questions before M1. See
[`docs/M0-RECOMMENDATION.md`](docs/M0-RECOMMENDATION.md) for the results.

| Part | What | Where |
|---|---|---|
| (a) | libvterm + PTY (`forkpty`) shell in a gtkmm-3.0 window, Pango/Cairo glyph renderer, colours, cursor, keyboard, resize (`vterm_set_size` + `TIOCSWINSZ`), output fed through the effects pass | `src/vterm_session.*`, `src/terminal_view.*`, `src/grid_painter.*`, `src/main.cpp` |
| (b) | CPU effects pass in plain C++ (barrel warp, scanlines, bloom, burn-in, noise, jitter, glowing line, flicker, horizontal sync, ambient light), row-parallel | `src/effects.*`, `src/cpu_effects.*`, `src/thread_pool.*` |
| (c) | The same effects as GLSL 3.30 fragment shaders on a headless EGL context (Mesa llvmpipe with `LIBGL_ALWAYS_SOFTWARE=1`), golden comparison against the CPU output | `src/gl_effects.*`, `src/gl_shaders.hpp`, `tools/m0_bench.cpp`, `tools/make-comparison.sh` |

### Build

Needs `libgtkmm-3.0-dev`, `libvterm-dev` (0.3.3), `libepoxy-dev`, `libegl-dev`, Meson, Ninja, g++.

    meson setup build
    ninja -C build -j2
    meson test -C build          # effects, vterm and CPU-vs-GL golden tests

### Run

    build/lunduke-terminal                    # shell with the Default Amber look
    build/lunduke-terminal --no-effects       # plain renderer
    build/lunduke-terminal --threads 8 --font-px 22 -e htop

    build/lterm-m0-bench --bench --threads 1,4,8 --runs 5 --frames 30
    LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=4 build/lterm-m0-bench --bench-gl
    tools/make-comparison.sh build out/

The effect maths follows cool-retro-term 1.2.0 (GPL-3.0-or-later, Filippo Scognamiglio);
it was re-implemented here for C++ and GLSL 3.30 (both projects are GPL-3.0-or-later).
