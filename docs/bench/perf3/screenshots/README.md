# Screenshots (REAL)

window/: the real app at 1280x800 on my own Xvfb :95 with xfwm4 (alive for the whole capture). Made with shot.sh, a copy of tools/m2-shot.sh that uses the same fixed text and grabs the window area with PIL because ImageMagick is not on the box. The captures are: plain (`--no-effects`), amber GL static quality, and amber `--renderer cpu` static quality, each before (master 6283674) and after (perf-step3 995c820). **Before and after are pixel-identical in all three pairs** (PIL difference: no differing pixel). The grab box includes a thin strip of the WM frame at the right and bottom edges; this is the same in both.

offline/: raw frames from docs/bench/perf3/offline/render.cpp, converted to PNG. They show amber full quality at 1280x800 (scale 2) and the worst gl-match configuration, paper-white with bezel, static quality, at 640x400, on both back-ends, before and after. Each before/after pair is byte-identical, with the same PNG sha256.
