#!/usr/bin/env python3
# runs.py: the per-run table from out/steps.log (the on-box summaries) plus out/analysis_moves.txt; writes out/analysis/runs.txt
import re, collections
T = '/Users/alex/Dev/foolish-bench-results/campaign/trip1/out'
txt = open(T + '/steps.log').read()
blocks = re.split(r'\n(?=\[\d\d:\d\d:\d\d\] run \w+ prof=)', txt)
out = []
for b in blocks:
    m = re.match(r'\[[\d:]+\] run (\w+) prof=(\w+)', b)
    if not m: continue
    l = m.group(1); g = lambda pat, s=b: (re.search(pat, s) or [None, ''])[1]
    full = re.search(r'moves=(\d+) window_s=([\d.]+) moves_per_s=([\d.]+) move_p50_ms=([\d.]+) move_p99_ms=([\d.]+) err_files=(\d+)', b)
    play = re.search(r'play moves=(\d+) window_s=[\d.]+ moves_per_s=([\d.]+) move_p50_ms=([\d.]+) move_p99_ms=([\d.]+)', b)
    pw = b.split('== play window')[1] if '== play window' in b else ''
    cpu = g(r'cpu_us_per_move=([\d.]+)', pw); busy1 = g(r'cpu_busy_of_one_vcpu_pct=([\d.]+)', pw)
    th = re.findall(r'^thread tid=(\d+) comm=(\S+) last_cpu=(\d) cpu_s=[\d.]+ cpu_us_per_move=([\d.]+)', pw, re.M)
    sch = re.findall(r'^sched tid=(\d+) comm=(\S+) oncpu_s=[\d.]+ oncpu_us_per_move=([\d.]+) runq_wait_s=[\d.]+ runq_wait_us_per_move=([\d.]+) slices_per_move=([\d.]+) vol_cs_per_move=([\d.]+) invol_cs_per_move=([\d.]+)', pw, re.M)
    sc = {x[0]: x for x in sch}
    c0 = re.search(r'srvcpu cpu0 busy_pct=([\d.]+) user=([\d.]+) nice=[\d.]+ system=([\d.]+) irq=([\d.]+) softirq=([\d.]+) steal=([\d.]+) idle=([\d.]+) iowait=([\d.]+)', pw)
    c1 = re.search(r'srvcpu cpu1 busy_pct=([\d.]+) user=([\d.]+) nice=[\d.]+ system=([\d.]+) irq=([\d.]+) softirq=([\d.]+) steal=([\d.]+) idle=([\d.]+) iowait=([\d.]+)', pw)
    lall = g(r'play loadcpu all busy_pct_of_all_vcpus=([\d.]+)'); lpr = re.search(r'play loadcpu procs_running_mean=([\d.]+) max=(\d+)', b)
    lmax = max([float(x) for x in re.findall(r'play loadcpu cpu\d busy_pct=([\d.]+)', b)] or [0])
    hwm = g(r'VmHWM:\s+(\d+) kB'); rss = re.search(r'\n\d+\.\d+ (\d+) (\d+) (\d+)\n', b)
    inl = g(r'(inline dispatch: [^\n]+)'); jr = g(r'(journal: [^\n]+)')
    out.append(f'## {l} (profiler {m.group(2)})')
    if full: out.append(f'E1 window: moves {full[1]} in {full[2]} s = {full[3]}/s, p50 {full[4]} ms, p99 {full[5]} ms, err_files {full[6]}')
    if play: out.append(f'play window [t_go+2,t_go+23]: moves {play[1]} = {play[2]}/s, p50 {play[3]} ms, p99 {play[4]} ms')
    out.append(f'server CPU (proc ticks, play window): {cpu} us/move, {busy1}% of one vCPU; VmHWM {hwm} kB; RSS max {rss[1] if rss else "?"} kB')
    for tid, comm, lc, u in th:
        s = sc.get(tid)
        out.append(f'  thread {tid} {comm} cpu{lc}: ticks {u} us/move' + (f'; schedstat on-CPU {s[2]} us/move, run-queue wait {s[3]} us/move, switches vol {s[5]} invol {s[6]} per move' if s else ''))
    if c0: out.append(f'  cpu0 busy {c0[1]}% (user {c0[2]} sys {c0[3]} irq {c0[4]} softirq {c0[5]} steal {c0[6]}) idle {c0[7]} iowait {c0[8]}')
    if c1: out.append(f'  cpu1 busy {c1[1]}% (user {c1[2]} sys {c1[3]} irq {c1[4]} softirq {c1[5]} steal {c1[6]}) idle {c1[7]} iowait {c1[8]}')
    out.append(f'  load box (play): {lall}% of 8 vCPUs busy, busiest vCPU {lmax}%, procs_running mean {lpr[1] if lpr else "?"} max {lpr[2] if lpr else "?"}')
    out.append(f'  server log: {inl}'); out.append(f'  server log: {jr}'); out.append('')
open(T + '/analysis/runs.txt', 'w').write('\n'.join(out)); print('\n'.join(out))
