#!/usr/bin/env python3
# srclines.py LABEL TID|loop SYM...: recovers the static-PIE load base of cbase_D from the flat dump (intersection of
# [sym_start, sym_end) constraints over all resolved cbase_D samples), then maps the self samples of SYM on the thread to
# source lines (with inlined frames) through llvm-symbolizer on the unstripped twin. Prints us/move per line.
import sys, re, gzip, subprocess, collections
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts'); import analyze as A
lbl, th, syms = sys.argv[1], sys.argv[2], set(sys.argv[3:])
BIN = '/Users/alex/Dev/foolish-bench-results/campaign/trip1/pkg/cbase_D.unstripped'
tab = {}; dup = set()
for l in subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-objdump', '-t', BIN], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 6 and p[2] == 'F' or (len(p) >= 6 and 'F' in p[1:3]):
        try:
            if p[-1] in tab: dup.add(p[-1])
            tab[p[-1]] = (int(p[0], 16), int(p[-2], 16))
        except ValueError: pass
for d in dup: del tab[d]
w = A.load_window(lbl); s0, s1, pid = A.srv_mono_window(lbl, w); mv = w['moves']
lo, hi = -1 << 62, 1 << 62; pick = collections.Counter()
for comm, p, tid, cpu, t, per, fr in A.samples(f'{A.RAW}/proc/{lbl}/flat_{lbl}.txt.gz'):
    if not fr or 'cbase_D' not in fr[0][2]: continue
    ip = int(fr[0][0], 16); s = fr[0][1]
    if s in tab and tab[s][1] > 0:
        st, sz = tab[s]; lo = max(lo, ip - st - sz + 1); hi = min(hi, ip - st)
    if s0 <= t < s1 and p == pid and ((th == 'loop' and tid == pid) or str(tid) == th) and s in syms: pick[ip] += per
print(f'base interval [{lo:#x}, {hi:#x}]'); base = hi & ~0xfff if (hi & ~0xfff) >= lo else hi
addrs = sorted(pick); inp = '\n'.join(hex(a - base) for a in addrs)
out = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-symbolizer', '--obj=' + BIN, '--inlining', '--relative-address'], input=inp, capture_output=True, text=True).stdout.strip().split('\n\n')
lines = collections.Counter()
for a, blk in zip(addrs, out):
    fl = blk.strip().split('\n'); fr2 = [(fl[i], fl[i + 1].rsplit(':', 1)[0].split('/')[-1]) for i in range(0, len(fl) - 1, 2)]
    lines[' <- '.join(f'{f}@{loc}' for f, loc in fr2[:3])] += pick[a]
tot = sum(pick.values()); print(f'{sorted(syms)} self on {th}: {tot/2.5e9*1e6/mv:.3f} us/move, base {base:#x}')
for k, v in lines.most_common(15): print(f'{v/2.5e9*1e6/mv:.3f} {100*v/tot:5.1f}%  {k}')
