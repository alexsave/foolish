#!/usr/bin/env python3
"""Two or more scored runs on one pair of axes.

Top: the bottom edge's mean path against the safe band, so the SHAPE of a
change is visible and not just its number. Bottom: MSE per run as bars.

    msecmp.py out.png a.json b.json ...
"""
import json, sys
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

out, files = sys.argv[1], sys.argv[2:]
runs = [json.load(open(f)) for f in files]
fig, (ax, bx) = plt.subplots(2, 1, figsize=(11, 8.5),
                             gridspec_kw={"height_ratios": [3, 1]})
cols = ["#b71c1c", "#1b5e20", "#0d47a1", "#e65100", "#4a148c", "#006064"]
r0 = runs[0]
ax.axhspan(r0["lo"], r0["hi"], color="#9ccc65", alpha=0.35, zorder=0,
           label="safe band %.1f-%.1f" % (r0["lo"], r0["hi"]))
for i, r in enumerate(runs):
    t = [p[0] * 1000 for p in r["bottom"]]
    y = [p[1] for p in r["bottom"]]
    ax.plot(t, y, color=cols[i % len(cols)], lw=2.2,
            label="%s   MSE %.0f   peak %.1fpt" % (r["name"], r["mse"], r["peak_pt"]))
ax.invert_yaxis(); ax.grid(alpha=0.25)
ax.set_xlabel("ms from the last still frame")
ax.set_ylabel("box bottom edge (pt)")
ax.set_title("The auto-collapse's bottom edge - mean of %d takes each" % r0["takes"])
ax.legend(loc="lower right")
names = [r["name"] for r in runs]
bars = bx.bar(names, [r["mse"] for r in runs],
              color=[cols[i % len(cols)] for i in range(len(runs))])
for b, r in zip(bars, runs):
    bx.text(b.get_x() + b.get_width() / 2, b.get_height(), " %.0f" % r["mse"],
            ha="center", va="bottom", fontsize=9)
bx.set_ylabel("MSE"); bx.grid(alpha=0.25, axis="y")
fig.tight_layout(); fig.savefig(out, dpi=130)
print(out)
