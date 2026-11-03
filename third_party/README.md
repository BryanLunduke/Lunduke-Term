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
