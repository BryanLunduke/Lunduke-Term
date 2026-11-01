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
