#!/usr/bin/env python3
# Suit shading variants (none live): several greys inside each pip, derived
# by rule from the live shapes (glyphs.SMALL, glyphs.PIP; the club is the
# wide foot).  Only black, dark grey and white are used inside a pip: light
# grey is palette index 0, which a sprite draws as transparent, so a flying
# card would show holes; with these three every variant is sprite-safe.
# No extra tiles: each pip tile is already per suit.
# Run: python3 suits/shading.py  -> mockups/21_suit_shading.png
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import glyphs as G
import gen_art as A

K, D, W = "X", "o", "w"


def mask(rows):
    return {(x, y) for y, r in enumerate(rows) for x, ch in enumerate(r) if ch != "."}


def render(m, w, h, colour):
    rows = [["."] * w for _ in range(h)]
    for (x, y) in m:
        rows[y][x] = colour(x, y)
    return ["".join(r) for r in rows]


def edge(m, x, y):
    return any((x + dx, y + dy) not in m for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))


def glint(m):
    """The interior pixel nearest the top-left: where a highlight sits."""
    inner = [p for p in m if not edge(m, *p)] or list(m)
    return min(inner, key=lambda p: (p[0] + p[1], p[1]))


def shade(rows, suit, style, big):
    m = mask(rows)
    w, h = len(rows[0]), len(rows)
    red = G.RED[suit]
    g = glint(m)
    cx = (w - 1) / 2

    def colour(x, y):
        e = edge(m, x, y)
        if style == "A":                      # shaded 3D: light top-left, shadow bottom-right
            if big and (x, y) == g:
                return W
            if e and (x - cx) + (y - h / 2) > 1:
                return K
            return D if red else (D if (big and not e and (x, y) in ((g[0] + 1, g[1]), (g[0], g[1] + 1))) else K)
        if style == "B":                      # outline + fill: red suits hollow-looking, black solid
            if red:
                return K if e else D
            return K
        if style == "C":                      # an inner facet per suit
            if suit == "D":
                return W if (not e and abs(x - cx) < 1 and big) or (not big and (x, y) == (2, 2)) else D
            if suit == "H":
                return W if (x, y) == g and big else D
            if suit == "S":
                return D if (not e and x == round(cx) and big) else K
            if suit == "C":
                return D if (not e and big and y in (3,) and x in (1, 5)) or (big and (x, y) == (3, 1)) else K
        if style == "D":                      # highlight only
            if big and (x, y) == g:
                return W
            return D if red else K
        if style == "E":                      # mine: red outlined, filled, glinting; black solid with a sheen
            if red:
                if big and (x, y) == g:
                    return W
                return K if e else D
            if big and not e and (x, y) == g:
                return D
            return K
    return render(m, w, h, colour)


VARIANTS = {"A SHADED": "A", "B OUTLINE": "B", "C FACET": "C", "D HIGHLIGHT": "D", "E MINE": "E"}


def face(rank, suit, style):
    cv = A.Canvas(A.CW, A.CH, None)
    A.card_outline(cv, A.W)
    cv.glyph(2, 2, G.RANK[rank], A.K)
    cv.glyph(2, 9, shade(G.SMALL[suit], suit, style, False), A.K)
    cv.glyph(A.PIP_X, A.PIP_Y, shade(G.PIP[suit], suit, style, True), A.K)
    return cv


def sheet():
    names = list(VARIANTS)
    hand = (("6", "H"), ("9", "D"), ("K", "S"), ("A", "C"), ("7", "S"), ("8", "H"), ("J", "C"), ("Q", "D"))
    col = 4 * 20 + 8 + 8 * 8 + 12
    cv = A.Canvas(8 + col, 8 + (len(names) + 1) * 36, A.T)
    ui = A.ui_bank()
    rows = [("NOW", None)] + [(n, VARIANTS[n]) for n in names]
    for k, (n, style) in enumerate(rows):
        y = 8 + k * 36
        for i, (r, s) in enumerate((("6", "H"), ("9", "D"), ("K", "S"), ("A", "C"))):
            cv.blit(A.card_face(r, s) if style is None else face(r, s, style), 4 + i * 20, y)
        for i, (r, s) in enumerate(hand):
            cv.blit(A.card_face(r, s) if style is None else face(r, s, style), 4 + 88 + i * 8, y)
        for i, t in enumerate(A.text_tiles(n, ui)):
            tc = A.Canvas(8, 8, None)
            tc.set_tile(0, 0, t)
            cv.blit(tc, 4 + 8 * i, y + 26)
    return cv


if __name__ == "__main__":
    from PIL import Image
    cv = sheet()
    big = cv.image(4)
    one = cv.image(1)
    out = Image.new("RGB", (big.width + 24 + one.width, big.height), (40, 40, 40))
    out.paste(big, (0, 0))
    out.paste(one, (big.width + 12, 12))
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", "21_suit_shading.png")
    out.save(p)
    print(os.path.normpath(p))
