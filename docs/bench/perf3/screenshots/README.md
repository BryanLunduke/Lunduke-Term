# Screenshots (REAL)

The before and after captures were byte-identical in every pair, so only one PNG per pair is kept
(PR #11 review N2). In the table, "before" is master 6283674 and "after" is the perf-step3 build
named below. The sha256 is the same for both.

| file | sha256 (before = after) |
|---|---|
| `offline/amber-full-s2-1280x800-cpu.png` | `d17bb4060264348ccd0af241caa761f3948d580dc02feaacd422b2af51978377` |
| `offline/amber-full-s2-1280x800-gl.png` | `56691d80f6375cc0118c01a665f2393b37848fdd7e92232cf04b0c9deaf52527` |
| `offline/paper-white-bezel-static-s1-640x400-cpu.png` | `ef031f8469f1187b7b1fa880d65db2b74177bbb260d659e2c376a46a8e9c07bc` |
| `offline/paper-white-bezel-static-s1-640x400-gl.png` | `7a74c7f5600b536010bc003c7aefa3f93fdbf4dd877c768ef037751a89e76e32` |
| `window/amber-cpu-static.png` | `95211342ee8386373a8e10086d78596e646f2597683f4b82f9cd197ac77d6bc7` |
| `window/amber-gl-static.png` | `5ab6964ecd7a0ffda1f83142ecf683eb140823be8791bb73c5fe658dcf0d4bab` |
| `window/plain.png` | `122d7b27f6d834b48bd4de88b204447c3f03c88e76fb70c4ccda77d229dad779` |

window/: the real app at 1280x800 on my own Xvfb with xfwm4, which was alive for the whole capture. These shots were made with shot.sh, a copy of tools/m2-shot.sh that uses the same fixed text and grabs the window area with PIL, because ImageMagick is not on the box. The three shots are plain (`--no-effects`), amber GL static quality, and amber `--renderer cpu` static quality. Before/after was first compared at 995c820, and again at the row-prepass head (see "Re-check at the PR #11 review head" below). The grab box includes a thin strip of the WM frame at the right and bottom edges, the same in both.

offline/: raw frames from docs/bench/perf3/offline/render.cpp, converted to PNG. They show amber full quality at 1280x800 (scale 2) and the worst gl-match configuration (paper-white with bezel, static quality, 640x400) on both back-ends. They were made at 995c820. At the review head, all 97 raw files of the offline render are still byte-identical to master's (experiments/psnr-rowprepass.md), so these PNGs stand for that head too.

"Identical" holds on this build: x86-64 without FMA contraction (see ../README.md).

## Re-check at the PR #11 review head (REAL)

The three window shots were captured again with master and with the build of 30479da (row prepass + `-ffp-contract=off`) on a fresh Xvfb :96 with xfwm4 (alive). All six captures have the same sha256 as the kept file for their pair: 95211342… (amber CPU), 5ab6964e… (amber GL), 122d7b27… (plain). The re-captures were not added.
