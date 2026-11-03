#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Default scrollback (M6, editor order): 5000 lines when settings.ini has no scrollback-lines;
# a value already saved there (10000, the old default, or any other) is used as is, and the file
# is not rewritten. Runs the real binary, so the settings.ini path in main.cpp is covered.
# Usage: scrollback_default_test.sh path/to/lunduke-terminal. Skips (77) without $DISPLAY.
bin=$1
[ -n "$DISPLAY" ] || { echo "skip: no DISPLAY"; exit 77; }
tmp=$(mktemp -d /tmp/lterm-sbdefault.XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0

# Prints the last sb-lines= value of a run that prints 12000 lines, then exits.
run() {
    XDG_CONFIG_HOME=$1 timeout 60 "$bin" --profile plain --renderer cpu --stats-every 1 --on-exit close \
        -e sh -c 'seq 1 12000; sleep 2' 2>&1 | sed -n 's/^stats-mem .* sb-lines=\([0-9]*\) .*/\1/p' | tail -n 1
}
check() {  # name config-dir expected
    got=$(run "$2")
    echo "$1: sb-lines=$got (want $3)"
    [ "$got" = "$3" ] || fail=1
}

mkdir -p "$tmp/none/lunduke-terminal" "$tmp/old/lunduke-terminal" "$tmp/other/lunduke-terminal"
printf '[terminal]\nprofile=plain\n' > "$tmp/none/lunduke-terminal/settings.ini"
printf '[terminal]\nscrollback-lines=10000\n' > "$tmp/old/lunduke-terminal/settings.ini"
printf '[terminal]\nscrollback-lines=7000\n' > "$tmp/other/lunduke-terminal/settings.ini"
cp "$tmp/old/lunduke-terminal/settings.ini" "$tmp/old.ini"
cp "$tmp/other/lunduke-terminal/settings.ini" "$tmp/other.ini"

check "no settings.ini" "$tmp/empty" 5000
check "settings.ini without scrollback-lines" "$tmp/none" 5000
check "saved scrollback-lines=10000" "$tmp/old" 10000
check "saved scrollback-lines=7000" "$tmp/other" 7000
cmp -s "$tmp/old.ini" "$tmp/old/lunduke-terminal/settings.ini" || { echo "settings.ini (10000) was rewritten"; fail=1; }
cmp -s "$tmp/other.ini" "$tmp/other/lunduke-terminal/settings.ini" || { echo "settings.ini (7000) was rewritten"; fail=1; }
[ "$fail" = 0 ] && echo "scrollback default: ok"
exit $fail
