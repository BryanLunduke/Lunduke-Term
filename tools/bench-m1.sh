#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# M1 benchmark matrix (checkpoint C, item 5). POSIX sh, no extra dependencies.
#
#   tools/bench-m1.sh OUTDIR [BUILDDIR]      (BUILDDIR: where lterm-m1-bench / lunduke-terminal are)
#
# Environment:
#   SIZES="1280x800 1920x1080"  RUNS=20  THREADS="1 2 4"
#   NOAVX=1        also run "no AVX" variants: llvmpipe with GALLIUM_OVERRIDE_CPU_CAPS=sse4.1 and
#                  LP_NATIVE_VECTOR_WIDTH=128; CPU path with pixman/glibc AVX paths masked
#   IDLE=1         also measure idle CPU of the real terminal (needs $DISPLAY); IDLE_SECS=10
#   IDLE_WARMUP=6  seconds after launch before measuring
#   IDLE_FOCUS=cmd optional command run with the terminal's pid as $1 to focus its window
# Writes OUTDIR/system.txt, OUTDIR/bench.txt (raw benchmark lines) and OUTDIR/idle.txt.
set -u
OUT=${1:?usage: bench-m1.sh OUTDIR [BUILDDIR]}
BIN=${2:-build}
SIZES=${SIZES:-"1280x800 1920x1080"}
RUNS=${RUNS:-20}
THREADS=${THREADS:-"1 2 4"}
NOAVX=${NOAVX:-1}
B="$BIN/lterm-m1-bench"
T="$BIN/lunduke-terminal"
mkdir -p "$OUT"

# Masks for the "no AVX" variants. llvmpipe honours both variables (GALLIUM_DUMP_CPU=true shows
# avx=0 avx2=0); pixman honours PIXMAN_DISABLE, glibc the hwcaps tunable. Our own code is built
# for baseline x86-64 (no -march), see the ymm/zmm count in system.txt.
NOAVX_GL="GALLIUM_OVERRIDE_CPU_CAPS=sse4.1 LP_NATIVE_VECTOR_WIDTH=128"
NOAVX_CPU="PIXMAN_DISABLE=avx2 GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX,-AVX2,-AVX512F,-AVX512VL,-AVX512BW,-AVX512DQ,-FMA"

{
    echo "date: $(date)"
    echo "uname: $(uname -srm)"
    grep -m1 'model name' /proc/cpuinfo
    echo "cpus: $(getconf _NPROCESSORS_ONLN)"
    echo "flags (avx*/sse4*): $(grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\n' | grep -E '^(avx|sse4|fma)' | tr '\n' ' ')"
    grep -E 'MemTotal|MemAvailable' /proc/meminfo
    echo "load: $(cat /proc/loadavg)"
    if command -v objdump >/dev/null 2>&1; then
        for f in "$B" "$T"; do
            echo "$(basename "$f"): ymm/zmm instructions: $(objdump -d "$f" 2>/dev/null | grep -cE '%[yz]mm')"
        done
    fi
    LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DUMP_CPU=true "$B" --tick-gl --runs 1 --size 64x64 2>&1 | grep -iE 'avx|sse4|llvmpipe' | head -8
    echo "no-AVX llvmpipe ($NOAVX_GL):"
    # shellcheck disable=SC2086
    env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DUMP_CPU=true $NOAVX_GL "$B" --tick-gl --runs 1 --size 64x64 2>&1 | grep -iE 'has_avx =|has_avx2|llvmpipe' | head -4
} > "$OUT/system.txt" 2>&1

run() {  # run [VAR=val ...] -- args   (env vars via env(1); logs the command line)
    e=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do e="$e $1"; shift; done
    shift
    echo "\$ env$e $B $*" >> "$OUT/bench.txt"
    # shellcheck disable=SC2086
    env $e "$B" "$@" >> "$OUT/bench.txt" 2>&1
}

: > "$OUT/bench.txt"
for s in $SIZES; do
    tl=$(echo $THREADS | tr ' ' ',')
    for p in plain amber; do run -- --paint --size "$s" --profile "$p" --runs "$RUNS"; done
    run -- --tick --threads "$tl" --size "$s" --profile amber --runs "$RUNS"
    if [ "$NOAVX" = 1 ]; then
        run $NOAVX_CPU -- --paint --size "$s" --profile amber --runs "$RUNS"
        run $NOAVX_CPU -- --tick --threads "$tl" --size "$s" --profile amber --runs "$RUNS"
    fi
    for n in $THREADS; do
        run LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=$n -- --tick-gl --size "$s" --profile amber --runs "$RUNS"
        [ "$NOAVX" = 1 ] && run LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=$n $NOAVX_GL -- --tick-gl --size "$s" --profile amber --runs "$RUNS"
    done
done

cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

# Idle CPU: utime+stime from /proc over IDLE_SECS while the shell sits at a prompt.
# LTENV (space-separated VAR=val) is passed through env(1).
idle() {
    label=$1; shift
    # shellcheck disable=SC2086
    env LIBGL_ALWAYS_SOFTWARE=1 ${LTENV:-} "$T" --size 1280x800 "$@" >/dev/null 2>&1 &
    pid=$!
    sleep "${IDLE_WARMUP:-6}"
    if [ -n "${IDLE_FOCUS:-}" ]; then sh -c "$IDLE_FOCUS" sh "$pid" >/dev/null 2>&1; sleep 2; fi
    hz=$(getconf CLK_TCK); a=$(cpu_ticks $pid); sleep "${IDLE_SECS:-10}"; b=$(cpu_ticks $pid)
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
    echo "$label: cpu $(awk -v a="$a" -v b="$b" -v hz="$hz" -v s="${IDLE_SECS:-10}" 'BEGIN{printf "%.1f", (b-a)*100/hz/s}')% rss ${rss:-?}kB" >> "$OUT/idle.txt"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    LTENV=""
}

if [ "${IDLE:-0}" = 1 ]; then
    : > "$OUT/idle.txt"
    echo "# window 1280x800, idle shell, cpu = (utime+stime)/wall over ${IDLE_SECS:-10} s; 100% = one core" >> "$OUT/idle.txt"
    idle "plain (default)"
    idle "amber GL" --profile amber --renderer gl
    for n in $THREADS; do idle "amber CPU threads=$n" --profile amber --renderer cpu --threads "$n"; done
    for n in $THREADS; do LTENV="LP_NUM_THREADS=$n"; idle "amber GL LP_NUM_THREADS=$n" --profile amber --renderer gl; done
    if [ "$NOAVX" = 1 ]; then
        for n in $THREADS; do
            LTENV="LP_NUM_THREADS=$n $NOAVX_GL"; idle "amber GL LP_NUM_THREADS=$n no-AVX" --profile amber --renderer gl
        done
        LTENV="$NOAVX_CPU"; idle "amber CPU threads=4 no-AVX" --profile amber --renderer cpu --threads 4
    fi
    idle "amber GL --effects-fps 10" --profile amber --renderer gl --effects-fps 10
    idle "ibm-3278 GL (static profile)" --profile ibm-3278 --renderer gl
fi
echo "done: $(date)" >> "$OUT/system.txt"
