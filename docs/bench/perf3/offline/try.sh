#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# try.sh TAG : rebuild the release build, render the 48 configurations with its objects, and
# compare them with the reference render in $PERF3_WORK/out-master (make that one first, with
# TAG=master on a master checkout, or point PERF3_WORK at a directory that has it).
#   LTERM_SRC   repository checkout (default: the one this script is in)
#   LTERM_BUILD its release build directory (default: $LTERM_SRC/build)
#   PERF3_WORK  scratch directory for objects, binaries and renders (default: /tmp/perf3-offline)
set -e
tag=$1; here=$(cd "$(dirname "$0")" && pwd)
src=${LTERM_SRC:-$(cd "$here/../../../.." && pwd)}; bld=${LTERM_BUILD:-$src/build}; work=${PERF3_WORK:-/tmp/perf3-offline}
mkdir -p "$work"; ninja -C "$bld" -j2 > "$work/b-$tag.log" 2>&1 || { tail -20 "$work/b-$tag.log"; exit 1; }
rm -rf "$work/$tag-libs"; for l in liblterm-gl liblterm-effects; do mkdir -p "$work/$tag-libs/$l"; (cd "$bld" && cp $(ar t $l.a) "$work/$tag-libs/$l/"); done
(cd "$here" && ./build.sh "$src" "$work/$tag-libs" "$work/render-$tag")
rm -rf "$work/out-$tag" && mkdir -p "$work/out-$tag" && LIBGL_ALWAYS_SOFTWARE=1 "$work/render-$tag" "$work/out-$tag" >/dev/null
[ -d "$work/out-master" ] || { echo "no $work/out-master to compare with"; exit 0; }
python3 "$here/psnr.py" "$work/out-master" "$work/out-$tag" > "$work/psnr-$tag.md"; sort -t'|' -k4 "$work/psnr-$tag.md" | sed -n 3,6p; tail -4 "$work/psnr-$tag.md"
