#!/usr/bin/env python3
"""The ruler's SQUARES, found in one frame. Shared by tween.py and tablesquares.py.

Under `dev.ruler` the live board draws 12pt squares (CollapseRuler.swift):

    cyan / yellow / green   the centre of each table pair, by pair index mod 3
    magenta                 the centre of the first opponent's card view

They replaced two full-width bars (magenta through the table, yellow through the
opponent) that ran straight through the pairs' squares. Owner: "I think the
lines are messing with the squares. Have one or the other not both. Or maybe
turn the lines into squares themselves."

    squares_in(a) -> [(colour, x_pt, y_pt)]     screen points

A GRID, NOT A SCAN. Owner: "scan strides of 20 px in a grid" and then "you might
as well use 30 pixel strides" for a 36px square. The grid is classified in one
vectorised pass; only the neighbourhood of a hit is looked at closely.
"""
import numpy as np
from scipy import ndimage

SIDE_PT = 12.0            # CollapseRuler.squareSide
STRIDE_PT = 10.0          # 30px at 3x: under the square's 36px, so every square is hit
# The ruler's band strip down the left edge is cyan/magenta/yellow cells of
# about a square's size; nothing on the board is marked that close to the edge.
STRIP_PX = 80
TABLE = ("cyan", "yellow", "green")
OPPONENT = "magenta"


def mask(win, name):
    r, g, b = (win[:, :, i].astype(np.int16) for i in range(3))
    hi, lo = 140, 90
    if name == "cyan":
        return (g > hi) & (b > hi) & (r < lo)
    if name == "yellow":
        return (r > hi) & (g > hi) & (b < lo)
    if name == "green":
        return (g > hi) & (r < lo) & (b < lo)
    return (r > hi) & (b > hi) & (g < lo)            # magenta


def squares_in(a):
    """Every whole square in one frame, as (colour, x_pt, y_pt)."""
    h, w = a.shape[0], a.shape[1]
    s = 3 if h >= 2000 else 2
    side = SIDE_PT * s
    reach = int(2 * side)
    stride = int(round(STRIDE_PT * s))
    ys_ = np.arange(stride // 2, h, stride)
    xs_ = np.arange(STRIP_PX, w, stride)
    if not len(xs_):
        return []
    grid = a[ys_][:, xs_]
    hits = []
    for name in TABLE + (OPPONENT,):
        gy, gx = np.nonzero(mask(grid, name))
        hits += [(name, int(xs_[j]), int(ys_[i])) for i, j in zip(gy, gx)]
    out, done = [], []
    for name, x, y in hits:
        if any(n == name and x0 <= x <= x1 and y0 <= y <= y1 for n, x0, y0, x1, y1 in done):
            continue
        wx0, wy0 = max(STRIP_PX, x - reach), max(0, y - reach)
        m = mask(a[wy0:min(h, y + reach), wx0:min(w, x + reach)], name)
        lab, _ = ndimage.label(m)
        k = lab[y - wy0, x - wx0]
        if k == 0:
            continue
        ys, xs = np.nonzero(lab == k)
        x0, x1, y0, y1 = xs.min() + wx0, xs.max() + wx0, ys.min() + wy0, ys.max() + wy0
        done.append((name, x0, y0, x1, y1))
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        # Square-sized and square-shaped, or it is something else (a glyph, a
        # square half under a flying card, one cut by the window).
        if not (0.75 * side <= bw <= 1.25 * side and 0.75 * side <= bh <= 1.25 * side):
            continue
        if len(xs) < 0.7 * side * side:
            continue
        out.append((name, round((x0 + x1) / 2.0 / s, 2), round((y0 + y1) / 2.0 / s, 2)))
    return out
