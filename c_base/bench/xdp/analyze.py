#!/usr/bin/env python3
# analyze.py RESULTS_DIR : the tables of the results doc from results.txt (RUN lines and the server snapshots taken at
# the start and the end of each run's measurement window). Per-move numbers divide by the server's own move counter.
# /proc/stat is in 10 ms ticks, so box CPU over a 45 s window is good to about 1%; the server's own CPU comes from
# /proc/self/schedstat (ns).
import sys, re, collections, statistics as stt

d = sys.argv[1]
runs, s0, s1 = {}, {}, {}
for line in open(d + '/results.txt'):
    parts = line.split()
    if not parts:
        continue
    kv = {}
    for p in parts[1:]:
        if '=' in p:
            k, v = p.split('=', 1)
            kv[k] = v
    lab = kv.get('label')
    {'RUN': runs, 'SNAP0': s0, 'SNAP1': s1}.get(parts[0], {})[lab] = kv


def num(x):
    try:
        return float(x)
    except Exception:
        return None


def delta(lab):
    a, b = s0.get(lab), s1.get(lab)
    if not a or not b or 't_ns' not in a or 't_ns' not in b:
        return None
    return {k: num(b[k]) - num(a[k]) for k in b if k in a and num(b[k]) is not None and num(a[k]) is not None}


def cpus(dl):
    return sorted({int(m.group(1)) for k in dl for m in [re.match(r'c(\d+)_user', k)] if m})


def metrics(lab):
    dl = delta(lab)
    r = runs.get(lab, {})
    if not dl:
        return None
    dt = dl['t_ns'] / 1e9
    mv = dl.get('moves', 0)
    cs = cpus(dl)
    T = 10000.0  # us per tick (USER_HZ 100)
    busy = sum(dl['c%d_%s' % (c, f)] for c in cs for f in ('user', 'nice', 'sys', 'irq', 'softirq')) * T
    m = {'dt': dt, 'moves': mv, 'rate': mv / dt if dt else 0,
         'box_busy_us_s': busy / dt, 'box_pct': 100.0 * busy / (dt * 1e6 * len(cs)) if cs else 0,
         'steal_pct': 100.0 * sum(dl['c%d_steal' % c] for c in cs) * T / (dt * 1e6 * len(cs)) if cs else 0,
         'si_us_s': sum(dl['c%d_softirq' % c] for c in cs) * T / dt, 'irq_us_s': sum(dl['c%d_irq' % c] for c in cs) * T / dt,
         'srv_us_s': dl.get('sched_ns', 0) / 1e3 / dt,
         'idlex_s': sum(dl.get('idlex_%d' % c, 0) for c in cs) / dt}
    for c in cs:
        m['si%d_us_s' % c] = dl['c%d_softirq' % c] * T / dt
        m['irq%d_s' % c] = dl.get('nicirq_%d' % c, 0) / dt
        m['idlex%d_s' % c] = dl.get('idlex_%d' % c, 0) / dt
    if mv:
        sc = sum(dl.get(k, 0) for k in ('sc_wait', 'sc_recv', 'sc_send', 'sc_accept', 'sc_kick', 'sc_other'))
        m.update(srv_us=dl.get('sched_ns', 0) / 1e3 / mv, srv_ru_us=(dl.get('ut_us', 0) + dl.get('st_us', 0)) / mv,
                 box_us=busy / mv, si_us=sum(dl['c%d_softirq' % c] for c in cs) * T / mv,
                 irq_us=sum(dl['c%d_irq' % c] for c in cs) * T / mv,
                 idlex=sum(dl.get('idlex_%d' % c, 0) for c in cs) / mv,
                 cs=(dl.get('nvcsw', 0) + dl.get('nivcsw', 0)) / mv, box_ctxt=dl.get('ctxt', 0) / mv,
                 sc=sc / mv, sends=dl.get('sc_send', 0) / mv, kicks=dl.get('sc_kick', 0) / mv, waits=dl.get('sc_wait', 0) / mv,
                 nicirq=sum(dl.get('nicirq_%d' % c, 0) for c in cs) / mv,
                 netrx=sum(dl.get('si_NET_RX_%d' % c, 0) for c in cs) / mv,
                 tp_sys=dl.get('tp_sys', 0) / mv if 'tp_sys' in dl else None,
                 rexmit=dl.get('rexmit', 0), tx_pkts=dl.get('tx_pkts', 0) / mv, rx_pkts=dl.get('rx_pkts', 0) / mv)
    for k in ('p50_us', 'p95_us', 'p99_us', 'p999_us', 'max_us', 'mean_us', 'lost', 'dups', 'gaps', 'badbytes', 'badmeta',
              'done_win', 'completed', 'moves_sent', 'client_cpu_us'):
        if k in r:
            m[k] = num(r[k])
    m['raw'] = dl
    return m


M = {lab: metrics(lab) for lab in runs}
M = {k: v for k, v in M.items() if v}

# idle floors: per (backend, rep, scenario block)
idle = {}
for lab, m in M.items():
    g = re.match(r'idle_(kern|kernx|xdp)_r(\d)_?(\w*)', lab)
    if g:
        idle[(g.group(1), g.group(2), g.group(3))] = m

BE = ['kern', 'kernx', 'xdp', 'xdpP']
rows = collections.defaultdict(list)
for lab, m in M.items():
    g = re.match(r'(hum|sat)_k(\d+)_s(\d+)_w(\d+)_(kern|kernx|xdp|xdpP)_r(\d)', lab)
    if not g:
        continue
    sc, k, size, spin, be, rep = g.groups()
    fl = idle.get(({'xdpP': 'xdp'}.get(be, be), rep, sc))
    if fl and m.get('moves'):
        m['box_net_us'] = (m['box_busy_us_s'] - fl['box_busy_us_s']) * m['dt'] / m['moves']
        m['idlex_net'] = (m['idlex_s'] - fl['idlex_s']) * m['dt'] / m['moves']
    rows[(sc, int(k), int(size), int(spin), be)].append(m)


def agg(ms, key, fmt='%.1f'):
    v = [m[key] for m in ms if m.get(key) is not None]
    if not v:
        return '-'
    if len(v) == 1:
        return fmt % v[0]
    return (fmt % stt.mean(v)) + ' (' + (fmt % min(v)) + '-' + (fmt % max(v)) + ')'


def table(sc, cols, title):
    print('\n### %s (%s)\n' % (title, sc))
    print('| K | size | spin | backend | ' + ' | '.join(c[1] for c in cols) + ' |')
    print('|' + '---|' * (4 + len(cols)))
    for (s, k, size, spin, be) in sorted(rows, key=lambda x: (x[1], x[2], -x[3], BE.index(x[4]))):
        if s != sc:
            continue
        ms = rows[(s, k, size, spin, be)]
        print('| %d | %d | %d | %s | ' % (k, size, spin, be) + ' | '.join(agg(ms, c[0], c[2] if len(c) > 2 else '%.1f') for c in cols) + ' |')


cpu_cols = [('srv_us', 'server CPU us/move'), ('box_us', 'box CPU us/move'), ('box_net_us', 'box CPU us/move above idle'),
            ('si_us', 'softirq us/move'), ('irq_us', 'irq us/move'), ('sc', 'syscalls/move'), ('cs', 'ctx sw/move', '%.2f'),
            ('idlex', 'idle exits/move', '%.2f'), ('nicirq', 'NIC irqs/move', '%.2f'), ('rate', 'moves/s', '%.0f'),
            ('box_pct', 'box CPU %', '%.2f')]
lat_cols = [('p50_us', 'p50 us'), ('p95_us', 'p95 us'), ('p99_us', 'p99 us'), ('max_us', 'max us', '%.0f'), ('lost', 'lost', '%.0f'),
            ('dups', 'dup', '%.0f'), ('gaps', 'gaps', '%.0f'), ('badbytes', 'bad bytes', '%.0f')]
for sc in ('hum', 'sat'):
    table(sc, cpu_cols, 'CPU per move')
    table(sc, lat_cols, 'latency and integrity')

print('\n### idle floors (no moves, connections open)\n')
print('| label | conns | box CPU us/s | box CPU % | softirq us/s | server us/s | idle exits/s | NIC irqs/s |')
print('|---|---|---|---|---|---|---|---|')
for lab in sorted(l for l in M if l.startswith('idle')):
    m = M[lab]
    print('| %s | %s | %.0f | %.2f | %.0f | %.0f | %.0f | %.1f |' % (lab, runs[lab].get('conns'), m['box_busy_us_s'], m['box_pct'], m['si_us_s'],
                                                                   m['srv_us_s'], m['idlex_s'], sum(v for k, v in m.items() if re.match(r'irq\d+_s', k))))

print('\n### correctness and smoke runs\n')
print('| label | moves sent | completed | lost | dup | gaps | bad bytes | bad meta | p50 us | p99 us | server rexmit segs | xdp drops | lwIP tx drops | syscalls/move (in-app) | syscalls/move (tracepoint) |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for lab in sorted(l for l in M if l.startswith(('smoke', 'drop', 'coexist'))):
    m = M[lab]
    dl = m['raw']
    print('| %s | %d | %d | %d | %d | %d | %d | %d | %.1f | %.1f | %d | %d | %d | %s | %s |' % (
        lab, m.get('moves_sent', 0), m.get('completed', 0), m.get('lost', 0), m.get('dups', 0), m.get('gaps', 0), m.get('badbytes', 0),
        m.get('badmeta', 0), m.get('p50_us', 0), m.get('p99_us', 0), dl.get('rexmit', 0), dl.get('xdp_drop', 0), dl.get('drop_tx', 0),
        '%.2f' % m['sc'] if m.get('sc') is not None else '-', '%.2f' % m['tp_sys'] if m.get('tp_sys') is not None else '-'))
