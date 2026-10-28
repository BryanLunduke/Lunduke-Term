#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Renders the M0 golden frame with the CPU and GL (llvmpipe) back-ends and builds the
# side-by-side / zoom / diff images with ImageMagick.
#   tools/make-comparison.sh BUILD_DIR OUT_DIR
set -eu
BUILD=${1:-build}
OUT=${2:-m0-out}
mkdir -p "$OUT"
LIBGL_ALWAYS_SOFTWARE=1 "$BUILD/lterm-m0-bench" --compare --out "$OUT" | tee "$OUT/compare.txt"
cd "$OUT"
convert \( cpu.png -gravity north -background '#202020' -fill white -pointsize 28 -splice 0x44 -annotate +0+6 'CPU (C++, 1 thread)' \) \
        \( gl.png -gravity north -background '#202020' -fill white -pointsize 28 -splice 0x44 -annotate +0+6 'GL (GLSL 3.30, llvmpipe)' \) \
        +append side-by-side-cpu-vs-gl.png
convert cpu.png -crop 400x250+0+0 +repage -filter point -resize 200% zoom-cpu.tmp.png
convert gl.png -crop 400x250+0+0 +repage -filter point -resize 200% zoom-gl.tmp.png
convert zoom-cpu.tmp.png zoom-gl.tmp.png +append zoom-corner-cpu-vs-gl.png
rm -f zoom-cpu.tmp.png zoom-gl.tmp.png
compare cpu.png gl.png -highlight-color red diff-highlight.png 2>/dev/null || true
{
  printf 'ImageMagick AE (pixels differing): '; compare -metric AE cpu.png gl.png null: 2>&1 || true; echo
  printf 'ImageMagick PSNR: '; compare -metric PSNR cpu.png gl.png null: 2>&1 || true; echo
  printf 'ImageMagick MAE: '; compare -metric MAE cpu.png gl.png null: 2>&1 || true; echo
} | tee -a compare.txt
