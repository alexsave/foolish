#!/usr/bin/env python3
# The S2 TO CENTRE club (Alex's pick) with its seams in different tints
# (none live).  Seam pixels are 'S' in the shapes below; each row of the
# sheet paints them a different way.
#
# Sprites (a club card flying): light grey is palette index 0 in our tiles,
# which a sprite always draws transparent.  The clean fix for (b) and (d2):
# sprite copies of the club's pip tiles with the seam stored as index 2, and
# club cards drawn with OBP1 = 0xD0 (index 1 white, 2 light grey, 3 black).
# A club card has no dark grey anywhere else, so nothing else on it changes;
# OBP1 is unused today.  Cost: 4 sprite tiles (SUIT_C, PIPT_C, PIPL_C,
# PIPR_C copies) and the OBP1 slot.  (d1) mixes dark and light grey with
# black and white: four opaque colours, one more than a sprite has, so a
# flying (d1) card would lose one of the two greys.
# Run: python3 clubs/tints.py -> mockups/25_club_s2_tints.png
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import glyphs as G
import gen_art as A

S2_SMALL = [".XXX.",
            ".XXX.",
            "XSXSX",
            "XXSXX",
            "..X..",
            "XXXXX"]
S2_BIG = ["..XXX..",
          "..XXX..",
          "XXSXSXX",
          "XXXSXXX",
          "XXSXSXX",
          "..XXX..",
          "XXXXXXX"]

# name -> how a seam pixel at (x, y) is painted on the card
TINTS = {
    "A DARK GREY": lambda x, y: "o",
    "B LIGHT GREY": lambda x, y: "t",
    "C WHITE": lambda x, y: "w",
    "D1 DARK+LIGHT": lambda x, y: "o" if (x + y) % 2 == 0 else "t",
    "D2 LIGHT+WHITE": lambda x, y: "t" if (x + y) % 2 == 0 else "w",
}


def paint(rows, tint):
    return ["".join(tint(x, y) if ch == "S" else ch for x, ch in enumerate(r)) for y, r in enumerate(rows)]


def face(rank, small, big):
    keep = (G.SMALL["C"], G.PIP["C"])
    G.SMALL["C"], G.PIP["C"] = small, big
    cv = A.card_face(rank, "C")
    G.SMALL["C"], G.PIP["C"] = keep
    return cv


def rail_icon(small):
    """The status bar's club, white on the dark rail; the seams in their tint
    as is (so dark grey shows as a notch of the rail's own colour, white
    disappears into the white icon)."""
    cv = A.Canvas(8, 8, A.D)
    m = {"X": A.W, "o": A.D, "t": A.T, "w": A.W}
    for j, r in enumerate(small):
        for i, ch in enumerate(r):
            if ch in m:
                cv.put(1 + i, 1 + j, m[ch])
    return cv.tile(0, 0)


def as_sprite(cv, naive):
    """The card as a flying sprite: index 0 (light grey) is transparent. With
    naive=False the seams use the OBP1 copies, so light grey stays opaque."""
    out = A.Canvas(cv.w, cv.h, None)
    for y in range(cv.h):
        for x in range(cv.w):
            v = cv.get(x, y)
            out.px[y][x] = None if (v is None or (v == A.T and naive)) else v
    return out


def sheet():
    ui = A.ui_bank()
    rows = [("NOW", G.SMALL["C"], G.PIP["C"]), ("S2 DARK", paint(S2_SMALL, TINTS["A DARK GREY"]), paint(S2_BIG, TINTS["A DARK GREY"]))]
    rows += [(n, paint(S2_SMALL, t), paint(S2_BIG, t)) for n, t in TINTS.items()]
    W = 4 + 4 * 20 + 8 * 8 + 16 + 64 + 8 + 48
    cv = A.Canvas(W, 8 + len(rows) * 40, A.T)
    for k, (n, s, b) in enumerate(rows):
        y = 8 + k * 40
        x = 4
        cv.blit(face("K", s, b), x, y)
        cv.blit(face("7", s, b), x + 20, y)
        cv.blit(A.card_face("K", "S"), x + 40, y)
        cv.blit(A.card_face("Q", "H"), x + 60, y)
        hx = x + 84
        for i, (r, su) in enumerate((("9", "C"), ("9", "S"), ("J", "C"), ("Q", "D"), ("A", "C"), ("6", "H"))):
            cv.blit(face(r, s, b) if su == "C" else A.card_face(r, su), hx + i * 8, y)
        bar = A.Canvas(64, 8, A.D)
        for i, t in enumerate(A.text_tiles("{MINIDECK}24 ", ui)):
            bar.set_tile(i, 0, t)
        bar.set_tile(4, 0, rail_icon(s))
        bar.set_tile(6, 0, rail_icon(s))
        cv.blit(bar, hx + 64, y + 4)
        if n == "B LIGHT GREY":
            # the flying card over a white card: with OBP1 (left), and naive (right: holes)
            sx = hx + 64 + 68
            cv.blit(A.card_face("8", "S"), sx, y + 14)
            cv.blit(as_sprite(face("K", s, b), naive=False), sx + 6, y + 6)
            cv.blit(A.card_face("8", "S"), sx + 32, y + 14)
            cv.blit(as_sprite(face("K", s, b), naive=True), sx + 38, y + 6)
        for i, t in enumerate(A.text_tiles(n, ui)):
            tc = A.Canvas(8, 8, None)
            tc.set_tile(0, 0, t)
            cv.blit(tc, x + 8 * i, y + 28)
    return cv


if __name__ == "__main__":
    from PIL import Image
    cv = sheet()
    big = cv.image(4)
    one = cv.image(1)
    out = Image.new("RGB", (big.width + 24 + one.width, big.height), (40, 40, 40))
    out.paste(big, (0, 0))
    out.paste(one, (big.width + 12, 12))
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", "25_club_s2_tints.png")
    out.save(p)
    print(os.path.normpath(p))
