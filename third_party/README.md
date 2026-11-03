<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Third-party code

## libvterm 0.3.3 (`third_party/libvterm/`)

- **What:** libvterm, the VT220/xterm terminal emulator library by Paul Evans. Lunduke Terminal
  uses it to parse terminal output and keep the screen and scrollback.
- **Upstream:** <https://www.leonerd.org.uk/code/libvterm/> (also <https://launchpad.net/libvterm>).
- **Version:** 0.3.3, imported **verbatim** from `libvterm-0.3.3.tar.gz`, sha256
  `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`. That is byte-identical to
  Debian's `libvterm_0.3.3.orig.tar.gz`, the source of the `libvterm0` package (0.3.3-3 has no
  Debian patches).
- **Licence:** MIT, `third_party/libvterm/LICENSE`, Copyright 2008 Paul Evans
  <leonerd@leonerd.org.uk>. `src/unicode.c` also contains Markus Kuhn's `wcwidth()` tables
  (2007-05-26) under his permissive notice: "Permission to use, copy, modify, and distribute
  this software for any purpose and without fee is hereby granted. The author disclaims all
  warranties with regard to this software."
- **Pristine:** nothing under `third_party/libvterm/` is edited. `libvterm-SHA256SUMS` lists every
  file of the tarball, and the `libvterm-pristine` test checks the tree against it.
- **Build:** by default meson builds it as the static library `vterm-bundled` (C99, `-Wall`, as
  upstream does), so the binary no longer needs `libvterm0`. `src/screen.c` is the only patched
  file. `tools/libvterm_patch.py` copies it into the build directory as `libvterm-screen.c` and
  applies the patches listed in `libvterm-patches/series` with `patch --fuzz=0`; a patch that no
  longer fits stops the build. `-Dlibvterm=system` builds against the installed libvterm
  (>= 0.3) instead, without the patches.
- **Patches** (`third_party/libvterm-patches/`, in `series` order; each file says what it changes
  and why):
  - `0001-screen-rep-wide-glyph-at-right-margin.patch`: REP (`CSI b`) of a wide glyph can start
    the glyph in the last column; stock `putglyph()` then writes the right half through a NULL
    cell (`screen.c:190`) and damages one column past the screen. The patch stops at the margin
    and clamps the damage rect. Found by `tools/fuzz/vterm_fuzz`
    (`tests/data/fuzz/crash-46ca723d-libvterm-rep.bin`); test `libvterm-rep`.
  - `0002-screen-ring-rotation-for-full-screen-scroll.patch`: a whole-screen scroll up (every
    line of `seq`, `cat`, ...) moved every remaining row of the cell buffer. Each buffer now has
    a row offset (`ringtop`) that `getcell()` adds, and such a scroll only advances it; partial
    scrolls are unchanged, and `resize_buffer()` puts the rows back in order first. The vacated
    rows get the old bottom rows' contents, as stock's move leaves them, because erasing keeps
    `chars[1..]` and `get_cell()` can show them. Output is byte-identical to stock: tests
    `libvterm-t-patched` (upstream's `t/` suite), `libvterm-diff` and `libvterm-diff-repfix`
    (randomised escape-sequence streams, stock vs patched) and `libvterm-feed-diff` (sources and
    `seq`); long runs in `docs/bench/perf2/`.
- **Test-only builds:** `vterm-stock` (pristine `screen.c`) and `vterm-repfix` (0001 only) are
  built for those tests and are not linked into the program.
- **Stock bugs the patches do not fix** (both builds have them; the session guards against them,
  `docs/FUZZING.md`): F1–F3, F7, F9, F10, F15, F17, and two found by the differential test: a
  double-width line on a 1-column screen writes through a NULL cell, and more than 5 combining
  marks after one base (or marks starting a later write after the cursor moved) make a width-0
  glyph that REP repeats forever (an F7 variant).
- Local changes only; nothing has been sent upstream.
