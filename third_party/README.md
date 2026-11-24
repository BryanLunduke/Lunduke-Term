<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Third-party code

## libghostty-vt 1.0.1

Lunduke Terminal's VT engine is [libghostty-vt](https://lib.ghostty.org/vt/) 1.0.1
(MIT, stable C ABI). It is **not** vendored as source. `tools/build-libghostty-vt.sh`
downloads the release tarball and its Zig dependency tarball, checks the SHA256
pins in `third_party/ghostty-vt.pins`, and builds a static library with Zig 0.17.1
(also pinned). The archive is linked into `lunduke-terminal`, so the package
depends on nothing new at runtime (the library itself needs only libc and libm).

Tarballs are cached in `third_party/cache/` (gitignored) or `$LTERM_TARBALL_CACHE`.
A second build with the cache filled does not use the network. Copy the three
tarballs named in the script into that directory to build offline.

The build is `zig build -Doptimize=ReleaseFast -Dcpu=baseline --system <deps>`.
Zig's static library is position-independent. Measured on this pin: stripped
`libghostty-vt.so` 2.4 MB, static `libghostty-vt.a` 22 MB.

Scrollback, reflow, and key, mouse, paste, and focus encoding are libghostty's.
The painter, effects, profiles, and window code are unchanged in role. Cell
attributes (bold, italic, faint, strike, overline, underline styles, SGR 58)
are engine-neutral and drawn by `GridPainter`.
