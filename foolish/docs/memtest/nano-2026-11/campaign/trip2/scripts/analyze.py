#!/usr/bin/env python3
# analyze.py: trip 2 tables (scratch analysis, single-threaded). Inputs: out/steps.log (the on-box summaries printed by
# trip2.sh: load.sh run's summ/playwin/lreport lines, cpu.sh report/report2, irq.sh pct, srv.sh stop, the server's
# shutdown lines) and out/raw/load/LABEL.tgz (ev.* files, through scripts/pushlat.py). Writes out/analysis/runs.tsv,
# out/analysis/pushlat.txt and prints the per-run, per-arm and comparison tables as Markdown.
import re, os, sys, subprocess, tarfile, statistics as st, shutil
T = '/Users/alex/Dev/foolish-bench-results/campaign/trip2'
OUT = T + '/out'; AN = OUT + '/analysis'; os.makedirs(AN, exist_ok=True)
SCR = '/private/tmp/claude-501/-Users-alex-Dev-amgi/8dce4517-583f-417d-bdf2-8f13b7e41f4b/scratchpad/t2ev'
log = open(OUT + '/steps.log').read()

def num(pat, s, f=float):
    m = re.search(pat, s)
    return f(m.group(1)) if m else None

runs = []
for m in re.finditer(r'\] run (\S+) arm=(\S+) build=(\S+) irq=(\S+) knob=auto IP=\S+ pairs=192 think=(\d+)-(\d+) dur_ms=(\d+) window=\[t_go\+([\d.]+),t_go\+([\d.]+)\]', log):
    l = m.group(1)
    r = dict(label=l, arm=m.group(2), build=m.group(3), irq=m.group(4), pw0=float(m.group(8)), pw1=float(m.group(9)))
    # pre: the start line (pid) and the irq show line
    pre = log[log.find(f"== pre {l} build="):]
    r['pid'] = num(rf'start {l} build \S+ pid (\d+)', pre, int)
    # load output: from this run's start line to the next "] run LABEL moves=" line
    a = m.end(); b = log.find(f'] run {l} moves=', a); ld = log[a:b]
    r['moves_full'] = num(r'\nmoves=(\d+) window_s', ld, int) or num(r'moves=(\d+) window_s=[\d.]+ moves_per_s=[\d.]+ move_p50_ms=[\d.]+ move_p99_ms=[\d.]+ err_files', ld, int)
    r['err_files'] = num(r'err_files=(\d+)', ld, int)
    pl = re.search(r'play moves=(\d+) window_s=([\d.]+) moves_per_s=([\d.]+) move_p50_ms=([\d.]+) move_p99_ms=([\d.]+) .*move_p95_ms=([\d.]+) move_max_ms=([\d.]+)', ld)
    if pl: r.update(play_moves=int(pl[1]), mps=float(pl[3]), p50=float(pl[4]), p99=float(pl[5]), p95=float(pl[6]), pmax=float(pl[7]))
    r['load_busy'] = num(r'play loadcpu all busy_pct_of_all_vcpus=([\d.]+)', ld)
    lmax = [float(x) for x in re.findall(r'play loadcpu cpu\d+ busy_pct=([\d.]+)', ld)]
    r['load_busiest'] = max(lmax) if lmax else None
    r['ready_done'] = (re.search(r'ready=(\d+) done=(\d+) err_files_with_failed=(\d+)', ld) or [None])[0]
    # post block
    pa = log.find(f'== post {l}\n'); pb = log.find(f'== end post {l}', pa); po = log[pa:pb] if pa >= 0 else ''
    r['irq_cpu0'] = num(r'irqpct queue_on_cpu0_pct=([\d.-]+)', po)
    r['irq_show_post'] = (re.search(r'irq mode=show [^\n]+', po) or [''])[0]
    pw = po.split('== play window')[1] if '== play window' in po else ''
    r['cpu_us'] = num(r'cpu_us_per_move=([\d.]+) cpu_busy', pw)
    th = re.findall(r'^thread tid=(\d+) comm=(\S+) last_cpu=(\d+) cpu_s=[\d.]+ cpu_us_per_move=([\d.]+)', pw, re.M)
    sch = {x[0]: x for x in re.findall(r'^sched tid=(\d+) comm=(\S+) oncpu_s=[\d.]+ oncpu_us_per_move=([\d.]+) runq_wait_s=[\d.]+ runq_wait_us_per_move=([\d.]+)', pw, re.M)}
    pid = str(r['pid'])
    unnamed = sorted([x for x in th if x[1].startswith('cbase_') and x[0] != pid], key=lambda x: -float(x[3]))
    def thr(x):
        if not x: return (None, None)
        s = sch.get(x[0]); return (float(x[3]), float(s[3]) if s else None)
    loop = [x for x in th if x[0] == pid]; job = [x for x in th if x[1] == 'job-0']; req = [x for x in th if x[1] == 'req-0']
    r['loop_us'], r['loop_rq'] = thr(loop[0] if loop else None)
    r['flusher_us'], r['flusher_rq'] = thr(unnamed[0] if unnamed else None)
    r['job_us'], r['job_rq'] = thr(job[0] if job else None)
    r['req_us'], r['req_rq'] = thr(req[0] if req else None)
    r['rq_total'] = num(r'sched total oncpu_us_per_move=[\d.]+ runq_wait_us_per_move=([\d.]+)', pw)
    r['cpu0_busy'] = num(r'srvcpu 0 busy_pct=([\d.]+)', pw) or num(r'srvcpu cpu0 busy_pct=([\d.]+)', pw)
    r['cpu0_softirq'] = num(r'srvcpu (?:cpu)?0 busy_pct=[\d.]+ user=[\d.]+ nice=[\d.]+ system=[\d.]+ irq=[\d.]+ softirq=([\d.]+)', pw)
    r['cpu1_busy'] = num(r'srvcpu (?:cpu)?1 busy_pct=([\d.]+)', pw)
    r['vmhwm'] = num(r'VmHWM:\s+(\d+) kB', po, int)
    # RSS max from the raw sampler log (epoch VmRSS VmHWM RssAnon every 0.2 s); a sample with an empty field is skipped
    # (r2C's on-box 'sort | tail -1' picked such a line)
    rf = f"{OUT}/raw/srv/rss_{l}.log"
    rv = [int(x.split()[1]) for x in open(rf) if len(x.split()) == 4] if os.path.exists(rf) else []
    r['rssmax'] = max(rv) if rv else None
    inl = re.search(r'inline dispatch: (\d+) requests on the loop thread \((\d+) sent on to the pool\), (\d+) over budget, valve tripped (\d+) times', po)
    if inl: r.update(inl_loop=int(inl[1]), to_pool=int(inl[2]), over_budget=int(inl[3]), valve=int(inl[4]))
    jr = re.search(r'journal: (\d+) record\(s\) in (\d+) batch\(es\), (\d+) write\(s\), (\d+) byte\(s\); (\d+) flush\(es\) of (\d+) move\(s\); (\d+) WAL frame\(s\)', po)
    if jr: r.update(j_records=int(jr[1]), j_batches=int(jr[2]), j_flushes=int(jr[5]), j_wal=int(jr[7]))
    r['warn_lines'] = num(r'warn_lines (\d+)', po, int)
    runs.append(r)

# push latency from the raw tgz of each run
pl_lines = []
for r in runs:
    tg = f"{OUT}/raw/load/{r['label']}.tgz"
    if not os.path.exists(tg): r['push_note'] = 'no tgz'; continue
    shutil.rmtree(SCR, ignore_errors=True); os.makedirs(SCR)
    with tarfile.open(tg) as tf: tf.extractall(SCR, filter='data')
    o = subprocess.run([sys.executable, '-I', T + '/scripts/pushlat.py', f"{SCR}/{r['label']}", str(r['pw0']), str(r['pw1'])], capture_output=True, text=True).stdout.strip()
    pl_lines.append(f"{r['label']} {o}")
    for k, v in re.findall(r'(\w+)=([\d.na-]+)', o): r['pl_' + k] = v
shutil.rmtree(SCR, ignore_errors=True)
open(AN + '/pushlat.txt', 'w').write('\n'.join(pl_lines) + '\n')
keys = sorted({k for r in runs for k in r})
with open(AN + '/runs.tsv', 'w') as f:
    f.write('\t'.join(keys) + '\n')
    for r in runs: f.write('\t'.join(str(r.get(k, '')) for k in keys) + '\n')
print(f"{len(runs)} runs parsed; written {AN}/runs.tsv and pushlat.txt")

# ---- tables ----
def f(v, d=1):
    if v is None or v == '': return 'n/a'
    try: return f"{float(v):,.{d}f}"
    except ValueError: return str(v)
pli = lambda r, k: (int(float(r['pl_' + k])) if r.get('pl_' + k) not in (None, '', 'nan') else None)
md = []
md.append('| run | arm | build | irq | moves/s | p50 ms | p99 ms | CPU us/move | loop | flusher | job-0 | req-0 | runq wait us/move | VmHWM kB | RSS max kB | err_files | queue IRQs on CPU 0 % | load busy % (busiest vCPU) | to pool | over budget | valve | journal records / flushes / WAL frames | push n | push p50 / p99 ms | push > 100 ms |')
md.append('|' + '---|' * 4 + '--:|' * 21)
for r in runs:
    nm = r['label'] + (' (warm-up, not counted)' if r['label'] == 'wA' else '')
    md.append(f"| {nm} | {r['arm']} | {r['build']} | {r['irq']} | {f(r.get('mps'))} | {f(r.get('p50'),2)} | {f(r.get('p99'),2)} | {f(r.get('cpu_us'),2)} | {f(r.get('loop_us'),2)} | {f(r.get('flusher_us'),2)} | {f(r.get('job_us'),2)} | {f(r.get('req_us'),2)} | {f(r.get('rq_total'),2)} | {f(r.get('vmhwm'),0)} | {f(r.get('rssmax'),0)} | {r.get('err_files')} | {f(r.get('irq_cpu0'))} | {f(r.get('load_busy'))} ({f(r.get('load_busiest'))}) | {r.get('to_pool','n/a')} | {r.get('over_budget','n/a')} | {r.get('valve','n/a')} | {r.get('j_records','n/a')} / {r.get('j_flushes','n/a')} / {r.get('j_wal','n/a')} | {r.get('pl_push_n','n/a')} | {f(r.get('pl_push_p50_ms'),2)} / {f(r.get('pl_push_p99_ms'),2)} | {r.get('pl_push_over_100ms','n/a')} |")
open(AN + '/table_runs.md', 'w').write('\n'.join(md) + '\n'); print('\n'.join(md)); print()

counted = [r for r in runs if re.match(r'r\d[A-Z]$', r['label'])]
arms = {}
for r in counted: arms.setdefault(r['arm'], []).append(r)
MET = [('moves/s', 'mps', 0), ('p50 ms', 'p50', 2), ('p99 ms', 'p99', 2), ('CPU us/move', 'cpu_us', 2), ('loop us/move', 'loop_us', 2),
       ('flusher us/move', 'flusher_us', 2), ('RSS max kB', 'rssmax', 0), ('push > 100 ms', 'pl_push_over_100ms', 0), ('to pool', 'to_pool', 0), ('valve trips', 'valve', 0)]
def vals(a, k): return [float(r[k]) for r in arms.get(a, []) if r.get(k) not in (None, '', 'nan')]
desc = {'A': 'D / split', 'B': 'P1 / split', 'C': 'P3 / split', 'E': 'ALL / split', 'F': 'D / cpu1', 'G': 'ALL / cpu1', 'N': 'N1 / split'}
md = ['| arm | n | ' + ' | '.join(m[0] + ' mean (values)' for m in MET) + ' |', '|---|--:|' + '---|' * len(MET)]
for a in 'ABCEFGN':
    if a not in arms: continue
    cells = []
    for nm, k, d in MET:
        v = vals(a, k); cells.append(f"{f(st.mean(v), d)} ({', '.join(f(x, d) for x in v)})" if v else 'n/a')
    md.append(f"| {a} ({desc[a]}) | {len(arms[a])} | " + ' | '.join(cells) + ' |')
open(AN + '/table_arms.md', 'w').write('\n'.join(md) + '\n'); print('\n'.join(md)); print()

CMP = [('B', 'A', 'P1 vs baseline (split)'), ('C', 'A', 'P3 vs baseline (split)'), ('E', 'A', 'ALL vs baseline (split)'), ('N', 'A', 'N1 vs baseline (split)'),
       ('F', 'A', 'interrupt layout alone (cpu1 vs split, cbase_D)'), ('G', 'F', 'the PRs under cpu1 (ALL vs D)'), ('G', 'A', 'ALL cpu1 vs baseline split')]
md = ['| comparison | metric | X mean | Y mean | X - Y | % | range X | range Y | sets overlap? | outside spread? |', '|---|---|--:|--:|--:|--:|--:|--:|---|---|']
for x, y, what in CMP:
    for nm, k, d in (('moves/s', 'mps', 0), ('CPU us/move', 'cpu_us', 2), ('loop us/move', 'loop_us', 2), ('flusher us/move', 'flusher_us', 2), ('p99 ms', 'p99', 2)):
        vx, vy = vals(x, k), vals(y, k)
        if not vx or not vy: md.append(f"| {x} vs {y}: {what} | {nm} | n/a | n/a | | | | | | |"); continue
        mx, my = st.mean(vx), st.mean(vy); rx, ry = max(vx) - min(vx), max(vy) - min(vy)
        ov = not (min(vx) > max(vy) or min(vy) > max(vx))
        out = (not ov) and abs(mx - my) > max(rx, ry)
        md.append(f"| {x} vs {y}: {what} | {nm} | {f(mx,d)} | {f(my,d)} | {mx-my:+,.{d}f} | {100*(mx-my)/my:+.1f}% | {f(rx,d)} | {f(ry,d)} | {'yes' if ov else 'no'} | {'YES' if out else 'no'} |")
open(AN + '/table_cmp.md', 'w').write('\n'.join(md) + '\n'); print('\n'.join(md))
