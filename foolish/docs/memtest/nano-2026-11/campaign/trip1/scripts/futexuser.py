#!/usr/bin/env python3
# futexuser.py LABEL: loop-thread samples inside futex_wait or futex_wake, grouped by their first five user frames (us/move)
import sys, collections
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts'); import analyze as A
lbl = sys.argv[1]; w = A.load_window(lbl); s0, s1, pid = A.srv_mono_window(lbl, w); mv = w['moves']
c = collections.Counter(); tot = 0
for comm, p, tid, cpu, t, per, fr in A.samples(f'{A.RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
    if not (s0 <= t < s1) or p != pid or tid != pid: continue
    names = [f[1] for f in fr]
    if 'futex_wait' not in names and 'futex_wake' not in names: continue
    kind = 'wait' if 'futex_wait' in names else 'wake'
    c[kind + ': ' + ' <- '.join(f[1] for f in fr if not A.iskern(f))[:200]] += per; tot += per
print(f'{lbl} loop futex on-CPU total {tot/2.5e9*1e6/mv:.3f} us/move')
for k, v in c.most_common(12): print(f'{v/2.5e9*1e6/mv:.3f}  {k}')
