#!/usr/bin/env python3
"""x and y of every square over one scenario of a reel, as a picture.

    squareplot.py REEL_DIR "scenario label" out.png [--offset S] [--pad S]

REEL_DIR is an anim_reel.sh film directory (squares.csv + marks.txt). The
window runs from the scenario's "open:" mark to the next scenario's, so both
the move and its Undo are in it; the move and the Undo are drawn as lines.
`--offset` is how much later the movie started than the recorder - the reel's
reader prints it ("the movie starts 1.71s after the recorder did").

Table pairs keep their colour; flying cards are orange; x and y are points in
the drawer box (the frame the reader measures in), so a jump reads as a
vertical cliff in a line that is otherwise a slope.
"""
import argparse, csv
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLS = {"cyan": "#00a5b8", "yellow": "#c9a400", "green": "#2e9d3a",
        "magenta": "#b0269c", "orange": "#e36a00", "blue": "#1f3fd1"}

ap = argparse.ArgumentParser()
ap.add_argument("reel"); ap.add_argument("label"); ap.add_argument("out")
ap.add_argument("--offset", type=float, default=0.0)
ap.add_argument("--pad", type=float, default=0.0)
a = ap.parse_args()

marks = []
for line in open(f"{a.reel}/marks.txt"):
    t, _, name = line.strip().partition(" ")
    if t != "rec0":
        marks.append((float(t) - a.offset, name))
names = [n for _, n in marks]
start_i = names.index("open: " + a.label) if "open: " + a.label in names else names.index(a.label)
t0 = marks[start_i][0]
nxt = [t for t, n in marks[start_i + 1:] if n.startswith("open:") or n.startswith("replay")]
t1 = nxt[0] if nxt else float("inf")
inside = {n: t for t, n in marks[start_i:] if t < t1}
move_t, undo_t = inside.get(a.label), inside.get("undo " + a.label)
lo = (move_t if move_t is not None else t0) - a.pad

pts = {}
for r in csv.DictReader(open(f"{a.reel}/squares.csv")):
    t = float(r["t"])
    if lo <= t < t1 and r["colour"] != "magenta":
        pts.setdefault(r["colour"], []).append((t, float(r["x_in_box_pt"]), float(r["y_in_box_pt"])))

fig, axes = plt.subplots(2, 1, figsize=(13, 8), sharex=True)
for ax, k, name in ((axes[0], 1, "x (pt, in drawer)"), (axes[1], 2, "y (pt, in drawer)")):
    for colour, ps in sorted(pts.items()):
        ps.sort()
        ax.plot([p[0] for p in ps], [p[k] for p in ps], ".", ms=3,
                color=COLS.get(colour, "k"), label=colour)
    for t, lab, st in ((move_t, "move", "-"), (undo_t, "undo tapped", "--")):
        if t is not None:
            ax.axvline(t, color="#555", ls=st, lw=1)
            ax.text(t, ax.get_ylim()[1], " " + lab, va="top", fontsize=9, color="#555")
    ax.set_ylabel(name); ax.grid(alpha=.25)
axes[0].legend(loc="upper right", fontsize=8, markerscale=3)
axes[1].invert_yaxis()
axes[1].set_xlabel("film seconds")
fig.suptitle(f"{a.label}: every table square (by pair) and flying card (orange), every frame")
fig.tight_layout(); fig.savefig(a.out, dpi=110)
print(a.out)
