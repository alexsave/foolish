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
from sprig import sprig

# The card back's sprig layout (sprig/sprig.py): "hanging", "diagonal" or "arching".
BACK_LAYOUT = "final_2b_framed"   # Alex picked 2B, then its curl, vine and corner leaf, and the frame back

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


def card_outline(cv, fill, border=K):
    for y in range(CH):
        for x in range(CW):
            corner = (x in (0, CW - 1)) and (y in (0, CH - 1))
            if corner:
                cv.px[y][x] = None
            elif x in (0, CW - 1) or y in (0, CH - 1):
                cv.px[y][x] = border
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


def card_back(layout=None):
    """Black lacquer; a "_framed" layout has a dark grey outer ring and a black
    ring inside it round its 12x20 field; the others fill 14x22 inside a black border."""
    layout = layout or BACK_LAYOUT
    cv = Canvas(CW, CH, None)
    if layout.endswith("_framed"):
        # two rings: dark grey outermost (it parts overlapped backs), black inside
        card_outline(cv, K, border=D)
        cv.glyph(2, 2, sprig.LAYOUTS[layout]())
    else:
        card_outline(cv, K)
        cv.glyph(1, 1, sprig.LAYOUTS[layout]())
    return cv


def pass_slot():
    """The empty 'pass' space target mode points at: a dashed outline round
    a patch of plain table (opaque, so it is the same 6 tiles over any wool)."""
    cv = Canvas(CW, CH, T)
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
    assert t[0] == t[7] == t[56] == t[63] == T   # round card corners over wool = plain
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


def lacquer_font_bank():
    """The font for the black lacquer screens (white on black), same order as
    the rail font: the code prints the bot's name on the win screen with it."""
    bank = {}
    for ch in FONT_CHARS:
        if ch == " ":
            bank["LQ_ "] = tuple([K] * 64)
        else:
            bank["LQ_" + ch] = rail_glyph_tile(G.FONT[ch], fg=W, bg=K)
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
    for c in (T, K, D):        # table; a face's black border; another back's grey ring
        bank["BACK_TL_u%d" % c] = fill_none(b.tile(0, 0), c)
    bank["BACK_TR"] = fill_none(b.tile(1, 0), T)
    bank["BACK_ML"] = b.tile(0, 1)
    bank["BACK_MR"] = b.tile(1, 1)
    for c in (T, K, D):
        bank["BACK_BL_u%d" % c] = fill_none(b.tile(0, 2), c)
    bank["BACK_BR"] = fill_none(b.tile(1, 2), T)
    p = pass_slot()
    for name, (tx, ty) in (("TL", (0, 0)), ("TR", (1, 0)), ("ML", (0, 1)),
                           ("MR", (1, 1)), ("BL", (0, 2)), ("BR", (1, 2))):
        t = p.tile(tx, ty)
        bank["PASS_" + name] = fill_none(t, T)
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


def narrow_back():
    """The card back squeezed to half width: the deal's flip, frame 2."""
    b = card_back()
    cv = Canvas(8, CH, None)
    for y in range(CH):
        for x in range(8):
            cv.px[y][x] = b.px[y][x * 2 + (1 if x >= 4 else 0)]
    cv.px[0][0] = cv.px[0][7] = cv.px[CH - 1][0] = cv.px[CH - 1][7] = None
    return cv


def edge_card():
    """A card seen on its edge: the flip's middle frame."""
    cv = Canvas(8, CH, None)
    for y in range(1, CH - 1):
        cv.put(3, y, K)
        cv.put(4, y, W)
        cv.put(5, y, K)
    cv.put(4, 0, K)
    cv.put(4, CH - 1, K)
    return cv


def sprite_bank():
    bank = {}
    for name, art in SPRITES.items():
        cv = Canvas(8, 8, T)
        cv.glyph(0, 0, G.g(art))
        bank["SP_" + name] = cv.tile(0, 0)
    for name, cv in (("NARROW", narrow_back()), ("EDGE", edge_card())):
        for ty, part in enumerate(("T", "M", "B")):
            bank["SP_%s_%s" % (name, part)] = fill_none(cv.tile(0, ty), T)
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

    def check_sprites(self, banks):
        """Sprites only (full-screen pictures ship their own tile maps)."""
        lookup = {t for bank in banks.values() for t in bank.values()}
        lines = [0] * 144
        for (x, y, t) in self.sprites:
            assert any(flip(t, fx, fy) in lookup for fx in (0, 1) for fy in (0, 1)), \
                "%s: sprite tile not in a bank" % self.name
            for j in range(8):
                if 0 <= y + j < 144:
                    lines[y + j] += 1
        assert len(self.sprites) <= 40, self.name
        assert max(lines + [0]) <= 10, "%s: %d sprites on one line" % (self.name, max(lines))
        self.sprite_count, self.max_line = len(self.sprites), max(lines + [0])
        return self

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
# Rows: 0 top rail (bot, deck, trump) | 1-3 bot hand | 4-11 table |
#       12 raise row | 13-15 your hand | 16 underline row | 17 foot rail (status)
ROW_BOTRAIL, ROW_BOTHAND, ROW_HAND, ROW_UNDER, ROW_FOOT = 0, 1, 13, 16, 17
TRUMP_AT, DECK_AT, DISCARD_AT = (0, 4), (1, 4), (0, 8)


def battle_layout(n):
    """Attack card positions of n battles, centred on the screen.  Up to
    three sit in one row centred vertically on the table; four to six use two
    rows (three on top).  A cover sits one tile right and one down."""
    if n <= 3:
        rows = [(n, 6)]
    else:
        rows = [(3, 4), (n - 3, 8)]
    out = []
    for m, y in rows:
        x0 = (20 - (4 * m - 1)) // 2
        out += [(x0 + 4 * k, y) for k in range(m)]
    return out


def pass_pos(n):
    """The empty space just right of the last attack (the website's pass point)."""
    x, y = battle_layout(n)[-1]
    return x + 4, y


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


def put_text(scr, tx, ty, s, ui):
    for i, t in enumerate(text_tiles(s, ui)):
        scr.fill_tile(tx + i, ty, t)


def rail(scr, row, left, right, ui):
    for tx in range(20):
        scr.fill_tile(tx, row, ui["CH_ "])
    put_text(scr, 0, row, left, ui)
    put_text(scr, 20 - len(text_tiles(right, ui)), row, right, ui)


def wool_rows(scr, rows, ui):
    for ty in rows:
        for tx in range(20):
            scr.fill_tile(tx, ty, ui["WOOL"])


def put_card(scr, cv, tx, ty):
    scr.bg.blit(cv, tx * 8, ty * 8)


def table_base(name, title, ui, bot="SEOUL", bot_n=6, deck_n=18, trump=("9", "H"), discard=0):
    scr = Screen(name, title)
    wool_rows(scr, range(1, 17), ui)
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
    for i in range(min(discard, 2)):
        put_card(scr, card_back(), DISCARD_AT[0] + i, DISCARD_AT[1])
    return scr


def battles(scr, pairs):
    pos = battle_layout(len(pairs))
    for (x, y), (a, c) in zip(pos, pairs):
        put_card(scr, card_face(*a), x, y)
        if c:
            put_card(scr, card_face(*c), x + 1, y + 1)
    return pos


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
        # hidden-card counts: small dark tabs at the ends of the underline row
        left, right = first, n - first - 19
        if left:
            put_text(scr, 0, ROW_UNDER, "{LEFT}%d" % left, ui)
        if right:
            s = "%d{RIGHT}" % right
            put_text(scr, 20 - len(text_tiles(s, ui)), ROW_UNDER, s, ui)
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
    of the cursor card's visible column to just under the target."""
    (sx, sy), (dx, dy) = src, dst
    L = math.hypot(dx - sx, dy - sy)
    step = 10 if dashed else 6
    n = int((L - 9) // step)
    for k in range(1, n + 1):
        f = k * step / L
        scr.spr(round(sx + (dx - sx) * f) - 2, round(sy + (dy - sy) * f) - 2, sp["SP_DOT"])
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
    battles(s, [(("7", "S"), ("10", "S")), (("7", "H"), None)])
    rail(s, ROW_FOOT, "YOUR ATTACK", "{UP}PLAY", ui)
    hand = [("6", "C"), ("7", "D"), ("9", "S"), ("J", "H"), ("Q", "C"), ("K", "D"), ("A", "S")]
    places, widths = draw_hand(s, hand)
    underline(s, sp, places[1], widths[1])
    s.notes.append("Cursor (dark underline) on the 7 of diamonds; UP plays it as another 7.")
    out.append(s)

    s = table_base("t2_bot_defends", "Seoul defends", ui, bot_n=3, deck_n=12, trump=("J", "C"), discard=2)
    battles(s, [(("8", "D"), ("Q", "D")), (("8", "S"), ("A", "S")), (("Q", "H"), ("6", "C")),
                (("Q", "S"), ("K", "S"))])
    rail(s, ROW_FOOT, "ALL COVERED", "{DOWN}DONE", ui)
    draw_hand(s, [("6", "H"), ("9", "D"), ("10", "C"), ("K", "D"), ("A", "H")])
    s.notes.append("Four battles: two rows, centred. No cursor: nothing can be added, so DOWN (done) is the move.")
    out.append(s)

    s = table_base("t3_target_cover", "Target mode: cover", ui, bot_n=4, deck_n=8, trump=("9", "H"))
    pos = battles(s, [(("8", "C"), None), (("8", "S"), None)])
    rail(s, ROW_FOOT, "{LEFT}{RIGHT}TARGET", "{UP}COVER", ui)
    hand = [("6", "D"), ("10", "C"), ("Q", "H"), ("K", "S"), ("A", "D"), ("7", "C")]
    places, widths = draw_hand(s, hand)
    underline(s, sp, places[2], widths[2])
    tx, ty = pos[1]
    arrow(s, sp, card_top_center(places[2], widths[2]), card_bottom(tx, ty))
    brackets(s, sp, tx, ty)
    s.notes.append("The queen of hearts (trump) covers either 8: the arrow and brackets mark the 8 of spades; left/right moves them.")
    out.append(s)

    s = table_base("t3b_target_pass", "Target mode: pass", ui, bot_n=4, deck_n=8, trump=("9", "H"))
    battles(s, [(("8", "C"), None), (("8", "S"), None)])
    px, py = pass_pos(2)
    put_card(s, pass_slot(), px, py)
    rail(s, ROW_FOOT, "{LEFT}{RIGHT}TARGET", "{UP}PASS", ui)
    hand = [("6", "D"), ("8", "H"), ("Q", "H"), ("K", "S"), ("A", "D"), ("7", "C")]
    places, widths = draw_hand(s, hand)
    underline(s, sp, places[1], widths[1])
    arrow(s, sp, card_top_center(places[1], widths[1]), card_bottom(px, py), dashed=True)
    chip(s, ui, px * 8 - 8, py * 8 - 9, "PASS")
    s.notes.append("The 8 of hearts can cover (trump) or pass (an 8): one step right of the last attack, the dashed arrow points at the empty pass space.")
    out.append(s)

    s = table_base("t4_pickup", "Pick up", ui, bot_n=2, deck_n=0, trump=("9", "H"), discard=1)
    battles(s, [(("K", "C"), ("A", "C")), (("K", "S"), None), (("A", "S"), None)])
    rail(s, ROW_FOOT, "YOU DEFEND", "{DOWN}PICK UP", ui)
    draw_hand(s, [("6", "S"), ("7", "D"), ("9", "C"), ("J", "D")])
    s.notes.append("Deck gone: the top rail keeps the trump suit. DOWN picks up the table.")
    out.append(s)

    s = table_base("t5_big_hand", "Big hand, two selected", ui, bot_n=1, deck_n=0, trump=("9", "H"), discard=2)
    hand = [(r, su) for su in G.SUITS for r in ("6", "7", "8", "9", "10", "J")][:22]
    nines = [i for i, c in enumerate(hand) if c[0] == "9"]
    places, widths = draw_hand(s, hand, selected=set(nines[:2]), first=2, ui=ui)
    underline(s, sp, places[nines[2]], widths[nines[2]])
    rail(s, ROW_FOOT, "2 SELECTED", "{UP}PLAY", ui)
    s.notes.append("22 cards: 19 show one column each; small tabs count the hidden ones each side and the hand scrolls with the cursor.")
    out.append(s)

    s = table_base("t6_rearrange", "Rearrange (SELECT held)", ui, bot_n=6, deck_n=18)
    hand = [("6", "C"), ("Q", "C"), ("9", "S"), ("J", "H"), ("7", "D"), ("K", "D"), ("A", "S")]
    places, widths = draw_hand(s, hand)
    p = places[1]
    underline(s, sp, p, widths[1])
    y = (p[1] + 3) * 8
    s.spr(p[0] * 8 - 6, y - 1, sp["SP_NUDGE"])
    s.spr(p[0] * 8 + 14, y - 1, sp["SP_NUDGE"], fx=True)
    rail(s, ROW_FOOT, "SELECT+{LEFT}{RIGHT}", "MOVE CARD", ui)
    s.notes.append("While SELECT is held the cursor card carries nudge arrows; left/right slides it through the hand.")
    out.append(s)
    return out


# ---- motion: card sprites -------------------------------------------------
def card_sprites(scr, cv, x, y):
    """A whole card as six 8x8 sprites (two per scanline).  The tiles are
    the card bank's own, corners index 0 = transparent."""
    for ty in range(3):
        for tx in range(2):
            scr.spr(x + tx * 8, y + ty * 8, fill_none(cv.tile(tx, ty), T))


def strip_sprites(scr, sp, name, x, y):
    for ty, part in enumerate(("T", "M", "B")):
        scr.spr(x, y + ty * 8, sp["SP_%s_%s" % (name, part)])


def lerp(a, b, f):
    return round(a + (b - a) * f)


def screens_motion(ui, sp):
    out = []
    # A card flies from the hand to the table: 4 of its ~12 frames.  The
    # battles re-centre for the new count when it lands.
    hand = [("6", "C"), ("7", "D"), ("9", "S"), ("J", "H"), ("Q", "C"), ("K", "D"), ("A", "S")]
    first = [(("7", "S"), ("10", "S"))]
    dst = battle_layout(2)[1]
    for k, f in enumerate((0.0, 0.35, 0.7, 1.0)):
        s = table_base("m1_fly_%d" % k, "Fly frame %d" % k, ui, bot_n=6, deck_n=18)
        rail(s, ROW_FOOT, "YOUR ATTACK", "", ui)
        if f < 1.0:
            battles(s, first)
            cols = hand_columns(len(hand))
            for i, c in enumerate(hand):
                if i != 1:
                    put_card(s, card_face(*c), cols[i], ROW_HAND)
            sx, sy = cols[1] * 8, (ROW_HAND - 1) * 8
            dx, dy = dst[0] * 8, dst[1] * 8
            card_sprites(s, card_face("7", "D"), lerp(sx, dx, f),
                         lerp(sy, dy, f) - round(12 * math.sin(math.pi * f)))
        else:
            battles(s, first + [(("7", "D"), None)])
            draw_hand(s, hand[:1] + hand[2:])
        out.append(s)
    # The deal: deck -> bot (back), deck -> you (flips in flight)
    frames = [("bot", 0.5), ("you", 0.25), ("you", 0.5), ("you", 0.75)]
    for k, (who, f) in enumerate(frames):
        s = table_base("m2_deal_%d" % k, "Deal frame %d" % k, ui, bot_n=2, deck_n=30)
        rail(s, ROW_FOOT, "DEALING", "", ui)
        draw_hand(s, [("8", "H"), ("K", "C")])
        sx, sy = DECK_AT[0] * 8, DECK_AT[1] * 8
        if who == "bot":
            dx, dy = 10 * 8, ROW_BOTHAND * 8
            card_sprites(s, card_back(), lerp(sx, dx, f), lerp(sy, dy, f))
        else:
            dx, dy = 12 * 8, ROW_HAND * 8
            x, y = lerp(sx, dx, f), lerp(sy, dy, f)
            if f < 0.4:
                strip_sprites(s, sp, "NARROW", x + 4, y)
            elif f < 0.6:
                strip_sprites(s, sp, "EDGE", x + 4, y)
            else:
                card_sprites(s, card_face("A", "H"), x, y)
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
    motion = screens_motion(ui, sp)
    for s in motion:
        s.check(banks)
        s.compose().image().save(os.path.join(OUT_MOCK, s.name + ".png"))
        print("%-18s unique bg tiles %3d  sprites %2d  max/line %d"
              % (s.name, s.unique_bg, s.sprite_count, s.max_line))
    contact([s.name for s in motion if s.name.startswith("m1")], os.path.join(OUT_MOCK, "04_fly.png"))
    contact([s.name for s in motion if s.name.startswith("m2")], os.path.join(OUT_MOCK, "05_deal.png"))
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
    import wins
    fw = {"fireworks": wins.firework_bank()}
    for s in wins.win_screens():
        tiles = set(s.bg.tile(tx, ty) for ty in range(18) for tx in range(20))
        assert len(tiles) <= wins.TILESET_MAX, (s.name, len(tiles))
        s.check_sprites(fw)
        s.compose().image().save(os.path.join(OUT_MOCK, s.name + ".png"))
        print("%-18s unique bg tiles %3d  sprites %2d  max/line %d" % (s.name, len(tiles), s.sprite_count, s.max_line))
    contact(["w_miami", "w_new_york", "w_seoul", "w_warsaw"], os.path.join(OUT_MOCK, "08_win_cities.png"))
    strip = []
    for f in range(len(wins.SCHEDULE)):
        s = wins.win_screen("SEOUL", frame=f)
        s.name = "w_seoul_frame%d" % f
        s.check_sprites(fw)
        s.compose().image().save(os.path.join(OUT_MOCK, s.name + ".png"))
        strip.append(s.name)
    for city in wins.CITIES:          # every frame of every city keeps the fireworks in the sky
        for f in range(len(wins.SCHEDULE)):
            wins.win_screen(city, frame=f)
    contact(strip, os.path.join(OUT_MOCK, "09_fireworks_frames.png"), cols=4)


if __name__ == "__main__":
    main()
