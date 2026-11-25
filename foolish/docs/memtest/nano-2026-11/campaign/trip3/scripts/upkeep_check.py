#!/usr/bin/env python3
# upkeep_check.py: base check for upkeep.py: which perf leaf symbols get a different outermost function from llvm-symbolizer
# (base 0), weighted by cycles in the perfM3 play window on the server process.
import sys, collections, subprocess
sys.path.insert(0, '/Users/alex/Dev/foolish-bench-results/campaign/trip1/scripts'); import analyze as A
A.RAW = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/out/raw'
BIN = '/Users/alex/Dev/foolish-bench-results/campaign/trip3/bin/unstripped/cbase_M3'
w = A.load_window('perfM3'); s0, s1, pid = A.srv_mono_window('perfM3', w)
c = collections.Counter()
for comm, p, tid, cpu, t, per, fr in A.samples(f'{A.RAW}/proc/perfM3/cc_perfM3.txt.gz'):
    if fr and 'cbase_M3' in fr[0][2] and s0 <= t < s1 and p == pid: c[(int(fr[0][0], 16), fr[0][1])] += per
ips = sorted(c); o = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-symbolizer', '--obj=' + BIN, '--relative-address'], input='\n'.join(hex(a) for a, _ in ips), capture_output=True, text=True).stdout.strip().split('\n\n')
mm = collections.Counter()
for (a, s), blk in zip(ips, o):
    f = blk.split('\n')[0]
    if f != s and not s.startswith(f + '.'): mm[(s, f)] += c[(a, s)]
tot = sum(c.values())
for k, v in mm.most_common(10): print(f'{100*v/tot:.2f}% perf={k[0]} symbolizer={k[1]}')
