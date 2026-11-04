# SPDX-License-Identifier: GPL-3.0-or-later
import re,glob,statistics as st,os
import sys; R=sys.argv[1] if len(sys.argv)>1 else '/workspace/perf3-work/measure/raw'
def pd(f):
    if not os.path.exists(f): return None
    m=re.search(r'Private_Dirty:\s+(\d+)',open(f).read()); return int(m.group(1)) if m else None
rows=[];agg={}
for prof in ['amber-cpu-1280']:
  for b in ['before','after']:
    for r in [1,2,3]:
      t=f'p3r-{b}-{prof}-r{r}'; s=f'{R}/{t}-summary.txt'
      if not os.path.exists(s): continue
      S=open(s).read()
      g=lambda k: (re.search(k+r' (\S+)',S) or [None,None])[1]
      st_=re.search(r'median paint ([\d.]+) ms, median effects-source ([\d.]+) ms, median tick ([\d.]+) ms',S)
      cf=re.search(r'content-frames=(\d+).*ticks=(\d+)',S)
      geom=re.search(r'Position: (\S+)',S); wm='yes' if 'wm_alive_before_after yes' in S else 'NO'
      v=dict(fresh=pd(f'{R}/{t}-fresh.txt'),after=pd(f'{R}/{t}-after.txt'),i0=g('idle_cpu_pct_30s'),i1=g('idle_cpu_pct_after_30s'),
             paint=st_ and st_.group(1),src=st_ and st_.group(2),tick=st_ and st_.group(3),work=g('work_cpu_s'))
      rows.append(f"| {prof} | {b} | {r} | {v['i0']} | {v['i1']} | {v['fresh']} | {v['after']} | {v['tick']} | {v['src']} | {v['paint']} | {v['work']} | {cf and cf.group(1)}/{cf and cf.group(2)} | {geom and geom.group(1)} | {wm} |")
      agg.setdefault((prof,b),[]).append(v)
print('| profile | build | run | idle CPU % fresh | idle CPU % after work | Private_Dirty fresh kB | Private_Dirty after kB | median tick ms | median effects-source ms | median paint ms | work CPU s | content frames/ticks | window pos | WM alive before+after |')
print('|'+'---|'*14); print('\n'.join(rows)); print()
def med(l,k):
    x=[float(v[k]) for v in l if v[k] not in (None,) and re.match(r'^-?[\d.]+$',str(v[k]))]
    return (st.median(x),len(x)) if x else (None,0)
print('| profile | idle CPU % fresh | idle CPU % after work | PD fresh kB (delta) | PD after kB (delta) | median tick ms | median effects-source ms |')
print('|---|---|---|---|---|---|---|')
for prof in ['amber-cpu-1280']:
  B=agg.get((prof,'before'),[]);A=agg.get((prof,'after'),[])
  def c(k,d=False):
    b,nb=med(B,k);a,na=med(A,k)
    if b is None or a is None: return 'n/a'
    s=f'{b:g} -> {a:g}'
    if d: s+=f' ({a-b:+g})'
    return s+(f' (n {nb}/{na})' if not d and (nb<3 or na<3) else '')
  print(f"| {prof} | {c('i0')} | {c('i1')} | {c('fresh',1)} | {c('after',1)} | {c('tick')} | {c('src')} |")
