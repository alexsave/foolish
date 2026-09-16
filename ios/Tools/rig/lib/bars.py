#!/usr/bin/env python3
"""Every ruler bar, scored two ways.

    bars.py "<glob of edge.csv>" [--span 0.75]

JERK - each frame-to-frame step, squared, summed. "Did it get there in one
motion." Dominated by the big steps, which is the point: one 30pt lurch costs
as much as nine 10pt ones.

STRAY - the owner's second measure, for the bars that are SUPPOSED to travel.
Distance from the straight line between where the bar starts and where it ends,
squared, summed. Jerk cannot tell a bar that slid cleanly from one that went the
long way round smoothly; this can. A bar that leaves and comes back - the table
cards going off the top of the screen and returning - scores enormously here
while its jerk stays modest, and that excursion is exactly what the eye reads as
a jump.
"""
import argparse, csv, glob
import numpy as np

BARS = [("bot_pt", "green   hand + buttons"), ("table_pt", "magenta table cards"),
        ("opp_pt", "yellow  opponent view"), ("top_pt", "red     box top")]

ap = argparse.ArgumentParser()
ap.add_argument("pattern"); ap.add_argument("--span", type=float, default=0.75)
a = ap.parse_args()

paths = sorted(glob.glob(a.pattern))
print("%-22s %6s %10s %10s %9s" % ("bar", "takes", "jerk", "stray", "travel"))
for col, label in BARS:
    jerks, strays, travels = [], [], []
    for p in paths:
        rows = list(csv.DictReader(open(p)))
        # ANCHOR ON THE FLIP, not on the first frame of the film. `offset` is
        # measured from the first frame with a ruler in it, and a take opens with
        # about 0.65s of still board - scored from there, the window is mostly
        # lead and the bars look as though they barely travelled.
        tops = [(float(r["offset"]), r["top_pt"]) for r in rows if r["top_pt"]]
        if len(tops) < 8:
            continue
        first = tops[0][1]
        flip = next((o for o, v in tops if v != first), None)
        if flip is None:
            continue
        y = np.array([float(r[col]) for r in rows if r.get(col)])
        t = np.array([float(r["offset"]) for r in rows if r.get(col)]) - flip
        if len(y) < 10:
            continue
        m = (t >= 0) & (t <= a.span)
        if m.sum() < 5:
            continue
        w, tw = y[m], t[m]
        jerks.append(float((np.diff(w) ** 2).sum()))
        # The straight line from the first sample to the last, in TIME - not in
        # frame index, so a dropped frame does not tilt the reference.
        line = np.interp(tw, [tw[0], tw[-1]], [w[0], w[-1]])
        strays.append(float(((w - line) ** 2).sum()))
        travels.append(abs(float(w[-1] - w[0])))
    if jerks:
        print("%-22s %6d %10.0f %10.0f %9.1f"
              % (label, len(jerks), np.mean(jerks), np.mean(strays), np.mean(travels)))
    else:
        print("%-22s %6s %10s" % (label, 0, "NOT FOUND"))
