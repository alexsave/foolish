#!/usr/bin/env python3
# irqs.py: per run, ENA interrupt counts per CPU (end - start snapshot of /proc/interrupts) and NET_RX/NET_TX softirqs per CPU
import re
R = '/Users/alex/Dev/foolish-bench-results/campaign/trip1/out/raw/srv'
def irq(f):
    d = {}
    for l in open(f):
        p = l.split()
        if len(p) > 4 and p[0].endswith(':') and ('ena' in l or 'Tx-Rx' in l): d[p[-1]] = (int(p[1]), int(p[2]))
        if p and p[0] == 'EPOCH': d['_t'] = float(p[1])
    return d
def sirq(f):
    d = {}
    for l in open(f):
        p = l.split()
        if p and p[0] in ('NET_RX:', 'NET_TX:', 'TIMER:', 'SCHED:', 'RCU:', 'TASKLET:'): d[p[0][:-1]] = (int(p[1]), int(p[2]))
    return d
out = []
for l in ('base1', 'perfcy', 'perfclk', 'bpf1', 'base2', 'irq1', 'irq0'):
    a, b = irq(f'{R}/irq_{l}_start.txt'), irq(f'{R}/irq_{l}_end.txt'); s, e = sirq(f'{R}/softirq_{l}_start.txt'), sirq(f'{R}/softirq_{l}_end.txt')
    dt = b['_t'] - a['_t']
    row = [f'{l:8s} span {dt:5.1f} s']
    for k in sorted(k for k in b if k != '_t'): row.append(f'{k.split("@")[0]} cpu0 {b[k][0]-a[k][0]} cpu1 {b[k][1]-a[k][1]}')
    row.append(' | softirq ' + ' '.join(f'{k} cpu0 {e[k][0]-s[k][0]} cpu1 {e[k][1]-s[k][1]}' for k in ('NET_RX', 'NET_TX')))
    out.append('; '.join(row))
txt = '\n'.join(out); print(txt); open('/Users/alex/Dev/foolish-bench-results/campaign/trip1/out/analysis/irqs.txt', 'w').write(txt + '\n')
