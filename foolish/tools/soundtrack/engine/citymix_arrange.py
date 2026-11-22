#!/usr/bin/env python3
"""citymix_arrange.py spec.txt sources.tsv out.cue.txt : writes a splice cue sheet from an arrangement spec.

A scene loops bars of one recording; its passes are varied by stems; scenes meet by a beat-matched blend or a breakdown.
sources.tsv: id <tab> path <tab> stems_dir|- <tab> beat_s <tab> first_bar_line_s <tab> beats_per_bar   (one row per source)

Spec, one statement a line, '#' comments:
  master LUFS DBTP | tails RT:DB:LEN
  scene SRC A B [key=value ...]     A, B: bar numbers from the first bar line (integers, may be negative), or seconds
                                    written 12.5s (then the scene cannot loop). Keys:
      plan=f,o,dbo,...  one entry per pass of A..B: f = the mix; letters d b o v = drums, bass, other, vocals stems;
                        a pass may end in /lpin /hpin /lpout (a filter over that pass)
      hit=1     after the last pass the scene lands on the first beat of bar A, which rings out (a phrase's own last hit)
      lufs=  semi=  rate=  eq=lo:mid:hi  role=word  extra="KEY VAL ..." (added to every line of the scene)
  blend N        the next scene enters N bars before this one ends, beat-matched: its bass and kick are held under
                 150 Hz until the swap at the end of the overlap and it rises from -24 dB; the outgoing one thins (rides
                 down 6 dB, its top closes) and hands over its bass at the swap. The next scene's rate is set to match.
  break BEATS    a breakdown: the outgoing scene rings out (echo on the beat, hall, top closing over its last bar) and the
                 next one starts BEATS beats later (of the outgoing tempo), entering over one bar
  join           a plain join on the bar line, the outgoing tail ringing (TAILS)
  raw LINE       a cue line copied as it is
"""
import sys, shlex

def fail(msg):
    sys.exit('arrange: ' + msg)

src = {}
for line in open(sys.argv[2]):
    w = line.rstrip('\n').split('\t')
    if len(w) >= 6 and not w[0].startswith('#'):
        src[w[0]] = dict(path=w[1], stems=w[2], beat=float(w[3]), first=float(w[4]), bpb=int(w[5]))

items, head = [], []
for n, line in enumerate(open(sys.argv[1]), 1):
    line = line.split('#', 1)[0].strip() if not line.lstrip().startswith('raw ') else line.strip()
    if not line:
        continue
    w = shlex.split(line) if not line.startswith('raw ') else ['raw', line[4:]]
    k = w[0]
    if k == 'master':
        head.append('MASTER %s %s' % (w[1], w[2]))
    elif k == 'tails':
        head.append('TAILS ' + w[1])
    elif k == 'scene':
        if w[1] not in src:
            fail('line %d: no source %s' % (n, w[1]))
        opt = dict(x.split('=', 1) for x in w[4:])
        items.append(dict(kind='scene', src=w[1], a=w[2], b=w[3], opt=opt, line=n))
    elif k in ('blend', 'break'):
        items.append(dict(kind=k, n=float(w[1])))
    elif k == 'join':
        items.append(dict(kind='join'))
    elif k == 'raw':
        items.append(dict(kind='raw', text=w[1]))
    else:
        fail('line %d: cannot read %s' % (n, k))

def bar_len(s):
    return s['beat'] * s['bpb']

def t_of(s, v):
    if v.endswith('s'):
        return float(v[:-1])
    return s['first'] + int(v) * bar_len(s) - 0.005

STEM = {'d': 'drums', 'b': 'bass', 'o': 'other', 'v': 'vocals'}
out = ['# written by citymix_arrange.py from ' + sys.argv[1]] + head
used = set()
for it in items:
    if it['kind'] == 'scene':
        used.add(it['src'])
        for c in it['opt'].get('plan', 'f').replace('/lpin', '').replace('/hpin', '').replace('/lpout', '').split(','):
            if c != 'f':
                for ch in c:
                    used.add(it['src'] + ':' + STEM[ch])
for u in sorted(used):
    if ':' in u:
        sid, st = u.split(':')
        if src[sid]['stems'] == '-':
            fail('no stems for ' + sid)
        out.append('SRC %s_%s %s/%s.wav FROM %s' % (sid, st[0], src[sid]['stems'], st, src[sid]['path']))
    else:
        out.append('SRC %s %s' % (u, src[u]['path']))

eff_prev = None   # bar length in song seconds of the previous scene
pending = None    # the transition to apply at the start of the next scene
prev_last = None  # index in out of the previous scene's last pass lines, and its data
for i, it in enumerate(items):
    if it['kind'] == 'raw':
        out.append(it['text'])
        eff_prev = None
        prev_last = None
        continue
    if it['kind'] != 'scene':
        pending = it
        continue
    s = src[it['src']]
    o = it['opt']
    a, b = t_of(s, it['a']), t_of(s, it['b'])
    rate = float(o.get('rate', 1))
    if pending and pending['kind'] == 'blend' and 'rate' not in o:
        if eff_prev is None:
            fail('line %d: blend after a scene with no bar length' % it['line'])
        rate = bar_len(s) / eff_prev
        if abs(rate - 1) > 0.04:
            fail('line %d: blend needs a %.1f%% stretch' % (it['line'], (rate - 1) * 100))
        if abs(rate - 1) < 0.002:
            rate = 1.0
    eff = bar_len(s) / rate
    plan = o.get('plan', 'f').split(',')
    common = []
    if 'lufs' in o: common.append('LUFS ' + o['lufs'])
    if 'semi' in o: common.append('SEMI ' + o['semi'])
    if rate != 1: common.append('RATE %.5f' % rate)
    if 'eq' in o: common.append('EQ ' + o['eq'])
    if 'extra' in o: common.append(o['extra'])
    role = o.get('role', it['src'])
    for p, code in enumerate(plan):
        mods = code.split('/')
        code, mods = mods[0], mods[1:]
        keys = list(common)
        span = b - a
        for m in mods:
            keys.append({'lpin': 'LPIN %.3f:350' % (span / rate), 'hpin': 'HPIN %.3f:1500' % (span / rate),
                         'lpout': 'LPOUT %.3f:350' % (span / rate)}[m])
        first = p == 0
        if first and pending:
            if pending['kind'] == 'blend':
                nb = pending['n']; ov = nb * eff
                keys.append('GAP %.6f PRE 0 FIN %.6f' % (-ov, ov / 2))
                keys.append('RIDE %.4f:-24,%.4f:-10,%.4f:0' % (a, a + nb / 2 * bar_len(s), a + nb * bar_len(s) - 0.05))
                keys.append('BASS %.4f:-40,%.4f:-40,%.4f:0' % (a, a + nb * bar_len(s) - 0.06, a + nb * bar_len(s) - 0.005))
                jt = 'blend_%g_bars' % nb
                if prev_last is not None:
                    pa, pb, pbar = prev_last['a'], prev_last['b'], prev_last['bar']
                    for j in prev_last['idx']:
                        out[j] = out[j].replace(' ROLE ', ' RIDE %.4f:0,%.4f:-6 LPOUT %.6f:1500 BASS %.4f:0,%.4f:0,%.4f:-40 FOUT 0.03 ROLE ' % (
                            pb - nb * pbar, pb, nb / 2 * prev_last['effbar'], pb - 0.06, pb - 0.05, pb - 0.005), 1)
            elif pending['kind'] == 'break':
                gap = pending['n'] * (eff_prev / src_prev_bpb if eff_prev else 0.5)
                keys.append('GAP %.6f PRE 0 FIN %.6f' % (gap, eff))
                jt = 'breakdown_%g_beats' % pending['n']
                if prev_last is not None:
                    pb = prev_last['b']; beat = prev_last['effbar'] / prev_last['bpb']
                    for j in prev_last['idx']:
                        out[j] = out[j].replace(' ROLE ', ' ECHO %.6f:0.5:-8:%.6f WASH 3.2:-8:%.6f LPOUT %.6f:900 ROLE ' % (
                            beat, beat, prev_last['effbar'], prev_last['effbar']), 1)
            else:
                jt = 'join_on_the_bar'
        else:
            jt = ('start' if i == 0 else 'join_on_the_bar') if first else 'loop_pass_%d' % (p + 1)
            if not first:
                keys.append('PRE 0 FIN 0.004')
        idx = []
        tag = '%s_pass%d' % (role, p + 1)
        if code == 'f':
            out.append('CLIP %s %.3f %.3f %s ROLE %s JOIN %s' % (it['src'], a, b, ' '.join(keys), tag, jt))
            idx.append(len(out) - 1)
        else:
            for q, ch in enumerate(code):
                kw = 'CLIP' if q == 0 else 'LAYER'
                extra = keys if q == 0 else [k for k in keys if not k.startswith('GAP')] + ['OFF 0']
                out.append('%s %s_%s %.3f %.3f %s ROLE stem_%s_%s JOIN %s' % (kw, it['src'], STEM[ch][0], a, b, ' '.join(extra), STEM[ch], tag, jt))
                idx.append(len(out) - 1)
        pending = None
    if o.get('hit') == '1':
        # the phrase is closed by its own downbeat: the first beat of bar A again, its ring left to hall and echo
        beat = s['beat']
        out.append('CLIP %s %.3f %.3f %s PRE 0 FIN 0.004 FOUT 0.35 ROLE %s_final_hit JOIN the_phrase_lands_on_its_downbeat' % (
            it['src'], a, a + beat, ' '.join(common), role))
        idx = [len(out) - 1]
        a, b = a, a + beat
    prev_last = dict(idx=idx, a=a, b=b, bar=bar_len(s), effbar=eff, bpb=s['bpb'])
    src_prev_bpb = s['bpb']
    eff_prev = eff
open(sys.argv[3], 'w').write('\n'.join(out) + '\n')
