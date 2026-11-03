#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Perf step 1: --help and --version print to stdout and exit 0 without opening a window (so no
# $DISPLAY is needed); an unknown option still exits 2 with a message on stderr.
# Usage: cli_test.sh path/to/lunduke-terminal VERSION
bin=$1
version=$2
tmp=$(mktemp -d /tmp/lterm-cli.XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
unset DISPLAY WAYLAND_DISPLAY
export XDG_CONFIG_HOME="$tmp/config"

expect() {  # name want-exit got-exit
    echo "$1: exit $3 (want $2)"
    [ "$3" = "$2" ] || fail=1
}
has() {  # file text
    grep -qF -e "$2" "$1" || { echo "missing in $(basename "$1"): $2"; fail=1; }
}

for opt in --help -h; do
    "$bin" $opt > "$tmp/help.out" 2> "$tmp/help.err"
    expect "$opt" 0 $?
    has "$tmp/help.out" "Usage: lunduke-terminal"
    has "$tmp/help.out" "--quit-after MS"
    has "$tmp/help.out" "milliseconds"
    has "$tmp/help.out" "--version"
    has "$tmp/help.out" "Ctrl+Shift+T"
    [ -s "$tmp/help.err" ] && { echo "$opt wrote to stderr:"; cat "$tmp/help.err"; fail=1; }
done
# --help wins wherever it is before -e, even after a value option.
"$bin" --profile amber --help > "$tmp/help2.out" 2>&1
expect "--profile amber --help" 0 $?
has "$tmp/help2.out" "Usage: lunduke-terminal"

"$bin" --version > "$tmp/version.out" 2> "$tmp/version.err"
expect "--version" 0 $?
echo "--version printed: $(cat "$tmp/version.out")"
[ "$(cat "$tmp/version.out")" = "lunduke-terminal $version" ] || { echo "want: lunduke-terminal $version"; fail=1; }
[ -s "$tmp/version.err" ] && { echo "--version wrote to stderr"; fail=1; }

"$bin" --no-such-option > "$tmp/bad.out" 2> "$tmp/bad.err"
expect "--no-such-option" 2 $?
has "$tmp/bad.err" "unknown option --no-such-option"
has "$tmp/bad.err" "--help"
[ -s "$tmp/bad.out" ] && { echo "unknown option wrote to stdout"; fail=1; }

"$bin" --renderer bogus > /dev/null 2> "$tmp/bad2.err"
expect "--renderer bogus" 2 $?

[ "$fail" = 0 ] && echo "cli: ok"
exit $fail
