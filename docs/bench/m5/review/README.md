# M5 review fixes: before/after measurements

All REAL, measured on the shared build box (other agents' load varies), Xvfb :44 1024x768,
Mesa llvmpipe software GL (**not a GPU**; no GPU on this box). Times CST. Tool sources in
`tools/m5_*_bench.cpp`; "before" is the M5 branch before the respective fix.

## Item 1: output flood (`lterm-m5-flood-bench`)

A 10 ms timer at default priority stands in for key events; its longest gap is the longest
main-loop freeze. For `yes`, Ctrl+C (byte 0x03 via the session, as the key handler sends it) is
due at 2 s.

| Run | Before (badd3f9) | After (10 ms slices) |
|---|---|---|
| `yes`, plain | frozen; killed by `timeout 120`, the timer never ran again, Ctrl+C never sent | longest gap 19 ms, p99 16 ms; Ctrl+C sent at 2009 ms, child exited 4 ms later |
| `yes`, amber | timer ran once in 49.5 s (gap 49516 ms); Ctrl+C sent 47.5 s late, child still running at the end | longest gap 103 ms, p99 20 ms; Ctrl+C sent at 2013 ms, exited 3 ms later |
| `seq 1 3000000`, plain, 15 s | longest gap 10001 ms; 1 content frame | longest gap 18 ms, p99 14 ms; 485 content frames (32/s) |
| `seq 1 3000000`, amber, 15 s | longest gap 5657 ms; 1 content frame | longest gap 111 ms, p99 18 ms; 292 content frames (19.5/s) |

`yes` prints the same screen every frame, so its "content frames" (frames with a changed row)
stay low after the fix too; `seq` is the frame measure. The amber gaps of ~100 ms are single
llvmpipe effect frames, not PTY reads. Raw: `item1-{before,after}-*.txt`.

## Item 5: cursor blink (`lterm-m5-idle-bench blink`)

One focused view at a shell prompt, cursor blink on, 1024x640, terminal process CPU (all its
threads, llvmpipe's included) per 10 s window. Before: 38acc4e. After: blink stops (cursor
solid) after GTK's `gtk-cursor-blink-timeout` (10 s default) without a key, output or focus-in,
like GTK entries; any of those restarts it.

| Config | Before 0-10 / 10-20 / 20-30 s | After 0-10 / 10-20 / 20-30 s |
|---|---|---|
| amber, GL, static quality | 11.5 / 11.9 / 12.1 % (19 frames each) | 9.0 / 0.0 / 0.0 % (15 / 0 / 0 frames) |
| amber, C++ path, static | 9.5 / 9.1 / 9.0 % (19 each) | 6.1 / 0.0 / 0.0 % (13 / 0 / 0) |
| amber, GL, auto (= reduced on llvmpipe) | 29.0 / 30.4 / 28.9 % | 28.4 / 17.6 / 17.8 % |
| plain | 0.1 / 0.1 / 0.1 % | 0.1 / 0.0 / 0.0 % |

Limit (partial fix): during the first 10 s every blink phase still runs the whole effects pass
(~0.5 s CPU per second of blinking on llvmpipe). Rendering only the cursor cell, or caching the
two blink states, was not done (curvature, bloom and the bezel spread the cursor over many
pixels). "auto" keeps ~17.7 % after the blink stops: that is the reduced-quality animation
(noise/flicker ticks), not the cursor; `static` quality removes it. Raw: `idle-{before,after}-blink-*.txt`.

## Item 7: hidden tabs (`lterm-m5-idle-bench tabs`)

Tab 0 prints 20 numbered lines every 20 ms; tab 1 is an idle shell. "hidden" shows tab 1.
Content frames are tab 0's.

| Config | Before: CPU, tab-0 frames per 10 s | After |
|---|---|---|
| plain, tab 0 shown | 6.8 %, 476 | 7.2 %, 476 |
| plain, tab 0 hidden | 5.2 %, 475 (painted while hidden) | 0.9 %, 0 |
| amber (auto = reduced), tab 0 shown | 187 %, ~300 | 203 %, ~298 |
| amber (auto), tab 0 hidden | 21.6 %, 477 (painted while hidden) | 19.5 %, 0 |

The amber "hidden" rows include the shown idle amber tab's own reduced-quality animation
(~17.7 %, see item 5); the hidden tab's share is DERIVED as ~3.9 % before vs ~1.8 % after
(parsing only). Static quality isolates it (no animation on the shown tab): amber, tab 0 hidden, static:
3.6 %, 476 frames painted while hidden (before) vs 0.8 %, 0 frames (after); shown, static:
161 % vs 170 % (noise of this shared box). Raw: `idle-{before,after}-tabs-amber-*-static.txt`
(written by the item 3 run). A hidden view now only
parses; its changed rows are painted once when it is shown again (test `review`:
testHiddenTab). Shown-tab numbers vary by ±10 % between runs on this shared box. Raw:
`idle-{before,after}-tabs-*.txt`.

## Item 3: effects memory of hidden tabs (`lterm-m5-idle-bench rss`)

1280x800 window, quality static, N amber (or plain) tabs; each tab is shown 1.5 s, then tab 0
for 6 s; then VmRSS/VmHWM of the process (REAL). "GL textures" is what our pipelines allocated
(all tabs). Before: 38acc4e. After: hidden for 3 s (`ViewOptions::effectsFreeHiddenMs`), a tab
frees its GL frame-sized textures (programs stay; burn-in history restarts on show) or the C++
path's float buffers and worker threads; showing it rebuilds them. All float textures are
RGBA16F instead of RGBA32F (gl-match against the C++ reference: min PSNR 60.68 dB, threshold 40).

| Profile | 1 tab | 4 tabs | 8 tabs | Per extra tab (DERIVED, (8-1)/7) | GL textures 1 / 8 tabs |
|---|---|---|---|---|---|
| plain, before | 53.3 MB | 72.0 MB | 91.7 MB | 5.5 MB | 0 |
| plain, after | 53.5 MB | 71.8 MB | 91.7 MB | 5.5 MB | 0 |
| amber GL, before | 230.2 MB | 468.5 MB | 783.8 MB | **79.1 MB** | 58.7 / 449.1 MB |
| amber GL, after | 202.4 MB | 304.3 MB | 497.9 MB | **42.2 MB** | 31.3 / 43.9 MB |
| amber C++ path, before | 124.9 MB | 333.5 MB | 610.5 MB | **69.4 MB** (28 threads) | 0 |
| amber C++ path, after | 124.8 MB | 272.6 MB | 365.5 MB | **34.4 MB** (7 threads) | 0 |

The review measured ~77 MB per amber tab (GL); this box gives 79.1 MB before and 42.2 MB after
(-47 %). Limits:
- The remaining ~37 MB per hidden amber GL tab over plain is mostly the GtkGLArea's own GL
  context and llvmpipe's per-context memory, plus each hidden tab's 2 MB noise texture; freeing
  the context itself (unrealizing the GLArea) was not done.
- The *shown* tab is not lighter except for RGBA16F (1 tab: 230 -> 202 MB); it still needs
  its full buffers.
- VmHWM stays higher than VmRSS (e.g. 585 MB at 8 C++-path tabs): the peak is while tabs are
  shown in turn before they are freed. RSS only falls where glibc returns the memory.
- The C++ path's buffers are freed when hidden, not made lighter (still float RGBA).
- llvmpipe, not a GPU: a GPU driver keeps textures in VRAM, so RSS there will differ (not
  measured).
Raw: `item3-rss-{before,after}.txt`. Test: `review` testHiddenFrees (gl: 5.9 MB -> 2.0 MB hidden
-> 5.9 MB shown; cpu: buffers freed and rebuilt).
