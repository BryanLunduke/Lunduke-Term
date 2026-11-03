# Perf step 1: item check (branch perf-step1)

For each order item: where it is in the code, and which test covers it.

| # | Order item | Code | Test |
|---|---|---|---|
| 1 | Lazy texOut_/fboOut_ | `GlPipeline::resize()` no longer creates them. `renderToMemory()` creates them on first readback, and `freeSized()` frees them. Only `GlEffects::render()` calls `renderToMemory()`, and only `tests/gl_match_test.cpp` uses `GlEffects`. The window renders through `glPipe_->render(..., fbo)` into the GtkGLArea framebuffer, so a window never allocates them. | gl-match "lazy output texture": with no readback 8 textures / 6 FBOs; the first readback adds 1 texture and 1 FBO (+1,024,000 B at 640x400x4); readback is reused; resize frees them. |
| 2 | --help/-h, --version; unknown option exits 2; --quit-after in ms | Usage text and a pre-scan in `src/main.cpp`. The pre-scan stops at `-e`, so `-e prog --help` is passed to prog. | New meson test `cli`: --help, -h and `--profile amber --help` exit 0 and the text says "--quit-after MS ... milliseconds"; --version prints "lunduke-terminal 0.0.1" and exits 0; --no-such-option and `--renderer bogus` exit 2. |
| 3 | F11 fullscreen | `TerminalWindow::toggleFullscreen()`. Only plain F11 is taken; Shift/Ctrl/Alt/Super+F11 still reach the program (Super added in the PR #9 review fixes). Documented in README and --help. | ux `testFullscreenKey`: under xfwm4 the window goes to 1920x1200 fullscreen and back. Without a window manager the state check is skipped. |
| 4 | Ctrl+= / Ctrl++ / Ctrl+- / Ctrl+0 | `TerminalView::zoomFont()` / `resetFontZoom()` keep a runtime offset (±1 px, kept in 6..96 px; a base size outside that range steps 1 px toward it, `TerminalView::zoomedFontPx()`). It is never saved, and `opts_.fontPx` (the M6 Settings value) is not changed. Changing the profile or font resets the zoom. | ux `testFontZoom`: 16 px / 79 cols, zoom in 3 → 19 px / 72 cols, Ctrl+0 → 16 px; clamps hold; the Settings field still shows 16 while zoomed; saving writes no font-px; a --font-px 30 override goes to 31 and back to 30; switching to amber resets to 22. |
| 5 | cpuBudget 0.25 → 0.15 | `ViewOptions::cpuBudget` and `GovernorConfig::budget` are both 0.15. README `--cpu-budget` default is 15; notes added in M2/M3. | governor_test checks `GovernorConfig().budget == 0.15` (the demotion tests set 0.25 explicitly). view_test checks `cpuBudget == 0.15` and the view governor budget. |
| 6 | Uniform locations cached after link | X-macro lists in `gl_pipeline.hpp`. `lookUpUniforms()` runs once in the constructor after the programs link, and every draw uses `loc_`. | gl-match: 69 `glGetUniformLocation` calls in the constructor, 0 across 50 resizes, frames and readback. PSNR lines are identical to master. |

Ctrl+0 resets to the configured size: the profile's size, or the --font-px/Settings override when one is set.
Plain F11 and Ctrl+- are no longer sent to the program. On master 21eff10, F11 sent ESC[23~ and Ctrl+- sent ESC[45;5u
(checked with xdotool on a master build). Ctrl+_ (Ctrl+Shift+-) still sends 0x1f (^_), so readline/Emacs undo keeps working.
