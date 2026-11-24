# Review fixes on master 163e451 (libghostty-vt)

Measured on this box: release build (`-O3`), g++ 13, llvmpipe only where a window is opened.
CPU effects and the glyph painter do not use the GPU. "Before" is the same bench linked
against the objects from master `163e451`. "After" is this branch.

The amber window used for the keystroke is the default 4:3 size, 868×652. The 4:3 paint is an
80×24 grid of 10×25 cells (DejaVu Sans Mono 16 px stretched to the tube). Scrollback is 100 000
lines of `line N the quick brown fox` at 80 columns.

| | Before | After |
|---|---|---|
| Amber content update, one text row | 7.38 ms (the whole frame; bloom 2.04 ms) | **1.57 ms** (bloom 0.16 ms) |
| Amber content update, full frame | 7.17 ms (bloom 1.89 ms) | 8.03 ms (bloom 2.39 ms) |
| Dynamic pass after that update | 8.54 ms | 9.32 ms |
| 4:3 glyph paint, warm cache | 7.15 ms | **1.61 ms** |
| fillGrid, 24×80, scrolled into 100k lines | 0.60 ms | **0.09 ms** |
| fillGrid after one more byte (live screen) | 0.62 ms | **0.08 ms** |
| Copy of all 100k lines (3.1 MB of text) | 2540 ms | **220 ms** |
| RSS with 100k lines | +63.2 MB | +63.3 MB |
| libghostty scrollback bytes, then compress | 69.5 MB | **1.87 MB** |
| RSS after compress and `malloc_trim` | — | 22.9 MB |

A one-row change of the CPU effects matches a full recompute with max channel difference 0
(`effects_test`). GL versus CPU stays in the existing PSNR band (`gl_match`, about 62 dB,
max difference 2). The dynamic pass (noise, flicker, scanlines) still covers the whole frame;
scrolling the source by a blit is left as a follow-up.

Search of 10 000 lines in `search_test` went from 78 ms to 26 ms once rows are read in bulk.
Building the index over 100 000 lines went from 314 slices to 35.

Screenshots (CPU renderer, same command, Xvfb). Amber's phosphor noise moves between frames,
so the pictures are not byte-identical; the mean per-pixel difference is under half a grey
level. IBM DOS is quieter (under 1% of pixels differ).

| | Before | After |
|---|---|---|
| Amber | [before-amber.png](before-amber.png) | [after-amber.png](after-amber.png) |
| IBM DOS | [before-ibm.png](before-ibm.png) | [after-ibm.png](after-ibm.png) |
