#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 item 6: one line per snapshot from LABEL-TAG-rollup.txt (kB).
printf "%-28s %-6s %8s %8s %10s %10s\n" label snap Rss Pss Priv_Dirty Shared_Cl
for f in "$1"/*-rollup.txt; do
  b=$(basename "$f" -rollup.txt); tag=${b##*-}; lab=${b%-*}
  awk -v l="$lab" -v t="$tag" '/^Rss:/{r=$2}/^Pss:/{p=$2}/^Private_Dirty:/{d=$2}/^Shared_Clean:/{s=$2}END{printf "%-28s %-6s %8d %8d %10d %10d\n",l,t,r,p,d,s}' "$f"
done
