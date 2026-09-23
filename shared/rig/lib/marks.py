#!/usr/bin/env python3
"""Every MotionRuler mark in every frame of a take -> one CSV (shared/swift/MotionRuler.swift).

    marks.py <frames_dir> <out.csv> [--strip-pt 24] [--quads cyan]

A frames dir is what extract_frames.sh / `rig.sh film` writes: f00001.png ... and
times.txt (one pts per frame). Per frame this finds:

  red, green   the y (points) of the full-width top and bottom edge bars
  clock        the MotionRulerClock value (ms mod 16384) under the red bar, blank
               when it cannot be read cleanly; a repeat while geometry moved is a
               frame the app did not render
  <ink>_x/_y   the centre (points) of each 12pt square, by HUE, so eight inks are
               told apart without a colour per rule: magenta cyan yellow orange
               blue violet lime pink (the MotionRuler.Ink palette)

A colour that appears more than once (the board's four corners) is split by
quadrant around the magenta square: cyan_tl, cyan_tr, cyan_bl, cyan_br.
A mark that is not on screen in a frame is left blank - a missing sample, never a
position (motion_metrics.py drops it rather than diffing across it).

By hue and not by fixed RGB boxes because the palette carries half-channel inks
(orange, violet, lime, pink) that sit 30 degrees apart; a saturation and value
floor keeps paper, ink and anti-aliased edges out.
"""
import argparse, csv, glob, os
import numpy as np
from PIL import Image
from scipy import ndimage

HUES = {  # degrees
    "red": 0, "orange": 30, "yellow": 60, "lime": 90, "green": 120,
    "cyan": 180, "blue": 240, "violet": 270, "magenta": 300, "pink": 330,
}
SQUARE_INKS = ["magenta", "cyan", "yellow", "orange", "blue", "violet", "lime", "pink"]
SIDE_PT = 12.0


def hsv(a):
    f = a[:, :, :3].astype(np.float32) / 255
    mx, mn = f.max(2), f.min(2)
    d = mx - mn
    r, g, b = f[:, :, 0], f[:, :, 1], f[:, :, 2]
    h = np.zeros_like(mx)
    dd = np.where(d == 0, 1, d)
    h = np.where(mx == r, ((g - b) / dd) % 6, h)
    h = np.where(mx == g, (b - r) / dd + 2, h)
    h = np.where(mx == b, (r - g) / dd + 4, h)
    s = np.where(mx == 0, 0, d / np.where(mx == 0, 1, mx))
    return h * 60, s, mx


def ink_mask(h, s, v, name, tol=13):
    dh = np.abs((h - HUES[name] + 180) % 360 - 180)
    return (dh < tol) & (s > 0.85) & (v > 0.8)


def bar_y(m, w, scale):
    """The centre row of a full-width bar: rows more than 60% covered."""
    rows = np.where(m.sum(1) > 0.6 * w)[0]
    if len(rows) == 0:
        return None
    return float(rows.mean()) / scale


CLOCK_X_PT, CLOCK_CELL_PT, CLOCK_BITS, EDGE_PT = 24.0, 12.0, 14, 4.0


def read_clock(a, red_pt, scale):
    """The clock strip under the red bar (MotionRulerClock), or None."""
    if red_pt is None:
        return None
    y = int(round((red_pt + EDGE_PT / 2 + CLOCK_CELL_PT / 2) * scale))
    if not (0 <= y < a.shape[0]):
        return None
    lum = a[y].astype(np.int16).mean(1)
    v = 0
    for i in range(CLOCK_BITS):
        x = int(round((CLOCK_X_PT + CLOCK_CELL_PT * (i + 0.5)) * scale))
        c = lum[x - 3:x + 4]
        if c.min() > 200:
            v = v * 2 + 1
        elif c.max() < 60:
            v = v * 2
        else:
            return None                                   # not a clean read
    return v


def frame_marks(full, strip_px, scale):
    # HALF RESOLUTION for the colour work: a 12pt square is still 18px across
    # at 3x, and a take is several hundred 1320x2868 frames.
    a = full[::2, ::2]
    scale, strip_px = scale / 2, strip_px // 2
    H, W = a.shape[:2]
    h, s, v = hsv(a)
    out = {"red": bar_y(ink_mask(h, s, v, "red"), W, scale),
           "green": bar_y(ink_mask(h, s, v, "green"), W, scale)}
    out["clock"] = read_clock(full, out["red"], scale * 2)
    side = SIDE_PT * scale
    lo, hi = 0.35 * side * side, 1.6 * side * side
    found = {}
    for ink in SQUARE_INKS:
        m = ink_mask(h, s, v, ink)
        m[:, :strip_px] = False
        lab, n = ndimage.label(m)
        if n == 0:
            continue
        objs = ndimage.find_objects(lab)
        areas = ndimage.sum(m, lab, range(1, n + 1))
        for i, sl in enumerate(objs):
            ys, xs = sl
            hh, ww = ys.stop - ys.start, xs.stop - xs.start
            if not (lo <= areas[i] <= hi):
                continue
            if max(hh, ww) > 1.5 * side or min(hh, ww) < 0.5 * side:
                continue
            cy, cx = ndimage.center_of_mass(m[sl])
            found.setdefault(ink, []).append(((xs.start + cx) / scale, (ys.start + cy) / scale))
    return out, found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames"); ap.add_argument("out")
    ap.add_argument("--strip-pt", type=float, default=24)
    ap.add_argument("--quads", default="cyan", help="inks split by quadrant around magenta")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.frames, "f*.png")) + glob.glob(os.path.join(a.frames, "f*.ppm")))
    times = [float(x) for x in open(os.path.join(a.frames, "times.txt")).read().split()]
    quads = set(a.quads.split(","))
    rows = []
    cols = set()
    for f, t in zip(files, times):
        img = np.asarray(Image.open(f).convert("RGB"))
        scale = 3.0 if img.shape[0] >= 2000 else 2.0
        bars, found = frame_marks(img, int(a.strip_pt * scale), scale)
        row = {"t": t, "frame": os.path.basename(f), **{k: v for k, v in bars.items()}}
        centre = found.get("magenta", [None])[0]
        for ink, pts in found.items():
            if ink in quads and centre and len(pts) > 1:
                for (x, y) in pts:
                    q = ("t" if y < centre[1] else "b") + ("l" if x < centre[0] else "r")
                    row[f"{ink}_{q}_x"], row[f"{ink}_{q}_y"] = x, y
            elif len(pts) == 1:
                row[f"{ink}_x"], row[f"{ink}_y"] = pts[0]
        cols |= set(row)
        rows.append(row)
    head = ["t", "frame", "clock", "red", "green"] + sorted(c for c in cols if c not in ("t", "frame", "clock", "red", "green"))
    with open(a.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=head)
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if r.get(k) is None else (round(r[k], 2) if isinstance(r.get(k), float) else r[k])) for k in head})
    print(f"{a.out}: {len(rows)} frames, {len(head) - 2} columns")


if __name__ == "__main__":
    main()
