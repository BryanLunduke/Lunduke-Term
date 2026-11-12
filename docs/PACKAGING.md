# Lunduke Terminal — packaging notes

Version **1.1.0** (Debian package **1.1.0-1**) is the first release and the default terminal
on LCOS 1.1.0, in place of Cool Retro Term. `packaging/build-deb.sh` builds the binary `.deb`
(`VERSION=1.1.0` in that script; the changelog revision is `-1`). Nothing here is uploaded,
and this repository does not change lcos-live, apt or the ISO.

## What `meson install` puts where

`meson setup build --prefix=/usr && meson install -C build --destdir "$PWD/stage"` installs:

| Path | From |
|---|---|
| `/usr/bin/lunduke-terminal` | `src/main.cpp` and friends |
| `/usr/share/applications/org.lunduke.LundukeTerminal.desktop` | `data/` (`Categories=System;TerminalEmulator;GTK;`) |
| `/usr/share/xfce4/helpers/lunduke-terminal.desktop` | `data/xfce4/helpers/` (XFCE preferred terminal) |
| `/usr/share/man/man1/lunduke-terminal.1` | `data/lunduke-terminal.1.in` (the `.deb` ships it gzipped) |
| `/usr/share/metainfo/org.lunduke.LundukeTerminal.metainfo.xml` | `data/` (AppStream, CC0-1.0) |
| `/usr/share/icons/hicolor/scalable/apps/org.lunduke.LundukeTerminal.svg` | `data/icons/` (original M3 icon, CC0-1.0) |
| `/usr/share/doc/lunduke-terminal/{README.md,FONTS.md,PACKAGING.md}` | repo |

The benches (`lterm-m0-bench`, `lterm-m1-bench`) and tests are not installed.

- The application id is `org.lunduke.LundukeTerminal`. It is the `Gtk::Application` id, the
  `.desktop` file name, the AppStream `<id>` and the icon name.
- `StartupWMClass=lunduke-terminal` is the default GTK WM_CLASS (the program name).
- `appstreamcli validate --pedantic` has one pedantic note: the id contains upper-case letters.
  The id was chosen by the project, so the note stays.
- Validation runs as meson tests `desktop-file` and `metainfo` when `desktop-file-validate` and
  `appstreamcli` are on `PATH`. They are skipped (not failed) when the tools are missing.
  `desktop-file-validate` rejects `Type=X-XFCE-Helper` (it rejects XFCE's own helpers the same
  way), so `xfce-helper` checks the keys exo reads instead. `man-page` checks that the
  installed page names this version.

## Default terminal

Wiring for the lcos-live image (this package does not edit that repository):

| Piece | Name |
|---|---|
| Binary | `/usr/bin/lunduke-terminal` |
| Package | `lunduke-terminal` `1.1.0-1`, `Provides: x-terminal-emulator` |
| Alternative | `x-terminal-emulator` → `/usr/bin/lunduke-terminal`, priority **40** |
| Man-page slave | link `/usr/share/man/man1/x-terminal-emulator.1.gz`, slave name `x-terminal-emulator.1.gz`, path `/usr/share/man/man1/lunduke-terminal.1.gz` |
| Menu / panel launcher | `/usr/share/applications/org.lunduke.LundukeTerminal.desktop` |
| XFCE helper | `/usr/share/xfce4/helpers/lunduke-terminal.desktop` |
| helpers.rc | `TerminalEmulator=lunduke-terminal` |

`debian/lunduke-terminal.postinst` registers the alternative on `configure`; `prerm` removes it
on `remove` or `deconfigure` (the slave goes with the master). Priority 40 is the priority
gnome-terminal and xfce4-terminal use, above xterm's 20, so this terminal is the automatic
choice when those are not installed. Debian policy 11.8.3's suggested priority is 20; 40 is
the desktop-terminal priority, which is what makes it the default.

The helper's `X-XFCE-Category` is `TerminalEmulator`. `X-XFCE-Commands` is `%B;` (just the
binary). `X-XFCE-CommandsWithParameter` is `%B -x %s;`. For a terminal helper, `%s` is a
command (`exo-open --launch TerminalEmulator htop`), not a directory. Thunar's "Open Terminal
Here" action is `exo-open --working-directory %f --launch TerminalEmulator`: exo changes to
that directory and then runs `X-XFCE-Commands`, so the terminal inherits the directory. A
caller that execs the binary itself can pass the directory with `--working-directory` or
`--workdir`.

Flags those callers use, all accepted (`-e` was already there):

| Flag | Meaning |
|---|---|
| `-e COMMAND [ARG...]` | rest of the line is the command, passed to `execvp` (must be last) |
| `-x COMMAND [ARG...]`, `--execute COMMAND [ARG...]` | same as `-e` (the XFCE helper's form) |
| `-T TITLE`, `--title TITLE`, `--title=TITLE` | window title |
| `--cwd DIR`, `--workdir DIR`, `--working-directory DIR` | working directory; `=` forms of all three work too |

## x-terminal-emulator

See the table above. Policy 11.8.3 requires `-e command [args]` and `-T title`, a
`Provides: x-terminal-emulator`, and a working man-page slave. The slave path is the gzipped
page `dh_compress` writes; `update-alternatives` skips a slave whose file is missing, so the
page is installed by `meson install` (`data/lunduke-terminal.1.in`).

## Font dependencies

The licences were checked in [FONTS.md](FONTS.md) from each Debian package's `copyright` file.
Only license-clean fonts are pulled in. **No Apple II (PrintChar21/PRNumber3), Commodore PET
(PetMe) or C64 Pro Mono fonts**, and no other font that Debian excludes from cool-retro-term.
No profile names those families, and `profiles_test` fails if one does.

```
Depends: ${shlibs:Depends}, ${misc:Depends},
         fonts-dejavu-core | fonts-dejavu-mono
Recommends: fonts-terminus | fonts-terminus-otb,
            fonts-pc,
            fonts-3270
Suggests: fonts-hermit,
          fonts-proggy,
          fonts-atarismall,
          fonts-atarist,
          fonts-amiga
```

| Package | License | Used by |
|---|---|---|
| fonts-dejavu-core / fonts-dejavu-mono | Bitstream-Vera + PD | `plain` and last fallback for every profile, including the default `amber` (M5). Hard dependency, so no profile ever lands on a proportional font. |
| fonts-terminus (TTF, family `Terminus (TTF)`) or fonts-terminus-otb (`Terminus`) | OFL-1.1 | amber, green, green-scanlines, vintage, futuristic, paper-white (the CRT-1.2.0-style profiles) |
| fonts-pc (`Px IBM VGA8`, …) | CC-BY-SA-4.0 | ibm-dos |
| fonts-3270 (`IBM 3270`) | BSD-3-Clause OR OFL-1.1 | ibm-3278 |
| fonts-hermit (`Hermit`) | OFL-1.1 | `modern` (M5); also `--font Hermit` |
| fonts-proggy (`ProggyTinyTT`) | MIT | `modern`'s second choice (M5); also `--font ProggyTinyTT` |
| fonts-atarismall (`"Atari800"`) | permissive, non-SPDX (one legal read advised, FONTS.md) | `pixel-8bit` (M5) |
| fonts-atarist (`Atari`) | BSD-3-Clause | `pixel-hires-mono` (M5) |
| fonts-amiga (`Amiga`) | OFL-1.1 | `pixel-16bit` (M5) |
| fonts-sixtyfour (`C-64`) | OFL-1.1 | `pixel-40col` (M5, editor decision) |

- fonts-terminus, fonts-hermit and fonts-proggy are built from Debian's `cool-retro-term`
  source, which is repacked as `+ds2` without the non-free Kreative and Style64 fonts.
- fonts-terminus's copyright file has a gap, described in FONTS.md: it has no stanza for the TTF,
  but the font itself says OFL-1.1. `fonts-terminus-otb` is the alternative with a clean copyright
  file.
- Recommends rather than Depends: every profile falls back to DejaVu Sans Mono, so a missing
  retro font is a cosmetic problem, not a broken package.
- fonts-pc and fonts-3270 are already on the LCOS 1.0 ISO. fonts-terminus is not, and it is the
  one font the CRT-style profiles really want. Sizes, licence and options for adding it are in
  [`TERMINUS-ON-ISO.md`](TERMINUS-ON-ISO.md) (a proposal only; nothing was changed on the ISO).

## Build dependencies

See `debian/control`: meson, ninja, pkgconf, patch, python3, libgtkmm-3.0-dev (>= 3.24),
libepoxy-dev, libegl-dev, libfontconfig-dev and libx11-dev (view_test). For `dh_auto_test`
(skipped with `nocheck`), it also needs xvfb, xauth, libgl1-mesa-dri (llvmpipe for gl-match) and
fonts-dejavu-core.

`debian/rules` runs the tests under `xvfb-run -a` with `--no-parallel`. view_test is built with
AddressSanitizer and needs `$DISPLAY`.

libvterm is bundled (`third_party/libvterm`, 0.3.3, MIT; patches in `third_party/libvterm-patches`,
applied at build time) and linked statically, so the package no longer depends on `libvterm0`.
`debian/copyright` has its stanza. `-Dlibvterm=system` (with `libvterm-dev`) is still possible, but
then the REP fix and the scroll speed-up are missing.
A `-Dlibvterm=system` build still has the REP crash: a NULL dereference at screen.c:190 when REP
repeats a wide character at the right margin (patch 0001, test `libvterm-rep`).

## Build

`packaging/build-deb.sh` checks that `VERSION`, `meson.build`, `debian/changelog` (`VERSION-1`)
and the AppStream `<release>` agree, then runs `dpkg-buildpackage -us -uc -b`. The `.deb`
lands in the parent directory. The source format stays `3.0 (native)`; a native *source*
build rejects a Debian revision, so the script builds the binary package only. The package
version is still `1.1.0-1`.

## Not done (on purpose)

- No upload, no apt repository and no ISO / lcos-live change.
- `debian/source/format` is still `3.0 (native)`. A quilt package with an orig tarball can
  wait until the archive layout wants one.
- The icon (M3) is an original drawing: a CRT with an amber `>_` prompt, hand-written SVG paths,
  CC0-1.0, no third-party artwork. Only the scalable SVG is installed; PNG previews at
  16/32/48/128/256 were rendered for review outside the repo and are not shipped.
