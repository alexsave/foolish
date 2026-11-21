#!/usr/bin/env python3
# Durak (foolish) for the original Game Boy: the art generator.
#
# One-off art tooling (Python is fine here per the languages table): it draws
# every tile from the ASCII shapes in glyphs.py, writes the tile banks as
# indexed 4-shade PNGs for GBDK's png2asset, composes every screen mockup
# from those same tiles, and checks the hardware limits (every screen tile
# exists in a bank, unique tiles per screen, sprites per scanline and total).
#
# Palette indices (the same for the background and for OBP0):
#   0 = table   light grey   (sprite colour 0: transparent)
#   1 = paper   white
#   2 = gold    dark grey    (red suits, the khokhloma gold, the rails)
#   3 = ink     black
# BGP = OBP0 = 0xE1 maps them to shades (index0->1, index1->0, index2->2,
# index3->3).  Card corners are index 0, so the same card tiles drawn as
# sprites get round corners for free.
#
# Run: python3 gen_art.py     (writes png/ and mockups/)

import math
import os
from PIL import Image

import glyphs as G

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_PNG = os.path.join(HERE, "png")
OUT_MOCK = os.path.join(HERE, "mockups")

T, W, D, K = 0, 1, 2, 3          # table, white, dark grey, black
SHADE = {T: (170, 170, 170), W: (255, 255, 255), D: (85, 85, 85), K: (0, 0, 0)}
SCALE = 4
CHAR = {"X": K, "#": K, "o": D, "w": W, "t": T}


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

    def set_tile(self, tx, ty, t):
        for j in range(8):
            for i in range(8):
                self.px[ty * 8 + j][tx * 8 + i] = t[j * 8 + i]

    def image(self, scale=SCALE):
        im = Image.new("RGB", (self.w, self.h))
        im.putdata([SHADE[v if v is not None else T] for row in self.px for v in row])
        if scale != 1:
            im = im.resize((self.w * scale, self.h * scale), Image.NEAREST)
        return im


def flip(t, fx, fy):
    out = []
    for j in range(8):
        for i in range(8):
            out.append(t[(7 - j if fy else j) * 8 + (7 - i if fx else i)])
    return tuple(out)


# --------------------------------------------------------------------------
# Cards (16x24 = 2x3 tiles)
#   tile (0,0) rank, with the top-left round corner
#   tile (1,0) top right, blank
#   tile (0,1) small suit (and the pip's top row, left part)
#   tile (1,1) the pip's top row, right part
#   tile (0,2) the pip's left part, with the bottom-left round corner
#   tile (1,2) the pip's right part
# --------------------------------------------------------------------------
CW, CH = 16, 24
PIP_X, PIP_Y = 5, 15


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
    cv.glyph(PIP_X, PIP_Y, G.PIP[suit], ink)
    return cv


def card_back():
    cv = Canvas(CW, CH, None)
    card_outline(cv, D)            # the gold frame is the fill's outer ring
    cv.rect(2, 2, CW - 4, CH - 4, K)
    cv.glyph(2, 2, G.BACK_FIELD)
    return cv


def pass_slot():
    """The empty 'pass' space target mode points at: a dashed outline."""
    cv = Canvas(CW, CH, None)
    for x in range(1, CW - 1):
        if (x // 2) % 2 == 1:
            cv.put(x, 0, K)
            cv.put(x, CH - 1, K)
    for y in range(1, CH - 1):
        if (y // 2) % 2 == 1:
            cv.put(0, y, K)
            cv.put(CW - 1, y, K)
    return cv


# --------------------------------------------------------------------------
# Table: wool weave (the website's wool background), 1 tile repeat.
# Its four corner pixels are index 0 so a card's round corner over the
# wool is the plain variant wherever the card sits.
# --------------------------------------------------------------------------
WOOL = G.g("""
........
.w.w....
..w.....
........
........
.....w.w
......w.
........
""")


def wool_tile():
    cv = Canvas(8, 8, T)
    cv.glyph(0, 0, WOOL)
    t = cv.tile(0, 0)
    assert t[0] == t[7] == t[56] == t[63] == T
    return t


# --------------------------------------------------------------------------
# UI font on the rail (white on dark grey)
# --------------------------------------------------------------------------
FONT_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,!?-:'/+"
ICONS = ["UP", "DOWN", "LEFT", "RIGHT", "BTN_A", "BTN_B", "POINT",
         "MINIBACK", "MINIDECK", "TIMES", "S", "H", "C", "D"]


def rail_glyph_tile(rows, fg=W, bg=D, x=1, y=1):
    cv = Canvas(8, 8, bg)
    for j, r in enumerate(rows):
        for i, ch in enumerate(r):
            if ch == "X":
                cv.put(x + i, y + j, fg)
            elif ch == "o":
                cv.put(x + i, y + j, T)   # second tone on the rail: light grey
            elif ch == "w":
                cv.put(x + i, y + j, bg)  # cut-outs show the rail
    return cv.tile(0, 0)


def ui_bank():
    """name -> tile, in export order."""
    bank = {}
    bank["WOOL"] = wool_tile()
    for ch in FONT_CHARS:
        if ch == " ":
            bank["CH_ "] = tuple([D] * 64)
        else:
            bank["CH_" + ch] = rail_glyph_tile(G.FONT[ch])
    for name in ICONS:
        if name in G.SUITS:
            fg = T if G.RED[name] else W
            bank["IC_" + name] = rail_glyph_tile(G.SMALL[name], fg=fg, x=1, y=1)
        else:
            bank["IC_" + name] = rail_glyph_tile(G.ICON[name], x=0, y=0)
    return bank


# --------------------------------------------------------------------------
# Card bank: every card tile and its round-corner variants.
#   A round-corner pixel shows whatever is beneath it.  Beneath is always
#   one of: the wool (0), a neighbour card's white middle (1), a red pip (2)
#   or a neighbour card's black border or black pip (3).
# --------------------------------------------------------------------------
TL_UNDER = (T, K, W)          # what a top-left corner can land on
BL_UNDER = (T, K, W, D)       # what a bottom-left corner can land on


def fill_none(t, c):
    return tuple(c if v is None else v for v in t)


def card_bank():
    bank = {}
    faces = {(r, s): card_face(r, s) for r in G.RANKS for s in G.SUITS}
    ref = faces[("6", "S")]
    for c in TL_UNDER:
        for r in G.RANKS:
            bank["RANK_%s_u%d" % (r, c)] = fill_none(faces[(r, "S")].tile(0, 0), c)
    bank["FACE_TR"] = fill_none(ref.tile(1, 0), T)
    for s in G.SUITS:
        bank["SUIT_" + s] = faces[("6", s)].tile(0, 1)
    for s in G.SUITS:
        bank["PIPT_" + s] = faces[("6", s)].tile(1, 1)
    for s in G.SUITS:
        bank["PIPR_" + s] = fill_none(faces[("6", s)].tile(1, 2), T)
    for s in G.SUITS:
        for c in BL_UNDER:
            bank["PIPL_%s_u%d" % (s, c)] = fill_none(faces[("6", s)].tile(0, 2), c)
    b = card_back()
    for c in (T, K):
        bank["BACK_TL_u%d" % c] = fill_none(b.tile(0, 0), c)
    bank["BACK_TR"] = fill_none(b.tile(1, 0), T)
    bank["BACK_ML"] = b.tile(0, 1)
    bank["BACK_MR"] = b.tile(1, 1)
    for c in (T, K):
        bank["BACK_BL_u%d" % c] = fill_none(b.tile(0, 2), c)
    bank["BACK_BR"] = fill_none(b.tile(1, 2), T)
    p = pass_slot()
    wool = wool_tile()
    for name, (tx, ty) in (("TL", (0, 0)), ("TR", (1, 0)), ("ML", (0, 1)),
                           ("MR", (1, 1)), ("BL", (0, 2)), ("BR", (1, 2))):
        t = p.tile(tx, ty)
        bank["PASS_" + name] = tuple(wool[k] if v is None else v for k, v in enumerate(t))
    # a face is rank tile + five tiles that depend on the suit only
    for (r, s), cv in faces.items():
        for (tx, ty) in ((1, 0), (0, 1), (1, 1), (0, 2), (1, 2)):
            assert cv.tile(tx, ty) == faces[("6", s)].tile(tx, ty), (r, s, tx, ty)
    return bank


# --------------------------------------------------------------------------
# Sprite bank (OBP0; colour 0 transparent)
# --------------------------------------------------------------------------
SPRITES = {
# the cursor underline: the website's near-black bar with a white halo
"UL_ONE": """
.wwwwww.
wXXXXXXw
wXXXXXXw
.wwwwww.
........
........
........
........""",
"UL_LEFT": """
..wwwwww
.wXXXXXX
.wXXXXXX
..wwwwww
........
........
........
........""",
"UL_RIGHT": """
wwwwww..
XXXXXXw.
XXXXXXw.
wwwwww..
........
........
........
........""",
# one dot of the cover/pass arrow's line
"DOT": """
.ww.....
wXXw....
wXXw....
.ww.....
........
........
........
........""",
# arrow heads: N; flip Y for S.  NE; flip X for NW.
"HEAD_N": """
...ww...
..wXXw..
.wXXXXw.
wXXXXXXw
wwwXXwww
..wXXw..
..wwww..
........""",
"HEAD_NE": """
..wwwww.
..wXXXXw
...wXXXw
..wXXXXw
.wXXXwXw
wXXXw.ww
wXXw....
.ww.....""",
# target bracket, top-left corner; flips give the other three
"BRACKET": """
wwwwww..
wXXXXXw.
wXXXXXw.
wXXwww..
wXXw....
wXXw....
.ww.....
........""",
# rearrange hint beside the cursor (SELECT held): left arrow, flip X for right
"NUDGE": """
........
...w....
..wXw...
.wXXw...
wXXXw...
.wXXw...
..wXw...
...w....""",
# menu pointer on the dashboard
"POINTER": """
XX......
XwX.....
XwwX....
XwwwX...
XwwwwX..
XwwwX...
XwwX....
XwX.....""",
}


def sprite_bank():
    bank = {}
    for name, art in SPRITES.items():
        cv = Canvas(8, 8, T)
        cv.glyph(0, 0, G.g(art))
        bank["SP_" + name] = cv.tile(0, 0)
    return bank


# --------------------------------------------------------------------------
# Screens
# --------------------------------------------------------------------------
class Screen:
    def __init__(self, name, title):
        self.name, self.title = name, title
        self.bg = Canvas(160, 144, None)
        self.sprites = []          # (x, y, tile): screen pixel of the tile's top-left
        self.notes = []

    def fill_tile(self, tx, ty, t):
        self.bg.set_tile(tx, ty, t)

    def spr(self, x, y, t, fx=False, fy=False):
        self.sprites.append((x, y, flip(t, fx, fy)))

    def compose(self):
        cv = Canvas(160, 144, T)
        cv.blit(self.bg, 0, 0)
        for (x, y, t) in self.sprites:
            for j in range(8):
                for i in range(8):
                    v = t[j * 8 + i]
                    if v not in (None, T):
                        cv.put(x + i, y + j, v)
        return cv

    def check(self, banks):
        lookup = {}
        for bname, bank in banks.items():
            for k, t in bank.items():
                lookup.setdefault(t, (bname, k))
        used, missing = set(), []
        for ty in range(18):
            for tx in range(20):
                t = self.bg.tile(tx, ty)
                if t in lookup:
                    used.add(t)
                else:
                    missing.append((tx, ty))
        assert not missing, "%s: tiles not in any bank at %s" % (self.name, missing[:8])
        lines = [0] * 144
        for (x, y, t) in self.sprites:
            assert any(flip(t, fx, fy) in lookup for fx in (0, 1) for fy in (0, 1)), \
                "%s: sprite tile not in a bank" % self.name
            for j in range(8):
                if 0 <= y + j < 144:
                    lines[y + j] += 1
        assert len(self.sprites) <= 40, self.name
        assert max(lines) <= 10, "%s: %d sprites on one line" % (self.name, max(lines))
        self.unique_bg = len(used)
        self.sprite_count = len(self.sprites)
        self.max_line = max(lines)
        return self


# ---- table screen pieces -------------------------------------------------
ROW_BOTRAIL, ROW_BOTHAND, ROW_STATUS, ROW_HAND, ROW_FOOT = 0, 1, 12, 14, 17
PAIR_X = (3, 7, 11)               # attack card columns of the three pairs in a row
PAIR_Y = (4, 8)                   # attack card rows of the two pair rows
TRUMP_AT, DECK_AT, DISCARD_AT = (0, 5), (1, 5), (17, 8)


def battle_pos(i):
    return PAIR_X[i % 3], PAIR_Y[i // 3]


def text_tiles(s, ui):
    """'{UP}' style tokens are icons, everything else font characters."""
    out, i = [], 0
    while i < len(s):
        if s[i] == "{":
            j = s.index("}", i)
            out.append(ui["IC_" + s[i + 1:j]])
            i = j + 1
        else:
            out.append(ui["CH_" + s[i]])
            i += 1
    return out


def rail(scr, row, left, right, ui):
    for tx in range(20):
        scr.fill_tile(tx, row, ui["CH_ "])
    for i, t in enumerate(text_tiles(left, ui)):
        scr.fill_tile(i, row, t)
    r = text_tiles(right, ui)
    for i, t in enumerate(r):
        scr.fill_tile(20 - len(r) + i, row, t)


def wool_rows(scr, rows, ui):
    for ty in rows:
        for tx in range(20):
            scr.fill_tile(tx, ty, ui["WOOL"])


def put_card(scr, cv, tx, ty):
    scr.bg.blit(cv, tx * 8, ty * 8)


def table_base(name, title, ui, bot="SEOUL", bot_n=6, deck_n=18, trump=("9", "H")):
    scr = Screen(name, title)
    wool_rows(scr, range(1, 12), ui)
    wool_rows(scr, range(13, 17), ui)
    rail(scr, ROW_BOTRAIL, "%s {MINIBACK}%d" % (bot, bot_n),
         ("{MINIDECK}%d {%s}" % (deck_n, trump[1])) if deck_n else "{%s}" % trump[1], ui)
    rail(scr, ROW_FOOT, "", "", ui)
    shown = min(bot_n, 18)
    x0 = (20 - (shown + 1)) // 2
    for i in range(shown):
        put_card(scr, card_back(), x0 + i, ROW_BOTHAND)
    if deck_n:
        put_card(scr, card_face(*trump), *TRUMP_AT)
        if deck_n > 1:
            put_card(scr, card_back(), *DECK_AT)
    return scr


def hand_columns(n):
    if n <= 10:
        x0 = (20 - 2 * n) // 2
        return [x0 + 2 * i for i in range(n)]
    if n <= 19:
        x0 = (20 - (n + 1)) // 2
        return [x0 + i for i in range(n)]
    return list(range(19))


def draw_hand(scr, cards, selected=(), first=0, ui=None):
    n = len(cards)
    cols = hand_columns(n)
    vis = list(range(n)) if n <= 19 else list(range(first, first + 19))
    places, widths = {}, {}
    for k, i in enumerate(vis):
        tx = cols[k]
        ty = ROW_HAND - 1 if i in selected else ROW_HAND
        put_card(scr, card_face(*cards[i]), tx, ty)
        places[i] = (tx, ty)
        nxt = cols[k + 1] if k + 1 < len(vis) else 99
        widths[i] = 2 if nxt - tx >= 2 else 1
    if n > 19 and ui is not None:
        left, right = first, n - first - 19
        rail(scr, ROW_FOOT, ("{LEFT}%d" % left) if left else "",
             ("%d{RIGHT}" % right) if right else "", ui)
    return places, widths


def underline(scr, sp, place, width):
    tx, ty = place
    y = (ty + 3) * 8 + 1
    if width == 1:
        scr.spr(tx * 8, y, sp["SP_UL_ONE"])
    else:
        scr.spr(tx * 8, y, sp["SP_UL_LEFT"])
        scr.spr(tx * 8 + 8, y, sp["SP_UL_RIGHT"])


def arrow(scr, sp, src, dst, dashed=False):
    """The cover/pass arrow: a line of dot sprites and a head, from the top
    of the cursor card's visible column to the target's middle."""
    (sx, sy), (dx, dy) = src, dst
    L = math.hypot(dx - sx, dy - sy)
    step = 10 if dashed else 6
    n = int((L - 9) // step)
    for k in range(1, n + 1):
        f = k * step / L
        y = round(sy + (dy - sy) * f) - 2
        if ROW_STATUS * 8 - 4 <= y < ROW_STATUS * 8 + 8:
            continue              # the line passes behind the status rail's text
        scr.spr(round(sx + (dx - sx) * f) - 2, y, sp["SP_DOT"])
    ang = math.degrees(math.atan2(-(dy - sy), dx - sx))   # 90 = straight up
    hx, hy = dx - 4, dy - 4
    if 67.5 <= ang <= 112.5:
        scr.spr(hx, hy, sp["SP_HEAD_N"])
    elif 22.5 <= ang < 67.5:
        scr.spr(hx, hy, sp["SP_HEAD_NE"])
    elif 112.5 < ang <= 157.5:
        scr.spr(hx, hy, sp["SP_HEAD_NE"], fx=True)
    else:
        scr.spr(hx, hy, sp["SP_HEAD_N"], fy=True)


def brackets(scr, sp, tx, ty, w=2, h=3):
    x0, y0 = tx * 8 - 3, ty * 8 - 3
    x1, y1 = (tx + w) * 8 + 3 - 8, (ty + h) * 8 + 3 - 8
    t = sp["SP_BRACKET"]
    scr.spr(x0, y0, t)
    scr.spr(x1, y0, t, fx=True)
    scr.spr(x0, y1, t, fy=True)
    scr.spr(x1, y1, t, fx=True, fy=True)


def chip(scr, ui, x, y, s):
    """A dark label made of rail font tiles used as sprites (the website's pills)."""
    for i, t in enumerate(text_tiles(s, ui)):
        scr.spr(x + 8 * i, y, t)


def card_top_center(place, width):
    tx, ty = place
    return tx * 8 + (4 if width == 1 else 8), ty * 8 - 1


def card_bottom(tx, ty):
    """Where the arrow's head lands: just under the target's bottom edge."""
    return tx * 8 + 8, ty * 8 + 24 + 8


# ---- the table screens -----------------------------------------------------
def screens_table(ui, sp):
    out = []

    s = table_base("t1_attack", "You attack", ui, bot_n=6, deck_n=18)
    x, y = battle_pos(0)
    put_card(s, card_face("7", "S"), x, y)
    put_card(s, card_face("10", "S"), x + 1, y + 1)
    put_card(s, card_face("7", "H"), *battle_pos(1))
    rail(s, ROW_STATUS, "YOUR ATTACK", "{UP}PLAY", ui)
    hand = [("6", "C"), ("7", "D"), ("9", "S"), ("J", "H"), ("Q", "C"), ("K", "D"), ("A", "S")]
    places, widths = draw_hand(s, hand)
    underline(s, sp, places[1], widths[1])
    s.notes.append("Cursor (dark underline) on the 7 of diamonds; UP plays it as another 7.")
    out.append(s)

    s = table_base("t2_bot_defends", "Seoul defends", ui, bot_n=3, deck_n=12, trump=("J", "C"))
    for i, (a, c) in enumerate([(("8", "D"), ("Q", "D")), (("8", "S"), ("A", "S")), (("Q", "H"), ("6", "C"))]):
        x, y = battle_pos(i)
        put_card(s, card_face(*a), x, y)
        put_card(s, card_face(*c), x + 1, y + 1)
    put_card(s, card_back(), *DISCARD_AT)
    put_card(s, card_back(), DISCARD_AT[0] + 1, DISCARD_AT[1])
    rail(s, ROW_STATUS, "ALL COVERED", "{DOWN}DONE", ui)
    draw_hand(s, [("6", "H"), ("9", "D"), ("10", "C"), ("K", "S"), ("A", "H")])
    s.notes.append("No cursor: nothing in the hand can be added, so DOWN (done) is the move.")
    out.append(s)

    s = table_base("t3_target_cover", "Target mode: cover", ui, bot_n=4, deck_n=8, trump=("9", "H"))
    put_card(s, card_face("8", "C"), *battle_pos(0))
    put_card(s, card_face("8", "S"), *battle_pos(1))
    rail(s, ROW_STATUS, "{LEFT}{RIGHT}TARGET", "{UP}COVER", ui)
    hand = [("6", "D"), ("10", "C"), ("Q", "H"), ("K", "S"), ("A", "D"), ("7", "C")]
    places, widths = draw_hand(s, hand)
    underline(s, sp, places[2], widths[2])
    tx, ty = battle_pos(1)
    arrow(s, sp, card_top_center(places[2], widths[2]), card_bottom(tx, ty))
    brackets(s, sp, tx, ty)
    s.notes.append("The queen of hearts (trump) covers either 8: the arrow and brackets mark the 8 of spades; left/right moves them.")
    out.append(s)

    s = table_base("t3b_target_pass", "Target mode: pass", ui, bot_n=4, deck_n=8, trump=("9", "H"))
    put_card(s, card_face("8", "C"), *battle_pos(0))
    put_card(s, card_face("8", "S"), *battle_pos(1))
    px, py = battle_pos(2)
    put_card(s, pass_slot(), px, py)
    rail(s, ROW_STATUS, "{LEFT}{RIGHT}TARGET", "{UP}PASS", ui)
    hand = [("6", "D"), ("8", "H"), ("Q", "H"), ("K", "S"), ("A", "D"), ("7", "C")]
    places, widths = draw_hand(s, hand)
    underline(s, sp, places[1], widths[1])
    arrow(s, sp, card_top_center(places[1], widths[1]), card_bottom(px, py), dashed=True)
    chip(s, ui, (px + 2) * 8 + 2, py * 8 + 8, "PASS")
    s.notes.append("The 8 of hearts can cover (trump) or pass (an 8): one step right of the last attack, the dashed arrow points at the empty pass space.")
    out.append(s)

    s = table_base("t4_pickup", "Pick up", ui, bot_n=2, deck_n=0, trump=("9", "H"))
    for i, (a, c) in enumerate([(("K", "C"), ("A", "C")), (("K", "S"), None), (("A", "S"), None)]):
        x, y = battle_pos(i)
        put_card(s, card_face(*a), x, y)
        if c:
            put_card(s, card_face(*c), x + 1, y + 1)
    put_card(s, card_back(), *DISCARD_AT)
    rail(s, ROW_STATUS, "YOU DEFEND", "{DOWN}PICK UP", ui)
    draw_hand(s, [("6", "S"), ("7", "D"), ("9", "C"), ("J", "D")])
    s.notes.append("Deck gone: the top rail keeps the trump suit. DOWN picks up the table.")
    out.append(s)

    s = table_base("t5_big_hand", "Big hand, two selected", ui, bot_n=1, deck_n=0, trump=("9", "H"))
    hand = [(r, su) for su in G.SUITS for r in ("6", "7", "8", "9", "10", "J")][:22]
    nines = [i for i, c in enumerate(hand) if c[0] == "9"]
    places, widths = draw_hand(s, hand, selected=set(nines[:2]), first=2, ui=ui)
    underline(s, sp, places[nines[2]], widths[nines[2]])
    rail(s, ROW_STATUS, "2 SELECTED", "{UP}PLAY", ui)
    s.notes.append("22 cards: 19 show one column each; the foot rail counts the hidden ones each side and the hand scrolls with the cursor.")
    out.append(s)

    s = table_base("t6_rearrange", "Rearrange (SELECT held)", ui, bot_n=6, deck_n=18)
    hand = [("6", "C"), ("Q", "C"), ("9", "S"), ("J", "H"), ("7", "D"), ("K", "D"), ("A", "S")]
    places, widths = draw_hand(s, hand)
    p = places[1]
    underline(s, sp, p, widths[1])
    y = (p[1] + 3) * 8           # beside the underline, in the foot rail
    s.spr(p[0] * 8 - 6, y - 1, sp["SP_NUDGE"])
    s.spr(p[0] * 8 + 14, y - 1, sp["SP_NUDGE"], fx=True)
    rail(s, ROW_STATUS, "SELECT+{LEFT}{RIGHT}", "MOVE CARD", ui)
    s.notes.append("While SELECT is held the cursor card carries nudge arrows; left/right slides it through the hand.")
    out.append(s)
    return out


# --------------------------------------------------------------------------
# Deliverable 1: the card sheet
# --------------------------------------------------------------------------
def sheet_cards():
    pad = 4
    cols, rows = 9, 4
    w = pad + cols * (CW + pad)
    h = pad + rows * (CH + pad) + 8 + CH + 16 + pad
    cv = Canvas(w, h, T)
    for j, s in enumerate(G.SUITS):
        for i, r in enumerate(G.RANKS):
            cv.blit(card_face(r, s), pad + i * (CW + pad), pad + j * (CH + pad))
    y0 = pad + rows * (CH + pad) + 8
    x = pad
    cv.blit(card_back(), x, y0 + 8)
    for i in range(4):
        cv.blit(card_back(), x + CW + pad + i * 8, y0 + 8)
    x += CW + pad + 5 * 8 + pad * 2
    hand = [("6", "S"), ("9", "H"), ("Q", "C"), ("A", "D"), ("10", "H")]
    for i, (r, s) in enumerate(hand):
        cv.blit(card_face(r, s), x + i * 8, y0 + 8 - (8 if i == 2 else 0))
    ux = x + 3 * 8
    for i in range(1, 7):
        cv.put(ux + i, y0 + 8 + CH + 2, K)
        cv.put(ux + i, y0 + 8 + CH + 3, K)
    return cv


def contact(names, path, cols=2, gap=16):
    ims = [Image.open(os.path.join(OUT_MOCK, n + ".png")) for n in names]
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + (cols + 1) * gap, rows * h + (rows + 1) * gap), (40, 40, 40))
    for k, im in enumerate(ims):
        sheet.paste(im, (gap + (k % cols) * (w + gap), gap + (k // cols) * (h + gap)))
    sheet.save(path)


def show(path):
    os.system("osascript -e 'tell application \"Preview\" to close every window' >/dev/null 2>&1")
    os.system("open '%s'" % path)


def main():
    os.makedirs(OUT_PNG, exist_ok=True)
    os.makedirs(OUT_MOCK, exist_ok=True)
    ui, cards, sp = ui_bank(), card_bank(), sprite_bank()
    banks = {"ui": ui, "cards": cards, "sprites": sp}
    sheet_cards().image().save(os.path.join(OUT_MOCK, "01_cards_sheet.png"))
    print("banks: ui %d, cards %d, sprites %d  (bg+window %d of 256)"
          % (len(ui), len(cards), len(sp), len(ui) + len(cards)))
    for s in screens_table(ui, sp):
        s.check(banks)
        s.compose().image().save(os.path.join(OUT_MOCK, s.name + ".png"))
        print("%-18s unique bg tiles %3d  sprites %2d  max/line %d"
              % (s.name, s.unique_bg, s.sprite_count, s.max_line))
    names = [s.name for s in screens_table(ui, sp)]
    contact(names, os.path.join(OUT_MOCK, "02_table_layouts.png"))
    import screens_full
    full = screens_full.screens_full(sp)
    for s in full:
        tiles = set(s.bg.tile(tx, ty) for ty in range(18) for tx in range(20))
        assert len(tiles) <= 240, (s.name, len(tiles))
        s.unique_bg = len(tiles)
        s.check_sprites_only = True
        s.compose().image().save(os.path.join(OUT_MOCK, s.name + ".png"))
        print("%-18s unique bg tiles %3d  sprites %2d" % (s.name, s.unique_bg, len(s.sprites)))
    contact([s.name for s in full], os.path.join(OUT_MOCK, "03_dashboard_win_lose.png"))


if __name__ == "__main__":
    main()
