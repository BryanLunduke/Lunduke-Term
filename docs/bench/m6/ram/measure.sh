#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 item 6: memory of one Lunduke Terminal (or baseline) process, fresh and after output.
# Usage: measure.sh LABEL OUTDIR -- COMMAND...   (COMMAND's own PID is measured, not its children)
# Snapshots: t=8 s ("fresh", before any output) and t=30 s ("after", the shell has printed
# ~20k lines + source files from t=10 s). Writes LABEL-{fresh,after}-{rollup,groups,smaps}.txt and LABEL-stdout.txt.
label=$1; out=$2; shift 3
: "${DISPLAY:?}"
cfg=$(mktemp -d /tmp/lterm-ram-XXXXXX)   # empty settings / profiles
XDG_CONFIG_HOME=$cfg "$@" > "$out/$label-stdout.txt" 2>&1 &
pid=$!
here=$(dirname "$0")
snap() {
  local tag=$1
  { echo "# $label $tag pid $pid $(date '+%F %T %Z')"; cat /proc/$pid/smaps_rollup; } > "$out/$label-$tag-rollup.txt"
  bash "$here/smaps-group.sh" $pid > "$out/$label-$tag-groups.txt"
  cp /proc/$pid/smaps "$out/$label-$tag-smaps.txt"  # kept as raw-smaps-*.tar.xz in git
}
sleep 8; snap fresh
sleep 22; snap after
kill $pid; wait $pid 2>/dev/null
rm -rf "$cfg"
