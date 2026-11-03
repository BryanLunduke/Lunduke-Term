#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs tests/libvterm_diff.c built against two libvterm builds for a range of seeds and checks
that their outputs are byte-identical (and that both exit the same way).

Usage: libvterm_diff_run.py A_EXE B_EXE MODE FIRST_SEED COUNT STEPS [-j N] [--timeout S]
Prints one summary line; on a difference, the seed, the offset and both sides around it.
"""
import argparse
import concurrent.futures
import subprocess
import sys
import tempfile
import threading


CHUNK = 1 << 16


def compare(args, seed):
    """Streams both outputs and compares them chunk by chunk (memory stays bounded and a hang
    is killed after --timeout seconds). Returns (seed, message or None, bytes, exit code)."""
    cmd = [str(seed), str(args.steps), args.mode]
    errs = [tempfile.TemporaryFile(), tempfile.TemporaryFile()]
    procs = [subprocess.Popen([exe] + cmd, stdout=subprocess.PIPE, stderr=err) for exe, err in zip((args.a, args.b), errs)]
    timer = threading.Timer(args.timeout, lambda: [p.kill() for p in procs])
    timer.start()
    total, tail, diff = 0, b"", None
    try:
        while True:
            ca, cb = procs[0].stdout.read(CHUNK), procs[1].stdout.read(CHUNK)
            if ca != cb:
                off = next((i for i in range(min(len(ca), len(cb))) if ca[i] != cb[i]), min(len(ca), len(cb)))
                ctx = tail[-600:]
                diff = (total + off, ctx + ca[:off + 300], ctx + cb[:off + 300])
                break
            if not ca:
                break
            total += len(ca)
            tail = ca[-600:]
    finally:
        for p in procs:
            p.stdout.close()
        rcs = [p.wait() for p in procs]
        timed_out = not timer.is_alive()
        timer.cancel()
    stderr = []
    for e in errs:
        e.seek(0)
        stderr.append(e.read()[-2000:].decode("utf-8", "replace"))
        e.close()
    if timed_out:
        return seed, f"seed {seed}: TIMEOUT after {args.timeout} s (exit {rcs})\n" + "\n".join(stderr), total, -999
    if diff is None and rcs[0] == rcs[1]:
        return seed, None, total, rcs[0]
    msg = f"seed {seed}: DIFFERENT (exit {rcs[0]} vs {rcs[1]})"
    if diff:
        off, sa, sb = diff
        msg += (f", first difference at byte {off}\n--- {args.a}\n{sa.decode('utf-8', 'replace')}\n"
                f"--- {args.b}\n{sb.decode('utf-8', 'replace')}")
    msg += "\n--- stderr\n" + "\n".join(stderr)
    return seed, msg, total, rcs[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("mode", choices=["a", "b"])
    ap.add_argument("first", type=int)
    ap.add_argument("count", type=int)
    ap.add_argument("steps", type=int)
    ap.add_argument("-j", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=120, help="seconds per seed")
    args = ap.parse_args()
    seeds = range(args.first, args.first + args.count)
    total = 0
    bad = []
    exits = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
        for seed, msg, n, rc in pool.map(lambda s: compare(args, s), seeds):
            total += n
            exits[rc] = exits.get(rc, 0) + 1
            if msg:
                bad.append(seed)
                if len(bad) <= 3:
                    print(msg)
    ex = ", ".join(f"exit {k}: {v}" for k, v in sorted(exits.items()))
    print(f"libvterm diff mode {args.mode}: seeds {args.first}..{args.first + args.count - 1}, {args.steps} steps each, "
          f"{total} bytes compared per side, {len(bad)} different or timed out ({ex})")
    if bad:
        print("different seeds:", " ".join(map(str, bad[:50])))
        return 1
    if any(k != 0 for k in exits):
        print("non-zero exits (the same in both builds)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
