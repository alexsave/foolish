#!/usr/bin/env python3
# Paired analysis of the info-hiding-tax eval. Joins per-seed seat-0 finish
# positions from the OG_HIDE_MASK=1 (hide) and OG_HIDE_MASK=0 (control) runs and
# reports the paired mean-finish difference (with SE) and win-rate delta.
#   analyze.py <hide files...> --ctrl <control files...>
import sys, math

def load(files):
    d = {}
    for f in files:
        for line in open(f):
            if line.startswith('S '):
                _, s, fp = line.split()
                d[int(s)] = int(fp)
    return d

args = sys.argv[1:]
i = args.index('--ctrl')
hide = load(args[:i]); ctrl = load(args[i+1:])
seeds = sorted(set(hide) & set(ctrl))
n = len(seeds)
if not n:
    print('no paired seeds'); sys.exit(1)

hw = sum(hide[s] == 1 for s in seeds)
cw = sum(ctrl[s] == 1 for s in seeds)
hm = sum(hide[s] for s in seeds) / n
cm = sum(ctrl[s] for s in seeds) / n
# paired finish diff (hide - ctrl); negative = hide finishes better
diffs = [hide[s] - ctrl[s] for s in seeds]
md = sum(diffs) / n
var = sum((d - md) ** 2 for d in diffs) / (n - 1) if n > 1 else 0
se = math.sqrt(var / n)
# win-rate delta with paired-bootstrap-free normal approx on the win indicators
wd = [(hide[s] == 1) - (ctrl[s] == 1) for s in seeds]
mwd = sum(wd) / n
wvar = sum((w - mwd) ** 2 for w in wd) / (n - 1) if n > 1 else 0
wse = math.sqrt(wvar / n)

print(f'paired seeds: {n}')
print(f'win rate  hide={100*hw/n:.2f}%   ctrl={100*cw/n:.2f}%   delta={100*mwd:+.2f}%  (SE {100*wse:.2f}%, z={mwd/wse if wse else 0:+.2f})')
print(f'mean finish hide={hm:.4f}  ctrl={cm:.4f}  diff={md:+.4f}  (SE {se:.4f}, z={md/se if se else 0:+.2f})   [lower finish = better]')
print(f'seeds where hide finished better: {sum(d<0 for d in diffs)}   worse: {sum(d>0 for d in diffs)}   equal: {sum(d==0 for d in diffs)}')
