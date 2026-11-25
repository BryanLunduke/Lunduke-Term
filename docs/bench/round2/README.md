# Round 2 review, measured on this box

Release build (`-O3`), g++ 13, llvmpipe (Mesa 25.2.8, LLVM 20). "Before" is the
same bench linked against the effects objects from master `db9c1e8`. "After"
is this branch. The idle tick is one content update, then frames at t = 3 s,
which is after amber's burn-in has faded. Window 868×652, amber, full quality.

| | Before | After |
|---|---|---|
| CPU idle tick, 1 thread | 32.79 ms | **25.11 ms** |
| CPU idle tick, 4 threads | 8.27 ms | **6.61 ms** |
| GL idle tick (no readback) | 3.11 ms | **2.32 ms** |

The 1-thread CPU number is the combined per-pixel tick (noise, static sample,
burn-in, scanlines). Skipping pixels outside the tube, and skipping the
burn-in sample once it has faded, is what moves it. On this llvmpipe the GL
tick is quicker in absolute terms than the 12.9 ms figure from the review;
the cut is the same one (the faded burn-in texture is not sampled) and the
frames stay in the existing CPU-vs-GL PSNR band (`gl_match`). `cpu-hash`
still matches master's pinned hashes, so the C++ pixels are unchanged.

Search of 100 000 lines (`line N the quick brown fox`, 80 columns), RSS of
`lterm-round2-bench`:

| | RSS | libghostty scrollback |
|---|---|---|
| Compressed, before the search | 11.7 MB | 1.87 MB |
| Search index built (10 000 matches) | 88.7 MB | 69.5 MB |
| Index dropped, recompressed, `malloc_trim` | **12.4 MB** | 1.87 MB |

The search decompresses the scrollback and holds it. Closing the search
recompresses and `malloc_trim` returns about **76 MB**. `malloc_trim` also
runs when ordinary output goes idle, not only after a flood larger than 1 MiB.

Screenshots are the CPU renderer, 868×652, the same command, Xvfb. Amber's
phosphor noise moves between frames, so the pictures are not byte-identical.
Mean absolute error is 0.13 grey levels (amber) and 0.05 (IBM DOS). No
scrollbar is drawn on either; it stays off for CRT profiles.

| | Before | After |
|---|---|---|
| Amber | [before-amber.png](before-amber.png) | [after-amber.png](after-amber.png) |
| IBM DOS | [before-ibm.png](before-ibm.png) | [after-ibm.png](after-ibm.png) |

Commands:

    LIBGL_ALWAYS_SOFTWARE=1 build/lterm-m1-bench --tick-idle --profile amber --size 868x652 --quality full --runs 15
    build/lterm-round2-bench
