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
TILESET_MAX = LACQUER_BASE
NAME_ROW = 14          # the bot's name, columns (20 - len) / 2, lacquer font
GROUND = 104           # the skyline stands on this y; water below to y 111
CITIES = ("MIAMI", "NEW YORK", "SEOUL", "WARSAW")

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
    def deco(x, w, h, s):
        cv.rect(x - 1, st - h - 10, w + 2, h + 10, K)          # a lacquer gap: in front
        cv.rect(x, st - h, w, h, D)
        cv.rect(x + w // 2 - 2, st - h - 6, 4, 6, D)           # the fin
        cv.rect(x + w // 2 - 1, st - h - 9, 2, 3, D)
        cv.rect(x, st - h + 2, w, 1, W)                       # neon band
        for wx in range(x + 2, x + w - 2, 3):                 # eyebrow windows
            for wy in range(st - h + 5, st - 2, 4):
                cv.put(wx, wy, T if (wx + wy + s) % 4 else W)
    deco(2, 18, 16, 0)
    deco(116, 16, 12, 1)
    deco(138, 20, 18, 2)
    # the Freedom Tower in front: an arcaded base, a tiered tower with corner
    # piers, a belfry, an octagonal domed cupola, a lantern and a finial
    cx = 70
    cv.rect(cx - 19, st - 13, 39, 13, K)
    cv.rect(cx - 9, st - 64, 19, 52, K)
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


def new_york(cv):
    g = GROUND
    for (x, w, h, s) in ((40, 10, 26, 1), (92, 9, 30, 2), (148, 12, 22, 3)):
        filler(cv, x, w, h, s)
    # the Brooklyn Bridge: a stone tower with twin pointed arches, cable fans
    tx = 18
    cv.rect(tx - 7, g - 36, 15, 36, D)
    for ax in (tx - 5, tx + 1):
        cv.rect(ax, g - 28, 4, 14, K)
        cv.put(ax + 1, g - 29, K)
        cv.put(ax + 2, g - 29, K)
        cv.put(ax + 1, g - 30, K)
    cv.rect(tx - 8, g - 37, 17, 1, D)                      # its cornice
    cv.rect(0, g - 12, 40, 1, T)                           # the deck
    for k in range(4):                                     # cable fans to the deck
        for sign in (-1, 1):
            x0, y0 = tx + sign * 7, g - 35
            x1, y1 = tx + sign * (10 + k * 6), g - 12
            n = max(abs(x1 - x0), abs(y1 - y0))
            for step in range(0, n + 1, 2):
                cv.put(round(x0 + (x1 - x0) * step / n), round(y0 + (y1 - y0) * step / n), T)
    # the Empire State building: a podium, a tall slim shaft with vertical
    # piers, setbacks, the observation deck, a long mast
    cx = 72
    cv.rect(cx - 15, g - 8, 30, 8, D)                      # podium
    lit(cv, cx - 13, g - 6, cx + 14, g - 1, 3, 3, 4)
    cv.rect(cx - 10, g - 44, 20, 36, D)                    # the shaft
    column(cv, cx - 8, cx + 9, g - 42, g - 9, 3, 1)
    cv.rect(cx - 8, g - 49, 16, 5, D)                      # setbacks
    column(cv, cx - 6, cx + 7, g - 48, g - 45, 3, 2)
    cv.rect(cx - 6, g - 53, 12, 4, D)
    cv.rect(cx - 4, g - 57, 8, 4, D)                       # observation floors
    cv.rect(cx - 3, g - 56, 6, 1, W)
    cv.rect(cx - 2, g - 61, 4, 4, D)                       # the crown
    cv.rect(cx - 1, g - 74, 2, 13, D)                      # the mast
    cv.put(cx - 1, g - 66, T)
    # the Chrysler building: terraced arches with sunburst windows, the needle
    x = 118
    cv.rect(x, g - 40, 16, 40, D)
    column(cv, x + 2, x + 15, g - 38, g - 2, 3, 5)
    for k in range(5):
        w = 16 - k * 3
        y0 = g - 42 - k * 4
        cv.rect(x + (16 - w) // 2, y0, w, 4, D)
        for t in range(x + (16 - w) // 2 + 1, x + (16 - w) // 2 + w - 1, 2):
            cv.put(t, y0 + 1, W)                           # the sunburst triangles
    cv.rect(x + 7, g - 72, 2, 10, D)                       # the needle
    cv.put(x + 7, g - 74, D)
    return cx, g - 74


def seoul(cv):
    g = GROUND
    for (x, w, h, s) in ((96, 10, 22, 2), (106, 12, 16, 3), (150, 10, 18, 4)):
        filler(cv, x, w, h, s)
    # Namsan: a wooded hill, tree crowns along its rim
    for i in range(80):
        h = int(26 * math.sin(math.pi * i / 80))
        cv.rect(22 + i, g - h, 1, h, D)
        if h > 2 and i % 3 == 0:
            cv.put(22 + i, g - h - 1, D)                   # the trees' bumps
    # N Seoul Tower: its foot building, a slim white shaft, the barrel pod,
    # a long striped antenna
    cx = 62
    cv.rect(cx - 6, g - 30, 13, 4, D)                      # the foot building
    cv.rect(cx - 1, g - 52, 3, 22, W)                      # the shaft
    cv.rect(cx - 3, g - 54, 7, 2, T)                       # pod: base ring
    cv.rect(cx - 5, g - 58, 11, 4, D)                      # the observation barrel
    cv.rect(cx - 4, g - 57, 9, 1, W)                       # its window band
    cv.rect(cx - 4, g - 59, 9, 1, T)
    cv.rect(cx - 3, g - 61, 7, 2, D)                       # the roof
    for y in range(g - 74, g - 61):                        # the antenna, striped
        w = 2 if y > g - 68 else 1
        cv.rect(cx, y, w, 1, W if (y // 2) % 2 else D)
    # Lotte World Tower: a smooth slender cone, split at the crown
    x = 132
    for j in range(64):
        w = max(2, 14 - j * 12 // 64)
        cv.rect(x + (14 - w) // 2, g - 1 - j, w, 1, D)
        cv.put(x + (14 - w) // 2, g - 1 - j, T)            # the lit edge
    cv.put(x + 7, g - 63, K)
    cv.put(x + 7, g - 62, K)                               # the split crown
    # Namdaemun: a stone base with its arched gate, a two-tier hipped roof
    gx = 2
    cv.rect(gx, g - 8, 28, 8, D)
    for yy in range(g - 7, g, 2):
        cv.rect(gx, yy, 28, 1, T if yy % 4 else D)         # stone courses
    cv.rect(gx + 12, g - 5, 4, 5, K)
    cv.put(gx + 13, g - 6, K)
    cv.put(gx + 14, g - 6, K)
    def roof(x0, w, y):
        cv.rect(x0, y, w, 2, D)
        cv.put(x0 - 1, y - 1, D)                           # upturned eaves
        cv.put(x0 + w, y - 1, D)
        cv.rect(x0 + 2, y - 2, w - 4, 2, D)
    cv.rect(gx + 6, g - 12, 16, 4, D)                      # lower hall
    roof(gx + 2, 24, g - 14)
    cv.rect(gx + 9, g - 19, 10, 3, D)                      # upper hall
    roof(gx + 6, 16, g - 21)
    return cx, g - 74


def warsaw(cv):
    g = GROUND
    # Old Town: tall narrow houses with steep gables
    x = 0
    for k, (w, h) in enumerate(((6, 22), (5, 26), (6, 20), (5, 24))):
        cv.rect(x, g - h, w, h, D)
        tri(cv, x + w // 2, g - h - w // 2, w // 2 + 1, D)
        lit(cv, x + 1, g - h + 3, x + w - 1, g - 2, 2, 4, k)
        x += w
    # the Royal Castle with its clock tower and spire
    cv.rect(22, g - 16, 18, 16, D)
    lit(cv, 23, g - 14, 39, g - 2, 3, 4, 3)
    cv.rect(27, g - 30, 6, 14, D)
    cv.put(29, g - 26, W)
    cv.put(30, g - 26, W)
    tri(cv, 30, g - 38, 8, D, 0.4)
    # Sigismund's Column: a plinth, the slim column, the king with his cross
    sx = 46
    cv.rect(sx - 2, g - 6, 5, 6, D)
    cv.rect(sx, g - 36, 1, 30, T)
    cv.rect(sx - 1, g - 37, 3, 1, T)
    cv.rect(sx, g - 41, 1, 4, W)
    cv.rect(sx - 1, g - 40, 3, 1, W)
    cv.rect(sx + 1, g - 44, 1, 4, W)                       # the cross
    cv.rect(sx, g - 43, 3, 1, W)
    # the Palace of Culture and Science: wide wings, a stepped tower with
    # pinnacles at every tier, the clock, the long spire
    cx = 86
    cv.rect(cx - 26, g - 12, 53, 12, D)                    # the wings
    for px in (cx - 26, cx - 20, cx + 20, cx + 26):        # their corner pinnacles
        cv.rect(px, g - 16, 1, 4, D)
    lit(cv, cx - 24, g - 10, cx + 26, g - 2, 3, 4, 5)
    tiers = ((24, 18), (18, 12), (14, 10), (10, 6), (6, 4))
    y = g - 12
    for k, (w, h) in enumerate(tiers):
        cv.rect(cx - w // 2, y - h, w, h, D)
        cv.put(cx - w // 2, y - h - 1, D)                  # pinnacles
        cv.put(cx + w // 2 - 1, y - h - 1, D)
        cv.put(cx - w // 2, y - h - 2, D)
        cv.put(cx + w // 2 - 1, y - h - 2, D)
        if h > 6:
            column(cv, cx - w // 2 + 2, cx + w // 2 - 1, y - h + 1, y - 1, 3, k)
        y -= h
    cv.rect(cx - 1, y + 2, 2, 2, W)                        # the clock
    for yy in range(g - 74, y):                            # the spire
        cv.rect(cx - (1 if yy > y - 6 else 0), yy, 2 if yy > y - 6 else 1, 1, D)
    # Varso and the new towers
    for (xx, w, h, s) in ((124, 10, 36, 6), (136, 9, 28, 7), (146, 13, 44, 8)):
        filler(cv, xx, w, h, s)
    cv.rect(152, g - 56, 1, 12, D)
    return cx, g - 74


DRAW = {"MIAMI": miami, "NEW YORK": new_york, "SEOUL": seoul, "WARSAW": warsaw}



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


def firework_bank():
    return {"FW_SPARK": burst_quadrant(0), "FW_BURST": burst_quadrant(1),
            "FW_FADE": burst_quadrant(2), "FW_SHELL": shell_tile()}


# where the bursts go (centre x, centre y) and when: a 4-frame loop, each
# burst one phase behind the last; -1 = a rising shell under it
BURSTS = {                 # burst centres per city, found clear of every building
    "MIAMI": ((24, 32), (136, 34)),
    "NEW YORK": ((20, 40), (86, 36), (140, 40)),
    "SEOUL": ((20, 40), (80, 40), (150, 40)),
    "WARSAW": ((20, 40), (70, 40), (140, 40)),
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
    water(cv)
    if city == "MIAMI":
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
    check_sky(s)
    return s


def win_screens(bake=True, frame=2):
    return [win_screen(c, frame, bake) for c in CITIES]
