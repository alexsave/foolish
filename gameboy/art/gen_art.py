#!/usr/bin/env python3
# Durak (foolish) for the original Game Boy: the art generator.
#
# One-off art tooling (Python is fine here per the languages table): it draws
# every tile from the ASCII shapes in glyphs.py, writes the tile banks as
# indexed 4-shade PNGs for GBDK's png2asset, composes every screen mockup
# from those same tiles, and checks the hardware limits (unique tiles per
# screen, sprites per scanline).
#
# Palette indices (the same for the background and for OBP0):
#   0 = table   light grey   (sprite colour 0: transparent)
#   1 = paper   white
#   2 = ink2    dark grey    (red suits, the card back field)
#   3 = ink     black
# BGP = OBP0 = 0xE1 maps them to shades (index0->1, index1->0, index2->2,
# index3->3).  Card corners are index 0, so the same card tiles drawn as
# sprites get round corners for free.
#
# Run: python3 gen_art.py            (writes png/, mockups/)

import os
import sys
from PIL import Image

import glyphs as G

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_PNG = os.path.join(HERE, "png")
OUT_MOCK = os.path.join(HERE, "mockups")

T, W, D, K = 0, 1, 2, 3          # table, white, dark grey, black
SHADE = {T: (170, 170, 170), W: (255, 255, 255), D: (85, 85, 85), K: (0, 0, 0)}
SCALE = 4
CHAR = {"X": K, "o": D, "w": W}


# --------------------------------------------------------------------------
# Canvas: pixel indices, None = nothing drawn (transparent)
# --------------------------------------------------------------------------
class Canvas:
    def __init__(self, w, h, fill=T):
        self.w, self.h = w, h
        self.px = [[fill] * w for _ in range(h)]

    def put(self, x, y, c):
        if c is not None and 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def get(self, x, y):
        return self.px[y][x]

    def rect(self, x, y, w, h, c):
        for j in range(h):
            for i in range(w):
                self.put(x + i, y + j, c)

    def glyph(self, x, y, rows, color=K):
        for j, r in enumerate(rows):
            for i, ch in enumerate(r):
                if ch == "X":
                    self.put(x + i, y + j, color)
                elif ch in CHAR:
                    self.put(x + i, y + j, CHAR[ch])

    def blit(self, other, x, y):
        for j in range(other.h):
            for i in range(other.w):
                v = other.px[j][i]
                if v is not None:
                    self.put(x + i, y + j, v)

    def tile(self, tx, ty):
        return tuple(self.px[ty * 8 + j][tx * 8 + i] for j in range(8) for i in range(8))

    def image(self, scale=SCALE):
        im = Image.new("RGB", (self.w, self.h))
        im.putdata([SHADE[v if v is not None else T] for row in self.px for v in row])
        if scale != 1:
            im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        return im


def tiles_of(cv):
    return [cv.tile(tx, ty) for ty in range(cv.h // 8) for tx in range(cv.w // 8)]


# --------------------------------------------------------------------------
# Cards (16x24 = 2x3 tiles)
# --------------------------------------------------------------------------
CW, CH = 16, 24


def card_outline(cv, fill):
    for y in range(CH):
        for x in range(CW):
            corner = (x in (0, CW - 1)) and (y in (0, CH - 1))
            if corner:
                cv.px[y][x] = None
            elif x in (0, CW - 1) or y in (0, CH - 1):
                cv.px[y][x] = K
            else:
                cv.px[y][x] = fill


def card_face(rank, suit):
    cv = Canvas(CW, CH, None)
    card_outline(cv, W)
    ink = D if G.RED[suit] else K
    cv.glyph(2, 2, G.RANK[rank], K)
    cv.glyph(2, 9, G.SMALL[suit], ink)
    cv.glyph(8, 9, G.PIP[suit], ink)
    return cv


def card_back():
    cv = Canvas(CW, CH, None)
    card_outline(cv, D)
    # inner frame (the website's gold border), inset 2px
    for x in range(2, CW - 2):
        cv.put(x, 2, W)
        cv.put(x, CH - 3, W)
    for y in range(2, CH - 2):
        cv.put(2, y, W)
        cv.put(CW - 3, y, W)
    cv.glyph(3, 7, G.STAR)
    return cv


def pass_slot():
    """The empty 'pass' space target mode points at: a dashed outline."""
    cv = Canvas(CW, CH, None)
    for x in range(1, CW - 1):
        if (x // 2) % 2 == 0:
            cv.put(x, 0, D)
            cv.put(x, CH - 1, D)
    for y in range(1, CH - 1):
        if (y // 2) % 2 == 0:
            cv.put(0, y, D)
            cv.put(CW - 1, y, D)
    return cv


def check_pip_corners():
    """A card corner laid over a pip tile must never hide ink: card pixels
    (8,8) and (8,15) are where a cover card's and a raised card's corner
    land, and both must be paper on every face."""
    for s in G.SUITS:
        for r in G.RANKS:
            cv = card_face(r, s)
            for (x, y) in ((8, 8), (8, 15)):
                assert cv.get(x, y) == W, (r, s, x, y)


# --------------------------------------------------------------------------
# Deliverable 1: the card sheet
# --------------------------------------------------------------------------
def sheet_cards():
    pad = 4
    cols, rows = 9, 4
    w = pad + cols * (CW + pad)
    h = pad + rows * (CH + pad) + 8 + CH + 16 + pad
    w = max(w, 8 * 24)
    cv = Canvas(w, h, T)
    for j, s in enumerate(G.SUITS):
        for i, r in enumerate(G.RANKS):
            cv.blit(card_face(r, s), pad + i * (CW + pad), pad + j * (CH + pad))
    y0 = pad + rows * (CH + pad) + 8
    x = pad
    # back
    cv.blit(card_back(), x, y0)
    x += CW + pad * 2
    # a little hand: 5 cards overlapped one column each, the 3rd raised
    # (selected), the 4th under the cursor underline
    hand = [("6", "S"), ("9", "H"), ("Q", "C"), ("A", "D"), ("10", "H")]
    for i, (r, s) in enumerate(hand):
        dy = -8 if i == 2 else 0
        cv.blit(card_face(r, s), x + i * 8, y0 + 8 + dy)
    # cursor underline under card 4's visible column
    ux = x + 3 * 8
    for i in range(1, 7):
        cv.put(ux + i, y0 + 8 + CH + 2, K)
        cv.put(ux + i, y0 + 8 + CH + 3, K)
    x += 6 * 8 + 16
    # pass slot
    cv.blit(pass_slot(), x, y0)
    return cv


def show(path):
    os.system("osascript -e 'tell application \"Preview\" to close every window' >/dev/null 2>&1")
    os.system("open '%s'" % path)


def main():
    os.makedirs(OUT_PNG, exist_ok=True)
    os.makedirs(OUT_MOCK, exist_ok=True)
    check_pip_corners()
    cv = sheet_cards()
    p = os.path.join(OUT_MOCK, "01_cards_sheet.png")
    cv.image().save(p)
    print(p)


if __name__ == "__main__":
    main()
