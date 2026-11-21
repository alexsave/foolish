#!/usr/bin/env python3
# Club "three stems" variants (none live): the live wide-foot silhouette, the
# joins where the three lobes meet marked in a lighter shade so the
# three-stem structure reads.  On the card the marks are dark grey ('o') on
# the black body: dark grey is a sprite colour, so a flying card keeps them.
# Light grey is never used on the card (it is sprite colour 0: transparent).
# In the status bar the icon is white on the dark rail and the marks are
# light grey: the rail is background (or window), never a sprite, so light
# grey is safe there.  No extra tiles: the club's pip tiles are per suit.
# Run: python3 clubs/stems.py  -> mockups/23_club_stems.png
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import glyphs as G
import gen_art as A

VARIANTS = {
"1 CROSS": (           # a dark grey plus where the three lobes meet
    [".XXX.",
     ".XoX.",
     "XoooX",
     "XXoXX",
     "..X..",
     "XXXXX"],
    ["..XXX..",
     "..XoX..",
     "XX.o.XX",
     "XoooooX",
     "XX.o.XX",
     "..XXX..",
     "XXXXXXX"]),
"2 Y STEMS": (         # a Y of dark grey: the stem rising into each lobe
    [".XXX.",
     ".XoX.",
     "XXoXX",
     "XoXoX",
     "..X..",
     "XXXXX"],
    ["..XXX..",
     "..XoX..",
     "XX.o.XX",
     "XooXooX",
     "XX.o.XX",
     "..XoX..",
     "XXXXXXX"]),
"3 SEAMS": (           # dark grey seams between the lobes instead of white notches
    [".XXX.",
     ".XXX.",
     "XoXoX",
     "XXXXX",
     "..X..",
     "XXXXX"],
    ["..XXX..",
     "..XXX..",
     "XXoXoXX",
     "XXXXXXX",
     "XXoXoXX",
     "..XXX..",
     "XXXXXXX"]),
"4 SEAMS+STEM": (      # the seams and the stem's line running down from them
    [".XXX.",
     ".XXX.",
     "XoXoX",
     "XXoXX",
     "..X..",
     "XXXXX"],
    ["..XXX..",
     "..XXX..",
     "XXoXoXX",
     "XXXoXXX",
     "XXoXoXX",
     "..XoX..",
     "XXXXXXX"]),
}


def face(rank, small, big):
    keep = (G.SMALL["C"], G.PIP["C"])
    G.SMALL["C"], G.PIP["C"] = small, big
    cv = A.card_face(rank, "C")
    G.SMALL["C"], G.PIP["C"] = keep
    return cv


def rail_icon(small):
    """The status bar's club: white on the dark rail, the joins light grey."""
    return A.rail_glyph_tile(small, fg=A.W, x=1, y=1)


def sheet():
    rows = [("NOW", G.SMALL["C"], G.PIP["C"])] + [(n, s, b) for n, (s, b) in VARIANTS.items()]
    ui = A.ui_bank()
    W = 4 + 4 * 20 + 8 * 8 + 16 + 64 + 8
    cv = A.Canvas(W, 8 + len(rows) * 40, A.T)
    for k, (n, s, b) in enumerate(rows):
        y = 8 + k * 40
        x = 4
        cv.blit(face("K", s, b), x, y)                       # the club on a face card
        cv.blit(face("7", s, b), x + 20, y)
        cv.blit(A.card_face("K", "S"), x + 40, y)            # beside today's other suits
        cv.blit(A.card_face("Q", "H"), x + 60, y)
        hx = x + 84
        for i, (r, su) in enumerate((("9", "C"), ("9", "S"), ("J", "C"), ("Q", "D"), ("A", "C"), ("6", "H"))):
            cv.blit(face(r, s, b) if su == "C" else A.card_face(r, su), hx + i * 8, y)
        # the status bar: "SEOUL" ... the trump club icon at the right
        bar = A.Canvas(64, 8, A.D)
        for i, t in enumerate(A.text_tiles("{MINIDECK}24 ", ui)):
            bar.set_tile(i, 0, t)
        bar.set_tile(4, 0, rail_icon(s))
        bar.set_tile(6, 0, rail_icon(s))
        cv.blit(bar, hx + 64, y + 4)
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
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", "23_club_stems.png")
    out.save(p)
    print(os.path.normpath(p))
