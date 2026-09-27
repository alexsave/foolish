import sys
sys.path.insert(0, 'c/tools/winprob')
from plot_winprob import read_strip, series

meta, pos, wp = read_strip('scratch/winprob.tsv')
names = dict(meta['name'])
n = len(pos)
for seat in range(8):
    xs, ys = series(wp['truth'], n, seat)
    lo = min(ys)
    loi = xs[ys.index(lo)]
    last = [x for x, y in zip(xs, ys) if y < 99.5]
    safe = (last[-1] + 1) if last else 0
    print(f"seat {seat} {names[seat]:20s} min {lo:5.1f}% at move {loi:3d}   never below 99.5% after move {safe:3d}")

print()
for seat, label in ((1, 'Miami'), (5, 'Vienna')):
    xs, ys = series(wp['truth'], n, seat)
    print(label, [(x, round(y)) for x, y in zip(xs, ys) if x >= 104][::3])

xs, ys = series(wp['truth'], n, 0)
print("\nAlex truth, first 14:", [(x, round(y)) for x, y in zip(xs, ys)][:14])
tb = dict(zip(*series(wp['truth'], n, 0)))
xb, yb = series(wp['belief'], n, 0)
gap = [(x, round(yb[i] - tb[x])) for i, x in enumerate(xb) if x in tb]
print("Alex belief minus truth: worst", min(gap, key=lambda t: t[1]),
      " best", max(gap, key=lambda t: t[1]))
