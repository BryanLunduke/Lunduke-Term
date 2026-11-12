#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# --help and --version print to stdout and exit 0 without opening a window (so no
# $DISPLAY is needed); an unknown option still exits 2 with a message on stderr.
# Also checks the x-terminal-emulator / Thunar / exo flags (-e, -x/--execute,
# --working-directory/--workdir, -T/--title). The exec checks need $DISPLAY and are
# skipped without one.
# Usage: cli_test.sh path/to/lunduke-terminal VERSION
bin=$1
version=$2
tmp=$(mktemp -d /tmp/lterm-cli.XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
DISPLAY_SAVED=${DISPLAY-}
WAYLAND_SAVED=${WAYLAND_DISPLAY-}
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
    has "$tmp/help.out" "F11"
    has "$tmp/help.out" "Ctrl+0"
    has "$tmp/help.out" "-x"
    has "$tmp/help.out" "--execute"
    has "$tmp/help.out" "--working-directory"
    has "$tmp/help.out" "--workdir"
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

# PR #9 review: the README says an unknown option and a bad value for these four exit 2.
for opt in --renderer --on-exit --cursor-shape --effects-quality; do
    "$bin" $opt bogus > /dev/null 2> "$tmp/bad2.err"
    expect "$opt bogus" 2 $?
    has "$tmp/bad2.err" "$opt must be"
done

# Launcher flags are accepted and do not open a window when --list-profiles follows them.
"$bin" --cwd / --workdir / --working-directory / \
    --cwd=/ --workdir=/ --working-directory=/ \
    -T title --title title --title=title \
    --list-profiles > "$tmp/flags.out" 2> "$tmp/flags.err"
expect "launcher flags then --list-profiles" 0 $?
has "$tmp/flags.out" "plain"

for opt in --cwd --workdir --working-directory -T --title; do
    "$bin" $opt > "$tmp/miss.out" 2> "$tmp/miss.err"
    expect "$opt missing argument" 2 $?
    has "$tmp/miss.err" "requires an argument"
    if grep -q "unknown option" "$tmp/miss.err"; then echo "$opt was reported as unknown"; fail=1; fi
done

# -e / -x / --execute are real options (a following --renderer is the command, not an option).
for opt in -e -x --execute; do
    "$bin" --renderer bogus $opt /bin/true > "$tmp/rx.out" 2> "$tmp/rx.err"
    expect "--renderer bogus $opt" 2 $?
    has "$tmp/rx.err" "--renderer must be"
    if grep -q "unknown option" "$tmp/rx.err"; then echo "$opt was reported as unknown"; fail=1; fi
done

# Those three swallow the rest of the line, so --help after them is not our help.
for opt in -e -x --execute; do
    timeout 15 "$bin" $opt --help > "$tmp/swallow.out" 2> "$tmp/swallow.err"
    rc=$?
    echo "$opt --help: exit $rc (want non-zero, no usage, and not a hang)"
    if [ "$rc" -eq 0 ] || [ "$rc" -eq 124 ]; then fail=1; fi
    if grep -q "Usage: lunduke-terminal" "$tmp/swallow.out"; then echo "$opt --help printed usage"; fail=1; fi
    if grep -q "unknown option" "$tmp/swallow.err"; then echo "$opt --help: unknown option"; fail=1; fi
done

# With a display, the command really runs and the working-directory flags are the child's cwd.
if [ -n "$DISPLAY_SAVED" ]; then
    export DISPLAY=$DISPLAY_SAVED
    if [ -n "$WAYLAND_SAVED" ]; then export WAYLAND_DISPLAY=$WAYLAND_SAVED; fi
    mkdir -p "$tmp/nocfg" "$tmp/home" "$tmp/work"
    launch() {  # name expected-marker [args...]
        name=$1
        want=$2
        shift 2
        rm -f "$tmp/marker"
        XDG_CONFIG_HOME="$tmp/nocfg" HOME="$tmp/home" \
            timeout -k 5 40 "$bin" --profile plain --renderer cpu --on-exit close --quit-after 20000 \
            "$@" > "$tmp/launch.out" 2> "$tmp/launch.err"
        rc=$?
        got=$(tr -d '\r\n' < "$tmp/marker" 2>/dev/null || echo MISSING)
        echo "$name: exit $rc marker=$got (want $want)"
        [ "$rc" = 0 ] || { echo "$name stderr:"; cat "$tmp/launch.err"; fail=1; }
        [ "$got" = "$want" ] || fail=1
    }
    launch "e" "e-ok" -e /bin/sh -c "printf e-ok > '$tmp/marker'"
    launch "x" "x-ok" -x /bin/sh -c "printf x-ok > '$tmp/marker'"
    launch "execute" "execute-ok" --execute /bin/sh -c "printf execute-ok > '$tmp/marker'"
    launch "working-directory" "$tmp/work" --working-directory "$tmp/work" -T 'Lunduke Terminal test' \
        -e /bin/sh -c "pwd > '$tmp/marker'"
    launch "workdir" "$tmp/work" --workdir "$tmp/work" -e /bin/sh -c "pwd > '$tmp/marker'"
    launch "working-directory=" "$tmp/work" --working-directory="$tmp/work" \
        -e /bin/sh -c "pwd > '$tmp/marker'"
    unset DISPLAY WAYLAND_DISPLAY
else
    echo "skip: exec/cwd checks (no DISPLAY)"
fi

[ "$fail" = 0 ] && echo "cli: ok"
exit $fail
