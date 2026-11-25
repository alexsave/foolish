#!/usr/bin/env python3
# Orchestrator's independent parse of trip2/out/steps.log: one row per run, then per-arm means and comparisons.
import re,sys,statistics as st
L=open('/Users/alex/Dev/foolish-bench-results/campaign/trip2/out/steps.log').read().split('\n')
runs=[];cur=None;play=None;byl={}
for l in L:
    mm=re.search(r'== (pre|post) (\S+)',l)
    if mm and mm[2] in byl:
        if mm[1]=='post': cur=byl[mm[2]]
        else: cur=byl[mm[2]]; play=cur
        continue
    m=re.match(r'\[[0-9:]+ \+\d+s\] run (\S+) arm=(\S+) build=(\S+) irq=(\S+).*think=(\S+)',l)
    if m:
        nr=dict(label=m[1],arm=m[2],build=m[3],irq=m[4],think=m[5],threads=[]);runs.append(nr);byl[m[1]]=nr;continue
    if cur is None: continue
    m=re.match(r'play moves=(\d+) window_s=([\d.]+) moves_per_s=([\d.]+) move_p50_ms=([\d.]+) move_p99_ms=([\d.]+).*move_p95_ms=([\d.]+) move_max_ms=([\d.]+)',l)
    if m and play is not None: play.update(moves=int(m[1]),mps=float(m[3]),p50=float(m[4]),p99=float(m[5]),p95=float(m[6]),mx=float(m[7]))
    m=re.match(r'cpu label=\S+ .*cpu_us_per_move=([\d.]+)',l)
    if m: cur['cpu']=float(m[1])
    m=re.match(r'thread tid=\d+ comm=(\S+) last_cpu=(\d) cpu_s=[\d.]+ cpu_us_per_move=([\d.]+)',l)
    if m: cur['threads'].append((m[1],int(m[2]),float(m[3])))
    m=re.search(r'inline dispatch: (\d+) requests on the loop thread \((\d+) sent on to the pool\), (\d+) over budget, valve tripped (\d+)',l)
    if m: cur.update(ran=int(m[1]),pool=int(m[2]),over=int(m[3]),trip=int(m[4]))
    m=re.match(r'irqpct queue_on_cpu0_pct=([\d.]+)',l)
    if m: cur['irq0']=float(m[1])
    m=re.match(r'play loadcpu all busy_pct_of_all_vcpus=([\d.]+)',l)
    if m and play is not None: play['load']=float(m[1])
    m=re.match(r'srvcpu cpu(\d) busy_pct=([\d.]+).*softirq=([\d.]+).*idle=([\d.]+)',l)
    if m: cur['busy%s'%m[1]]=float(m[2]); cur['idle%s'%m[1]]=float(m[4])
    m=re.search(r'VmHWM:\s+(\d+) kB',l)
    if m: cur['hwm']=int(m[1])
    m=re.search(r'over_100ms=(\d+)',l)
    if m: cur['p100']=int(m[1])
    m=re.search(r'err_files=(\d+)',l)
    if m and play is not None: play['err']=int(m[1])
def th(r,name):
    t=r['threads']
    if name=='loop': v=[x[2] for x in t if x[0].startswith('cbase') and x[1]==0]; return max(v) if v else float('nan')
    if name=='flusher':
        v=[x[2] for x in t if x[0].startswith('cbase')]; v=sorted(v); return v[-2] if len(v)>1 else float('nan')
    v=[x[2] for x in t if x[0]==name]; return v[0] if v else float('nan')
print('label arm build irq think | moves/s p50 p99 max | cpu/move loop flusher job req | pool trips irq0% load% idle0 idle1 hwm p100 err')
for r in runs:
    if 'mps' not in r: print(r['label'],'INCOMPLETE'); continue
    print(f"{r['label']:9s} {r['arm']} {r['build']:3s} {r['irq']:5s} {r['think']:9s} | {r['mps']:8.0f} {r['p50']:5.2f} {r['p99']:6.2f} {r['mx']:7.1f} | {r.get('cpu',0):6.2f} {th(r,'loop'):6.2f} {th(r,'flusher'):6.2f} {th(r,'job-0'):5.2f} {th(r,'req-0'):5.2f} | {r.get('pool','?')} {r.get('trip','?')} {r.get('irq0','?')} {r.get('load','?')} {r.get('idle0','?')} {r.get('idle1','?')} {r.get('hwm','?')} {r.get('p100','?')} {r.get('err','?')}")
arms={}
for r in runs:
    if 'mps' in r and re.match(r'r\d',r['label']): arms.setdefault(r['arm'],[]).append(r)
print()
def row(a):
    R=arms[a]; f=lambda k:[x[k] for x in R]; g=lambda n:[th(x,n) for x in R]
    return dict(n=len(R),mps=f('mps'),p50=f('p50'),p99=f('p99'),cpu=f('cpu'),loop=g('loop'),fl=g('flusher'),pool=[x.get('pool',0) for x in R],hwm=[x.get('hwm',0) for x in R])
S={a:row(a) for a in arms}
for a in sorted(S):
    s=S[a]; R=arms[a][0]
    print(f"{a} {R['build']:3s}/{R['irq']:5s} n={s['n']} moves/s {st.mean(s['mps']):7.0f} [{' '.join('%.0f'%v for v in s['mps'])}] p50 {st.mean(s['p50']):.2f} p99 {st.mean(s['p99']):.2f} [{' '.join('%.2f'%v for v in s['p99'])}] cpu {st.mean(s['cpu']):.2f} [{' '.join('%.2f'%v for v in s['cpu'])}] loop {st.mean(s['loop']):.2f} flusher {st.mean(s['fl']):.2f} pool {s['pool']} hwm {s['hwm']}")
