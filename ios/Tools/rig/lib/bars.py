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

FLOOR - what jerk a bar with this travel would score if it rode the drawer
PERFECTLY: the fitted host spring (CollapseTween.hostResponse, 0.338s,
critically damped) scaled to the bar's own travel and sampled at the take's own
frame times. Jerk is a sum of squared steps, so a bar that genuinely travels
400pt in 0.3s scores thousands however smoothly it goes, and "how far above the
floor" is the number that says whether anything is wrong. The floor also stands
in for the drawer where the red bar is not a clean reference (it was clamped or
stale in every film before the collapse layers).

ROUGH - the sum of squared SECOND differences: the step-to-step change in the
step. A bar that rides the drawer has a step that changes gently frame to
frame; a bar that is a render late alternates a double step with no step at
all - the staircase in the per-frame dump - and that alternation is what this
scores, irrespective of how far the bar travels. The measure closest to what
the eye calls judder.

A clamped sample is a position, not a step: a bar scored at the edge it left by
(tween.py writes red at 0 and green at the screen's height) is skipped for every
step it ends, as mse.py already does for the bottom edge.
"""
import argparse, csv, glob, math
import numpy as np

BARS = [("bot_pt", "green   hand + buttons"), ("table_pt", "squares table cards"),
        ("opp_pt", "magenta opponent view"), ("top_pt", "red     drawer top")]
HOST_RESPONSE = 0.338     # CollapseTween.hostResponse


def host_progress(t, response=HOST_RESPONSE):
    w = 2 * math.pi / response
    return np.where(t > 0, 1 - (1 + w * t) * np.exp(-w * t), 0.0)


ap = argparse.ArgumentParser()
ap.add_argument("pattern"); ap.add_argument("--span", type=float, default=0.75)
a = ap.parse_args()

paths = sorted(glob.glob(a.pattern))
print("%-22s %6s %10s %8s %10s %10s %9s"
      % ("bar", "takes", "jerk", "floor", "rough", "stray", "travel"))
for col, label in BARS:
    jerks, floors, roughs, strays, travels = [], [], [], [], []
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
        have = [r for r in rows if r.get(col)]
        y = np.array([float(r[col]) for r in have])
        t = np.array([float(r["offset"]) for r in have]) - flip
        if col == "top_pt":
            clamped = y <= 0.0
        elif col == "bot_pt":
            clamped = np.array([r.get("offscreen", "").strip() == "1" for r in have])
        else:
            clamped = np.zeros(len(y), dtype=bool)
        if len(y) < 10:
            continue
        m = (t >= 0) & (t <= a.span)
        if m.sum() < 5:
            continue
        w, tw, cl = y[m], t[m], clamped[m]
        # A DROPPED SAMPLE IS A GAP, NOT A STEP. The marker bars are occluded
        # from time to time - at four seats and at eight the ring puts a badge
        # on the same row as the battle cards and the yellow bar paints over the
        # magenta one - and those frames leave the series entirely. Diffing
        # across the hole then reads as one enormous move that nothing on screen
        # made: at eight players it inflated the table's roughness to 6171 while
        # the samples either side of every gap ran 563, 695, 725, 747, 751, 753,
        # 754, monotonic, no reversals. So a step whose two ends are further
        # apart in TIME than a frame and a half is not a step.
        if len(tw) > 3:
            dt = np.diff(tw)
            cl = cl | np.concatenate([[False], dt > 1.5 * float(np.median(dt))])
        ok1 = ~(cl[1:] | cl[:-1])                       # a step with both ends real
        d1 = np.diff(w)
        jerks.append(float((d1[ok1] ** 2).sum()))
        ok2 = ok1[1:] & ok1[:-1]
        d2 = np.diff(d1)
        roughs.append(float((d2[ok2] ** 2).sum()))
        # The straight line from the first sample to the last, in TIME - not in
        # frame index, so a dropped frame does not tilt the reference.
        line = np.interp(tw, [tw[0], tw[-1]], [w[0], w[-1]])
        strays.append(float((((w - line) ** 2)[~cl]).sum()))
        real = w[~cl]
        travel = abs(float(real[-1] - real[0])) if len(real) else 0.0
        travels.append(travel)
        ideal = travel * host_progress(tw)
        floors.append(float((np.diff(ideal)[ok1] ** 2).sum()))
    if jerks:
        print("%-22s %6d %10.0f %8.0f %10.0f %10.0f %9.1f"
              % (label, len(jerks), np.mean(jerks), np.mean(floors), np.mean(roughs),
                 np.mean(strays), np.mean(travels)))
    else:
        print("%-22s %6s %10s" % (label, 0, "NOT FOUND"))
