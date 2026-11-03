#!/usr/bin/env python3
# psnr.py DIR_A [DIR_B]: per config, CPU vs GL PSNR in A (and B), and A vs B per back-end.
import sys, os, glob, math
import numpy as np
def load(f): return np.fromfile(f, dtype=np.uint8).reshape(-1, 4)[:, :3].astype(np.int32)
def psnr(a, b):
    d = a - b; mse = float((d * d).mean()); mx = int(np.abs(d).max())
    return (99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)), mx
A = sys.argv[1]; B = sys.argv[2] if len(sys.argv) > 2 else None
names = sorted(os.path.basename(f)[:-9] for f in glob.glob(A + '/*-cpu.rgba'))
hdr = '| config | CPU vs GL (A) |' + (' CPU vs GL (B) | CPU A vs B | GL A vs B |' if B else '')
print(hdr); print('|---' * (hdr.count('|') - 1) + '|')
worst = {}
for n in names:
    row = [n]
    pa = psnr(load(f'{A}/{n}-cpu.rgba'), load(f'{A}/{n}-gl.rgba')); row.append('%.2f dB (max %d)' % pa)
    worst.setdefault('A', []).append(pa[0])
    if B:
        pb = psnr(load(f'{B}/{n}-cpu.rgba'), load(f'{B}/{n}-gl.rgba')); row.append('%.2f dB (max %d)' % pb)
        worst.setdefault('B', []).append(pb[0])
        for k in ('cpu', 'gl'):
            p = psnr(load(f'{A}/{n}-{k}.rgba'), load(f'{B}/{n}-{k}.rgba')); row.append('%.2f dB (max %d)' % p)
            worst.setdefault(k, []).append(p[0])
    print('| ' + ' | '.join(row) + ' |')
print()
for k, v in worst.items(): print(f'min {k}: {min(v):.2f} dB')
