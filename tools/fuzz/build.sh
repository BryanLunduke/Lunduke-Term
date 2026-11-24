#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the libFuzzer harness against the static libghostty-vt from a meson build
# (or $LIBGHOSTTY_PREFIX). The VT library itself is the release build, not
# instrumented; the session and the harness are. Needs clang with libFuzzer.
# Usage: tools/fuzz/build.sh [OUTDIR]
set -e
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$SRC/build-fuzz}
GV=${LIBGHOSTTY_PREFIX:-$SRC/build/libghostty-prefix}
CLANG=${CLANG:-clang}
if [ ! -f "$GV/lib/libghostty-vt.a" ]; then
  echo "build libghostty-vt first: meson setup build && meson compile -C build" >&2
  echo "or set LIBGHOSTTY_PREFIX" >&2
  exit 1
fi
SAN="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
mkdir -p "$OUT/obj"
PC=$(pkg-config --cflags pangocairo fontconfig glib-2.0)
INC="-I$GV/include -I$SRC/src"
for f in vterm_session selection search profiles effects font_resolve thread_pool effects_governor custom_profiles cpu_effects synthetic_frame grid_painter; do
  $CLANG++ -std=c++17 $SAN $INC $PC -c "$SRC/src/$f.cpp" -o "$OUT/obj/$f.o"
done
$CLANG++ -std=c++17 $SAN $INC $PC -c "$SRC/tools/fuzz/vterm_fuzz.cpp" -o "$OUT/obj/vterm_fuzz.o"
$CLANG++ -fsanitize=fuzzer,address,undefined "$OUT"/obj/*.o "$GV/lib/libghostty-vt.a" -o "$OUT/vterm_fuzz" \
  $(pkg-config --libs pangocairo fontconfig glib-2.0) -lutil -lm
echo "$OUT/vterm_fuzz"
