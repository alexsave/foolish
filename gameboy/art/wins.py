# Per-bot win screens: the beaten city at night, the fool's cap on its
# tallest landmark, fireworks over it (sprites), "<CITY>" printed by the code
# on row NAME_ROW with the lacquer font, "IS THE DURAK" under it.
#
# Every pixel is placed by rule or by a fixed list: no randomness (lit
# windows follow an arithmetic pattern), so the art is the same every run.

import math

import glyphs as G
from gen_art import Canvas, Screen, T, W, D, K, lacquer_font_bank, flip
from screens_full import big_text, big_width, text, button

# VRAM on a win screen: its tileset from 0, the lacquer font at LACQUER_BASE,
# the four firework sprite tiles at FW_BASE, so a tileset may use up to 206.
LACQUER_BASE, FW_BASE = 206, 252
LACQUER_WIN_COUNT = 27  # a win screen loads only " A..Z" of the lacquer font (206..232)
TRAIN_BASE = 240        # the New York subway train's two sprite tiles
TILESET_MAX = LACQUER_BASE
NAME_ROW = 14          # the bot's name, columns (20 - len) / 2, lacquer font
GROUND = 104           # the skyline stands on this y; water below to y 111
CITIES = ("MIAMI", "NEW YORK", "SEOUL", "MOSCOW")

STARS = ((6, 34), (19, 4), (27, 40), (44, 30), (58, 38), (71, 33), (88, 2), (99, 36),
         (112, 30), (126, 39), (139, 33), (152, 3), (153, 41), (3, 12), (80, 44), (35, 44))


# ---- small helpers ---------------------------------------------------------
def lit(cv, x0, y0, x1, y1, sx=3, sy=4, seed=0):
    """Windows on a dark building: a regular grid, most light grey, a few
    white, a few dark, chosen by a fixed arithmetic rule."""
    for j, y in enumerate(range(y0, y1, sy)):
        for i, x in enumerate(range(x0, x1, sx)):
            v = (i * 7 + j * 3 + seed) % 9
            if v == 0:
                continue                      # this one is dark
            cv.put(x, y, W if v == 4 else T)


def tri(cv, cx, top, h, c, slope=1.0):
    for j in range(h):
        half = int(j * slope)
        cv.rect(cx - half, top + j, 2 * half + 1, 1, c)


def water(cv):
    cv.rect(0, GROUND, 160, 112 - GROUND, K)
    for y in range(GROUND + 2, 112, 3):       # reflections: short light dashes
        for x in range((y * 5) % 11, 160, 11):
            cv.rect(x, y, 3, 1, D)


def surf(cv):
    """Miami's water: foam where the surf breaks on the sand, gentle swell
    lines, and the moon's path on the water under it."""
    for x in range(0, 160):
        if (x // 3) % 3 != 2:
            cv.put(x, GROUND, W)                          # the foam line
    for y, ph in ((GROUND + 3, 0), (GROUND + 6, 4)):
        for x in range(ph, 160, 9):
            cv.rect(x, y, 4, 1, T)                        # swell
    for y in range(GROUND + 1, 112, 2):
        w = 2 + (y - GROUND) // 2
        cv.rect(152 - w // 2, y, w, 1, W)                 # the moon's path


def fool_cap(cv, cx, bottom):
    """The durak's jester cap, 13x7: two horns curling up and out, a bell
    on each tip, a chequered band, sitting on a spire top at (cx, bottom)."""
    art = G.g("""
b...........b
.hh.......hh.
..hhh...hhh..
...hhhhhhh...
..hhhhhhhhh..
.dwdwdwdwdwd.
.ddddddddddd.
""")
    m = {"b": W, "h": T, "w": W, "d": D}
    for j, r in enumerate(art):
        for i, ch in enumerate(r):
            if ch in m:
                cv.put(cx - 6 + i, bottom - len(art) + j, m[ch])
    return bottom - len(art)


def moon(cv, x, y):
    for j in range(9):
        for i in range(9):
            d = (i - 4) ** 2 + (j - 4) ** 2
            if d <= 17:
                cv.put(x + i, y + j, W)
            if (i - 6) ** 2 + (j - 3) ** 2 <= 10 and d <= 17:
                cv.put(x + i, y + j, K)        # a crescent


# ---- the four cities, drawn from reference photos (proportions and the
# signature details of each landmark); every landmark top stays at or below
# y = GROUND - 74 so the fool's cap clears the title ----------------------------
def column(cv, x0, x1, y0, y1, step=3, seed=0):
    """Vertical window stripes (the piers of a tall tower): lit columns broken
    by a few dark floors, chosen by a fixed rule."""
    for i, x in enumerate(range(x0, x1, step)):
        for y in range(y0, y1):
            if (y * 3 + i * 5 + seed) % 11 not in (0, 1):
                cv.put(x, y, T)


def filler(cv, x, w, h, s, step=3):
    cv.rect(x, GROUND - h, w, h, D)
    lit(cv, x + 1, GROUND - h + 2, x + w - 1, GROUND - 2, step, 4, s)


def condo(cv, x, top, kind, seed, base):
    """A Brickell / Miami Beach condo tower, 7 px wide on the 8 px tile grid
    (its balcony bands sit on the same rows everywhere, so their tiles repeat):
    a balcony band every 4 px, a few lit windows, and a top: 'sail' (a
    sloped wedge), 'curve' (a rounded crown) or 'crown' (a lit crown band)."""
    cv.rect(x, top, 7, base - top, D)
    for y in range(top + 2, base):
        if y % 4 == 1:
            cv.rect(x, y, 7, 1, T)                             # balcony band
        elif y % 4 == 3 and (y // 4 + x // 8 + seed) % 4 == 0:
            cv.put(x + 2 + seed % 3, y, W)                     # a lit window
    if kind == "sail":
        for j in range(5):
            cv.rect(x, top - 1 - j, 7 - j, 1, D)
    elif kind == "curve":
        cv.rect(x + 1, top - 1, 5, 1, D)
        cv.rect(x + 2, top - 2, 3, 1, D)
    elif kind == "crown":
        cv.rect(x, top - 3, 7, 3, D)
        cv.rect(x + 1, top - 2, 5, 1, W)                       # the lit crown
        cv.rect(x + 3, top - 6, 1, 3, D)


def miami(cv):
    """Miami Beach at night: Brickell condos behind, the Freedom Tower and
    the art deco hotels on the street, palms, then the beach (sand, a
    lifeguard hut, umbrellas), the surf and moonlight on the water."""
    g = GROUND
    st = g - 12                                                # the street, above the sand
    for (x, top, kind, seed) in ((0, st - 40, "curve", 0), (8, st - 48, "sail", 1), (16, st - 36, "crown", 2),
                                 (40, st - 64, "crown", 0), (48, st - 46, "sail", 2),
                                 (88, st - 50, "curve", 1), (96, st - 66, "sail", 0), (104, st - 40, "crown", 1),
                                 (120, st - 44, "crown", 2), (128, st - 38, "curve", 0), (144, st - 46, "sail", 1),
                                 (152, st - 36, "curve", 2)):
        condo(cv, x, top, kind, seed, st)
    # the art deco hotels on Ocean Drive, in front of the condos
    def deco(cv, x, w, h, s):
        cv.rect(x, st - h, w, h, D)
        cv.rect(x + w // 2 - 2, st - h - 6, 4, 6, D)           # the fin
        cv.rect(x + w // 2 - 1, st - h - 9, 2, 3, D)
        cv.rect(x, st - h + 2, w, 1, W)                       # neon band
        for wx in range(x + 2, x + w - 2, 3):                 # eyebrow windows
            for wy in range(st - h + 5, st - 2, 4):
                cv.put(wx, wy, T if (wx + wy + s) % 4 else W)
    for args in ((2, 18, 16, 0), (116, 16, 12, 1), (138, 20, 18, 2)):
        in_front(cv, lambda c, a=args: deco(c, *a))
    # the Freedom Tower in front: an arcaded base, a tiered tower with corner
    # piers, a belfry, an octagonal domed cupola, a lantern and a finial
    cx = 70

    def freedom(cv):
        cv.rect(cx - 18, st - 12, 37, 12, D)
        for ax in range(cx - 16, cx + 17, 5):
            cv.rect(ax, st - 7, 3, 5, K)
            cv.put(ax + 1, st - 8, K)
        cv.rect(cx - 7, st - 42, 15, 30, D)
        for k, y in enumerate(range(st - 40, st - 14, 7)):
            cv.rect(cx - 8, y + 5, 17, 1, D)
            for wx in (cx - 4, cx, cx + 4):
                cv.rect(wx, y, 1, 3, T if (k + wx) % 3 else W)
        cv.rect(cx - 8, st - 42, 2, 30, D)
        cv.rect(cx + 7, st - 42, 2, 30, D)
        cv.rect(cx - 5, st - 49, 11, 7, D)                        # the belfry
        for wx in (cx - 3, cx + 2):
            cv.rect(wx, st - 47, 2, 4, K)
            cv.put(wx, st - 48, K)
        for j in range(6):                                        # the dome
            w = 11 - (j * j) // 3
            cv.rect(cx - w // 2, st - 50 - j, max(w, 1), 1, D)
        cv.rect(cx - 1, st - 58, 3, 3, D)                         # lantern
        cv.put(cx, st - 57, T)
        cv.rect(cx, st - 62, 1, 4, D)                             # finial

    in_front(cv, freedom)
    top = st - 62
    # the beach: dithered sand
    for y in range(st, g):
        for x in range(160):
            if (x + y) % 2 == 0 and (y > st + 1 or x % 4 == 0):
                cv.put(x, y, T)
    # palms on the sand, in front of the hotels
    def palm(px, h):
        for y in range(st + 4 - h, st + 4):
            cv.put(px + (1 if y < st + 4 - h * 2 // 3 else 0), y, D)
        t = st + 4 - h
        for dx, dy in ((-5, 3), (-4, 2), (-3, 1), (-2, 0), (-1, 0), (1, 0), (2, 0), (3, 1), (4, 2), (5, 3),
                       (-3, -1), (-2, -2), (2, -2), (3, -1), (0, -1), (1, -1), (-1, -1)):
            cv.put(px + 1 + dx, t + dy, D)
    for px, h in ((26, 24), (36, 18), (100, 22), (110, 17)):
        palm(px, h)
    # a lifeguard hut on stilts with its ramp and flag
    hx, hy = 124, g - 11
    cv.rect(hx, hy, 10, 5, W)
    for xx in range(hx + 1, hx + 9, 2):
        cv.rect(xx, hy + 1, 1, 3, D)                          # its stripes
    cv.rect(hx - 1, hy - 1, 12, 1, D)                         # the roof
    cv.rect(hx + 1, hy + 5, 1, 5, D)                          # stilts
    cv.rect(hx + 8, hy + 5, 1, 5, D)
    for k in range(5):
        cv.put(hx - 1 - k, hy + 5 + k, D)                     # the ramp
    cv.rect(hx + 5, hy - 5, 1, 4, D)
    cv.rect(hx + 6, hy - 5, 2, 2, W)                          # the flag
    # two beach umbrellas
    for ux in (46, 92):
        for j, w in enumerate((3, 7, 9)):
            for i in range(w):
                cv.put(ux - w // 2 + i, g - 10 + j, W if (i // 2) % 2 == 0 else D)
        cv.rect(ux, g - 7, 1, 6, D)
    return cx, top


# ---- layered cities (like Miami): a back layer of towers on the 8 px tile
# grid with fixed window patterns (so their tiles repeat), the capped landmark
# in front behind a lacquer gap, a street layer, and a river with reflections.
RIVER = GROUND - 8          # buildings stand here; the river runs below to y 111


def back_tower(cv, x, top, style, roof, base=RIVER):
    """One back-layer tower, 7 px wide at an 8 px column, windows by a fixed
    pattern per style so the same tiles repeat across the skyline."""
    cv.rect(x, top, 7, base - top, D)
    for y in range(top + 2, base - 1):
        for i in range(7):
            xx = x + i
            if style == "grid" and y % 3 == 0 and i % 2 == 1:
                cv.put(xx, y, T)
            elif style == "glass" and i in (1, 3, 5) and y % 4 != 0:
                cv.put(xx, y, T)
            elif style == "slab" and y % 3 == 1 and 0 < i < 6:
                cv.put(xx, y, T)                    # the apartment's balcony rows
            elif style == "lit" and y % 4 == 2 and i % 3 == 1:
                cv.put(xx, y, W)
    if roof == "setback":
        cv.rect(x + 1, top - 3, 5, 3, D)
        cv.rect(x + 2, top - 5, 3, 2, D)
    elif roof == "water":                           # a New York roof water tower
        cv.rect(x + 2, top - 4, 3, 3, D)
        cv.put(x + 2, top - 1, D)
        cv.put(x + 4, top - 1, D)
        cv.put(x + 3, top - 5, D)
    elif roof == "spire":
        cv.rect(x + 3, top - 8, 1, 8, D)
    elif roof == "number":                          # a Seoul apartment's rooftop sign
        cv.rect(x + 1, top - 2, 5, 2, D)
        cv.put(x + 3, top - 2, W)


def river(cv, ferry=None, depth=None):
    """The river from RIVER down: black water, lit reflections of the skyline
    above it broken into ripples (down to `depth` px; the rest is plain swell,
    whose tiles repeat), and swell lines."""
    cv.rect(0, RIVER, 160, 112 - RIVER, K)
    for d in range(1, depth or (112 - RIVER)):
        y = RIVER + d
        if d % 3 == 0:
            continue                                # a ripple gap
        for x in range(160):
            v = cv.get(x, RIVER - d)
            if v in (W, T) and (x + d) % 2 == 0:
                cv.put(x, y, T if d > 2 or v == T else W)
    for x in range(0, 160, 7):
        cv.rect(x, RIVER + 10, 3, 1, D)
    if ferry:
        fx, fy = ferry
        cv.rect(fx, fy, 14, 3, D)
        cv.rect(fx + 3, fy - 2, 7, 2, D)
        for k in range(4):
            cv.put(fx + 2 + k * 3, fy + 1, W)
        cv.put(fx + 5, fy - 1, W)
        cv.put(fx + 7, fy - 1, W)


def new_york(cv):
    """Lower Manhattan and Midtown at night over the East River: the back
    blocks, One WTC, the capped Empire State, One Vanderbilt, the Chrysler,
    270 Park; the Brooklyn Bridge's tower on the left, the Manhattan Bridge
    with its subway deck on the right (the train is a sprite)."""
    st = RIVER
    for (x, h, style, roof) in ((0, 30, "grid", "water"), (56, 36, "grid", "setback"), (144, 32, "grid", "water"),
                                (152, 24, "grid", "flat")):
        back_tower(cv, x, st - h, style, roof)
    # ordinary Midtown mass where the skyline thins: setbacks and water towers
    for (x, h, style, roof) in ((40, 44, "grid", "setback"), (48, 36, "glass", "water"),
                                (96, 40, "grid", "setback"), (104, 32, "lit", "water")):
        back_tower(cv, x, st - h, style, roof)
    # One World Trade Center (downtown, left)
    ox = 26

    def one_wtc(cv):
        for j in range(56):
            half = 6 - j * 4 // 56
            y = st - 1 - j
            cv.rect(ox + 6 - half, y, 2 * half + 1, 1, D)
            cv.put(ox + 6 - half + (j % 7 == 0), y, T)
            if j % 2 == 0:
                cv.put(ox + 6 + (j // 8) % 3 - 1, y, T)
        cv.rect(ox + 6, st - 68, 1, 12, D)

    in_front(cv, one_wtc)
    # the Empire State building, front and centre, capped
    cx = 72

    def empire(cv):
        cv.rect(cx - 15, st - 8, 30, 8, D)
        lit(cv, cx - 13, st - 6, cx + 14, st - 1, 3, 3, 4)
        cv.rect(cx - 10, st - 44, 20, 36, D)
        column(cv, cx - 8, cx + 9, st - 42, st - 9, 3, 1)
        cv.rect(cx - 8, st - 49, 16, 5, D)
        column(cv, cx - 6, cx + 7, st - 48, st - 45, 3, 2)
        cv.rect(cx - 6, st - 53, 12, 4, D)
        cv.rect(cx - 4, st - 57, 8, 4, D)
        cv.rect(cx - 3, st - 56, 6, 1, W)
        cv.rect(cx - 2, st - 61, 4, 4, D)
        cv.rect(cx - 1, st - 66, 2, 5, D)
        cv.put(cx - 1, st - 63, T)

    in_front(cv, empire)
    top = st - 66
    # One Vanderbilt: a tapering faceted glass tower, angled terraces, crown and spire
    vx = 90

    def vanderbilt(cv):
        for j in range(58):
            y = st - 1 - j
            w = 9 - j * 5 // 58
            cut = 1 if (j % 14) > 10 else 0                           # the angled terraces
            cv.rect(vx - w // 2 + cut, y, w - cut, 1, D)
            if j % 3 == 0:
                cv.put(vx - w // 2 + 1, y, T)                          # the lit facet
        cv.rect(vx, st - 66, 1, 8, D)                                  # the spire

    in_front(cv, vanderbilt)
    # the Chrysler building: the shaft, then terraced arches stacked in
    # sunbursts of triangular windows, the needle
    x = 112

    def chrysler(cv):
        cv.rect(x, st - 36, 15, 36, D)
        column(cv, x + 2, x + 14, st - 34, st - 2, 3, 5)
        for k, w in enumerate((15, 13, 11, 9, 7, 5)):
            y0 = st - 38 - k * 4
            x0 = x + (15 - w) // 2
            cv.rect(x0, y0, w, 4, D)
            for t in range(x0 + 1, x0 + w - 1, 2):
                cv.put(t, y0 + 2, W)                                   # the triangle windows
                cv.put(t, y0 + 1, W if (t - x0) % 4 == 1 else D)
            cv.put(x0, y0, K)                                          # the arch's rounded shoulder
            cv.put(x0 + w - 1, y0, K)
        cv.rect(x + 7, st - 66, 1, 4, D)                               # the needle

    in_front(cv, chrysler)
    # 270 Park Avenue: fan columns at the base, a stepped tower braced with
    # a diamond lattice, its crown
    jx = 130

    def jpmorgan(cv):
        steps = ((13, 18), (11, 16), (9, 14), (7, 10))
        y = st
        for (w, h) in steps:
            cv.rect(jx - w // 2, y - h, w, h, D)
            for yy in range(y - h, y):
                for xx in range(jx - w // 2, jx - w // 2 + w):
                    if (xx - jx + yy) % 6 == 0 or (xx - jx - yy) % 6 == 0:
                        cv.put(xx, yy, T)                              # the diamond lattice
            y -= h
        cv.rect(jx - 1, y - 4, 3, 4, D)                                # the crown
        for k in range(4):                                             # the fan columns
            cv.put(jx - 6 + k, st - 1 - k, K)
            cv.put(jx + 6 - k, st - 1 - k, K)

    in_front(cv, jpmorgan)
    # the Brooklyn Bridge's tower and cables, left
    tx = 12

    def bridge_tower(cv):
        cv.rect(tx - 7, st - 29, 15, 29, D)
        for ax in (tx - 5, tx + 1):
            cv.rect(ax, st - 22, 4, 12, K)
            cv.put(ax + 1, st - 23, K)
            cv.put(ax + 2, st - 23, K)
            cv.put(ax + 1, st - 24, K)
        cv.rect(tx - 8, st - 30, 17, 1, D)

    in_front(cv, bridge_tower)
    cv.rect(0, st - 8, 23, 1, T)
    for xx in range(0, 23, 3):
        cv.put(xx, st - 7, W)
    for k in (0, 2):
        for sign in (-1, 1):
            x0, y0 = tx + sign * 7, st - 29
            x1, y1 = max(0, min(22, tx + sign * (9 + k * 2))), st - 8 - k * 4
            n = max(abs(x1 - x0), abs(y1 - y0))
            for step in range(0, n + 1, 2):
                cv.put(round(x0 + (x1 - x0) * step / n), round(y0 + (y1 - y0) * step / n), T)
    river(cv, depth=8)                  # reflections in the first water row only: tile budget
    # the Manhattan Bridge over the river, right: a latticed tower, the truss
    # road deck, the subway deck below it (the train runs there as a sprite)
    mx = 146
    cv.rect(mx - 4, RIVER - 24, 2, 28, D)
    cv.rect(mx + 3, RIVER - 24, 2, 28, D)
    cv.rect(mx - 4, RIVER - 25, 9, 1, D)
    for y in range(RIVER - 22, RIVER - 2, 4):
        cv.put(mx - 1, y, D)
        cv.put(mx + 1, y + 2, D)
    cv.rect(96, MAN_DECK, 64, 1, T)                                   # the road deck
    for xx in range(96, 160, 2):
        cv.put(xx, MAN_DECK + 1, D if xx % 4 else T)                  # its truss
    cv.rect(96, MAN_DECK + 2, 64, 1, D)
    cv.rect(96, MAN_DECK + 6, 64, 1, T)                               # the subway deck
    for k in range(0, 46, 3):                                          # the main cable's sag
        cv.put(mx - 4 - k, RIVER - 25 + (k * k) // 110, T)
    return cx, top


MAN_DECK = RIVER + 1        # the Manhattan Bridge's road deck; the train runs 5 px under it


def seoul(cv):
    st = RIVER
    # the mountains behind (Bukhansan): a jagged far ridge in dither
    for x in range(160):
        h = 30 + int(8 * math.sin(x / 9.0) + 5 * math.sin(x / 3.7 + 1))
        for y in range(st - h + 1, st - 20):
            if (x + y) % 2 == 0:
                cv.put(x, y, D)
        cv.put(x, st - h, T)                                  # the moonlit ridge line
    # apartment slabs and glass towers on the tile grid
    for (x, h, style, roof) in ((0, 26, "slab", "number"), (8, 30, "slab", "number"), (16, 22, "slab", "flat"),
                                (96, 28, "slab", "number"), (104, 36, "glass", "flat"), (112, 24, "slab", "number"),
                                (144, 30, "slab", "number"), (152, 38, "glass", "spire")):
        back_tower(cv, x, st - h, style, roof)
    # Namsan: wooded, its trees as texture, the cable car line
    for i in range(76):
        h = int(28 * math.sin(math.pi * i / 76))
        x = 25 + i
        cv.rect(x, st - h, 1, h, D)
        if h > 2:
            cv.put(x, st - h - (1 if i % 3 == 0 else 0), D)      # tree crowns on the rim
        for y in range(st - h + 2, st, 3):
            if (x + y) % 4 == 0:
                cv.put(x, y, K)                                  # trees in the wood
    for k in range(0, 26):
        cv.put(34 + k, st - 6 - k * 22 // 26, T)                  # the cable car line
    cv.rect(46, st - 15, 3, 2, W)                                # its cabin
    # N Seoul Tower
    cx = 62
    cv.rect(cx - 6, st - 32, 13, 4, D)
    cv.rect(cx - 1, st - 52, 3, 20, W)
    cv.rect(cx - 3, st - 54, 7, 2, T)
    cv.rect(cx - 5, st - 58, 11, 4, D)
    cv.rect(cx - 4, st - 57, 9, 1, W)
    cv.rect(cx - 4, st - 59, 9, 1, T)
    cv.rect(cx - 3, st - 61, 7, 2, D)
    for y in range(st - 66, st - 61):
        cv.rect(cx, y, 1, 1, W if (y // 2) % 2 else D)
    top = st - 66
    # Lotte World Tower
    x = 124
    for j in range(64):
        w = max(2, 14 - j * 12 // 64)
        cv.rect(x + (14 - w) // 2, st - 1 - j, w, 1, D)
        cv.put(x + (14 - w) // 2, st - 1 - j, T)
        if j % 6 == 3:
            cv.rect(x + (14 - w) // 2 + 2, st - 1 - j, max(w - 4, 0), 1, T)
    cv.put(x + 7, st - 63, K)
    cv.put(x + 7, st - 62, K)
    # Namdaemun, on the street at the right of the hill
    gx = 100
    cv.rect(gx, st - 8, 22, 8, D)
    for yy in range(st - 7, st, 2):
        cv.rect(gx, yy, 22, 1, T if yy % 4 else D)
    cv.rect(gx + 9, st - 5, 4, 5, K)
    cv.put(gx + 10, st - 6, K)
    cv.put(gx + 11, st - 6, K)
    def roof(x0, w, y):
        cv.rect(x0, y, w, 2, D)
        cv.put(x0 - 1, y - 1, D)
        cv.put(x0 + w, y - 1, D)
        cv.rect(x0 + 2, y - 2, w - 4, 2, D)
    cv.rect(gx + 4, st - 12, 14, 4, D)
    roof(gx, 22, st - 14)
    cv.rect(gx + 6, st - 18, 10, 3, D)
    roof(gx + 3, 16, st - 20)
    river(cv)
    # Banpo Bridge: a long low deck across the Han, its fountain arcs falling
    cv.rect(0, RIVER + 3, 160, 1, T)
    for x in range(4, 160, 12):
        cv.rect(x, RIVER + 4, 1, 4, D)                       # piers
        for k in range(5):
            cv.put(x + 2 + k, RIVER + 4 + (k * k) // 4, W if k % 2 == 0 else T)
    return cx, top


def in_front(cv, draw):
    """Draw a front silhouette on its own layer, then lay it on the picture
    with a one-pixel lacquer outline, so the towers behind stay whole and the
    silhouette still reads in front of them."""
    layer = Canvas(160, 144, None)
    draw(layer)
    for y in range(144):
        for x in range(160):
            if layer.px[y][x] is None:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < 160 and 0 <= yy < 144 and layer.px[yy][xx] is None:
                    cv.put(xx, yy, K)
    cv.blit(layer, 0, 0)


def onion(cv, cx, base_y, w, drum):
    """An onion dome of St Basil's: a drum, the bulb with twisted stripes, a
    small cross on top."""
    cv.rect(cx - w // 2 + 1, base_y - drum, w - 2, drum, D)              # the drum
    widths = [1, 1, 3, max(w - 2, 3), w, w, max(w - 2, 3), 3]
    top = base_y - drum - len(widths)
    for j, ww in enumerate(widths):
        for i in range(ww):
            x = cx - ww // 2 + i
            y = top + j
            cv.put(x, y, W if (x + y) % 3 == 0 else D)                    # the twisted stripes
    cv.rect(cx, top - 3, 1, 3, W)                                          # the cross
    cv.rect(cx - 1, top - 2, 3, 1, W)


def moscow(cv):
    st = RIVER
    # Moscow-City behind on the right: glass towers, the Federation's spire,
    # Mercury's stepped crown, Evolution's twist
    for (x, h, style, roof) in ((96, 38, "glass", "flat"), (104, 50, "lit", "flat"), (120, 44, "glass", "setback"),
                                (128, 30, "grid", "flat"), (144, 40, "glass", "flat"), (152, 28, "grid", "flat")):
        back_tower(cv, x, st - h, style, roof)
    fx = 112                                                            # Federation, the tallest
    cv.rect(fx, st - 60, 7, 60, D)
    for y in range(st - 58, st, 2):
        cv.rect(fx + 1, y, 5, 1, T if y % 4 else D)
    cv.rect(fx + 3, st - 70, 1, 10, D)
    ex = 136                                                            # Evolution, twisting
    cv.rect(ex, st - 46, 7, 46, D)
    for y in range(st - 46, st, 3):
        k = (y // 3) % 6
        cv.rect(ex + (k if k < 4 else 6 - k), y, 3, 1, T)
    # Moscow State University behind on the left: a Seven Sister
    mx = 22
    cv.rect(mx - 20, st - 14, 41, 14, D)                               # the wings
    for wx in (mx - 16, mx + 16):                                       # their towers
        cv.rect(wx - 2, st - 24, 5, 10, D)
        cv.rect(wx, st - 28, 1, 4, D)
    cv.rect(mx - 6, st - 32, 13, 18, D)                                 # the central tower
    cv.rect(mx - 4, st - 40, 9, 8, D)
    cv.rect(mx - 2, st - 46, 5, 6, D)
    cv.rect(mx, st - 54, 1, 8, D)                                       # its spire
    cv.put(mx, st - 55, T)                                              # and star
    lit(cv, mx - 18, st - 12, mx + 19, st - 2, 3, 4, 2)
    column(cv, mx - 4, mx + 5, st - 30, st - 15, 3, 1)
    # the Kremlin wall with its swallow-tail battlements

    def kremlin_wall(cv):
        cv.rect(40, st - 10, 72, 10, D)
        for x in range(40, 112, 4):
            cv.rect(x, st - 13, 3, 3, D)
            cv.put(x + 1, st - 13, K)                                       # the swallow tail

    in_front(cv, kremlin_wall)
    # the Spasskaya Tower, front and centre: body, clock, tiers, tent, star
    cx = 76

    def spasskaya(cv):
        cv.rect(cx - 6, st - 32, 13, 32, D)                                 # the body
        cv.rect(cx - 1, st - 8, 3, 8, K)                                    # the gate
        cv.put(cx, st - 9, K)
        for y in range(st - 30, st - 12, 4):
            cv.put(cx - 4, y, T)
            cv.put(cx + 4, y, T)
        cv.rect(cx - 7, st - 33, 15, 1, D)                                  # cornice
        cv.rect(cx - 5, st - 42, 11, 9, D)                                  # the clock tier
        for (dx, dy) in ((0, -2), (-2, 0), (2, 0), (0, 2), (-1, -1), (1, -1), (-1, 1), (1, 1)):
            cv.put(cx + dx, st - 38 + dy, W)                                # the clock face
        cv.put(cx, st - 38, T)
        for px in (cx - 6, cx + 6):                                         # pinnacles
            cv.rect(px, st - 46, 1, 4, D)
        cv.rect(cx - 4, st - 47, 9, 5, D)                                   # the octagonal tiers
        cv.rect(cx - 3, st - 50, 7, 3, D)
        cv.put(cx - 1, st - 49, T)
        cv.put(cx + 1, st - 49, T)
        for j in range(10):                                                 # the tented spire
            w = 7 - (j * 6) // 10
            cv.rect(cx - w // 2, st - 51 - j, max(w, 1), 1, D)
        cv.rect(cx, st - 62, 1, 1, D)
        star = G.g("""
    ..w..
    wwwww
    .www.
    .w.w.
    """)
        cv.glyph(cx - 2, st - 66, star)                                     # the red star

    in_front(cv, spasskaya)
    top = st - 66
    # St Basil's Cathedral, front right on Red Square
    bx = 124

    def basil(cv):
        cv.rect(bx - 14, st - 10, 29, 10, D)                                # its arcaded base
        for ax in range(bx - 12, bx + 14, 4):
            cv.rect(ax, st - 7, 2, 4, K)
            cv.put(ax, st - 8, K)
        for j in range(22):                                                 # the central tent
            w = 7 - (j * 6) // 22
            cv.rect(bx - w // 2, st - 11 - j, max(w, 1), 1, D)
            if j % 4 == 1:
                cv.put(bx, st - 11 - j, T)
        onion(cv, bx, st - 33, 5, 2)                                        # its small top dome
        onion(cv, bx - 9, st - 10, 7, 6)                                    # the onion domes round it
        onion(cv, bx + 9, st - 10, 7, 4)
        onion(cv, bx - 4, st - 10, 5, 12)
        onion(cv, bx + 5, st - 10, 5, 10)

    in_front(cv, basil)
    river(cv)
    # Bolshoy Moskvoretsky Bridge: one long low arch over the Moskva
    cv.rect(0, RIVER + 1, 56, 2, D)
    for x in range(0, 56):
        a = 4 - int(4 * math.sin(math.pi * x / 56))
        cv.put(x, RIVER + 3 + (4 - a), T)                               # the arch
    for x in range(2, 56, 4):
        cv.put(x, RIVER, W)                                             # its lamps
    return cx, top


DRAW = {"MIAMI": miami, "NEW YORK": new_york, "SEOUL": seoul, "MOSCOW": moscow}





# ---- fireworks (sprites) ---------------------------------------------------
def burst_quadrant(phase):
    """The top-left quadrant of a 16x16 burst; flips give the other three.
    phase 0: a small spark, 1: rays out, 2: fading tips."""
    cv = Canvas(8, 8, T)
    cx = cy = 7.5
    for a in (0, 22.5, 45, 67.5, 90):
        dx, dy = -math.cos(math.radians(a)), -math.sin(math.radians(a))
        if phase == 0:
            for r in (1.5, 2.5):
                cv.put(int(cx + dx * r), int(cy + dy * r), W)
        elif phase == 1:
            for r in (3, 4, 5):
                cv.put(int(cx + dx * r), int(cy + dy * r), D if r < 5 else W)
            cv.put(int(cx + dx * 6.5), int(cy + dy * 6.5), W)
        else:
            cv.put(int(cx + dx * 7), int(cy + dy * 7), W if a in (0, 45, 90) else D)
    return cv.tile(0, 0)


def shell_tile():
    cv = Canvas(8, 8, T)
    cv.put(3, 2, W)
    cv.put(3, 3, W)
    cv.put(3, 4, D)
    cv.put(3, 6, D)
    return cv.tile(0, 0)


TRAIN = {
"TRAIN_CAR": G.g("""
........
........
XXXXXXXX
owwowwoX
owwowwoX
ooooooox
.X....X.
........
"""),
"TRAIN_NOSE": G.g("""
........
........
..XXXXXX
.owwowoX
owwwowoX
ooooooox
.X....X.
........
"""),
}


def train_bank():
    out = {}
    for name, rows in TRAIN.items():
        cv = Canvas(8, 8, T)
        cv.glyph(0, 0, [r.replace("x", "X") for r in rows])
        out[name] = cv.tile(0, 0)
    return out


def train(s, frame):
    """The subway on the Manhattan Bridge's lower deck, heading west: a nose
    and three cars, 10 px further each frame; the code drops a car once it
    passes the bridge's west end (x < 96) or the screen's right edge."""
    tb = train_bank()
    x0 = 108 + (3 - frame) * 10
    for k in range(4):
        x = x0 + k * 8
        if 96 <= x <= 152:
            s.spr(x, MAN_DECK - 1, tb["TRAIN_NOSE" if k == 0 else "TRAIN_CAR"])


def firework_bank():
    return {"FW_SPARK": burst_quadrant(0), "FW_BURST": burst_quadrant(1),
            "FW_FADE": burst_quadrant(2), "FW_SHELL": shell_tile()}


# where the bursts go (centre x, centre y) and when: a 4-frame loop, each
# burst one phase behind the last; -1 = a rising shell under it
BURSTS = {                 # burst centres per city, found clear of every building
    "MIAMI": ((24, 32), (136, 34)),
    "NEW YORK": ((20, 36), (102, 32), (144, 36)),
    "SEOUL": ((20, 36), (80, 36), (142, 36)),
    "MOSCOW": ((12, 36), (92, 36), (140, 32)),
}
SCHEDULE = (            # frame -> phase per burst (None = not shown, "S" = shell)
    (0, "S", None),
    (1, 0, "S"),
    (2, 1, 0),
    (None, 2, 1),
)


def fireworks(s, fw, frame, city):
    for (cx, cy), ph in zip(BURSTS[city], SCHEDULE[frame]):
        if ph is None:
            continue
        if ph == "S":
            s.spr(cx - 4, cy + 10, fw["FW_SHELL"])
            continue
        t = fw[("FW_SPARK", "FW_BURST", "FW_FADE")[ph]]
        s.spr(cx - 8, cy - 8, t)
        s.spr(cx, cy - 8, t, fx=True)
        s.spr(cx - 8, cy, t, fy=True)
        s.spr(cx, cy, t, fx=True, fy=True)


# ---- the screen ------------------------------------------------------------
def check_sky(s):
    """No firework pixel may land on a building, the cap or the text: only on
    black sky or a star."""
    for (x, y, t) in s.sprites:
        for j in range(8):
            for i in range(8):
                if t[j * 8 + i] not in (None, T):
                    v = s.bg.get(x + i, y + j)
                    assert v == K or (x + i, y + j) in STARS, \
                        "%s: a firework sprite covers the picture at (%d, %d)" % (s.name, x + i, y + j)


def win_screen(city, frame=2, bake=True):
    s = Screen("w_%s" % city.lower().replace(" ", "_"), "Win over %s" % city)
    cv = s.bg
    cv.rect(0, 0, 160, 144, K)
    for (x, y) in STARS:
        cv.put(x, y, W if (x + y) % 3 else T)
    moon(cv, 148, 2)
    big_text(cv, (160 - big_width("YOU WIN", 2)) // 2, 6, "YOU WIN", scale=2, fg=W, outline=K, shadow=D)
    cx, top = DRAW[city](cv)
    fool_cap(cv, cx, top + 1)
    if city == "MIAMI":
        water(cv)
        surf(cv)
    if bake:
        lq = lacquer_font_bank()
        for i, ch in enumerate(city):
            cv.set_tile((20 - len(city)) // 2 + i, NAME_ROW, lq["LQ_" + ch])
    text(cv, (20 - 12) // 2 * 8, (NAME_ROW + 1) * 8, "IS THE DURAK", T)
    button(cv, 24, 136, "BTN_A")
    text(cv, 32, 136, "REMATCH", W)
    button(cv, 96, 136, "BTN_B")
    text(cv, 104, 136, "HOME", W)
    fireworks(s, firework_bank(), frame, city)
    check_sky(s)                       # the fireworks only; the train rides the bridge
    if city == "NEW YORK":
        train(s, frame)
    return s


def win_screens(bake=True, frame=2):
    return [win_screen(c, frame, bake) for c in CITIES]
