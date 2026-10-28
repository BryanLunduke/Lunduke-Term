# Terminus on the LCOS ISO: proposal for the editor

Status: **proposal only.** Nothing on the ISO, in lcos-live or in any apt repository has been
changed. This note answers one question: should a Terminus font ship on the LCOS ISO so that Lunduke
Terminal's CRT profiles look as designed?

All numbers come from the Debian trixie `.deb` files, downloaded over https from deb.debian.org into
`/workspace/lterm-tools/debs` and unpacked into a scratch directory with `dpkg-deb -x` (not
installed). The SHA-256 of each file matches the trixie `Packages` index.

## The two packages

| | fonts-terminus | fonts-terminus-otb |
|---|---|---|
| Version (trixie) | 1.2.0+ds2-4 | 4.48-3.1 |
| Debian source | `cool-retro-term` | `xfonts-terminus` |
| Font file | `TerminusTTF-4.46.0.ttf` (scalable TrueType, family `Terminus (TTF)`) | 4 bitmap `.otb` files (normal, bold, oblique, bold-oblique; 6x12 … 16x32), family `Terminus` |
| `.deb` size | 80 672 B | 287 260 B |
| `Installed-Size` (control) | 452 KiB | 1 464 KiB |
| Unpacked, bytes in files | 451 571 B (3 files) | 1 478 457 B (11 files incl. fontconfig snippet) |
| Font data alone | 440 944 B | 1 456 204 B |
| xz -9 of the font data (DERIVED, rough idea of the squashfs cost) | 74 632 B | 266 512 B |
| Depends | none | `xfonts-utils` (already on the ISO: 1:7.7+7) |
| Extra fontconfig config | none | `71-enable-terminus.conf`, which re-enables the bitmap family that Debian's `70-no-bitmaps-except-emoji.conf` rejects |
| SHA-256 of the `.deb` | `0355444509f2b6c5…3a91` | `a659226b011188a9…bc70` |

The ISO today (`lcos-live-100-01`, 1 532 952 576 B, 1236 packages) has neither package. It has no
Terminus file in its contents list. It does ship `cool-retro-term 1.2.0+ds2-4`, but Debian's
cool-retro-term does not depend on fonts-terminus. It also ships `fonts-dejavu-core`/`-mono`,
`fonts-pc`, `fonts-pc-extra` and `fonts-3270`.

## Licence

- **Upstream Terminus Font is SIL Open Font License 1.1.** The otb package's copyright file says:
  "Terminus Font is licensed under the SIL Open Font License, Version 1.1. Copyright (c) 2010-2014
  Dimitar Toshkov Zhekov, with Reserved Font Name "Terminus Font"". It includes the full OFL text.
- **The TTF's own name table** (fonts-terminus) says: "Copyright (C) 2016 Dimitar Toshkov Zhekov;
  Copyright (C) 2017 Tilman Blumenbach; Licensed under the SIL Open Font License, Version 1.1".
- OFL 1.1 allows bundling and redistribution with software, including on an ISO. The conditions
  are: keep the copyright notice and licence with each copy, do not sell the font by itself, and do
  not use the Reserved Font Name for a modified version. We would not modify it.

**Internal note: Debian copyright-file gap (for our records only; not reported anywhere).**
- `fonts-terminus`'s `/usr/share/doc/fonts-terminus/copyright` is the cool-retro-term copyright
  file. It has stanzas for Hermit, Fixedsys Excelsior, Inconsolata, ProFont and ProggyTiny, but
  **none for the Terminus TTF**. Formally the file therefore falls under `Files: *`, "Copyright
  2014-2022 Filippo Scognamiglio, License: GPL-3".
- The font itself says OFL-1.1, and upstream Terminus is OFL-1.1. So this is an incomplete
  packaging record, not a licence problem.
- If LCOS ships the TTF, the LCOS side should carry the correct notice: OFL-1.1, Zhekov /
  Blumenbach.
- `fonts-terminus-otb` has no such gap.
- Per the editor's rules, nothing about this has been sent to Debian or anyone else.

## Which profiles use it

From `src/profiles.cpp` (`kTerminus`). The first installed family wins.

| Profile | Font chain |
|---|---|
| plain (default) | DejaVu Sans Mono 16 px. **Does not use Terminus.** |
| amber, green, green-scanlines, vintage, futuristic, paper-white | `Terminus (TTF)` → `Terminus` (otb) → `DejaVu Sans Mono` → `Monospace`, 22 px |
| ibm-dos | `Px IBM VGA8` 32 px (fonts-pc, on the ISO) first; Terminus chain only as fallback |
| ibm-3278 | `IBM 3270` (fonts-3270, on the ISO) first; Terminus chain only as fallback |

`debian/control` has `Recommends: fonts-terminus | fonts-terminus-otb`.

## What happens without it (REAL, this host has no Terminus installed)

`build/lunduke-terminal --list-profiles` on the build host:

    plain            Plain (no effects) [default] font: DejaVu Sans Mono 16px
    amber            Amber                  font: DejaVu Sans Mono 22px (fallback)
    green            Green                  font: DejaVu Sans Mono 22px (fallback)
    ...

- The terminal never falls back to a proportional font. `src/font_resolve.*` checks each choice
  with fontconfig and keeps the cell grid monospace.
- Without Terminus, the CRT profiles look right in colour and effects, but the type is DejaVu
  instead of the intended bitmap-style face. Nothing breaks.
- The default `plain` profile is unaffected.
- The TCG VM runs in `docs/bench/` used the TTF from the fonts-terminus `.deb` (same SHA-256),
  copied onto the private payload disk with a private `FONTCONFIG_FILE`. It is not on the ISO.

## Options

1. **Do nothing (status quo).** CRT profiles use DejaVu 22 px on LCOS; users can `apt install
   fonts-terminus`. Cost 0. The amber look is less authentic.
2. **Add `fonts-terminus` (TTF) to the ISO package list.** +80.7 kB download, +452 KiB installed,
   roughly 75 kB more squashfs (DERIVED from xz -9; the real squashfs cost was not measured). It is
   scalable, so every size works and HiDPI is fine. The copyright file has the gap above, so LCOS
   should document the OFL itself.
3. **Add `fonts-terminus-otb`.** +287 kB download, +1.43 MiB installed, about 270 kB squashfs
   (DERIVED). It has a clean copyright file. It is bitmap only: sharp at its strikes (the profiles
   use 22 px), with no free scaling. It needs its fontconfig re-enable snippet, which the package
   ships.
4. **Let the terminal package pull it in.** When Lunduke Terminal itself is added to the ISO, apt's
   default install of Recommends brings in `fonts-terminus` automatically, so no separate decision is
   needed. That holds only if the ISO build installs Recommends; this was not checked.
5. **Bundle the TTF in the terminal package.** Not recommended: it duplicates a Debian package and
   moves the OFL obligations into our package.

**Suggestion:** decide this together with whether Lunduke Terminal goes on the ISO at all. If it
does, option 4 (or 2 explicitly) is the smallest change. If the editor prefers a clean upstream
copyright record, use option 3.
