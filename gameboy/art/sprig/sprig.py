#!/usr/bin/env python3
# The khokhloma back as stamps: one rowan sprig (tendril, veined leaves,
# a bunch of ringed berries) laid along a stem path.  Three layouts to pick
# from; the chosen one is baked into glyphs.BACK_FIELD (14x22, no frame).
import math
import sys

FW, FH = 14, 22

BERRY = [          # 5x5: white body, dark grey shaded underside, black calyx
".www.",
"wwwww",
"wwwwo",
"wwXoo",
".ooo.",
]
LEAF_DIAG = [      # 8x8, points up-left; the vein runs to the stalk at bottom-right
"www.....",
"wwwww...",
"wwowwww.",
".wwowwww",
".wwwowww",
"..wwwoww",
"...wwwo.",
".......o",
]


def flip_x(st):
    return [r[::-1] for r in st]


def flip_y(st):
    return st[::-1]


def rot90(st):     # clockwise
    return ["".join(st[len(st) - 1 - j][i] for j in range(len(st))) for i in range(len(st[0]))]


class Field:
    def __init__(self):
        self.px = [["." for _ in range(FW)] for _ in range(FH)]

    def put(self, x, y, c):
        if 0 <= x < FW and 0 <= y < FH:
            self.px[y][x] = c

    def stem(self, pts):
        self.stem_pts = getattr(self, "stem_pts", []) + list(pts)
        for (x, y) in pts:
            self.put(x, y, "w")

    def stamp(self, x, y, st, ring=False):
        cells = {(x + i, y + j) for j, r in enumerate(st) for i, ch in enumerate(r) if ch != "."}
        if ring:      # a one-pixel lacquer ring keeps each berry round and apart
            for (cx, cy) in cells:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if (cx + dx, cy + dy) not in cells:
                        self.put(cx + dx, cy + dy, ".")
        for j, r in enumerate(st):
            for i, ch in enumerate(r):
                if ch != ".":
                    self.put(x + i, y + j, ch)

    def rows(self):
        # a berry's ring may have cut the stem where it meets the bunch:
        # put back stem pixels the ring erased (never inside a berry)
        for (x, y) in getattr(self, "stem_pts", []):
            if 0 <= x < FW and 0 <= y < FH and self.px[y][x] == ".":
                self.px[y][x] = "w"
        return ["".join(r) for r in self.px]


def line(x0, y0, x1, y1):
    n = max(abs(x1 - x0), abs(y1 - y0))
    return [(round(x0 + (x1 - x0) * k / n), round(y0 + (y1 - y0) * k / n)) for k in range(n + 1)]


def tendril(cx, cy, mirror=False):
    """A curl, about 5x5, opening towards the stem."""
    pts = [(0, 2), (0, 1), (1, 0), (2, 0), (3, 1), (3, 2), (2, 3), (1, 2)]
    return [((cx - x) if mirror else (cx + x), cy + y) for (x, y) in pts]


def curl(x, y, mx=False, my=False):
    """A small Khokhloma spiral, 4x4, entered from its open corner."""
    pts = [(0, 3), (0, 2), (0, 1), (1, 0), (2, 0), (3, 1), (3, 2), (2, 3), (1, 2)]
    return [(x + (3 - px if mx else px), y + (3 - py if my else py)) for (px, py) in pts]


def hanging():
    f = Field()
    f.stem(tendril(6, 0) + line(7, 3, 7, 13) + line(7, 13, 4, 16) + line(7, 13, 9, 16))
    f.stamp(0, 3, LEAF_DIAG)
    f.stamp(6, 3, flip_x(LEAF_DIAG))
    f.stem([(6, 12), (5, 12)] + curl(1, 11, mx=True))       # curl off the stem, left
    f.stem([(8, 12), (9, 12)] + curl(10, 11))                # curl off the stem, right
    f.stamp(1, 16, BERRY, ring=True)
    f.stamp(8, 16, BERRY, ring=True)
    return f.rows()


def diagonal():
    f = Field()
    f.stem(tendril(0, 0) + line(3, 3, 9, 14))
    f.stamp(6, 1, flip_x(LEAF_DIAG))
    f.stamp(0, 6, flip_y(LEAF_DIAG))
    f.stem([(8, 11), (9, 11)] + curl(10, 10))                # curl off the stem, right
    f.stem(line(6, 15, 3, 17) + curl(0, 17, mx=True))        # curl off the stalk, bottom left
    f.stamp(8, 14, BERRY, ring=True)
    f.stamp(5, 17, BERRY, ring=True)
    return f.rows()


def arching():
    f = Field()
    arc = [(1, 10), (2, 9), (2, 8), (3, 7), (4, 6), (5, 6), (6, 5), (7, 5), (8, 5), (9, 6), (10, 7), (10, 8), (10, 9), (10, 10), (10, 11), (9, 12)]
    f.stem(tendril(12, 1, mirror=True) + [(12, 4), (11, 5), (10, 6)] + arc)
    f.stamp(0, 0, LEAF_DIAG)
    f.stamp(0, 10, flip_y(LEAF_DIAG))
    f.stem([(8, 6), (7, 7)] + curl(5, 8))                    # curl inside the arch
    f.stem(line(10, 13, 11, 17) + curl(10, 18))              # curl past the berries
    f.stamp(8, 13, BERRY, ring=True)
    f.stamp(5, 17, BERRY, ring=True)
    return f.rows()


# ---- the minimal version: one leaf, one curling vine, one small berry ----
BIG_CURL = [       # 6x6 spiral that ends the vine
"..www.",
".w...w",
"w..w.w",
"w.w..w",
"w..ww.",
".w....",
]


def hanging_min():
    f = Field()
    f.stamp(4, 0, BIG_CURL)
    f.stem([(5, 6), (6, 7)] + line(6, 7, 7, 15))
    f.stamp(0, 7, LEAF_DIAG)                        # stalk meets the vine at (7, 14)
    f.stem(line(7, 14, 7, 15))
    f.stamp(5, 16, BERRY, ring=True)
    return f.rows()


LEAF_FINE = [      # 8x8, points up-left: serrated edge, midrib and side veins
"ww.w....",
"wwwwww..",
"wwowwww.",
".wwowow.",
"wwwwowww",
".wowwow.",
"..ww.wo.",
"......wo",
]
SMALL_CURL = [".ww.", "w..w", "w.ww", ".w.."]


def diagonal_min():
    """Alex's pick (2B), with finer detail: a serrated, veined leaf; a second
    fine tendril off the vine and one off the berry stalk, both curling into
    the card's left strip so an overlapped back still shows the motif; a few
    accent dots; the berry shaded with its calyx."""
    f = Field()
    f.stamp(0, 0, BIG_CURL)
    f.stem([(1, 6), (2, 7)] + line(2, 7, 9, 15))
    f.stamp(6, 3, flip_x(LEAF_FINE))
    f.stem([(4, 10), (3, 11), (2, 12), (1, 13), (1, 14)])
    f.stamp(0, 15, flip_y(SMALL_CURL))
    f.stem([(7, 15), (6, 16), (5, 17), (5, 18), (4, 19), (3, 19), (3, 18)])
    for (x, y) in ((5, 2), (11, 12), (2, 20), (12, 9), (6, 13)):
        f.put(x, y, "o")
    f.stamp(8, 16, BERRY, ring=True)
    return f.rows()


def arching_min():
    f = Field()
    arc = [(1, 19), (1, 18), (1, 17), (2, 16), (2, 15), (3, 14), (3, 13), (4, 12), (4, 11), (5, 10),
           (6, 9), (7, 9), (8, 8), (9, 8), (10, 7)]
    f.stem(arc)
    f.stamp(8, 1, flip_x(BIG_CURL))                 # the vine ends in its curl, top right
    f.stamp(0, 2, LEAF_DIAG)
    f.stem(line(7, 9, 7, 13))
    f.stamp(5, 14, BERRY, ring=True)
    return f.rows()


# ---- 2B vine options: the leaf, the diagonal and the berry stay; only the
# vine changes.  One-pixel lines, never doubled, never touching each other.
def curve(fn, n=200):
    """Rasterise a parametric curve t in [0,1] to a clean one-pixel path."""
    pts = []
    for k in range(n + 1):
        p = tuple(int(round(v)) for v in fn(k / n))
        if not pts or p != pts[-1]:
            pts.append(p)
    out = []
    for p in pts:      # drop corner pixels so diagonal steps stay one pixel wide
        if len(out) >= 2 and abs(p[0] - out[-2][0]) <= 1 and abs(p[1] - out[-2][1]) <= 1:
            out[-1] = p
        else:
            out.append(p)
    return out


def spiral(cx, cy, r0, turns, start, sign=1):
    return lambda t: (cx + (r0 * (1 - 0.75 * t)) * math.cos(start + sign * 2 * math.pi * turns * t),
                      cy + (r0 * (1 - 0.75 * t)) * math.sin(start + sign * 2 * math.pi * turns * t))


def vine_option(name):
    f = Field()
    f.stamp(6, 3, flip_x(LEAF_FINE))                  # leaf stalk ends at (6, 10)
    if name == "a":        # one clean curl, then the vine, no branches
        f.stamp(0, 0, BIG_CURL)
        f.stem(line(2, 6, 10, 15))
    elif name == "b":      # a short straight stem with one small curl at its end
        f.stamp(2, 4, SMALL_CURL)
        f.stem(line(3, 8, 10, 15))
    elif name == "c":      # no curl: a straight stem to the berry
        f.stem(line(1, 1, 10, 15))
    elif name == "d":      # one S-shaped vine, even weight
        f.stem(curve(lambda t: (1.5 + 8.5 * t - 2.0 * math.sin(2 * math.pi * t), 1 + 14 * t)))
    elif name == "e":      # a C-shaped vine bowing into the left strip, a small curl on top
        f.stem(curve(lambda t: (10 - 4 * t - 7 * math.sin(math.pi * t), 15 - 11 * t)))
        f.stamp(3, 0, SMALL_CURL)
    f.stamp(8, 16, BERRY, ring=True)
    return f.rows()


VINE_OPTIONS = {k: (lambda k=k: vine_option(k)) for k in "abcde"}


# ---- Alex's combination: option a's curl + option d's uneven, hand-drawn
# vine; the same leaf and berry; something in the bottom-left corner.
LEAF_MINI = [      # 5x5, points down-left, stalk at the top right, a vein
"...ww",
".wwow",
"wwow.",
"wow..",
".w...",
]
BUD = [            # 3x5: a closed rowan bud with its dark tip
".w.",
"www",
"wow",
"www",
".o.",
]


def combo(corner):
    f = Field()
    f.stamp(6, 3, flip_x(LEAF_FINE))
    f.stamp(0, 0, BIG_CURL)
    # the uneven vine: a slow wobble that is not a sine, one pixel, no doubling
    f.stem(curve(lambda t: (1.8 + 8.4 * t + 1.7 * math.sin(9.5 * t + 0.3) * (1 - 0.6 * t) + 0.6 * math.sin(23 * t),
                            6 + 9 * t)))
    if corner == "leaf":          # a small second leaf on a short stalk
        f.stem([(5, 11), (4, 12), (3, 13)])
        f.stamp(0, 13, LEAF_MINI)
    elif corner == "bud":         # a bud on a curving stalk
        f.stem([(5, 11), (4, 12), (3, 13), (2, 14)])
        f.stamp(1, 15, BUD)
    elif corner == "curl":        # a tiny curl and a scatter of dots
        f.stem([(5, 11), (4, 12), (3, 13), (2, 13)] + [(1, 14), (1, 15), (2, 16), (3, 16), (3, 15)])
        for (x, y) in ((0, 18), (2, 19), (4, 18), (1, 21)):
            f.put(x, y, "o")
    f.stamp(8, 16, BERRY, ring=True)
    return f.rows()


COMBO_OPTIONS = {k: (lambda k=k: combo(k)) for k in ("leaf", "bud", "curl")}


LAYOUTS = {"hanging": hanging, "diagonal": diagonal, "arching": arching,
           "hanging_min": hanging_min, "diagonal_min": diagonal_min, "arching_min": arching_min}

if __name__ == "__main__":
    for n, fn in LAYOUTS.items():
        print(n)
        print("\n".join(fn()))
        print()
