#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 6: builds the libFuzzer harness. Needs clang 19 (here: extracted from Debian .debs into
# /workspace/lterm-tools, see docs/FUZZING.md). libvterm is compiled into the fuzzer with the same
# instrumentation: by default the bundled third_party/libvterm with the patches in
# third_party/libvterm-patches (what the program links); LIBVTERM_STOCK=1 leaves screen.c
# unpatched (stock 0.3.3, what the system libvterm0 runs); LIBVTERM_SRC=DIR uses another tree.
# Usage: tools/fuzz/build.sh [OUTDIR]   (default build-fuzz)
set -e
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$SRC/build-fuzz}
CLANG=${CLANG:-/workspace/lterm-tools/clang19.sh}
VT=${LIBVTERM_SRC:-$SRC/third_party/libvterm}
SAN="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
mkdir -p "$OUT/obj"
P=$SRC/third_party/libvterm-patches
if [ -n "${LIBVTERM_STOCK:-}" ]; then
  python3 "$SRC/tools/libvterm_patch.py" patch "$VT/src/screen.c" "$OUT/libvterm-screen.c" "$P/series"
else
  python3 "$SRC/tools/libvterm_patch.py" patch "$VT/src/screen.c" "$OUT/libvterm-screen.c" "$P/series" \
    $(grep -v '^#' "$P/series" | sed "s|^|$P/|")
fi
for f in encoding keyboard mouse parser pen screen state unicode vterm; do
  in="$VT/src/$f.c"
  [ $f = screen ] && in="$OUT/libvterm-screen.c"
  $CLANG clang -std=c99 $SAN -I"$VT/include" -I"$VT/src" -c "$in" -o "$OUT/obj/vt-$f.o"
done
PC=$(pkg-config --cflags pangocairo fontconfig glib-2.0)
for f in vterm_session selection search profiles effects font_resolve; do
  $CLANG clang++ -std=c++17 $SAN -I"$VT/include" -I"$SRC/src" $PC -c "$SRC/src/$f.cpp" -o "$OUT/obj/$f.o"
done
$CLANG clang++ -std=c++17 $SAN -I"$VT/include" -I"$VT/src" -I"$SRC/src" $PC -c "$SRC/tools/fuzz/vterm_fuzz.cpp" -o "$OUT/obj/vterm_fuzz.o"
$CLANG clang++ -fsanitize=fuzzer,address,undefined "$OUT"/obj/*.o -o "$OUT/vterm_fuzz" \
  $(pkg-config --libs pangocairo fontconfig glib-2.0) -lutil
echo "$OUT/vterm_fuzz"
