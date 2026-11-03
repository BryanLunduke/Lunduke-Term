#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# M6 item 6: Rss / Pss / Private_Dirty of one process grouped by mapping kind, from /proc/PID/smaps (kB).
# Based on docs/bench/m4/item4/smaps-group.sh; adds Pss and Private_Dirty and more file groups.
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
  else if (path ~ /libgallium|_dri\.so|libGLX_mesa|libEGL_mesa/) cat = "file:mesa"
  else if (path ~ /libgtk-3|libgdk-3/) cat = "file:gtk3"
  else if (path ~ /libicu/) cat = "file:icu"
  else if (path ~ /\.(ttf|otf|otb|pcf|pcf\.gz|ttc)$/) cat = "file:fonts"
  else if (path ~ /fontconfig|\.cache-/) cat = "file:fontconfig-cache"
  else if (path ~ /lunduke-terminal$/) cat = "file:lunduke-terminal"
  else if (path ~ /\.so/) cat = "file:other-libs"
  else cat = "file:other"
  next
}
/^Rss:/ { rss[cat] += $2 }
/^Pss:/ { pss[cat] += $2 }
/^Private_Dirty:/ { pd[cat] += $2 }
END { printf "%-26s %8s %8s %8s\n", "group", "Rss", "Pss", "PrivDirty"; for (c in rss) printf "%-26s %8d %8d %8d\n", c, rss[c], pss[c], pd[c] }' /proc/$1/smaps | sort
