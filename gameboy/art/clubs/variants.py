#!/usr/bin/env python3
# Club variants for Alex to pick from (none live): each a small pip (5 wide,
# up to 6 tall, under the rank) and a big pip (7x7, low on the card), in the
# same boxes as now.  'X' black, 'w' white (a highlight).
# Run: python3 clubs/variants.py   -> mockups/16_club_variants.png
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import glyphs as G
import gen_art as A

VARIANTS = {
"1 TREFOIL": (           # three notched lobes, gap-free, a flared stem
    ["..X..",
     ".XXX.",
     "X.X.X",
     "XXXXX",
     "X.X.X",
     ".XXX."],
    ["..XXX..",
     ".XXXXX.",
     "X.XXX.X",
     "XXXXXXX",
     "XXXXXXX",
     "X..X..X",
     ".XXXXX."]),
"2 BLOB": (              # heavy, like the classic pixel fonts
    [".XXX.",
     ".XXX.",
     "XXXXX",
     "XXXXX",
     "..X..",
     ".XXX."],
    ["..XXX..",
     "..XXX..",
     "XX.X.XX",
     "XXXXXXX",
     "XX.X.XX",
     "...X...",
     "..XXX.."]),
"3 SHINE": (             # round lobes, each with a one-pixel highlight
    [".XXX.",
     ".XwX.",
     "XXXXX",
     "XwXwX",
     "..X..",
     ".XXX."],
    ["..XXX..",
     "..XwX..",
     "XXXXXXX",
     "XwXXXwX",
     "XXX.XXX",
     "...X...",
     "..XXX.."]),
"4 NARROW": (            # a slimmer body that never looks like a spade
    ["..X..",
     ".XXX.",
     "..X..",
     "XX.XX",
     "XXXXX",
     "..X.."],
    ["...X...",
     "..XXX..",
     "...X...",
     ".XX.XX.",
     "XXXXXXX",
     ".XX.XX.",
     "..XXX.."]),
"5 CP437": (             # the PC's club glyph, fitted to the boxes
    ["..X..",
     ".XXX.",
     "..X..",
     "XXXXX",
     "XXXXX",
     "..X.."],
    ["..XXX..",
     ".XXXXX.",
     "..XXX..",
     "XXXXXXX",
     "XXXXXXX",
     "XX.X.XX",
     "..XXX.."]),
"6 WIDE FOOT": (         # my pick: the solid club with a wide flared foot,
    [".XXX.",            #   the one thing a spade never has
     ".XXX.",
     "XXXXX",
     "XXXXX",
     "..X..",
     "XXXXX"],
    ["..XXX..",
     "..XXX..",
     "XX.X.XX",
     "XXXXXXX",
     "XX.X.XX",
     "..XXX..",
     "XXXXXXX"]),
}


def card(rank, suit, small=None, big=None):
    keep = (G.SMALL["C"], G.PIP["C"])
    if small is not None:
        G.SMALL["C"], G.PIP["C"] = small, big
    cv = A.card_face(rank, suit)
    G.SMALL["C"], G.PIP["C"] = keep
    return cv


def sheet():
    names = list(VARIANTS)
    col = 16 * 2 + 4 + 16 + 8 + 5 * 8 + 12           # K club, 7 club, K spade, a little hand
    cv = A.Canvas(8 + col, 8 + len(names) * 36, A.T)
    ui = A.ui_bank()
    for k, n in enumerate(names):
        s, b = VARIANTS[n]
        y = 8 + k * 36
        x = 4
        cv.blit(card("K", "C", s, b), x, y)
        cv.blit(card("7", "C", s, b), x + 20, y)
        cv.blit(card("K", "S"), x + 40, y)                 # the spade beside it
        hx = x + 64                                         # an overlapped hand: club, spade, club...
        for i, (r, su) in enumerate((("9", "C"), ("9", "S"), ("J", "C"), ("Q", "S"), ("A", "C"))):
            cv.blit(card(r, su, s, b) if su == "C" else card(r, su), hx + i * 8, y)
        for i, t in enumerate(A.text_tiles(n, ui)):
            tc = A.Canvas(8, 8, None)
            tc.set_tile(0, 0, t)
            cv.blit(tc, x + 8 * i, y + 26)
    return cv


if __name__ == "__main__":
    from PIL import Image
    cv = sheet()
    big = cv.image(4)
    one = cv.image(1)
    out = Image.new("RGB", (big.width + 24 + one.width, big.height), (40, 40, 40))
    out.paste(big, (0, 0))
    out.paste(one, (big.width + 12, 12))        # true size, as on the Game Boy
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", "16_club_variants.png")
    out.save(p)
    print(os.path.normpath(p))
