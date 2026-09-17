#!/usr/bin/env python3
"""Track every table pair, frame by frame, and fail on a jump.

    tablesquares.py <take-dir> [--csv out.csv] [--jump PT] [--quiet]

The live table draws a coloured square at the centre of each pair's slot when
`dev.ruler` is on (`View.tableSquare`, CollapseRuler.swift): cyan, yellow, green,
repeating. The horizontal ruler bars cannot see a throw-in - it re-centres the
row SIDEWAYS, and a bar through the table's centre does not move while both
pairs slide 36pt left. These squares move exactly when the layout does.

Reads the frames and `times.txt` that `lib/window.sh` writes. Prints one line
per pair and exits 1 when any pair jumped (see `is_jump`): further than
`--jump` points between neighbouring frames, or too fast across a gap. Exits 2 when there is nothing to
judge (no squares at all - is the ruler on, is this a DEBUG build?).

THE MAGENTA BAR RUNS THROUGH THE SQUARES. On a single row the table's centre
line is the pairs' centre line, so each square comes out as two halves with a
4pt magenta stripe between them. The halves are joined by a vertical dilation
before labelling, and the square is measured on its RAW pixels, whose outer
edges are the square's own - so the centre is exact either way.

A SQUARE THAT IS PARTLY COVERED IS NOT A POSITION. A flying card passes over
the table; a square it clips has the wrong centre, so any shape that is not
square-sized is dropped for that frame. A pair that is missing for a while and
comes back somewhere else is still a jump (see `steps`).
"""
import argparse, glob, os, sys
from multiprocessing import Pool
import numpy as np
from PIL import Image
from scipy import ndimage

import tween

SIDE_PT = 12.0            # CollapseRuler.squareSide
BAR_PT = 4.0              # CollapseRuler.edge - the magenta bar's thickness
# The ruler's band strip down the left edge is cyan/magenta/yellow cells of
# about this size; nothing on the table comes near it.
STRIP_PX = 80


# 30px at 3x (10pt), under the square's 36px, so a grid point always lands
# inside a WHOLE square. Owner: "if the squares are 36 pixels you might as well
# use 30 pixel strides."
#
# A square the magenta bar runs through is not whole: it is two 12px halves
# with the 12px bar between them, and a 30px grid can step over all three. So
# the rows immediately above and below the bar are scanned as well - every
# square the bar cuts has a half on both of them.
STRIDE_PT = 10.0


def colour_of(px):
    """Which square colour an (r, g, b) pixel is, or None."""
    r, g, b = (int(v) for v in px)
    hi, lo = 140, 90
    if g > hi and b > hi and r < lo:
        return "cyan"
    if r > hi and g > hi and b < lo:
        return "yellow"
    if g > hi and r < lo and b < lo:
        return "green"
    return None


def mask(win, name):
    r, g, b = (win[:, :, i].astype(np.int16) for i in range(3))
    hi, lo = 140, 90
    if name == "cyan":
        return (g > hi) & (b > hi) & (r < lo)
    if name == "yellow":
        return (r > hi) & (g > hi) & (b < lo)
    return (g > hi) & (r < lo) & (b < lo)


def bar_edges(a, s):
    """The rows just outside the magenta bar, found down two columns (one of
    them may be under a flying card)."""
    h, w = a.shape[0], a.shape[1]
    out = set()
    for x in (w // 2, w - 40):
        col = a[:, x, :].astype(np.int16)
        ys = np.nonzero((col[:, 0] > 140) & (col[:, 2] > 140) & (col[:, 1] < 90))[0]
        for run in np.split(ys, np.nonzero(np.diff(ys) > 2)[0] + 1) if len(ys) else []:
            if abs(len(run) - BAR_PT * s) <= 3:
                out.update((int(run[0]) - 3, int(run[-1]) + 3))
    return {y for y in out if 0 <= y < h}


def read_frame(p):
    """Every whole square in one frame, as (colour, x_pt, y_pt).

    A GRID, NOT A SCAN. Owner: "scan strides of 20 px in a grid" - and
    then "you might as well use 30 pixel strides" for a 36px square. A square is wider than
    the stride at both scales, so at least one grid point lands inside every
    square; only the neighbourhood of a hit is then looked at closely.
    """
    a = np.asarray(Image.open(p).convert("RGB"))
    h, w = a.shape[0], a.shape[1]
    s = 3 if h >= 2000 else 2
    side = SIDE_PT * s
    reach = int(2 * side)
    stride = int(round(STRIDE_PT * s))
    rows = set(range(stride // 2, h, stride))
    rows.update(bar_edges(a, s))
    out, done = [], []
    for y in sorted(rows):
        for x in range(STRIP_PX, w, stride):
            name = colour_of(a[y, x])
            if name is None:
                continue
            if any(n == name and x0 <= x <= x1 and y0 <= y <= y1 for n, x0, y0, x1, y1 in done):
                continue
            wx0, wy0 = max(STRIP_PX, x - reach), max(0, y - reach)
            win = a[wy0:min(h, y + reach), wx0:min(w, x + reach)]
            m = mask(win, name)
            # Join the halves across the magenta bar (plus a pixel of h264 blur).
            joined = ndimage.binary_dilation(
                m, structure=np.ones((int(BAR_PT * s) + 3, 1), dtype=bool))
            lab, _ = ndimage.label(joined)
            k = lab[y - wy0, x - wx0]
            if k == 0:
                continue
            ys, xs = np.nonzero(m & (lab == k))
            x0, x1, y0, y1 = xs.min() + wx0, xs.max() + wx0, ys.min() + wy0, ys.max() + wy0
            done.append((name, x0, y0, x1, y1))
            bw, bh = x1 - x0 + 1, y1 - y0 + 1
            # Square-sized and square-shaped, or it is something else (a bar, a
            # glyph, a square half under a flying card, one cut by the window).
            if not (0.75 * side <= bw <= 1.25 * side and 0.75 * side <= bh <= 1.25 * side):
                continue
            # Most of the box is the colour: the bar may take 4pt of 12.
            if len(xs) < 0.45 * side * side:
                continue
            out.append((name, round((x0 + x1) / 2.0 / s, 2), round((y0 + y1) / 2.0 / s, 2)))
    return out


def track(frames_seen, times, link_pt=80.0):
    """Squares -> pairs. Same colour, nearest to where that pair last was."""
    tracks = []                      # dicts: colour, samples [(i, t, x, y)]
    for i, seen in enumerate(frames_seen):
        taken = set()
        for name, x, y in seen:
            best, bd = None, link_pt
            for k, tr in enumerate(tracks):
                if tr["colour"] != name or k in taken:
                    continue
                _, _, px, py = tr["samples"][-1]
                d = ((x - px) ** 2 + (y - py) ** 2) ** 0.5
                if d < bd:
                    best, bd = k, d
            if best is None:
                tracks.append({"colour": name, "samples": []})
                best = len(tracks) - 1
            taken.add(best)
            tracks[best]["samples"].append((i, times[i], x, y))
    return tracks


def steps(samples):
    """Every move between two samples of one pair: (from, to, dx, dy, dt).

    Consecutive samples, whether or not frames were missed between them: a pair
    hidden under a flying card that reappears 36pt away did not slide there,
    and skipping the gap is how a jump gets scored as nothing.
    """
    return [(a[0], b[0], b[2] - a[2], b[3] - a[3], b[1] - a[1])
            for a, b in zip(samples, samples[1:])]


NEAR_S = 0.040            # two samples this close are neighbouring frames
GAP_SPEED = 250.0         # pt/s: across a longer gap, faster than this is a jump


def is_jump(dist, dt, jump_pt):
    """A jump is DISTANCE between neighbouring frames, SPEED across a gap.

    Speed alone does not work: the recorder writes frames 3ms apart, and a
    2pt step of a perfectly smooth slide is 700pt/s over 3ms. Distance alone
    does not work either: the thrown card flies over a pair's square, and the
    first sample after 92ms under it was 7.8pt on - 85pt/s, a slide. A 35pt
    jump hidden for the same 92ms is 380pt/s. (Hidden for 200ms it would be
    175 and pass - the limit of what a covered square can say.)
    """
    if dist <= jump_pt:
        return False
    return dt <= NEAR_S or dist / max(dt, 1e-6) > GAP_SPEED


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("take")
    ap.add_argument("--csv")
    ap.add_argument("--jump", type=float, default=8.0,
                    help="points between two samples that count as a jump")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    frames = sorted(glob.glob(os.path.join(a.take, "f*.ppm"))) \
        or sorted(glob.glob(os.path.join(a.take, "f*.png")))
    if not frames:
        sys.exit("no frames in %s" % a.take)
    times = tween.frame_times(a.take, len(frames))
    with Pool() as pool:
        seen = pool.map(read_frame, frames, chunksize=8)
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("frame,t,colour,x_pt,y_pt\n")
            for i, s in enumerate(seen):
                for name, x, y in s:
                    fh.write("%d,%.4f,%s,%.2f,%.2f\n" % (i + 1, times[i], name, x, y))
    tracks = [t for t in track(seen, times) if len(t["samples"]) >= 3]
    if not tracks:
        print("NO TABLE SQUARES IN ANY FRAME - is `rig.sh ruler on` set, and is "
              "this a DEBUG build?", file=sys.stderr)
        sys.exit(2)
    jumps = 0
    print("%-8s %6s %8s %8s %9s %9s  %s" % ("pair", "frames", "x from", "x to",
                                            "max step", "sum sq", "jumps"))
    for tr in sorted(tracks, key=lambda t: t["samples"][0][2]):
        st = steps(tr["samples"])
        mags = [(dx * dx + dy * dy) ** 0.5 for _, _, dx, dy, _ in st]
        big = [(f0, f1, m) for (f0, f1, _, _, dt), m in zip(st, mags)
               if is_jump(m, dt, a.jump)]
        jumps += len(big)
        sm = tr["samples"]
        print("%-8s %6d %8.1f %8.1f %9.1f %9.1f  %s" % (
            tr["colour"], len(sm), sm[0][2], sm[-1][2], max(mags) if mags else 0.0,
            sum(m * m for m in mags),
            ", ".join("f%d->f%d %.1fpt" % (f0 + 1, f1 + 1, m) for f0, f1, m in big) or "-"))
    if jumps:
        print("\nJUMP: %d step(s) over %.0fpt - a table pair moved without sliding"
              % (jumps, a.jump))
        sys.exit(1)
    print("\nsmooth: no pair moved more than %.0fpt between frames" % a.jump)


if __name__ == "__main__":
    main()
