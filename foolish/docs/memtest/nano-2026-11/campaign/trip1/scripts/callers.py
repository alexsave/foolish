#!/usr/bin/env python3
# callers.py LABEL THREAD(loop|TID) FUNC...: rootward call paths (up to 6 user frames above the first FUNC on the chain), us/move
import sys, collections
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts')
import analyze as A
lbl, th, funcs = sys.argv[1], sys.argv[2], set(sys.argv[3:])
w = A.load_window(lbl); s0, s1, pid = A.srv_mono_window(lbl, w); mv = w['moves']
c = collections.Counter()
for comm, p, tid, cpu, t, per, fr in A.samples(f'{A.RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
    if not (s0 <= t < s1) or p != pid or not ((th == 'loop' and tid == pid) or str(tid) == th): continue
    names = [f[1] for f in fr]
    i = next((i for i, n in enumerate(names) if n in funcs), None)
    if i is None: continue
    c[' <- '.join(names[i:i + 7])] += per
for k, v in c.most_common(12): print(f'{v/2.5e9*1e6/mv:.3f}  {k}')
