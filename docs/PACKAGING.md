# Lunduke Terminal — packaging notes (M2, item 6)

This is **packaging prep only**. Nothing here has been built as a `.deb` and nothing touches
lcos-live, apt repositories or the ISO. The `debian/` directory is a starting point for whoever
packages it for LCOS.

## What `meson install` puts where

`meson setup build --prefix=/usr && meson install -C build --destdir "$PWD/stage"` installs:

| Path | From |
|---|---|
| `/usr/bin/lunduke-terminal` | `src/main.cpp` and friends |
| `/usr/share/applications/org.lunduke.LundukeTerminal.desktop` | `data/` |
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
  `appstreamcli` are on `PATH`. Both are skipped (not failed) when they are missing.

## x-terminal-emulator

`debian/lunduke-terminal.postinst` registers `/usr/bin/lunduke-terminal` as an
`x-terminal-emulator` alternative with priority 20, below xfce4-terminal and gnome-terminal.
Debian policy 11.8.3 wants `-e command [args]` and `-T title`. Both are supported (`-T` was added
for this).

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
          fonts-proggy
```

| Package | License | Used by |
|---|---|---|
| fonts-dejavu-core / fonts-dejavu-mono | Bitstream-Vera + PD | `plain` (default) and last fallback for every profile. Hard dependency, so the default never lands on a proportional font. |
| fonts-terminus (TTF, family `Terminus (TTF)`) or fonts-terminus-otb (`Terminus`) | OFL-1.1 | amber, green, green-scanlines, vintage, futuristic, paper-white (the CRT-1.2.0-style profiles) |
| fonts-pc (`Px IBM VGA8`, …) | CC-BY-SA-4.0 | ibm-dos |
| fonts-3270 (`IBM 3270`) | BSD-3-Clause OR OFL-1.1 | ibm-3278 |
| fonts-hermit (`Hermit`) | OFL-1.1 | optional font choice in the settings dialog / `--font Hermit` |
| fonts-proggy (`ProggyTinyTT`) | MIT | optional font choice / `--font ProggyTinyTT` |

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

See `debian/control`: meson, ninja, pkgconf, libgtkmm-3.0-dev (>= 3.24), libvterm-dev (>= 0.3),
libepoxy-dev, libegl-dev, libfontconfig-dev and libx11-dev (view_test). For `dh_auto_test`
(skipped with `nocheck`), it also needs xvfb, xauth, libgl1-mesa-dri (llvmpipe for gl-match) and
fonts-dejavu-core.

`debian/rules` runs the tests under `xvfb-run -a` with `--no-parallel`. view_test is built with
AddressSanitizer and needs `$DISPLAY`.

## Not done (on purpose)

- No `dpkg-buildpackage` or `sbuild` run, no lintian, no upload, no apt or ISO changes.
- `debian/source/format` is `3.0 (native)` for now. A real LCOS package will probably want
  `3.0 (quilt)` with a release tarball.
- The icon (M3) is an original drawing: a CRT with an amber `>_` prompt, hand-written SVG paths,
  CC0-1.0, no third-party artwork. Only the scalable SVG is installed; PNG previews at
  16/32/48/128/256 were rendered for review outside the repo and are not shipped.
