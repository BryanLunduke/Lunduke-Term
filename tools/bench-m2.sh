#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M2 benchmark (stage 1, item 2): effects-quality levels on software rendering. POSIX sh.
#
#   tools/bench-m2.sh OUTDIR [BUILDDIR]
#
# Environment:
#   SIZE=1280x800  RUNS=20  LPS="1 2 4"  CPUTHREADS="4"
#   IDLE=1        also measure idle CPU of the real terminal (needs $DISPLAY)
#   IDLE_SECS=10  IDLE_WARMUP=12 (seconds before measuring; the governor settles within a few s)
#   IDLE_FOCUS=cmd   optional command run with the terminal's pid as $1 to focus its window
# Writes OUTDIR/system.txt, OUTDIR/bench.txt and OUTDIR/idle.txt.
# "full --cpu-budget 0" is the M1 behaviour (no quality reduction, no governor): the "before".
set -u
OUT=${1:?usage: bench-m2.sh OUTDIR [BUILDDIR]}
BIN=${2:-build}
SIZE=${SIZE:-1280x800}
RUNS=${RUNS:-20}
LPS=${LPS:-"1 2 4"}
CPUTHREADS=${CPUTHREADS:-4}
B="$BIN/lterm-m1-bench"
T="$BIN/lunduke-terminal"
mkdir -p "$OUT"
{
    echo "date: $(date)"
    echo "uname: $(uname -srm)"
    grep -m1 'model name' /proc/cpuinfo
    echo "cpus: $(getconf _NPROCESSORS_ONLN)"
    echo "load: $(cat /proc/loadavg)"
    LIBGL_ALWAYS_SOFTWARE=1 "$B" --tick-gl --runs 1 --size 64x64 2>&1 | head -2
} > "$OUT/system.txt" 2>&1

run() {
    e=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do e="$e $1"; shift; done
    shift
    echo "\$ env$e $B $*" >> "$OUT/bench.txt"
    # shellcheck disable=SC2086
    env $e "$B" "$@" >> "$OUT/bench.txt" 2>&1
}
: > "$OUT/bench.txt"
tl=$(echo $CPUTHREADS | tr ' ' ',')
for q in full reduced static; do
    run -- --tick --threads "$tl" --size "$SIZE" --profile amber --quality $q --runs "$RUNS"
    for n in $LPS; do
        run LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=$n -- --tick-gl --size "$SIZE" --profile amber --quality $q --runs "$RUNS"
    done
done

cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
idle() {
    label=$1; shift
    # shellcheck disable=SC2086
    qa=$(( (${IDLE_WARMUP:-12} + ${IDLE_SECS:-10} + 6) * 1000 ))
    env LIBGL_ALWAYS_SOFTWARE=1 ${LTENV:-} "$T" --size "$SIZE" --stats --quit-after "$qa" "$@" > "$OUT/idle-stats.tmp" 2>&1 &
    pid=$!
    if [ -n "${IDLE_FOCUS:-}" ]; then sleep 3; sh -c "$IDLE_FOCUS" sh "$pid" >/dev/null 2>&1; fi
    sleep "${IDLE_WARMUP:-12}"
    hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep "${IDLE_SECS:-10}"; b=$(cpu_ticks $pid)
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
    wait "$pid" 2>/dev/null  # --quit-after ends it and prints --stats
    st=$(grep -E '^stats: quality|auto -> static' "$OUT/idle-stats.tmp" | tr '\n' ' ')
    echo "$label: cpu $(awk -v a="$a" -v b="$b" -v hz="$hz" -v s="${IDLE_SECS:-10}" 'BEGIN{printf "%.1f", (b-a)*100/hz/s}')% rss ${rss:-?}kB  [$st]" >> "$OUT/idle.txt"
    LTENV=""
}
if [ "${IDLE:-0}" = 1 ]; then
    : > "$OUT/idle.txt"
    echo "# window $SIZE, idle shell, cpu = (utime+stime)/wall over ${IDLE_SECS:-10} s after ${IDLE_WARMUP:-12} s; 100% = one core" >> "$OUT/idle.txt"
    idle "plain (default)"
    idle "ibm-3278 GL (static profile)" --profile ibm-3278 --renderer gl
    idle "amber GL BEFORE (full, no governor = M1)" --profile amber --renderer gl --effects-quality full --cpu-budget 0
    for q in auto full reduced static; do idle "amber GL $q" --profile amber --renderer gl --effects-quality $q; done
    idle "amber CPU BEFORE (full, no governor = M1)" --profile amber --renderer cpu --threads 4 --effects-quality full --cpu-budget 0
    for q in auto full reduced static; do idle "amber CPU threads=4 $q" --profile amber --renderer cpu --threads 4 --effects-quality $q; done
    rm -f "$OUT/idle-stats.tmp"
fi
echo "done: $(date)" >> "$OUT/system.txt"
