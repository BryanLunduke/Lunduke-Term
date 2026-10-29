#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 6: builds the libFuzzer harness. Needs clang 19 (here: extracted from Debian .debs into
# /workspace/lterm-tools, see docs/FUZZING.md) and the libvterm 0.3.3 source (Debian orig
# tarball), which is compiled into the fuzzer with the same instrumentation.
# Usage: tools/fuzz/build.sh [OUTDIR]   (default build-fuzz)
set -e
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$SRC/build-fuzz}
CLANG=${CLANG:-/workspace/lterm-tools/clang19.sh}
VT=${LIBVTERM_SRC:-/workspace/lterm-tools/src/libvterm-0.3.3}
SAN="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
mkdir -p "$OUT/obj"
for f in encoding keyboard mouse parser pen screen state unicode vterm; do
  $CLANG clang -std=c99 $SAN -I"$VT/include" -I"$VT/src" -c "$VT/src/$f.c" -o "$OUT/obj/vt-$f.o"
done
PC=$(pkg-config --cflags pangocairo fontconfig glib-2.0)
for f in vterm_session selection search profiles effects font_resolve; do
  $CLANG clang++ -std=c++17 $SAN -I"$VT/include" -I"$SRC/src" $PC -c "$SRC/src/$f.cpp" -o "$OUT/obj/$f.o"
done
$CLANG clang++ -std=c++17 $SAN -I"$VT/include" -I"$SRC/src" $PC -c "$SRC/tools/fuzz/vterm_fuzz.cpp" -o "$OUT/obj/vterm_fuzz.o"
$CLANG clang++ -fsanitize=fuzzer,address,undefined "$OUT"/obj/*.o -o "$OUT/vterm_fuzz" \
  $(pkg-config --libs pangocairo fontconfig glib-2.0) -lutil
echo "$OUT/vterm_fuzz"
