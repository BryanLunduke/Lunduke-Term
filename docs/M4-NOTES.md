<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# M4 notes (items 1-4)

Branch `m4-core`, on top of M3 master `d7a737e`. Box: no `/dev/dri`, so every GL number here is
Mesa llvmpipe (Mesa 25.0.7, LLVM 19.1.7) software rendering. These notes make no GPU claims. Labels:
**REAL** means measured, with the raw data path given. **DERIVED** means arithmetic on REAL numbers.
**NOT MEASURED** means what it says.

## Item 1: test robustness under ASan+UBSan

| Sub-item | Change | Evidence |
|---|---|---|
| 1(a) | `tests/sanitizer.hpp` (`LTERM_ASAN` from `__SANITIZE_ADDRESS__`/`__has_feature`). The search worst-slice bound is 250 ms under ASan and 60 ms otherwise. The 10000-line findAll bound is 6000 ms under ASan and 2000 ms otherwise. | M3 failure was 75.8 ms. 2025 ms was seen under ASan; release is 23.7 ms (REAL). |
| 1(b) | view_test uses `searchSync(v,"ME",true)`, so it waits for the rebuilt index after the case switch. | Tests pass. |
| 1(c) | The vterm reflow test requires a row that is exactly `abcdefghij`. | REAL: with `clearTopContinuation` disabled, both halves fail (`docs/bench/m4/item1/vterm-without-fix.txt`). The source was restored. |
| 1(d) | `tests/lsan.supp` (fontconfig, libgallium, libGLX_mesa, gdk toplevel list) is wired into the meson test env for every test. The env is `detect_leaks=1`, `abort_on_error=1`, `halt_on_error=1`. view_test also uses `fast_unwind_on_malloc=0`, because glib and fontconfig have no frame pointers. | REAL: before, there were 8 fontconfig reports per GL/painter test and view leaked 3.69 MB in Mesa/GDK caches. After, there are 0 `leak of` lines (`docs/bench/m4/item1/lsan.txt`). `docs/ci/ci.yml` still says `detect_leaks=0`, but the meson env overrides it; ci.yml was not touched. |
| 1(e) | SPDX lines added to `docs/bench/m3-item2/vm-*/vm.sh`. Every other script there already had one. | |
| 1(f) | view_test prints `ASan+UBSan build`, `release build, ASan in this test only` or `release build`, from `-DLTERM_SANITIZED_BUILD` (meson, when `b_sanitize != none`) plus `LTERM_ASAN`. | Checked in both builds. |

## Item 2: effects governor

- A probe now needs **3 consecutive** good windows; an over-budget window resets the count. Before
  this, good,bad,good,bad,good passed.
- A probe is **aborted** after its first frame when that frame costs more than `probeAbortFrameMs`.
  The default is 2 × budget × the 1 fps interval, which is 500 ms. The back-off is kept (10, 20,
  40, 80 s and beyond).
- governor_test REAL (`docs/bench/m4/item2/tests.txt`):
  - With the reset line disabled, 3 failures.
  - A 3900 ms frame aborts after 1 frame, and the next delay doubles.
  - A 400 ms frame does not abort.
- view_test REAL: the costly probe is aborted after 1 frame, for both auto and cpu.
- Limit: a frame that is already running cannot be interrupted, so a probe still costs at least one
  frame. See item 3 for what this means in the VM.

## Item 3: re-run on the M4 build (code 95fd762; M3 numbers were on 69598c8, before the libvterm reflow fix)

### TCG VM (REAL, `docs/bench/m4/item3/vm/`)
Setup: qemu 10.0.13 `-accel tcg` (no KVM), 4 vCPU, 2 GB, std VGA. The LCOS live ISO was opened
read-only (`readonly=on` plus `-snapshot`). Its sha256 was `33fc871b…4706e61` both before and
after the run. Each run measures process CPU (100 % = one vCPU) after a 40 s warm-up.

| Run | M3 (69598c8) process / guest busy | M4 (95fd762) process / guest busy | M4 RSS |
|---|---|---|---|
| guest baseline, no terminal | n/a / 30.4 % | n/a / 30.1 % | |
| plain, 20 s | 0.0 % / 29.0 % | 0.1 % / 27.7 % | 50.7 MB |
| amber GL auto, 60 s | 72.2 % / 44.9 % | 57.5 % / 41.8 % | 284.5 MB |
| amber CPU auto, 60 s | 16.7 % / 14.0 % | 8.2 % / 2.4 % | 101.2 MB |
| amber GL auto, 180 s (new) | NOT MEASURED | 25.5 % / 6.7 % | 274.0 MB |

What the governor did (REAL, from the guest logs with uptime stamps, `out/idle.txt`):
- **GL:** probes cost 3629–4356 ms per frame, and each probe demoted 2.3–3.3 s after it started,
  which is one frame. The back-off went 10/20/40/80 s, and 160 s in the 180 s run.
- **CPU:** probes cost 756–916 ms per frame, with the same back-off.

The 10 s CPU trace (REAL, `out/trace-*.txt`) shows **each GL probe costs 13.8–16.5 s of process
CPU** across llvmpipe threads, while the reported frame cost is about 4 s. So the abort keeps a probe
to one frame, but most of a probe's cost is outside the frame it measures. I have not checked
where that time goes; shader/JIT work on the quality switch is a guess. Between probes the process
uses 20–50 ms of CPU per 10 s. Each CPU-renderer probe costs about 4.7 s of process CPU.
- **DERIVED:** the lower M4 percentages come partly from where the probes fall inside each
  measuring window, so comparing these to M3 is rough.
- **NOT MEASURED:** the probe frame count inside the VM. The log line does not print it; the
  timing above is the evidence.

Problems in the VM run:
- Screenshots of the first three idle runs were missed because the driver held QMP's one client
  slot (fixed in `drive-m4.py`, which is now re-runnable).
- The plain run's log and trace files were lost to a tag-naming bug; its `idle.txt` line is intact.
- The guest did not power down cleanly, so I stopped qemu with QMP `quit` after the output was
  synced.

Not re-run: close bench, interactive tabs/search/settings, and virtio-vga.

Screenshots in `/workspace/lcos-app-reviews/terminal-m4/`:
- `m4-vm-boot.png`
- `m4-vm-tty-login.png`
- `m4-vm-idle-amber-gl-auto-long.png`

### Host matrix (REAL, `docs/bench/m4/item3/host/run/matrix.txt`)
Setup: Xvfb, llvmpipe, `effects-quality=auto`, 20 s warm-up then 20 s of process CPU, the same
script as M3. Every effects profile settles at quality=reduced, at 16–21 % of one core on either
renderer. That is the same range as M3:

| Profile | M3 GL | M4 GL | M3 CPU | M4 CPU |
|---|---|---|---|---|
| plain | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| amber | 16.7 % | 18.3 % | 19.2 % | 18.5 % |
| green | 16.5 % | 20.5 % | 21.2 % | 18.4 % |
| green-scanlines | 18.2 % | 17.8 % | 19.2 % | 19.5 % |
| ibm-dos | 20.1 % | 19.8 % | 16.4 % | 16.4 % |
| ibm-3278 (static) | 0.0 % | 0.0 % | 0.0 % | 0.0 % |
| vintage | 20.6 % | 15.9 % | 16.4 % | 21.0 % |
| futuristic | 17.2 % | 18.4 % | 22.4 % | 17.9 % |
| paper-white | 17.6 % | 20.4 % | 21.2 % | 21.2 % |

"Invalid UTF-8" counts: plain had 21 (GL) and 9 (CPU) on 69598c8. On M4 every profile has 0. That
is the libvterm reflow fix at work. GPU: NOT MEASURED (there is none).

Final tests (REAL, `docs/bench/m4/tests/final.txt`): release 13/13 OK, ASan+UBSan with leak checking 13/13 OK, 0 leak reports.

## Item 4: soak heap profile (the 222 → ~500 MB rise)

### Method
- `lunduke-terminal --stats-every 5` prints a `stats-mem` line every 5 s. It contains mallinfo2
  (arena, mmap, in-use, free, releasable), plus the sum over tabs of:
  - scrollback lines and bytes
  - glyph-cache surfaces and bytes
  - live GL pipeline textures and bytes (from what we allocate)
  - cairo surfaces
  - ViewStats samples
- `docs/bench/m4/item4/soak4.sh` is the M3 soak workload: each loop opens 3 tabs, types, closes 3
  and resizes 4 times. It samples `smaps_rollup`, and at 0/30/60 s and at the end it groups
  `/proc/PID/smaps` Rss by mapping (`smaps-group.sh`).
- No heaptrack, valgrind or gdb on the box, and nothing was downloaded. Raw data:
  `docs/bench/m4/item4/raw/`, summary in `runs.txt`.

### RSS per run (REAL, kB; mean over samples at t ≥ 30 s)

| Run | t=0 | mean | max |
|---|---|---|---|
| baseline amber GL, scrollback 10000, 600 s (no fix) | 221932 | 500920 | 502232 |
| scrollback 0, 120 s (no fix) | 205336 | 470683 | 472292 |
| MALLOC_MMAP_THRESHOLD_=131072, 120 s (no fix) | 221300 | 448069 | 448968 |
| amber CPU renderer, 120 s (no fix) | 166888 | 210309 | 211064 |
| plain, 120 s (no fix) | 138800 | 154935 | 157132 |
| **fix** amber GL, scrollback 10000, 600 s | 221696 | **333602** | 342512 |
| fix, scrollback 0, 120 s | 206036 | 298105 | 304704 |
| fix, MALLOC_ARENA_MAX=1, 120 s | 221564 | 337732 | 340860 |

### Contributors (baseline 600 s, averaged by open-tab count; REAL from `stats-mem` unless marked)

| Contributor | 1 tab | 4 tabs (peak) | Notes |
|---|---|---|---|
| malloc in-use (all arenas) | 150.3 MB | 412.7 MB | DERIVED: about +87 MB per extra GL tab |
| of that: GL pipeline textures (RGBA32F at window size) | 34.7 MB | 157.2 MB | about 39 MB per tab at 1024x640 |
| of that: cairo surfaces | 4.3 MB | 19.8 MB | about 5 MB per tab |
| of that: scrollback | 18.8 MB | 17.1 MB | only the busy first tab fills its 10000 lines |
| of that: glyph cache | 0.04 MB | 0.18 MB | negligible; no cap needed |
| other per GL tab: llvmpipe context, GtkGLArea buffers, Mesa state | | | DERIVED by subtraction: about 42 MB per tab, not broken down further |
| **malloc free but kept** | **273.6 MB** | 16.7 MB | releasable at the top of the heap is only ~2.6 MB |
| smaps `[heap]` (main arena) | 49 MB at t=0 | 359 MB at 65 s, 360 MB at 603 s | |
| smaps libLLVM (file-backed) | 56.8 MB | 56.8 MB | constant |
| smaps other libs and files | ~46 MB | ~47 MB | constant |

What the variants showed:
- **Renderer (REAL):** the CPU renderer adds about 45 MB per tab and plain about 6–14 MB. With
  either, RSS stays at 210 or 155 MB.
- **ViewStats samples:** about 1250 per 120 s, 3 vectors at 8 B each, about 30 KB, so negligible and left as is.

### Explanation
While 4 tabs are open, the GL tabs need up to 413 MB of malloc memory, almost all of it in glibc's
**main arena** (brk heap). When the tabs close, in-use drops back to about 150 MB, but the freed
chunks lie below live ones, so glibc cannot shrink the heap. About 270 MB stays resident as free
malloc memory. That is the whole rise, and it is flat afterwards because later tabs reuse it.

It is not a leak:
- In-use with one tab is 150–156 MB at both 5 s and 580 s.
- It is not the arenas: MALLOC_ARENA_MAX=1 changed nothing (337.7 vs 333.6 MB).
- A fixed mmap threshold only saves about 53 MB.
- Scrollback is about 17 MB for 10000 lines of this workload. RSS with 0 lines vs 10000 lines is
  −30 MB without the fix and −36 MB with it (DERIVED; the 120 s and 600 s runs differ in length).
- The glyph cache is under 0.2 MB.

### Fix (95fd762, with test)
`TerminalWindow` runs `malloc_trim(0)` once, 1 s after the last of a burst of tab closes (glibc only).
- REAL 600 s soak: mean RSS goes from 500.9 MB to **333.6 MB** (−167 MB, −33 %), and `[heap]`
  from 360 MB to 192 MB. The 4-tab peak in-use is unchanged at 413 MB.
- What remains above the 222 MB start is fragmentation plus the open tab.
- view_test checks that 19 closes give exactly one trim, after the delay. Under ASan the RSS change is
  about 0, because ASan has its own allocator.

Not done:
- Lighter GL textures, e.g. RGBA16F for `texSrc`. They would cut about 39 MB per tab at peak, but
  the gl-match PSNR would need re-validating. Left for later; NOT MEASURED.
- A per-tab breakdown of llvmpipe's own memory. NOT MEASURED: no heap profiler on the box.
