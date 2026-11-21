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


# ---- the four cities -------------------------------------------------------
def miami(cv):
    g = GROUND
    # art deco hotels along Ocean Drive, left and right
    def deco(x, w, h, fins):
        cv.rect(x, g - h, w, h, D)
        cv.rect(x + 2, g - h - 4, w - 4, 4, D)              # stepped top
        cv.rect(x + w // 2 - 1, g - h - 9, 2, 5, D)         # a fin
        for f in range(1, fins + 1):
            cv.rect(x + f * w // (fins + 1), g - h, 1, h - 2, K)
        cv.rect(x, g - h + 3, w, 1, W)                      # neon band
        lit(cv, x + 1, g - h + 6, x + w - 1, g - 2, 2, 3, x)
    deco(4, 22, 22, 3)
    deco(118, 20, 18, 2)
    deco(140, 18, 26, 2)
    # Freedom Tower: shaft, tiers, cupola, lantern; the tallest
    x = 62
    cv.rect(x, g - 44, 18, 44, D)
    cv.rect(x - 6, g - 18, 30, 18, D)                       # the base block
    lit(cv, x - 4, g - 15, x + 23, g - 2, 3, 4, 1)
    lit(cv, x + 2, g - 41, x + 17, g - 20, 3, 4, 2)
    cv.rect(x + 2, g - 52, 14, 8, D)
    cv.rect(x + 4, g - 50, 2, 4, T)
    cv.rect(x + 8, g - 50, 2, 4, T)
    cv.rect(x + 12, g - 50, 2, 4, T)
    for j in range(6):                                      # the cupola
        w = 12 - j * 2
        cv.rect(x + 9 - w // 2, g - 53 - j, w, 1, D)
    cv.rect(x + 8, g - 62, 2, 4, D)                         # lantern
    top = g - 62
    # palms along the beach
    def palm(px, h):
        for y in range(g - h, g):
            cv.put(px + (1 if y < g - h * 2 // 3 else 0), y, D)
        t = g - h
        for dx, dy in ((-5, 3), (-4, 2), (-3, 1), (-2, 0), (-1, 0), (1, 0), (2, 0), (3, 1), (4, 2), (5, 3),
                       (-3, -1), (-2, -2), (2, -2), (3, -1), (0, -1), (1, -1), (-1, -1)):
            cv.put(px + 1 + dx, t + dy, D)
    for px, h in ((34, 26), (44, 20), (100, 24), (110, 18)):
        palm(px, h)
    return x + 9, top


def new_york(cv):
    g = GROUND
    # filler blocks
    for (x, w, h, s) in ((0, 14, 30, 0), (14, 10, 22, 1), (100, 12, 34, 2), (146, 14, 24, 3), (28, 12, 40, 4)):
        cv.rect(x, g - h, w, h, D)
        lit(cv, x + 1, g - h + 2, x + w - 1, g - 2, 3, 4, s)
    # the Empire State building, the tallest, mast up
    x = 52
    cv.rect(x, g - 40, 26, 40, D)
    cv.rect(x + 3, g - 50, 20, 10, D)
    cv.rect(x + 6, g - 57, 14, 7, D)
    cv.rect(x + 9, g - 62, 8, 5, D)
    cv.rect(x + 11, g - 66, 4, 4, D)
    cv.rect(x + 12, g - 74, 2, 8, D)
    lit(cv, x + 2, g - 38, x + 25, g - 2, 3, 4, 5)
    lit(cv, x + 5, g - 48, x + 22, g - 41, 3, 4, 6)
    cv.rect(x + 12, g - 66, 2, 1, W)                       # the lit crown
    top = g - 74
    # the Chrysler building: arched crown with triangle windows, needle
    x = 116
    cv.rect(x, g - 44, 18, 44, D)
    lit(cv, x + 2, g - 42, x + 17, g - 2, 3, 4, 7)
    for k in range(5):                                      # crown arcs
        w = 18 - k * 4
        cv.rect(x + (18 - w) // 2, g - 46 - k * 3, w, 3, D)
        for t in range(x + (18 - w) // 2 + 1, x + (18 - w) // 2 + w - 1, 3):
            cv.put(t, g - 45 - k * 3, W)
    cv.rect(x + 8, g - 66, 2, 6, D)
    # Brooklyn Bridge tower and cables, low on the left
    cv.rect(4, g - 16, 6, 16, D)
    cv.put(6, g - 13, K)
    cv.put(7, g - 13, K)
    for k in range(0, 28):
        y = g - 14 + round(k * k / 70)
        cv.put(10 + k, min(y, g - 1), T)
    return 52 + 13, top


def seoul(cv):
    g = GROUND
    # city blocks
    for (x, w, h, s) in ((0, 12, 22, 0), (12, 10, 16, 1), (96, 10, 26, 2), (106, 12, 18, 3), (150, 10, 20, 4)):
        cv.rect(x, g - h, w, h, D)
        lit(cv, x + 1, g - h + 2, x + w - 1, g - 2, 3, 4, s)
    # Namsan with N Seoul Tower, the highest point
    for i in range(70):
        h = int(26 * math.sin(math.pi * i / 70))
        cv.rect(26 + i, g - h, 1, h, D)
    tx = 61
    cv.rect(tx - 1, g - 64, 3, 40, T)                      # the lit shaft
    cv.rect(tx - 5, g - 62, 11, 6, D)                      # the pod
    cv.rect(tx - 4, g - 60, 9, 1, W)
    cv.rect(tx - 3, g - 64, 7, 2, D)
    cv.rect(tx, g - 74, 1, 10, D)                          # mast
    top = g - 74
    # Lotte World Tower: a tall taper, lit seams
    x = 126
    for j in range(60):
        w = 14 - j * 10 // 60
        cv.rect(x + (14 - w) // 2, g - 1 - j, w, 1, D)
    for j in range(0, 56, 4):
        cv.put(x + 7, g - 2 - j, T)
    cv.rect(x + 5, g - 64, 4, 4, D)
    # Namdaemun gate: the curved roof, bottom left
    cv.rect(4, g - 6, 22, 6, D)
    cv.rect(13, g - 4, 4, 4, K)
    for i in range(30):
        y = g - 10 - (2 if i in (0, 29) else (1 if i in (1, 28) else 0))
        cv.put(i, y, T)
    cv.rect(2, g - 9, 26, 3, D)
    cv.rect(7, g - 13, 16, 3, D)
    return tx, top


def warsaw(cv):
    g = GROUND
    # Old Town tenements: narrow houses, steep gables
    x = 0
    for k, (w, h) in enumerate(((8, 20), (7, 24), (9, 18), (7, 22), (8, 19))):
        cv.rect(x, g - h, w, h, D)
        tri(cv, x + w // 2, g - h - w // 2, w // 2 + 1, D)
        lit(cv, x + 2, g - h + 3, x + w - 1, g - 2, 3, 4, k)
        x += w
    # Sigismund's Column
    cv.rect(44, g - 34, 3, 34, T)
    cv.rect(43, g - 37, 5, 3, T)
    cv.rect(44, g - 41, 3, 4, W)
    # the Palace of Culture and Science, spire up: the landmark
    x = 64
    cv.rect(x, g - 18, 40, 18, D)
    cv.rect(x + 5, g - 32, 30, 14, D)
    cv.rect(x + 9, g - 44, 22, 12, D)
    cv.rect(x + 12, g - 52, 16, 8, D)
    cv.rect(x + 15, g - 58, 10, 6, D)
    cv.rect(x + 18, g - 62, 4, 4, D)
    cv.rect(x + 19, g - 74, 2, 12, T)
    for (a, b, c, d, s) in ((x + 2, g - 15, x + 39, g - 2, 1), (x + 7, g - 30, x + 34, g - 19, 2),
                            (x + 11, g - 42, x + 30, g - 33, 3), (x + 14, g - 50, x + 27, g - 45, 4)):
        lit(cv, a, b, c, d, 3, 4, s)
    cv.rect(x + 18, g - 56, 4, 3, W)                       # the clock
    top = g - 74
    # Varso and modern towers, right
    for (xx, w, h, s) in ((118, 12, 40, 5), (132, 10, 30, 6), (144, 14, 46, 7)):
        cv.rect(xx, g - h, w, h, D)
        lit(cv, xx + 1, g - h + 2, xx + w - 1, g - 2, 3, 4, s)
    cv.rect(150, g - 56, 2, 10, D)
    return x + 20, top


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
BURSTS = ((24, 40), (104, 32), (140, 56))
SCHEDULE = (            # frame -> phase per burst (None = not shown, "S" = shell)
    (0, "S", None),
    (1, 0, "S"),
    (2, 1, 0),
    (None, 2, 1),
)


def fireworks(s, fw, frame):
    for (cx, cy), ph in zip(BURSTS, SCHEDULE[frame]):
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
    if bake:
        lq = lacquer_font_bank()
        for i, ch in enumerate(city):
            cv.set_tile((20 - len(city)) // 2 + i, NAME_ROW, lq["LQ_" + ch])
    text(cv, (20 - 12) // 2 * 8, (NAME_ROW + 1) * 8, "IS THE DURAK", T)
    button(cv, 24, 136, "BTN_A")
    text(cv, 32, 136, "REMATCH", W)
    button(cv, 96, 136, "BTN_B")
    text(cv, 104, 136, "HOME", W)
    fireworks(s, firework_bank(), frame)
    return s


def win_screens(bake=True, frame=2):
    return [win_screen(c, frame, bake) for c in CITIES]
