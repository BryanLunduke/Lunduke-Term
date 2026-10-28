# Lunduke Terminal — fonts (M1 checkpoint C, item 6)

**Lunduke Terminal ships no font files.**
- Profiles name font *families*. Those families come from Debian font packages, which a future
  package should pull in as runtime `Depends`/`Recommends` (packaging itself is not part of M1).
- A missing family falls back gracefully; see [Fallback](#fallback).

## How the licences were checked

Each `.deb` was downloaded over https from `https://deb.debian.org/debian/pool/main/...`. The
versions are those in Debian 13 "trixie" (`binary-all/Packages.xz`, 2026-10-28).
- Each package was unpacked with `dpkg-deb -x` into a scratch directory. Nothing was installed.
- I read `usr/share/doc/<pkg>/copyright`.
- I cross-checked the font files' own copyright and licence strings (name table, via `fc-scan`).

The `.deb` SHA-256 sums are in [`bench/fonts-SHA256SUMS`](bench/fonts-SHA256SUMS).

## Table

**Bundle-safe** means the font *file* could legally be shipped inside our own package. We still
recommend depending on the Debian package instead.

| Package | Version | License (SPDX) | Bundle-safe | Source | Notes |
|---|---|---|---|---|---|
| fonts-pc | 1.0-2 | CC-BY-SA-4.0 | yes (attribution; modified fonts stay CC-BY-SA) | https://int10h.org/oldschool-pc-fonts/ | VileR's Ultimate Oldschool PC Font Pack. Debian names the families **`Px IBM VGA8`**, `Px IBM VGA9`, `Px IBM CGA`, `Px IBM EGA8`, `Px IBM MDA`… (not `Px437`/`PxPlus`). **Already on the LCOS 1.0 ISO.** |
| fonts-pc-extra | 1.0-2 | CC-BY-SA-4.0 | yes (as fonts-pc) | https://int10h.org/oldschool-pc-fonts/ | More PC/BIOS faces (Phoenix, AMI, Tandy, Verite…). Optional. On the ISO. |
| fonts-3270 | 3.0.1-1 | BSD-3-Clause OR OFL-1.1 (Debian: `BSD3\|SIL`) | yes | https://github.com/rbanffy/3270font | Family `IBM 3270` (+ Condensed / Semi-Condensed). No Reserved Font Name. On the ISO. |
| fonts-terminus | 1.2.0+ds2-4 (src cool-retro-term) | OFL-1.1 | yes, but **needs-check of Debian's copyright file** | https://github.com/Swordfish90/cool-retro-term | `TerminusTTF-4.46.0.ttf`, family `Terminus (TTF)`. Its name table says "Copyright 2016 Dimitar Toshkov Zhekov; 2017 Tilman Blumenbach; … SIL Open Font License, Version 1.1". **Copyright-file gap:** `debian/copyright` has no stanza for this file, so formally it falls under `Files: *` = GPL-3 (Scognamiglio). That is a Debian packaging bug, not a licence problem, because the font itself says OFL-1.1. Upstream Terminus is OFL-1.1, see fonts-terminus-otb. A Debian bug report would fix it (not filed). We only depend on the package. **Not on the ISO.** |
| fonts-terminus-otb | 4.48-3.1 (src xfonts-terminus) | OFL-1.1 | yes (Reserved Font Name "Terminus Font") | https://terminus-font.sourceforge.net/ | Bitmap OTB, family `Terminus`. Clean copyright: "Copyright (c) 2010-2014 Dimitar Toshkov Zhekov, with Reserved Font Name "Terminus Font"". Debian's `70-no-bitmaps-except-emoji.conf` rejects bitmap fonts, but the package re-enables itself with `71-enable-terminus.conf`. Verified with that file: amber resolves to `Terminus` 22 px, spacing charcell (`bench/font-fallback.txt`, screenshot c-06). Second choice after the TTF. |
| fonts-hermit | 1.2.0+ds2-4 (src cool-retro-term) | OFL-1.1 | yes (Reserved Font Name "Hermit") | https://pcaro.es/p/hermit/ | Stanza `app/qml/fonts/modern-hermit/*`: 2013 Pablo Caro, OFL-1.1. Family `Hermit`. **Clean.** |
| fonts-proggy | 1.2.0+ds2-4 (src cool-retro-term) | MIT | yes | https://github.com/bluescan/proggyfonts | Stanza `app/qml/fonts/modern-proggy-tiny/*`: 2004-2005 Tristan Grimmer, MIT. Family `ProggyTinyTT`. **Clean.** |
| fonts-sixtyfour | 1.0-3 (src fonts-homecomputer) | OFL-1.1 | yes; trademark note | https://github.com/jenskutilek/homecomputer-fonts | `Files: *` OFL-1.1, "2019 The Homecomputer Fonts Project Authors". A *redrawing* inspired by the C64 screen font. Family `C-64`. "Commodore"/"C64" are trademarks: fine as a font name, but don't brand a profile with Commodore marks. On the ISO. |
| fonts-amiga | 1.0-3 (src fonts-homecomputer) | OFL-1.1 | yes; trademark note | https://github.com/jenskutilek/homecomputer-fonts | Same project and licence, Workbench-style redrawing. Family `Amiga`. On the ISO. |
| fonts-atarismall | 2.3-3 | LicenseRef-atari800-license (permissive) | yes (keep the notice) | https://github.com/alexmyczko/atari800-font | The text: "copy, modify, and distribute for both commercial and non-commercial use … provided this notice is preserved". Thomas A. Fine 1999, ntwk 2015-16, Alex Myczko. Family `"Atari800"`, with the quotes as part of the name, an upstream wart. Not an SPDX-listed licence, so one legal read is advised. It is DFSG-free (Debian main). On the ISO. |
| fonts-atarist | 0+git20230822+ds-2 | BSD-3-Clause | yes | https://github.com/ntwk/atarist-font | Atari ST system font redrawn by ntwk (2015), Cyrillic by Andrew Goncharov (2017). Family `Atari`, style `ST`. Not on the ISO. |
| fonts-dejavu-core / -mono | 2.37-8 | Bitstream-Vera (DejaVu changes public domain) | yes | https://dejavu-fonts.github.io/ | `DejaVu Sans Mono`: the plain profile and the last named fallback. On the ISO and on every Debian desktop. |

**Summary**
- **Clean:** fonts-pc, fonts-pc-extra, fonts-3270, fonts-terminus-otb, fonts-hermit, fonts-proggy,
  fonts-atarist, fonts-dejavu.
- **Clean, with notes:**
  - fonts-sixtyfour and fonts-amiga: trademark wording.
  - fonts-atarismall: non-SPDX permissive licence.
- **Needs-check:** only fonts-terminus, and only because Debian's copyright file is incomplete.
  The font is OFL-1.1.

### Why the Apple II, PET and original C64 fonts are excluded

Upstream cool-retro-term bundles five font files that Debian **removes** from its `1.2.0+ds2`
repack. They are listed under `Files-Excluded:` in `fonts-terminus`' copyright file, which is the
cool-retro-term source's copyright file:
- `1977-apple2/PrintChar21.ttf`
- `1977-apple2/PRNumber3.ttf`
- `1977-commodore-pet/PetMe.ttf`
- `1982-commodore64/C64_Pro_Mono-STYLE.ttf`
- `1979-atari-400-800/AtariClassic-Regular.ttf`

Debian's changelog says they were replaced by "the substituted fonts".

- **Apple ][ PrintChar21 / PRNumber3 and PetMe (Kreative Software).** They use the "Kreative
  Software Relay Fonts Free Use License 1.2f"
  (https://www.kreativekorp.com/software/fonts/FreeLicense.txt). It forbids selling copies (§1) and
  forbids modification and derivative works (§2), and Kreative may change it at any time (§5).
  That is not DFSG-free; ScanCode classes it "Proprietary Free". It is unsuitable for a GPL distro
  that may be sold on media.
- **C64 Pro Mono (Style64).** Its licence (https://style64.org/c64-truetype/license) forbids selling
  the font or including it in any font collection. Copies must be unmodified, keep their original
  file names, and may ship only inside software that is free to end users. Also non-free.
- **AtariClassic** was also dropped. The free fonts-atarismall and fonts-atarist cover the Atari
  looks.
- **Character ROMs.** These fonts are pixel copies of the original Apple II, PET and C64
  character ROMs. A free *redrawing* such as fonts-sixtyfour is the safe route. The original ROM
  glyphs come only under the restrictive terms above.

Lunduke Terminal therefore never references those families. A C64 look would use `C-64`
(fonts-sixtyfour) and an Amiga look `Amiga` (fonts-amiga).

`PxPlus_IBM_VGA8.ttf`, `PxPlus_IBM_BIOS.ttf` and `3270-Regular.ttf` are also in `Files-Excluded`,
but not for licence reasons. Debian ships the same fonts as fonts-pc and fonts-3270, and the
profiles use them by their Debian names.

## Recommended mapping (implemented in `src/profiles.cpp`)

The first installed choice wins, top to bottom. A size is per choice: pixel fonts look right only
at integer multiples of their design size.

| Profile | 1st choice (package) | then | Suggested relation |
|---|---|---|---|
| plain (default) | `DejaVu Sans Mono` 16 px (fonts-dejavu-core) | `Monospace` | Depends: fonts-dejavu-core |
| amber, green, green-scanlines | `Terminus (TTF)` 22 px (fonts-terminus) | `Terminus` (fonts-terminus-otb), `DejaVu Sans Mono`, `Monospace` | Recommends: fonts-terminus \| fonts-terminus-otb |
| ibm-dos | `Px IBM VGA8` **32 px** (fonts-pc): exactly 2× the 8×16 VGA glyphs, about 80×25 at 1280×800 | `Px437 IBM VGA8` and `PxPlus IBM VGA8` at 32 px (int10h originals, if the user installed them), then the Terminus chain at 22 px | Recommends: fonts-pc |
| ibm-3278 | `IBM 3270` 22 px (fonts-3270) | `3270`, then the Terminus chain | Recommends: fonts-3270 |

**Proposed for M2.** These families are licence-clean but have no profile yet:

| Future profile | Font (package) | Size hint |
|---|---|---|
| c64 | `C-64` (fonts-sixtyfour) | multiples of 8 px |
| amiga | `Amiga` (fonts-amiga) | multiples of 8 px |
| atari-800 | `"Atari800"` (fonts-atarismall) | multiples of 8 px |
| atari-st | `Atari` (fonts-atarist) | 16 px |
| modern | `Hermit` (fonts-hermit) / `ProggyTinyTT` (fonts-proggy) | 16–22 px / 16 px |

**LCOS 1.0 ISO** (lcos-live-100-01 package list).
- Already shipped: fonts-pc, fonts-pc-extra, fonts-3270, fonts-amiga, fonts-atarismall,
  fonts-sixtyfour and DejaVu.
- **fonts-terminus and fonts-terminus-otb are not shipped.** The ISO has cool-retro-term
  1.2.0+ds2-4, but Debian's cool-retro-term `Depends` on fonts-inconsolata, fonts-3270,
  fonts-agave, fonts-sixtyfour, fonts-pc, fonts-pc-extra, fonts-amiga and fonts-atarismall, not
  on fonts-terminus. That is why those fonts are on the ISO.
- There is no Terminus file in the ISO contents list.
- So on LCOS today amber and green fall back to DejaVu Sans Mono 22 px. The VM benchmark used a
  Terminus TTF copied onto its private payload disk.
- Adding fonts-terminus (80 kB .deb) is a later packaging decision. Nothing was changed here.

## Fallback

Pango/fontconfig never fail on a missing family. They silently substitute the default *sans*
font, which is **proportional** and breaks the cell grid. For example, without fonts-pc:

    $ fc-match "Px IBM VGA8"
    DejaVuSans.ttf: "DejaVu Sans" "Book"

`src/font_resolve.*` therefore resolves the family list itself, before Pango sees it.
- **Matching.** Each choice is matched with `FcFontMatch`. It counts only if one of the returned
  font's family names equals the request, case-insensitively.
- **Picking.** The first hit wins, at its own pixel size. If nothing matches, the result is
  `Monospace`. Generic aliases count as installed.
- **Monospace check.** The pick is checked for `FC_SPACING >= FC_DUAL`. If it fails, the terminal
  warns `lunduke-terminal: font 'X' is not monospaced`.
- **User fonts.** A `--font` / `font=` list is tried first, then the profile's own choices. A miss
  warns once: `lunduke-terminal: font 'X' is not installed, using 'Y'`.
- **`--list-profiles`** shows what each profile resolves to on this machine. `(fallback)` means
  "not the profile's first choice".

**Evidence**

[`bench/font-fallback.txt`](bench/font-fallback.txt) has the fc-match and `--list-profiles` output
for four private configs:
- all Debian fonts;
- fonts-terminus-otb only;
- DejaVu only;
- no monospace font at all.

`fc-match` itself needs `C\-64`, because `-` starts a size in fontconfig's pattern syntax. The
resolver uses `FcPatternAddString` and is not affected.

**Test:** `tests/fonts_test.cpp` (meson test `fonts`) uses private fontconfig configs, so it does
not depend on the build machine. It checks that:
- missing families fall back, at the fallback's own size and not the pixel font's 32 px;
- raw Pango's substitute *is* proportional ("iiiiiiii" vs "MMMMMMMM" widths), while the pick
  is not;
- every built-in profile resolves to an installed monospace family with a sane cell;
- with no monospace font installed, the pick is flagged.

**Screenshots** are in `/workspace/lcos-app-reviews/terminal-m1/`, Xvfb 1280×800, fonts loaded
from the `dpkg-deb -x` scratch dir via `FONTCONFIG_FILE`:

| Screenshot | Shows |
|---|---|
| `c-01` | plain, DejaVu Sans Mono |
| `c-02` | amber, Terminus (TTF) |
| `c-03` | green-scanlines, Terminus (TTF) |
| `c-04` | ibm-dos, Px IBM VGA8 32 px |
| `c-05` | ibm-3278, IBM 3270 |
| `c-06` | amber with only fonts-terminus-otb |
| `c-07` | ibm-dos with DejaVu only → DejaVu Sans Mono 22 px |
| `c-08` | amber with no monospace font → warning, proportional text |
