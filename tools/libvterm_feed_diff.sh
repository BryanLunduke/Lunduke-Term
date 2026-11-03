#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Screen/source dump comparison for third_party/libvterm-patches: feeds real output (the repo's
# own sources, `seq`, and any extra FILEs) through tests/libvterm_diff.c's feed mode, linked
# against stock and against the patched libvterm, at several window sizes, and checks that the
# logs (every callback, scrollback push with its cells, a full screen dump every 256 writes and
# at the end) are byte-identical: same length and SHA-256 (streamed, the logs can be GBs).
#
#   libvterm_feed_diff.sh STOCK_EXE PATCHED_EXE SOURCE_DIR SEQ_N [FILE...]
set -eu
a=$1 b=$2 src=$3 seqn=$4
shift 4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The sources, in a fixed order (C, C++, headers, Meson, docs: tabs, long lines, UTF-8).
find "$src/src" "$src/tests" "$src/third_party/libvterm/src" "$src/third_party/libvterm/include" \
     "$src/docs" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.inc' -o -name '*.md' \) |
  LC_ALL=C sort | xargs cat > "$tmp/sources.txt"
seq 1 "$seqn" > "$tmp/seq.txt"
fail=0
for f in "$tmp/sources.txt" "$tmp/seq.txt" "$@"; do
  for size in "24 80" "50 132" "7 33" "60 211"; do
    set -- $size
    ha=$( { "$a" "$1" "$2" f < "$f"; echo "exit $?"; } | sha256sum | cut -d' ' -f1)
    hb=$( { "$b" "$1" "$2" f < "$f"; echo "exit $?"; } | sha256sum | cut -d' ' -f1)
    len=$("$a" "$1" "$2" f < "$f" | wc -c)
    name=$(basename "$f")
    [ "$f" = "$tmp/sources.txt" ] && name="sources ($(wc -c < "$f") bytes)"
    [ "$f" = "$tmp/seq.txt" ] && name="seq 1 $seqn ($(wc -c < "$f") bytes)"
    if [ "$ha" = "$hb" ]; then
      echo "same: $name at ${1}x${2}, log $len bytes + exit status, sha256 $(echo "$ha" | cut -c1-16)"
    else
      echo "DIFFERENT: $name at ${1}x${2} (sha256 $ha vs $hb)"
      fail=1
    fi
  done
done
exit $fail
