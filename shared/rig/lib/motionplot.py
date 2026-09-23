#!/usr/bin/env python3
"""Chart a take the C finder tracked (shared/tools/motion: motion_take.sh -> take.tbl).

    motionplot.py <take.tbl> <out.png> [--title T] [--span 1.2] [--lead 0.15] [--whole]
                  [--anchor MARK=red|green|mid|none]...

Three panels against milliseconds from the first move of either bar: every
square's y and the bars (screen down), every square's x, and each square's y
minus its anchor's - the spec's edge it lives on (the header the red top bar,
the doors the green bottom bar, the board the drawer's centre), the same
anchors `motion score` uses. A mark that rides its anchor is a flat line in the
last panel and every step in it is a snap.

Plotting only: the numbers come from the C tool, and this reads its fixed-layout
table. No scoring lives here.
"""
import argparse
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK = {"magenta": "#e000e0", "cyan": "#00b8b8", "yellow": "#c8b400", "orange": "#ff8000",
       "blue": "#0000ff", "violet": "#8000ff", "lime": "#60c000", "pink": "#ff0080",
       "red": "#ff0000", "green": "#00a000"}
# motion.c mt_default_opts: the header holds the top, the doors the bottom, the board the centre
ANCHOR = {"orange": "red", "yellow": "red", "lime": "red", "blue": "green"}

ap = argparse.ArgumentParser()
ap.add_argument("tbl"); ap.add_argument("out")
ap.add_argument("--title", default="")
ap.add_argument("--span", type=float, default=1.2)
ap.add_argument("--lead", type=float, default=0.15)
ap.add_argument("--whole", action="store_true")
ap.add_argument("--anchor", action="append", default=[])
a = ap.parse_args()
for s in a.anchor:
    k, v = s.split("="); ANCHOR[k] = v

lines = [l.split() for l in open(a.tbl) if not l.startswith("#")]
head, rows = lines[0], [dict(zip(lines[0], r)) for r in lines[1:]]
num = lambda s: None if s in ("-", None) else float(s)
t = [float(r["t"]) for r in rows]


def first_move(col):
    v0 = None
    for i, r in enumerate(rows):
        v = num(r[col])
        if v is None:
            continue
        if v0 is None:
            v0 = v
        elif abs(v - v0) > 1:
            return i
    return None


cand = [i for i in (first_move("red"), first_move("green")) if i is not None]
i0 = min(cand) if cand and not a.whole else 0
t0 = t[i0]
keep = [i for i in range(len(rows)) if a.whole or t0 - a.lead <= t[i] <= t0 + a.span]
marks = [c[:-2] for c in head if c.endswith("_y")]
marks = [m for m in marks if sum(1 for i in keep if num(rows[i][m + "_y"]) is not None) >= 3]


def anchor(m, r):
    an = ANCHOR.get(m.split("_")[0], "mid")
    R, G = num(r["red"]), num(r["green"])
    if an == "none":
        return 0.0
    if an == "red":
        return R
    if an == "green":
        return G
    return None if R is None or G is None else (R + G) / 2


fig, (ay, ax, ao) = plt.subplots(3, 1, figsize=(11, 11), sharex=True)
xs = [(t[i] - t0) * 1000 for i in keep]
for m in marks + ["red", "green"]:
    c = INK.get(m.split("_")[0], "k")
    if m in ("red", "green"):
        ay.plot(xs, [num(rows[i][m]) for i in keep], "--", lw=1, color=c, label=m)
        continue
    ys = [num(rows[i][m + "_y"]) for i in keep]
    ay.plot(xs, ys, "-", marker=".", ms=3, lw=1, color=c, label=m)
    ax.plot(xs, [num(rows[i][m + "_x"]) for i in keep], "-", marker=".", ms=3, lw=1, color=c, label=m)
    off = []
    for y, i in zip(ys, keep):
        A = anchor(m, rows[i])
        off.append(None if y is None or A is None else y - A)
    ao.plot(xs, off, "-", marker=".", ms=3, lw=1, color=c,
            label=f"{m} - {ANCHOR.get(m.split('_')[0], 'mid')}")
ay.invert_yaxis(); ao.invert_yaxis()
ay.set_ylabel("y (pt, screen down)"); ax.set_ylabel("x (pt)"); ao.set_ylabel("y - anchor (pt)")
ao.set_xlabel("ms from the first move" if not a.whole else "ms from the start of the take")
ay.legend(fontsize=7, ncol=7, loc="upper right"); ao.legend(fontsize=7, ncol=5, loc="upper right")
ay.set_title(a.title or a.tbl, fontsize=10)
for p in (ay, ax, ao):
    p.grid(alpha=.3)
fig.tight_layout(); fig.savefig(a.out, dpi=100)
print(a.out)
