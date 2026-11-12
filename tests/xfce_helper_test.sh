#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The XFCE preferred-terminal helper. desktop-file-validate rejects Type=X-XFCE-Helper
# (it rejects the helpers XFCE ships, too), so check the keys exo actually reads.
# Usage: xfce_helper_test.sh path/to/lunduke-terminal.desktop
f=$1
fail=0
need() {
    grep -q "^$1$" "$f" || { echo "missing: $1"; fail=1; }
}
need 'Type=X-XFCE-Helper'
need 'X-XFCE-Category=TerminalEmulator'
need 'X-XFCE-Commands=%B;'
need 'X-XFCE-CommandsWithParameter=%B -x %s;'
need 'X-XFCE-Binaries=lunduke-terminal;'
[ "$fail" = 0 ] && echo "xfce helper: ok"
exit $fail
