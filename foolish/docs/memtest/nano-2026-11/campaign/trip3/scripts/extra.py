#!/usr/bin/env python3
# extra.py: trip 3 supplementary tables from out/analysis/runs.tsv (written by analyze.py) and out/steps.log (per-CPU idle,
# which analyze.py does not keep): per-run tail and push latency, run-queue wait per thread, per-CPU busy and idle; and the
# acceptance verdicts per comparison (moves/s up or neutral, CPU per move not worse, p99 not worse, pushes over 100 ms not
# worse, RSS not worse). "Not worse" = the X mean is not above the Y mean, or the difference is not outside the spread
# (trip 2's rule: run sets do not overlap AND the difference exceeds the larger of the two ranges). Writes
# out/analysis/table_extra.md and out/analysis/table_accept.md.
import csv, re, statistics as st
T = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/out'
runs = list(csv.DictReader(open(T + '/analysis/runs.tsv'), delimiter='\t'))
log = open(T + '/steps.log').read()
for r in runs:
    l = r['label']; pa = log.find(f'== post {l}\n'); pb = log.find(f'== end post {l}', pa); po = log[pa:pb]
    pw = po.split('== play window')[1] if '== play window' in po else ''
    for c in ('0', '1'):
        m = re.search(rf'srvcpu cpu{c} busy_pct=([\d.]+) .*softirq=([\d.]+) steal=[\d.]+ idle=([\d.]+) iowait=([\d.]+)', pw)
        r[f'c{c}'] = f'{m[1]} / {m[2]} / {m[3]}' if m else 'n/a'
    m = re.search(r'srvcpu procs_running_mean=([\d.]+)', pw); r['prun'] = m[1] if m else 'n/a'
f = lambda v, d=2: 'n/a' if v in (None, '', 'None') else f'{float(v):,.{d}f}'
md = ['| run | move p95 ms | move max ms | push p50 / p99 / max ms | push missing | runq wait us/move loop / flusher / job-0 / req-0 | CPU 0 busy / softirq / idle % | CPU 1 busy / softirq / idle % | requests on the loop | warn lines |',
      '|---|--:|--:|---|--:|---|---|---|--:|--:|']
for r in runs:
    md.append(f"| {r['label']} | {f(r['p95'])} | {f(r['pmax'])} | {f(r['pl_push_p50_ms'])} / {f(r['pl_push_p99_ms'])} / {f(r['pl_push_max_ms'],1)} | {r['pl_push_missing']} | "
              f"{f(r['loop_rq'])} / {f(r['flusher_rq'])} / {f(r['job_rq'])} / {f(r['req_rq'])} | {r['c0']} | {r['c1']} | {r['inl_loop']} | {r['warn_lines']} |")
open(T + '/analysis/table_extra.md', 'w').write('\n'.join(md) + '\n'); print('\n'.join(md)); print()

counted = [r for r in runs if re.match(r'r\d(D|M\d)$', r['label'])]
def vals(a, k): return [float(r[k]) for r in counted if r['arm'] == a and r.get(k) not in (None, '', 'nan')]
def outside(vx, vy):
    mx, my = st.mean(vx), st.mean(vy); ov = not (min(vx) > max(vy) or min(vy) > max(vx))
    return (not ov) and abs(mx - my) > max(max(vx) - min(vx), max(vy) - min(vy))
CRIT = [('moves/s up or neutral', 'mps', +1), ('CPU per move not worse', 'cpu_us', -1), ('p99 not worse', 'p99', -1),
        ('pushes over 100 ms not worse', 'pl_push_over_100ms', -1), ('RSS max not worse', 'rssmax', -1)]
CMP = [('M1', 'D', 'PR 496 on merged code'), ('M2', 'M1', 'PR 497'), ('M3', 'M2', 'PR 498'), ('M4', 'M3', 'PR 500'), ('M3', 'D', 'everything merged')]
md = ['| comparison | ' + ' | '.join(c[0] for c in CRIT) + ' |', '|---|' + '---|' * len(CRIT)]
for x, y, what in CMP:
    cells = []
    for nm, k, good in CRIT:
        vx, vy = vals(x, k), vals(y, k); d = st.mean(vx) - st.mean(vy); o = outside(vx, vy)
        if d * good >= 0: v = 'pass (better, outside the spread)' if o else 'pass (better or equal, within the spread)'
        else: v = 'FAIL (worse, outside the spread)' if o else 'pass (worse within the spread: neutral)'
        cells.append(f'{v}; {d:+,.2f}')
    md.append(f'| {x} vs {y} ({what}) | ' + ' | '.join(cells) + ' |')
open(T + '/analysis/table_accept.md', 'w').write('\n'.join(md) + '\n'); print('\n'.join(md))
