<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# libghostty-vt 1.0.1 vs libvterm 0.3.3

Same machine, three runs, medians. No GPU: the GL path is **llvmpipe** (LLVM 20.1.2, 256-bit), which is the software-rendering case this terminal is aimed at.

- **master:** `657d4bf`, meson release, bundled libvterm 0.3.3.
- **this branch:** meson release, libghostty-vt 1.0.1 linked statically (`zig build -Doptimize=ReleaseFast -Dcpu=baseline`, Zig 0.17.1).
- Window 1280×800. Plain profile is effects off. Amber profile is effects on (GLSL).
- RSS/PSS from `/proc/<pid>/status` and `smaps_rollup` after the child reached `sleep`, except the idle-bench RSS row which is `lterm-m5-idle-bench rss` (VmRSS only).
- Throughput is `lterm-m5-throughput-bench cat`: wall time until a sentinel is on screen. ASCII file is 49.5 MB of short lines. The Unicode file is 8.1 MB of emoji, CJK, and color/underline escapes.
- Package sizes are the Debian binary packages (`dpkg-buildpackage -b`, tests skipped, `g++`, hardening on, `dh_strip`).

## Memory

| case | master RSS | master PSS | libghostty RSS | libghostty PSS |
|---|---:|---:|---:|---:|
| idle, plain, CPU | 46.2 MiB | 24.2 MiB | 47.6 MiB | 25.7 MiB |
| idle, amber, GL | 167.7 MiB | 113.7 MiB | 169.1 MiB | 115.1 MiB |
| 5,000 lines, plain | 46.1 MiB | 24.2 MiB | 50.6 MiB | 28.7 MiB |
| 5,000 lines, amber | 168.3 MiB | 114.3 MiB | 172.4 MiB | 118.4 MiB |
| 100,000 lines, plain | 57.4 MiB | 35.3 MiB | 109.6 MiB | 87.7 MiB |
| 100,000 lines, amber | 181.0 MiB | 127.0 MiB | 231.2 MiB | 177.3 MiB |

Idle-bench VmRSS (one tab, 1280×800, no scrollback fill): plain 49.2 MiB vs 50.5 MiB, amber GL 183.5 MiB vs 184.9 MiB (master vs libghostty).

libghostty keeps history in pages, so a 100,000-line scrollback costs about 50 MiB more RSS than the old 16-byte packed cells. At the default 5,000 lines the difference is a few megabytes. Amber's resident set is dominated by the llvmpipe textures either way.

## Throughput and frames

| workload | master | libghostty-vt |
|---|---:|---:|
| startup, 1-line cat, plain (time to sentinel) | 0.02 s | 0.02 s |
| 49.5 MB ASCII, plain (effects off) | 2.77 s, 17.9 MB/s | 0.90 s, 54.7 MB/s |
| 49.5 MB ASCII, amber (effects on) | 3.44 s, 14.4 MB/s | 1.19 s, 41.4 MB/s |
| 8.1 MB Unicode/color, plain | 0.29 s, 28.9 MB/s | 0.14 s, 62.0 MB/s |
| 8.1 MB Unicode/color, amber | 0.44 s, 19.0 MB/s | 0.25 s, 34.0 MB/s |
| frame rate during the ASCII flood, amber | 8.2 /s (~122 ms) | 7.0 /s (~143 ms) |
| idle CPU, plain, 10 s | 0.0% of one core | 0.1% |
| idle CPU, amber GL, 10 s | 5.6% | 5.7% |

The 0.02 s startup figure is the 10 ms timer's floor; the two builds match. During an amber flood both builds stay under the 10 frame/s cap. libghostty finishes the flood sooner, so it paints fewer frames in total. Idle CPU on amber is the software GL path, not the parser.

## Package size

| | master (libvterm) | libghostty-vt 1.0.1 |
|---|---:|---:|
| `.deb` file | 271 KiB | 1.10 MiB (1,157,268 bytes) |
| Installed-Size | 747 KiB | 3278 KiB |
| stripped `/usr/bin/lunduke-terminal` | 696 KiB | 3.2 MiB (3,303,888 bytes) |

`ldd` on the libghostty package shows no `libghostty`. New runtime dependencies: none. The static archive before linking is 22 MB; the stripped shared library built from the same pin is 2.4 MB, which is what the stripped executable grew by.
