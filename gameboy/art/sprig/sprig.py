#!/usr/bin/env python3
# The khokhloma back as stamps: one rowan sprig (tendril, veined leaves,
# a bunch of ringed berries) laid along a stem path.  Three layouts to pick
# from; the chosen one is baked into glyphs.BACK_FIELD (14x22, no frame).
import sys

FW, FH = 14, 22

BERRY = [          # 5x5: white body, dark grey shade, black calyx
".www.",
"wwwww",
"wwwwo",
"wwXoo",
".ooo.",
]
SMALL_BERRY = [
".ww.",
"wwww",
"wwXo",
".oo.",
]
LEAF_DIAG = [      # points up-left; the vein runs to the stalk at bottom-right
"ww....",
"wwww..",
"wwoww.",
".wwoww",
"..wwo.",
"...w..",
]
LEAF_H = [         # points left, stalk on the right
"..wwww..",
".wwwwwww",
"wooooo..",
".wwwwwww",
"..wwww..",
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


def hanging():
    f = Field()
    f.stem(tendril(6, 0) + line(7, 3, 7, 10) + line(7, 10, 4, 12) + line(7, 10, 10, 12))
    f.stamp(0, 4, LEAF_DIAG)
    f.stamp(8, 4, flip_x(LEAF_DIAG))
    for (x, y, b) in ((0, 12, BERRY), (9, 12, BERRY), (4, 11, BERRY),
                      (2, 16, BERRY), (7, 16, BERRY), (5, 20, SMALL_BERRY)):
        f.stamp(x, y, b, ring=True)
    return f.rows()


def diagonal():
    f = Field()
    f.stem(tendril(0, 0) + line(3, 3, 8, 12) + line(8, 12, 9, 14))
    f.stamp(7, 2, flip_x(LEAF_DIAG))           # leaf up-right of the stem
    f.stamp(0, 8, flip_y(LEAF_DIAG))           # leaf down-left
    f.stamp(1, 13, rot90(flip_y(LEAF_DIAG))[::1])
    for (x, y, b) in ((8, 12, BERRY), (4, 15, BERRY), (9, 17, BERRY),
                      (5, 19, SMALL_BERRY), (11, 14, SMALL_BERRY)):
        f.stamp(x, y, b, ring=True)
    return f.rows()


def arching():
    f = Field()
    arc = [(0, 9), (1, 8), (2, 7), (3, 6), (4, 6), (5, 5), (6, 5), (7, 5), (8, 6), (9, 6), (10, 7), (10, 8), (10, 9), (10, 10)]
    f.stem(tendril(11, 1, mirror=True) + [(11, 4), (11, 5), (10, 6)] + arc)
    f.stamp(0, 0, LEAF_DIAG)
    f.stamp(2, 10, flip_y(LEAF_DIAG))
    for (x, y, b) in ((8, 11, BERRY), (4, 13, BERRY), (9, 15, BERRY),
                      (5, 17, BERRY), (1, 17, SMALL_BERRY), (9, 20, SMALL_BERRY)):
        f.stamp(x, y, b, ring=True)
    return f.rows()


LAYOUTS = {"hanging": hanging, "diagonal": diagonal, "arching": arching}

if __name__ == "__main__":
    for n, fn in LAYOUTS.items():
        print(n)
        print("\n".join(fn()))
        print()
