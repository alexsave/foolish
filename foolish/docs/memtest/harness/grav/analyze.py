#!/usr/bin/env python3
# analyze.py: the Graviton run's numbers from results/ (srv/ from the nano, load/ from the load box, both via S3).
# Per run: moves/s, move p50/p99 (the load's M records), RSS p50/p99/max in the play window (t_go..t_done, the load box's
# clock; both boxes run chrony on the Amazon Time Sync Service), VmHWM, and the mid capture's RssAnon and its offset from t_go.
import glob, os, re, sys
R = '/Users/alex/Dev/cdom-memtest/grav/results'
def rd(p):
    try: return open(p).read()
    except OSError: return ''
def kb(txt, key):
    m = re.search(rf'^{key}:\s+(\d+)', txt, re.M); return int(m.group(1)) if m else None
def pc(v, q): return v[min(len(v) - 1, int(q * (len(v) - 1) + .5))] if v else float('nan')
def load_stats(l):
    d = f'{R}/load/{l}'; m = []; c = []
    for f in glob.glob(d + '/rdv*/ev.*'):
        for line in open(f):
            p = line.split()
            if p and p[0] == 'M': m.append((int(p[3]), int(p[4])))
            if p and p[0] == 'C': c.append((int(p[2]), (int(p[4]) - int(p[3])) / 1000))
    if not m: return None
    w = (max(e for s, e in m) - min(s for s, e in m)) / 1e6
    lat = sorted((e - s) / 1000 for s, e in m)
    t0 = min(s for s, e in m)
    return dict(n=len(m), mps=len(m) / w, p50=pc(lat, .5), p99=pc(lat, .99), go=float(rd(d + '/t_go') or 0), done=float(rd(d + '/t_done') or 0),
                ends=sorted((e - t0) / 1e6 for s, e in m), c=c)
def rss_stats(l, go, done):
    rows = []
    for line in rd(f'{R}/srv/rss_{l}.log').split('\n'):
        p = line.split()
        if len(p) == 4 and all(p): rows.append((float(p[0]), int(p[1]), int(p[2]), int(p[3])))
    w = sorted(r for t, r, h, a in rows if go <= t <= done)
    wa = sorted(a for t, r, h, a in rows if go <= t <= done)
    if not w: return None
    return dict(p50=pc(w, .5) / 1024, p99=pc(w, .99) / 1024, max=w[-1] / 1024, anon_p50=pc(wa, .5), idle=rows[min(4, len(rows) - 1)][1] / 1024)
def run(l):
    s = load_stats(l)
    if not s: return None
    r = rss_stats(l, s['go'], s['done']) or {}
    st = rd(f'{R}/srv/status_{l}_mid.txt'); end = rd(f'{R}/srv/status_{l}_end.txt')
    tc = rd(f'{R}/srv/tcap_{l}_mid.txt')
    return dict(s, **{('rss_' + k): v for k, v in r.items()}, anon_mid=kb(st, 'RssAnon'), rss_mid=kb(st, 'VmRSS'),
                hwm=kb(end, 'VmHWM'), cap_off=(float(tc) - s['go']) if tc else None)
def tls(l):
    out = {}
    for line in rd(f'{R}/srv/tls_{l}.txt').split('\n'):
        p = line.split()
        if len(p) == 7 and not line.startswith('#'): out.setdefault(p[1], []).append(int(p[6]))
    return out
if __name__ == '__main__':
    which = sys.argv[1] if len(sys.argv) > 1 else 'all'
    print('label build moves/s p50 p99 | rss_p50 rss_p99 rss_max HWM(MiB) | anon_mid(kB) cap_off(s) | idle_at_start')
    for l in sorted(os.path.basename(d) for d in glob.glob(f'{R}/load/*') if os.path.isdir(d)):
        x = run(l)
        if not x: continue
        print(f"{l} {x['mps']:.0f} {x['p50']:.3f} {x['p99']:.3f} | {x.get('rss_p50', 0):.1f} {x.get('rss_p99', 0):.1f} {x.get('rss_max', 0):.1f} {(x['hwm'] or 0) / 1024:.1f} | {x['anon_mid']} {x['cap_off'] if x['cap_off'] is None else round(x['cap_off'], 1)} | {x.get('rss_idle', 0):.1f}")
        t = tls(l + '_mid')
        if t: print('   tls kB:', ' '.join(f'{k}={v}' for k, v in sorted(t.items())))
        if x['c']:
            for W in sorted(set(w for w, _ in x['c'])):
                v = sorted(d for w, d in x['c'] if w == W)
                print(f'   rematch wait {W} ms: n={len(v)} p50={pc(v, .5):.3f} p90={pc(v, .9):.3f} mean={sum(v) / len(v):.3f} max={v[-1]:.3f} ms')
        if l.startswith('g7'):
            e = x['ends']; T = e[-1]; b = 10
            print('   g7 moves/s per 10 s:', ' '.join(f'{sum(1 for t in e if k <= t < k + b) / b:.0f}' for k in range(0, int(T), b)))
