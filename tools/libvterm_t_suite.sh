#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs libvterm's own test suite (third_party/libvterm/t/*.test, driven by t/run-test.pl, as
# upstream's `make test` does) against one build of t/harness.c.
# Usage: libvterm_t_suite.sh HARNESS LIBVTERM_DIR
set -eu
harness=$(realpath "$1")
cd "$2"
n=0
for t in t/[0-9]*.test; do
    if ! out=$(perl t/run-test.pl -e "$harness" "$t" 2>&1); then
        printf '** %s **\n%s\n' "$t" "$out"
        echo "FAIL: $t ($n passed before it)"
        exit 1
    fi
    n=$((n + 1))
done
echo "libvterm t/ suite: $n test files passed ($harness)"
