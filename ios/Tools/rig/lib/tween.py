#!/usr/bin/env python3
"""Read the collapse ruler out of a filmed take, frame by frame.

`CollapseRuler` (FoolishKit) draws, behind `dev.ruler`, on the surface's SIZED
BOX - so it measures `boxHeight`, the number the tween actually moves, not the
drawer:

    red bar      the box TOP        full width, 4pt
    green bar    the box BOTTOM     full width, 4pt
    banded strip 10pt bands from the top, 18pt wide, pure primaries
    clock        14 white/black cells, 12pt, ms mod 16384, under the top bar

Do NOT try to find the box from the board's own pixels. The ruler's own file
says why: the collapse is composited by Messages from snapshots of our view over
featureless wool, so "where is the top of our box" is not answerable from
content - and the one landmark that is not wool (the hand) is exactly the thing
whose position is in question.

Two things this reports that a geometry trace alone cannot:

  CLOCK REPEATS. The strip is redrawn by `TimelineView(.animation)` on every
  display refresh, so it freezes on frames we did not draw. A frame whose clock
  repeats while the geometry moves is the HOST interpolating a stale snapshot,
  and no animation curve can fix a frame the app never rendered.

  BAND PITCH. Bands are 10pt apart by construction, so a pitch that is not 10
  means the imagery was SCALED on its way to the screen.

    tween.py <take-dir> [--csv out.csv] [--quiet]
"""
import argparse, glob, os, sys
from multiprocessing import Pool
import numpy as np
from PIL import Image

BAND, CELL, BITS, EDGE = 10.0, 12.0, 14, 4.0


def rows_of(mask, w, frac=0.55):
    """Row indices where `mask` covers at least `frac` of the width."""
    return np.nonzero(mask.sum(axis=1) > w * frac)[0]


def runs(idx, gap=2):
    if not len(idx):
        return []
    return np.split(idx, np.nonzero(np.diff(idx) > gap)[0] + 1)


def read_frame(p):
    a = np.asarray(Image.open(p).convert("RGB")).astype(int)
    h, w = a.shape[0], a.shape[1]
    s = 3 if w >= 1000 else 2
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    # Tolerant, because h264 4:2:0 will not hand back a pure primary. The
    # palette was chosen so that a CHANNEL TEST still separates them.
    red = (r > 140) & (g < 90) & (b < 90)
    grn = (g > 140) & (r < 90) & (b < 90)
    rr, gg = runs(rows_of(red, w)), runs(rows_of(grn, w))
    if not rr or not gg:
        return None
    top, bot = int(rr[0][0]), int(gg[-1][-1])
    # The box's own left edge, from the full-width top bar. The banded strip is
    # 18pt from THERE; slicing from the screen's edge measures board content and
    # reports a pitch that is not the ruler's.
    xs_ = np.nonzero(red[top:top + max(1, int(EDGE * s)), :].sum(axis=0) > 0)[0]
    bx = int(xs_[0]) if len(xs_) else 0
    out = {"top_pt": round(top / s, 1), "bot_pt": round(bot / s, 1),
           "h_pt": round((bot - top) / s, 1)}
    # Band pitch, off the 18pt strip at the box's leading edge: the y centres of
    # the cyan/magenta runs should sit BAND apart.
    # CYAN ONLY. The bands alternate cyan/magenta and are ADJACENT, so a mask of
    # both merges the whole strip into one run and the "pitch" that comes back
    # is an artefact - it read 44pt on a 10pt ruler and called every frame
    # scaled. One colour appears every OTHER band, so the expected spacing is
    # 2 x BAND.
    cy = (g > 130) & (b > 130) & (r < 90)
    strip = slice(bx, bx + int(18 * s))
    ys = sorted(int(np.mean(x)) for x in runs(rows_of(cy[:, strip], 18 * s, 0.5)))
    out["pitch_pt"] = round(float(np.median(np.diff(ys))) / s, 2) if len(ys) > 2 else None
    # The clock: 14 cells under the top bar, from the box's leading edge.
    if len(xs_):
        x0 = bx
        cy_ = top + int((EDGE + CELL / 2) * s)
        if 0 <= cy_ < h:
            bits = ""
            for i in range(BITS):
                cx = x0 + int((i + 0.5) * CELL * s)
                if cx >= w:
                    bits = ""; break
                px = a[cy_, max(0, cx - 2):cx + 3].mean()
                bits += "1" if px > 128 else "0"
            out["clock"] = int(bits, 2) if len(bits) == BITS else None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("take"); ap.add_argument("--csv"); ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    frames = sorted(glob.glob(os.path.join(a.take, "f*.png")))
    if not frames:
        print("no frames in %s" % a.take, file=sys.stderr); sys.exit(1)
    tp = os.path.join(a.take, "times.txt")
    times = [float(l) for l in open(tp)] if os.path.exists(tp) else []

    # A take is hundreds of full-resolution PNGs and the decode is the whole
    # cost, so decode them in parallel - it is the one part of this that is
    # embarrassingly so.
    with Pool() as pool:
        ms = pool.map(read_frame, frames, chunksize=8)
    rows = []
    for i, m in enumerate(ms):
        t = times[i] if i < len(times) else (times[-1] if times else i / 60.0)
        rows.append((i + 1, t, m))
    seen = [r for r in rows if r[2]]
    if not seen:
        print("NO RULER IN ANY FRAME - is `rig.sh ruler on` set, and is this a DEBUG build?",
              file=sys.stderr)
        sys.exit(2)

    t0 = seen[0][1]
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("frame,t,offset,top_pt,bot_pt,h_pt,pitch_pt,clock_ms\n")
            for n, t, m in rows:
                m = m or {}
                fh.write("%d,%.4f,%.4f,%s,%s,%s,%s,%s\n" % (
                    n, t, t - t0, m.get("top_pt", ""), m.get("bot_pt", ""),
                    m.get("h_pt", ""), m.get("pitch_pt", ""), m.get("clock_ms", m.get("clock", ""))))
    if not a.quiet:
        print("%-5s %8s %9s %9s %9s %7s  %s" % ("f", "+off", "top", "bot", "height", "pitch", "clock"))
        for n, t, m in rows:
            if not m:
                print("%-5d %8.3f   (no ruler)" % (n, t - t0)); continue
            print("%-5d %8.3f %9.1f %9.1f %9.1f %7s  %s"
                  % (n, t - t0, m["top_pt"], m["bot_pt"], m["h_pt"],
                     m["pitch_pt"] if m["pitch_pt"] is not None else "-",
                     m.get("clock", "-")))

    hs = [m["h_pt"] for _, _, m in seen]
    mv = [i for i in range(1, len(hs)) if hs[i] != hs[i - 1]]
    print("\nframes   %d (%d with a ruler)" % (len(rows), len(seen)))
    print("height   %.1f -> %.1f pt" % (hs[0], hs[-1]))
    if mv:
        span = seen[mv[-1]][1] - seen[mv[0] - 1][1]
        print("tween    %.3fs, %d frames, %.1f -> %.1f pt"
              % (span, mv[-1] - mv[0] + 2, hs[mv[0] - 1], hs[mv[-1]]))
    # A frame we did not draw: the clock repeats while the geometry moves.
    cl = [(i, m.get("clock"), m["h_pt"]) for i, (_, _, m) in enumerate(seen)]
    stale = sum(1 for i in range(1, len(cl))
                if cl[i][1] is not None and cl[i][1] == cl[i - 1][1] and cl[i][2] != cl[i - 1][2])
    print("stale    %d frame(s) where the clock repeated while the box moved" % stale)
    ps = [m["pitch_pt"] for _, _, m in seen if m["pitch_pt"]]
    if ps:
        want = 2 * BAND
        off = sum(1 for p in ps if abs(p - want) > 2.0)
        print("pitch    median %.1fpt (want %.0f), %d frame(s) off%s"
              % (float(np.median(ps)), want, off,
                 " - imagery was SCALED on its way to the screen" if off else ""))


if __name__ == "__main__":
    main()
