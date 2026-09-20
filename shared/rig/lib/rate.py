#!/usr/bin/env python3
"""How many of the composited frames did the APP actually draw?

The ruler carries a 14-bit clock redrawn by `TimelineView(.animation)`, so a
frame whose clock repeats is one the app did not render - the render server
composited our last picture under a drawer that had moved on. That gap is the
whole of the collapse's judder: the box is top-glued to the drawer's descending
edge, so a stale height puts the bottom edge one composite interval of drawer
travel away, and no animation curve can fix a frame that was never drawn.

So this is the oracle for anything meant to make the app render FASTER - the
`CADisableMinimumFrameDurationOnPhone` key above all. It reports the composite
rate (what the recorder caught) and the app's own redraw rate (how often the
clock changed) over the moving part of a take.

    rate.py <edge.csv> [<edge.csv> ...]
"""
import csv, glob, statistics, sys

def one(p):
    rows = [r for r in csv.DictReader(open(p)) if r["bot_pt"]]
    if len(rows) < 8:
        return None
    hs = [float(r["h_pt"]) for r in rows]
    mv = [i for i in range(1, len(hs)) if hs[i] != hs[i - 1]]
    if not mv:
        return None
    # The MOVING window only: a still board redraws when it feels like it, and
    # counting its frames would flatter or damn the result at random.
    w = rows[mv[0] - 1:mv[-1] + 1]
    t = [float(r["t"]) for r in w]
    span = t[-1] - t[0]
    if span <= 0:
        return None
    cl = [r["clock_ms"] for r in w]
    drew = sum(1 for i in range(1, len(cl)) if cl[i] != cl[i - 1])
    return (len(w) - 1) / span, drew / span, len(w)

if __name__ == "__main__":
    paths = []
    for a in sys.argv[1:]:
        paths += sorted(glob.glob(a)) if any(c in a for c in "*?[") else [a]
    got = [x for x in (one(p) for p in paths) if x]
    if not got:
        print("no usable takes", file=sys.stderr); sys.exit(1)
    comp = statistics.median(g[0] for g in got)
    app = statistics.median(g[1] for g in got)
    print("takes      %d" % len(got))
    print("composite  %5.1f Hz   (frames the recorder caught)" % comp)
    print("app drew   %5.1f Hz   (%.0f%% of them) - the rest are the render "
          "server re-compositing a stale picture" % (app, 100 * app / comp))
    print("stale gap  %5.1f ms   <- the drawer travel the box's bottom edge "
          "is behind by, on the frames we skip" % (1000 / app - 1000 / comp))
