#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 3 host driver: logs in on tty2 over QMP, starts /t/run.sh, then answers "SHOT name" lines
# on the serial log with a screendump. One short QMP connection per action (M3's watcher held the
# socket and hung). Ends on "MARK DONE" or after the timeout.
import sys, time, os
sys.path.insert(0, '/workspace/lterm-tcg'); from qmp import QMP
SOCK = '/tmp/q-lterm-m4.sock'; SER = '/workspace/lterm-tcg/serial-m4.log'
SHOTS = sys.argv[1]; TIMEOUT = float(sys.argv[2]) if len(sys.argv) > 2 else 1800
WATCH = len(sys.argv) > 3 and sys.argv[3] == 'watch'  # no login: only answer new SHOT lines
def q(): return QMP(SOCK)
def done(c): c.f.close(); c.s.close()  # both: the makefile keeps the fd (and QMP's one client slot) open
def ty(c, s, w=2): c.type(s, delay=0.08); c.keys('ret'); time.sleep(w)
if not WATCH:
  c = q()
  c.keys('ctrl', 'alt', 'f2'); time.sleep(6)
  ty(c, 'lcos', 6); ty(c, 'live', 15)
  ty(c, 'sudo mkdir -p /t; sudo mount LABEL=LTERM /t; sudo chown -R lcos /t/out; ls /t', 6)
  c.cmd('screendump', filename=os.path.join(SHOTS, 'm4-vm-tty-login.png'), format='png')
  ty(c, 'sh /t/run.sh &', 3)
  done(c)
seen = len(open(SER, errors='replace').read().splitlines()) if WATCH else 0; t0 = time.time()
while time.time() - t0 < TIMEOUT:
    lines = open(SER, errors='replace').read().splitlines()
    for l in lines[seen:]:
        l = l.strip()
        if l.startswith('SHOT '):
            n = l.split()[1]
            try:
                c = q(); print(n, c.cmd('screendump', filename=os.path.join(SHOTS, n + '.png'), format='png'), flush=True); done(c)
            except Exception as e: print('shot failed', n, e, flush=True)
        elif l.startswith('MARK'): print(l, round(time.time() - t0), flush=True)
        if l == 'MARK DONE':
            c = q(); print(c.cmd('system_powerdown'), flush=True); done(c); sys.exit(0)
    seen = len(lines); time.sleep(2)
print('timeout', flush=True); sys.exit(1)
