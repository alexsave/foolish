#!/usr/bin/env python3
"""Does every mark RIDE the drawer? Scores marks.py CSVs, one per take.

    ride.py <marks.csv> [...] [--ref red] [--span 0.9] [--snap 4]

Anchors each take on the reference bar's first move (the drawer starting), then
per mark, over the span, on the mark's offset from the reference (y - ref):

  snaps   frame-to-frame changes of that offset larger than --snap points: the
          element jumped relative to the drawer it lives in (a re-layout, a
          teleport, a frame behind the others)
  maxsnap the largest of them, in points
  late    snaps that land AFTER the reference has stopped (a re-layout once the
          drawer is still - the worst kind, because nothing hides it)
  rough   sum of squared second differences of y itself (the staircase judder)
  miss    frames inside the mark's own first..last sighting where it was not
          found (occluded, blurred, off-frame), counted as defects, not data

A mark that rides perfectly scores 0 snaps whatever the drawer does. Averages
are over takes; the per-take numbers print with --each.
"""
import argparse, csv, math
from collections import defaultdict


def fnum(s):
    return float(s) if s not in ("", None) else None


def score(path, ref, span, snap):
    rows = list(csv.DictReader(open(path)))
    t = [float(r["t"]) for r in rows]
    R = [fnum(r[ref]) for r in rows]
    first = next((R[i] for i in range(len(R)) if R[i] is not None), None)
    i0 = next((i for i in range(len(R)) if R[i] is not None and abs(R[i] - first) > 1), None)
    if i0 is None:
        return None
    t0 = t[i0]
    idx = [i for i in range(max(0, i0 - 3), len(rows)) if t[i] <= t0 + span]
    # the reference has stopped from the first frame after which it never moves >0.5pt
    last_move = max((i for i in idx[1:] if R[i] is not None and R[i - 1] is not None
                     and abs(R[i] - R[i - 1]) > 0.5), default=idx[0])
    marks = sorted(c[:-2] for c in rows[0] if c.endswith("_y"))
    out = {}
    for m in marks:
        ys = [fnum(rows[i][m + "_y"]) for i in idx]
        seen = [k for k, y in enumerate(ys) if y is not None]
        if len(seen) < 3:
            continue
        miss = sum(1 for k in range(seen[0], seen[-1] + 1) if ys[k] is None)
        snaps, late, maxsnap = 0, 0, 0.0
        for k in range(1, len(idx)):
            a, b = idx[k - 1], idx[k]
            if None in (ys[k - 1], ys[k], R[a], R[b]):
                continue
            d = (ys[k] - R[b]) - (ys[k - 1] - R[a])
            if abs(d) > snap:
                snaps += 1
                maxsnap = max(maxsnap, abs(d))
                if b > last_move:
                    late += 1
        rough = 0.0
        for k in range(2, len(idx)):
            if None in (ys[k - 2], ys[k - 1], ys[k]):
                continue
            rough += (ys[k] - 2 * ys[k - 1] + ys[k - 2]) ** 2
        out[m] = dict(snaps=snaps, maxsnap=maxsnap, late=late, rough=rough, miss=miss)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--ref", default="red")
    ap.add_argument("--span", type=float, default=0.9)
    ap.add_argument("--snap", type=float, default=4.0)
    ap.add_argument("--each", action="store_true")
    a = ap.parse_args()
    agg = defaultdict(lambda: defaultdict(list))
    n = 0
    for p in a.csv:
        s = score(p, a.ref, a.span, a.snap)
        if s is None:
            print(f"{p}: the reference never moved"); continue
        n += 1
        for m, d in s.items():
            for k, v in d.items():
                agg[m][k].append(v)
            if a.each:
                print(p, m, d)
    print(f"{n} takes, anchored on {a.ref}'s first move, {a.span}s, snap > {a.snap}pt")
    print(f"{'mark':<12}{'snaps':>7}{'maxsnap':>9}{'late':>6}{'rough':>9}{'miss':>6}  (mean per take)")
    for m in sorted(agg):
        d = agg[m]
        mean = lambda k: sum(d[k]) / len(d[k])
        print(f"{m:<12}{mean('snaps'):7.1f}{max(d['maxsnap']):9.1f}{mean('late'):6.1f}"
              f"{mean('rough'):9.0f}{mean('miss'):6.1f}")


if __name__ == "__main__":
    main()
