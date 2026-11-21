#!/usr/bin/env python3
# The khokhloma back as stamps: one rowan sprig (tendril, veined leaves,
# a bunch of ringed berries) laid along a stem path.  Three layouts to pick
# from; the chosen one is baked into glyphs.BACK_FIELD (14x22, no frame).
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


LAYOUTS = {"hanging": hanging, "diagonal": diagonal, "arching": arching}

if __name__ == "__main__":
    for n, fn in LAYOUTS.items():
        print(n)
        print("\n".join(fn()))
        print()
