#!/usr/bin/env python3
# upkeep.py LABEL: trip 3, the list upkeep of PR 496 in the perf cycles run LABEL (perfM3, cbase_M3 at 59c115054).
# Method (a copy of trip1/scripts/srclines.py's): the static-PIE load base of cbase_M3 is recovered from the callchain dump
# (intersection of [sym_start, sym_end) constraints over all resolved cbase_M3 leaf samples); then every cbase_M3 leaf IP in
# the play window is symbolized with its inlined frames through llvm-symbolizer on the unstripped twin, and a sample counts
# as list upkeep when its leaf symbol, or any inlined frame at its IP, is one of the list functions of journal.c
# (lru_out, lru_in, ov_touch, ov_link, wait_begin, wait_end, rating_lru_out, rating_lru_in, rating_touch, rating_link).
# Self cycles only: these functions call nothing but each other, so self is their whole cost (a cache miss stalls the
# load instruction inside them, which is where the sample lands).
# Prints us per move (cycles / 2.5 GHz / moves in the play window) per thread, by function and by source line.
import sys, collections, subprocess
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts'); import analyze as A
A.RAW = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/out/raw'
lbl = sys.argv[1]
BIN = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/bin/unstripped/cbase_M3'
LLVM = '/opt/homebrew/opt/llvm/bin/'
LISTF = {'lru_out', 'lru_in', 'ov_touch', 'ov_link', 'wait_begin', 'wait_end', 'rating_lru_out', 'rating_lru_in', 'rating_touch', 'rating_link'}
tab = {}; dup = set()
for l in subprocess.run([LLVM + 'llvm-objdump', '-t', BIN], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 6 and 'F' in p[1:3]:
        try:
            if p[-1] in tab: dup.add(p[-1])
            tab[p[-1]] = (int(p[0], 16), int(p[-2], 16))
        except ValueError: pass
for d in dup: del tab[d]
w = A.load_window(lbl); s0, s1, pid = A.srv_mono_window(lbl, w); mv = w['moves']
us = lambda c: c / 2.5e9 * 1e6 / mv
lo, hi = -1 << 62, 1 << 62
per_ip = collections.Counter(); th_tot = collections.Counter(); th_name = {}
for comm, p, tid, cpu, t, per, fr in A.samples(f'{A.RAW}/proc/{lbl}/cc_{lbl}.txt.gz'):
    if not fr or 'cbase_M3' not in fr[0][2]:
        if s0 <= t < s1 and p == pid: th_tot['loop' if tid == pid else f'{comm}:{tid}'] += per
        continue
    ip = int(fr[0][0], 16); s = fr[0][1]
    if s in tab and tab[s][1] > 0:
        st, sz = tab[s]; lo = max(lo, ip - st - sz + 1); hi = min(hi, ip - st)
    if s0 <= t < s1 and p == pid:
        th = 'loop' if tid == pid else f'{comm}:{tid}'
        th_tot[th] += per; per_ip[(th, ip, s)] += per
base = hi & ~0xfff if (hi & ~0xfff) >= lo else hi
print(f'# upkeep {lbl}: play window server mono [{s0:.3f},{s1:.3f}], moves {mv}, pid {pid}; base interval [{lo:#x}, {hi:#x}] -> base {base:#x}')
ips = sorted({ip for (_, ip, _) in per_ip})
out = subprocess.run([LLVM + 'llvm-symbolizer', '--obj=' + BIN, '--inlining', '--relative-address'],
                     input='\n'.join(hex(a - base) for a in ips), capture_output=True, text=True).stdout.strip().split('\n\n')
sym = {}
for a, blk in zip(ips, out):
    fl = blk.strip().split('\n')
    sym[a] = [(fl[i], fl[i + 1].split('/')[-1]) for i in range(0, len(fl) - 1, 2)]   # [(func, file:line:col)] innermost first
byfn = collections.defaultdict(collections.Counter); byline = collections.defaultdict(collections.Counter); up = collections.Counter()
mismatch = 0
for (th, ip, s), per in per_ip.items():
    fr = sym.get(ip, [])
    outer = fr[-1][0] if fr else '?'
    if s in tab and outer != s and not outer.startswith(s.split('.')[0]): mismatch += per
    hit = [f for f, _ in fr if f in LISTF]
    if s.split('.')[0] in LISTF or hit:
        up[th] += per
        fn = hit[0] if hit else s
        byfn[th][f'{fn} (in {s})'] += per
        loc = next((l for f, l in fr if f in LISTF), fr[0][1] if fr else '?')
        byline[th][f'{loc} {fn} <- {s}'] += per
allc = sum(per_ip.values()); print(f'# symbolizer outer frame differs from perf symbol on {100*mismatch/max(allc,1):.2f}% of cbase_M3 cycles (base check)')
tot_up = sum(up.values())
print(f'list upkeep, all threads: {us(tot_up):.4f} us/move ({sum(1 for _ in [0])} run, {tot_up} cycles)')
for th in sorted(th_tot, key=lambda t: -th_tot[t]):
    if us(th_tot[th]) < 0.05: continue
    print(f'{th:24s} thread {us(th_tot[th]):7.3f} us/move  upkeep {us(up[th]):.4f} us/move  share {100*up[th]/th_tot[th]:.3f}%')
for th in sorted(byfn, key=lambda t: -up[t]):
    print(f'-- {th} by function'); [print(f'   {us(v):.4f}  {k}') for k, v in byfn[th].most_common(12)]
    print(f'-- {th} by source line'); [print(f'   {us(v):.4f}  {k}') for k, v in byline[th].most_common(15)]
# resolution: one sample at 999 Hz is about 1/999 s of one CPU = 2.5e9/999 cycles
print(f'resolution: one sample = {2.5e9/999:.0f} cycles = {us(2.5e9/999):.5f} us/move over {mv} moves')
