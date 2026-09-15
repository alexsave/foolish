#!/usr/bin/env python3
"""The auto-collapse's cost, as ONE number, over many takes.

The box's BOTTOM edge is the thing under judgement. It sits at rest, the
collapse lifts it ~31pt in the first 50ms, and it comes back down to settle a
few points above where it began. That lift is the defect: nothing in the design
asks the bottom edge to move, and on a phone with a home indicator it travels
into the safe area and back out.

So define a SAFE BAND from the two positions the edge is allowed to hold - its
resting value before the tween and its final value after - and score every frame
by how far outside that band it is:

    err(frame) = 0                  if lo <= bot <= hi
                 lo - bot           if the edge is ABOVE the band
                 bot - hi           if the edge is BELOW it

Per the owner's definition the AVERAGE across takes is taken first and the
square second:

    MSE = sum over the collapse of ( mean_over_takes( err ) ) ** 2

which is not the same as averaging each take's MSE - averaging first cancels
noise that is not repeatable and keeps only the excursion the design actually
has. Both are printed, because a large gap between them is itself a finding:
it means the takes disagree, and a mean of disagreeing takes is not a curve.

The takes are put on a common time grid before any of this, anchored on the
LAST STILL FRAME before motion - a take's window starts wherever the recorder
happened to be, so raw frame indices do not line up between takes.

    mse.py <edge.csv> [<edge.csv> ...] [--lo 915.0] [--hi 921.7]
           [--plot out.png] [--label NAME] [--json out.json]
"""
import argparse, glob, json, os, sys
import numpy as np

# The Pro Max baseline, measured over 20 clean takes: the edge rests at 921.7pt
# and settles at 915.0. Pinned rather than derived so that a change which moves
# the RESTING place cannot quietly move the goalposts with it - the derived band
# is printed alongside, and a difference between the two is the change moving
# the endpoints rather than the excursion.
LO, HI = 915.0, 921.7
GRID = 1.0 / 60.0     # the display's own rate; takes are filmed at it


def load(path):
    """One take: (t, bot) with t=0 at the last still frame before motion."""
    ts, bots, hs = [], [], []
    with open(path) as fh:
        head = fh.readline().strip().split(",")
        it, ib, ih = head.index("offset"), head.index("bot_pt"), head.index("h_pt")
        for line in fh:
            f = line.rstrip("\n").split(",")
            if not f[ib]:
                continue
            ts.append(float(f[it])); bots.append(float(f[ib])); hs.append(float(f[ih]))
    if len(ts) < 4:
        return None
    t = np.array(ts); b = np.array(bots); h = np.array(hs)
    # Motion starts on the first frame whose HEIGHT differs from the still lead.
    # Height, not bottom: the bottom edge's first move is small enough to be a
    # rounding step, while the top edge travels 500pt and cannot be mistaken.
    mv = np.nonzero(h != h[0])[0]
    if not len(mv):
        return None
    t0 = t[mv[0] - 1] if mv[0] else t[0]
    return t - t0, b


def grid_of(takes, span):
    g = np.arange(0.0, span + GRID / 2, GRID)
    out = []
    for t, b in takes:
        # Hold the last known value past the end of a short take rather than
        # extrapolating: the edge really is parked there.
        out.append(np.interp(g, t, b, left=b[0], right=b[-1]))
    return g, np.array(out)


def err(b, lo, hi):
    return np.maximum(0.0, np.maximum(lo - b, b - hi))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--lo", type=float, default=LO)
    ap.add_argument("--hi", type=float, default=HI)
    ap.add_argument("--span", type=float, default=1.4, help="seconds of collapse to score")
    ap.add_argument("--plot"); ap.add_argument("--label", default="")
    ap.add_argument("--json")
    a = ap.parse_args()

    paths = []
    for p in a.csv:
        paths += sorted(glob.glob(p)) if any(c in p for c in "*?[") else [p]
    takes, kept = [], []
    for p in paths:
        r = load(p)
        if r is None:
            print("  skip (no motion): %s" % p, file=sys.stderr); continue
        takes.append(r); kept.append(p)
    if not takes:
        print("no usable takes", file=sys.stderr); sys.exit(1)

    g, B = grid_of(takes, a.span)
    E = err(B, a.lo, a.hi)              # per take, per grid point
    mean_e = E.mean(axis=0)             # average FIRST
    mse = float((mean_e ** 2).sum())    # then square, then sum
    per_take = (E ** 2).sum(axis=1)
    mean_of_mse = float(per_take.mean())

    # What the band WOULD be if derived from these takes rather than pinned.
    rest = float(np.median(B[:, 0])); final = float(np.median(B[:, -1]))
    d_lo, d_hi = min(rest, final), max(rest, final)

    # JUDDER, measured on each take separately and averaged.
    #
    # This is here because the MSE above can be BLIND to the thing the takes
    # actually show. The bottom edge does not overshoot smoothly - it alternates
    # roughly +/-14pt every frame while the top edge slides, i.e. the whole box
    # translates vertically at ~60Hz. Averaging the takes FIRST (which is the
    # definition asked for) cancels any of that whose phase differs take to
    # take, so a change that only re-phased the judder would read as a win.
    # Frame-to-frame absolute movement, per take, does not care about phase.
    # On the RAW frames, not the grid: the takes are filmed at ~120Hz and the
    # alternation IS at that rate, so resampling to 60 would alias exactly the
    # thing being counted.
    jd = []
    for t, b in takes:
        w = b[(t >= 0) & (t <= a.span)]
        if len(w) > 2:
            jd.append(np.abs(np.diff(w)))
    jud = float(np.mean([x.mean() for x in jd])) if jd else 0.0
    jmax = float(max(x.max() for x in jd)) if jd else 0.0

    peak_i = int(np.argmax(mean_e))
    name = a.label or os.path.basename(os.path.dirname(kept[0]))
    print("takes    %d" % len(takes))
    print("band     %.1f .. %.1f pt (pinned)   derived from these takes: %.1f .. %.1f"
          % (a.lo, a.hi, d_lo, d_hi))
    print("rest     %.1f pt      final  %.1f pt      net %+.1f"
          % (rest, final, final - rest))
    print("peak     %.1f pt away from the band, at +%.0f ms"
          % (mean_e[peak_i], g[peak_i] * 1000))
    print("outside  %d of %d grid points (%.0f%% of the scored span)"
          % (int((mean_e > 0.05).sum()), len(g), 100.0 * (mean_e > 0.05).mean()))
    print()
    print("MSE      %.1f   <- sum over frames of (mean across takes of err)^2" % mse)
    print("  spread %.1f   (mean of each take's own MSE; a gap here means the "
          "takes disagree)" % mean_of_mse)
    print("  worst  %.1f   best %.1f" % (per_take.max(), per_take.min()))
    print("judder   %.2f pt mean frame-to-frame move of the bottom edge "
          "(max %.1f) - phase-blind, so it sees what the mean cancels"
          % (jud, jmax))

    if a.json:
        json.dump({"name": name, "takes": len(takes), "mse": mse,
                   "mean_of_take_mse": mean_of_mse, "peak_pt": float(mean_e[peak_i]),
                   "peak_ms": float(g[peak_i] * 1000),
                   "judder": jud, "judder_max": jmax, "rest": rest, "final": final,
                   "lo": a.lo, "hi": a.hi,
                   "curve": [[float(x), float(y)] for x, y in zip(g, mean_e)],
                   "bottom": [[float(x), float(y)] for x, y in zip(g, B.mean(axis=0))]},
                  open(a.json, "w"))
    if a.plot:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(11, 6))
        for row in B:
            ax.plot(g * 1000, row, color="#2e7d32", alpha=0.18, lw=1)
        ax.plot(g * 1000, B.mean(axis=0), color="#1b5e20", lw=2.4, label="mean bottom edge")
        ax.axhspan(a.lo, a.hi, color="#9ccc65", alpha=0.35, zorder=0,
                   label="safe band %.1f-%.1f" % (a.lo, a.hi))
        ax.invert_yaxis()
        ax.set_xlabel("ms from the last still frame")
        ax.set_ylabel("box bottom edge (pt, screen coords)")
        ax.set_title("%s   MSE %.1f over %d takes" % (name, mse, len(takes)))
        ax.legend(loc="lower right"); ax.grid(alpha=0.25)
        fig.tight_layout(); fig.savefig(a.plot, dpi=130)
        print("\nplot     %s" % a.plot)


if __name__ == "__main__":
    main()
