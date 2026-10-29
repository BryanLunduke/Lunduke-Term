#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 4: Rss of one process grouped by mapping kind, from /proc/PID/smaps (kB).
# Anonymous mappings are split by size: glibc non-main arenas reserve 64 MiB each (Size: 65536 kB).
awk '
/^[0-9a-f]+-[0-9a-f]+ / { path = (NF >= 6) ? $6 : ""; next }
/^Size:/ {
  sz = $2
  if (path == "[heap]") cat = "heap(main-arena)"
  else if (path == "[stack]") cat = "stack"
  else if (path ~ /^\[/) cat = "other-special"
  else if (path == "") cat = (sz == 65536 || sz == 131072) ? "anon-64MiB(arena-like)" : (sz >= 1024 ? "anon>=1MiB" : "anon<1MiB")
  else if (path ~ /memfd:|\/dev\/shm|SYSV/) cat = "shm/memfd"
  else if (path ~ /libLLVM/) cat = "file:libLLVM"
  else if (path ~ /libgallium/) cat = "file:libgallium"
  else if (path ~ /\.so/) cat = "file:other-libs"
  else cat = "file:other"
  next
}
/^Rss:/ { rss[cat] += $2 }
END { for (c in rss) printf "%s %d\n", c, rss[c] }' /proc/$1/smaps | sort
