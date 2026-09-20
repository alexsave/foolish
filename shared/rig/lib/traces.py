#!/usr/bin/env python3
"""Both bars' y over time, and where the jerk score actually comes from.

    traces.py out.png "label=glob" "label=glob" ...
"""
import csv, glob, sys
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt


def take(path):
    rows = list(csv.DictReader(open(path)))
    t, top, bot, cl = [], [], [], []
    for r in rows:
        if not r["top_pt"] and not r["bot_pt"]:
            continue
        t.append(float(r["offset"]))
        top.append(float(r["top_pt"]) if r["top_pt"] else np.nan)
        bot.append(float(r["bot_pt"]) if r["bot_pt"] else np.nan)
        cl.append(r.get("offscreen", "").strip() == "1"
                  or r.get("topoff", "").strip() == "1")
    t = np.array(t); top = np.array(top); bot = np.array(bot); cl = np.array(cl)
    mv = np.nonzero(top != top[0])[0]
    if not len(mv):
        return None
    t0 = t[mv[0] - 1] if mv[0] else t[0]
    return t - t0, top, bot, cl


runs = []
for spec in sys.argv[2:]:
    label, pat = spec.split("=", 1)
    ts = [x for x in (take(p) for p in sorted(glob.glob(pat))) if x]
    if ts:
        runs.append((label, ts))

fig, (ax, bx) = plt.subplots(2, 1, figsize=(12, 9),
                             gridspec_kw={"height_ratios": [3, 2]})
cols = ["#b71c1c", "#0d47a1", "#1b5e20", "#e65100"]
for i, (label, ts) in enumerate(runs):
    c = cols[i % len(cols)]
    for k, (t, top, bot, cl) in enumerate(ts):
        m = (t >= -0.05) & (t <= 0.75)
        ax.plot(t[m] * 1000, top[m], color=c, lw=1.6, alpha=0.75,
                label="%s  red (box top)" % label if k == 0 else None)
        ax.plot(t[m] * 1000, bot[m], color=c, lw=1.6, alpha=0.75, ls="--",
                label="%s  green (box bottom)" % label if k == 0 else None)
        for y, ls in ((top, "-"), (bot, "--")):
            d = np.diff(y); keep = ~(cl[1:] | cl[:-1]) & ~np.isnan(d)
            tt = t[1:][m[1:] & keep]
            bx.plot(tt * 1000, (d[m[1:] & keep]) ** 2 + 1e-3, ls,
                    color=c, lw=1.3, alpha=0.7,
                    label=("%s %s" % (label, "red" if ls == "-" else "green"))
                          if k == 0 else None)
ax.invert_yaxis(); ax.grid(alpha=0.25)
ax.set_ylabel("y on screen (pt)"); ax.set_xlabel("ms from the flip")
ax.set_title("Both ruler bars through the collapse")
ax.legend(fontsize=8, ncol=2, loc="center right")
bx.set_yscale("log"); bx.grid(alpha=0.25)
bx.set_ylabel("squared step (pt²), log"); bx.set_xlabel("ms from the flip")
bx.set_title("Where the jerk score comes from - every frame's step, squared")
bx.legend(fontsize=8, ncol=2)
fig.tight_layout(); fig.savefig(sys.argv[1], dpi=130)
print(sys.argv[1])
