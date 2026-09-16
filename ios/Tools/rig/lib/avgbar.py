#!/usr/bin/env python3
"""One bar, averaged across takes, on its own chart.

    avgbar.py <out-prefix> <glob> [--span 0.75]

Writes <prefix>_red.png and <prefix>_green.png.

CLAMPED SAMPLES ARE LEFT OUT OF THE MEAN. A bar that has gone off the frame is
recorded at the edge it left by, which is the right answer to "where was it"
and a lie to average: a handful of 0s drag the red mean hundreds of points
toward the top of the screen and invent a dip nothing on the phone does. They
are counted instead, and the count is drawn under the curve, so a stretch of
the mean that rests on fewer takes says so rather than looking equally solid.
"""
import argparse, csv, glob, os
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

GRID = 1.0 / 120.0


def take(path):
    t, top, bot, cl_t, cl_b = [], [], [], [], []
    for r in csv.DictReader(open(path)):
        if not r["top_pt"] and not r["bot_pt"]:
            continue
        t.append(float(r["offset"]))
        top.append(float(r["top_pt"]) if r["top_pt"] else np.nan)
        bot.append(float(r["bot_pt"]) if r["bot_pt"] else np.nan)
        cl_t.append(r.get("topoff", "").strip() == "1")
        cl_b.append(r.get("offscreen", "").strip() == "1")
    if len(t) < 8:
        return None
    t = np.array(t); top = np.array(top); bot = np.array(bot)
    top[np.array(cl_t)] = np.nan
    bot[np.array(cl_b)] = np.nan
    mv = np.nonzero(~np.isnan(top) & (top != top[np.argmax(~np.isnan(top))]))[0]
    if not len(mv):
        return None
    t0 = t[max(0, mv[0] - 1)]
    return t - t0, top, bot


def chart(out, g, series, colour, title, ylab):
    fig, ax = plt.subplots(figsize=(11, 6))
    for y in series:
        ax.plot(g * 1000, y, color=colour, alpha=0.16, lw=1)
    n = np.sum(~np.isnan(series), axis=0)
    mean = np.nanmean(np.where(n[None, :] > 0, series, np.nan), axis=1 - 1) \
        if False else np.nanmean(series, axis=0)
    ax.plot(g * 1000, mean, color=colour, lw=2.6, label="mean of %d takes" % len(series))
    ax.invert_yaxis(); ax.grid(alpha=0.25)
    ax.set_xlabel("ms from the flip"); ax.set_ylabel(ylab)
    ax.set_title(title)
    bx = ax.twinx()
    bx.fill_between(g * 1000, 0, n, color="#9e9e9e", alpha=0.18, step="mid")
    bx.set_ylim(0, len(series) * 4)
    bx.set_yticks([0, len(series)])
    bx.set_ylabel("takes in frame", color="#616161")
    ax.legend(loc="center right")
    fig.tight_layout(); fig.savefig(out, dpi=130)
    print(out)


ap = argparse.ArgumentParser()
ap.add_argument("prefix"); ap.add_argument("pattern")
ap.add_argument("--span", type=float, default=0.75)
a = ap.parse_args()

g = np.arange(-0.05, a.span + GRID / 2, GRID)
reds, greens = [], []
for p in sorted(glob.glob(a.pattern)):
    r = take(p)
    if not r:
        continue
    t, top, bot = r
    for src, dst in ((top, reds), (bot, greens)):
        ok = ~np.isnan(src)
        y = np.interp(g, t[ok], src[ok], left=np.nan, right=src[ok][-1])
        # An interp across a clamped gap would draw a straight line through it;
        # blank any grid point whose nearest real sample is further than two
        # frames away, so a gap reads as a gap.
        near = np.min(np.abs(g[:, None] - t[ok][None, :]), axis=1)
        y[near > 2 * GRID] = np.nan
        dst.append(y)
reds = np.array(reds); greens = np.array(greens)
chart(a.prefix + "_red.png", g, reds, "#b71c1c",
      "RED bar - the table group's top edge, averaged", "y on screen (pt)")
chart(a.prefix + "_green.png", g, greens, "#1b5e20",
      "GREEN bar - the hand and buttons' edge, averaged", "y on screen (pt)")
