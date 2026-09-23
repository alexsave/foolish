#!/usr/bin/env python3
"""Plot every mark of a marks.py CSV over time: y on top, offset from the drawer below.

    marksplot.py <marks.csv> <out.png> [--title T] [--ref red] [--span 1.2]

Top panel: each square's y (points, screen down) and the edge bars, so a teleport
is a vertical cliff and a lag is a curve that starts late. Bottom panel: each
square's y minus the reference bar's, so an element that RIDES the drawer is a
flat line and every step in it is a re-layout. Anchored on the reference's
first move, like ride.py.
"""
import argparse, csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK = {"magenta": "#e000e0", "cyan": "#00b8b8", "yellow": "#c8b400", "orange": "#ff8000",
       "blue": "#0000ff", "violet": "#8000ff", "lime": "#60c000", "pink": "#ff0080",
       "red": "#ff0000", "green": "#00a000"}


def f(s):
    return float(s) if s not in ("", None) else None


ap = argparse.ArgumentParser()
ap.add_argument("csv"); ap.add_argument("out")
ap.add_argument("--title", default="")
ap.add_argument("--ref", default="red")
ap.add_argument("--span", type=float, default=1.2)
a = ap.parse_args()
rows = list(csv.DictReader(open(a.csv)))
t = [float(r["t"]) for r in rows]
R = [f(r[a.ref]) for r in rows]
first = next(x for x in R if x is not None)
i0 = next((i for i, x in enumerate(R) if x is not None and abs(x - first) > 1), 0)
t0 = t[i0]
keep = [i for i in range(len(rows)) if t0 - 0.15 <= t[i] <= t0 + a.span]
marks = sorted(c[:-2] for c in rows[0] if c.endswith("_y"))
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
for m in marks + ["red", "green"]:
    col = m + "_y" if m in marks else m
    xs = [(t[i] - t0) * 1000 for i in keep]
    ys = [f(rows[i][col]) for i in keep]
    c = INK.get(m.split("_")[0], "k")
    ls = "--" if m in ("red", "green") else "-"
    ax1.plot(xs, ys, ls, marker=".", ms=3, lw=1, color=c, label=m)
    if m in marks:
        off = [(y - R[i]) if (y is not None and R[i] is not None) else None for y, i in zip(ys, keep)]
        ax2.plot(xs, off, "-", marker=".", ms=3, lw=1, color=c, label=m)
ax1.invert_yaxis(); ax2.invert_yaxis()
ax1.set_ylabel("y (pt, screen)"); ax2.set_ylabel(f"y - {a.ref} (pt)")
ax2.set_xlabel(f"ms from {a.ref}'s first move")
ax1.legend(fontsize=7, ncol=6, loc="upper right")
ax1.set_title(a.title or a.csv, fontsize=10)
ax1.grid(alpha=.3); ax2.grid(alpha=.3)
fig.tight_layout(); fig.savefig(a.out, dpi=110)
print(a.out)
