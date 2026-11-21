# Full-screen pictures: the dashboard (title + bot picker), win and lose.
# Each one ships as its own png2asset map (tileset + 20x18 map), so the
# only limit is its unique tile count (checked in gen_art.py, under 256
# with room left for the sprites).

import glyphs as G
from gen_art import Canvas, Screen, T, W, D, K, card_face, card_back, flip, ui_bank, lacquer_font_bank

# ---- fonts ---------------------------------------------------------------


def text(cv, x, y, s, fg, bg=None, icons=None):
    """Draw text with the 5x7 font, one 8px cell per character."""
    for i, ch in enumerate(s):
        if bg is not None:
            cv.rect(x + 8 * i, y, 8, 8, bg)
        if ch != " ":
            cv.glyph(x + 8 * i + 1, y + 1, G.FONT[ch], fg)


def big_text(cv, x, y, s, scale=3, fg=K, outline=W, shadow=D):
    """The title lettering: the 5x7 font at 3x, outlined, with a drop shadow."""
    layer = Canvas(cv.w, cv.h, None)
    for i, ch in enumerate(s):
        rows = G.FONT.get(ch, [])
        for j, r in enumerate(rows):
            for k, c in enumerate(r):
                if c == "X":
                    layer.rect(x + i * (5 * scale + scale) + k * scale, y + j * scale, scale, scale, fg)
    ink = [(xx, yy) for yy in range(cv.h) for xx in range(cv.w) if layer.px[yy][xx] == fg]
    inkset = set(ink)
    for (xx, yy) in ink:                       # shadow, down-right 2px
        for dx, dy in ((2, 2), (1, 2), (2, 1)):
            if (xx + dx, yy + dy) not in inkset:
                cv.put(xx + dx, yy + dy, shadow)
    for (xx, yy) in ink:                       # outline
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if (xx + dx, yy + dy) not in inkset and cv.get(xx + dx, yy + dy) != shadow:
                    cv.put(xx + dx, yy + dy, outline)
    for (xx, yy) in ink:
        cv.put(xx, yy, fg)


def big_width(s, scale=3):
    return len(s) * 6 * scale - scale


# ---- the skyline: Miami up to Moscow ------------------------------------
def windows(cv, x0, y0, x1, y1, step_x=3, step_y=4, c=T):
    for y in range(y0, y1, step_y):
        for x in range(x0, x1, step_x):
            cv.put(x, y, c)


def skyline(cv, base):
    """Five cities left to right, black silhouettes with light windows,
    standing on y = base.  Miami, New York, Seoul, then Moscow: Moscow-City and the Kremlin."""
    # Miami: two palms and a low art-deco hotel
    def palm(x, h):
        for y in range(base - h, base):
            cv.put(x + (1 if y < base - h * 2 // 3 else 0), y, K)
        top = base - h
        for dx, dy in ((-4, 2), (-3, 1), (-2, 0), (-1, 0), (1, 0), (2, 0), (3, 1), (4, 2),
                       (-3, -1), (-2, -2), (2, -2), (3, -1), (0, -1), (1, -1), (-1, -1),
                       (-5, 3), (5, 3), (0, 1), (1, 1), (2, 1)):
            cv.put(x + 1 + dx, top + dy, K)
    cv.rect(6, base - 14, 18, 14, K)
    cv.rect(10, base - 18, 10, 4, K)
    cv.rect(13, base - 21, 4, 3, K)
    windows(cv, 8, base - 12, 23, base - 2)
    palm(2, 20)
    palm(26, 16)
    # New York: the Empire State building, stepped, with its mast
    x = 36
    cv.rect(x, base - 26, 16, 26, K)
    cv.rect(x + 2, base - 32, 12, 6, K)
    cv.rect(x + 4, base - 37, 8, 5, K)
    cv.rect(x + 6, base - 41, 4, 4, K)
    cv.rect(x + 7, base - 44, 2, 3, K)
    windows(cv, x + 2, base - 24, x + 15, base - 2)
    cv.rect(x + 18, base - 18, 8, 18, K)                     # a neighbour
    windows(cv, x + 19, base - 16, x + 26, base - 2)
    # Seoul: N Seoul Tower on its hill
    x = 70
    for i in range(24):
        w = 24 - i
        cv.rect(x - w // 2 + 6, base - 1 - i // 3, w, 1, K)  # Namsan hill
    cv.rect(x + 5, base - 40, 2, 34, K)
    cv.rect(x + 2, base - 38, 8, 5, K)                       # the pod
    cv.rect(x + 3, base - 40, 6, 2, K)
    cv.put(x + 3, base - 36, T)
    cv.put(x + 5, base - 36, T)
    cv.put(x + 7, base - 36, T)
    cv.rect(x + 5, base - 43, 2, 3, K)                       # mast
    # Moscow-City: a cluster of glass towers, the Federation's spire,
    # Evolution's twist
    for (gx, w, h) in ((98, 7, 28), (106, 8, 38), (115, 7, 32), (123, 6, 24)):
        cv.rect(gx, base - h, w, h, K)
        for y in range(base - h + 2, base - 1, 3):
            cv.rect(gx + 1, y, w - 2, 1, T)
    cv.rect(109, base - 43, 1, 5, K)
    # Moscow: the Kremlin's Spasskaya tower (tent roof, the star) behind
    # the wall's swallow-tail battlements
    x = 132
    cv.rect(x, base - 10, 28, 10, K)
    for xx in range(x, x + 28, 4):             # swallow-tail merlons
        cv.rect(xx, base - 13, 3, 3, K)
        cv.put(xx + 1, base - 13, T)
    tx = x + 14
    cv.rect(tx - 5, base - 26, 10, 16, K)      # tower body
    cv.rect(tx - 4, base - 31, 8, 5, K)        # clock tier
    cv.put(tx - 1, base - 29, T)
    cv.put(tx, base - 29, T)
    cv.put(tx - 1, base - 28, T)
    cv.put(tx, base - 28, T)
    cv.rect(tx - 3, base - 35, 6, 4, K)        # belfry
    cv.put(tx - 1, base - 34, T)
    cv.put(tx, base - 33, T)
    for j in range(7):                         # tent roof
        w = 6 - j if j < 6 else 1
        cv.rect(tx - (w + 1) // 2, base - 36 - j, max(w, 1), 1, K)
    star = G.g("""
..X..
XXXXX
.XXX.
.X.X.
""")
    cv.glyph(tx - 2, base - 46, star)
    cv.rect(tx, base - 42, 1, 1, K)
    windows(cv, x + 1, base - 7, x + 27, base - 1, step_x=4, step_y=8)


# ---- khokhloma border on black lacquer ------------------------------------
VINE = G.g("""
.ww.....ww......
w..w..ow..w.....
...ww.oo...ww..o
..w..w........ww
""")


def lacquer(cv, y0, y1):
    cv.rect(0, y0, 160, y1 - y0, K)
    for x in range(0, 160, 16):                 # a gold vine along the top
        cv.glyph(x, y0 + 2, VINE)
    cv.rect(0, y0, 160, 1, D)


# ---- screens -----------------------------------------------------------------
def outcome(win, bot="SEOUL", bake=True):
    s = Screen("e1_win" if win else "e2_lose", "Win" if win else "Lose")
    cv = s.bg
    cv.rect(0, 0, 160, 144, T)
    lacquer(cv, 0, 144)
    # khokhloma vine also along the bottom (flipped)
    for x in range(0, 160, 16):
        cv.glyph(x, 144 - 6, VINE[::-1])
    if win:
        big = "YOU WIN"
        sub1, sub2 = bot, "IS THE DURAK"
        fan = [("A", "S"), ("A", "H"), ("A", "C"), ("A", "D")]
    else:
        big = "ДУРАК!"
        sub1, sub2 = "YOU ARE", "THE FOOL"
        fan = None
    scale = 2 if len(big) > 6 else 3
    big_text(cv, (160 - big_width(big, scale)) // 2, 14, big, scale=scale, fg=W, outline=K, shadow=D)
    if win:
        # tile-aligned: the name at row NAME_ROW, columns (20 - len) / 2, in
        # the lacquer font; the exported map leaves that row black
        if bake:
            lq = lacquer_font_bank()
            for i, ch in enumerate(sub1):
                cv.set_tile((20 - len(sub1)) // 2 + i, NAME_ROW, lq["LQ_" + ch])
        text(cv, (20 - len(sub2)) // 2 * 8, (NAME_ROW + 1) * 8, sub2, W)
    else:
        yb = 14 + 7 * scale + 8
        text(cv, (160 - len(sub1) * 8) // 2, yb, sub1, W)
        text(cv, (160 - len(sub2) * 8) // 2, yb + 10, sub2, W)
    if fan:
        x0 = (160 - (len(fan) * 16 + 8)) // 2
        for i, c in enumerate(fan):
            cv.blit(card_face(*c), x0 + i * 20, 80)
    else:
        # the fool left holding the cards: a fan of backs, and the cap
        x0 = (160 - 7 * 8 - 8) // 2
        for i in range(7):
            cv.blit(card_back(), x0 + i * 8, 94)
        fool_cap(cv, 80, 70)
    yb = 120 if fan is None else 112
    button(cv, 40, yb, "BTN_A")
    text(cv, 52, yb, "REMATCH", W)
    button(cv, 40, yb + 9, "BTN_B")
    text(cv, 52, yb + 9, "HOME", W)
    return s


def button(cv, x, y, name):
    """A round A/B button on the lacquer: white ring, white letter."""
    for j, r in enumerate(G.ICON[name]):
        for i, ch in enumerate(r):
            if ch == "X":
                cv.put(x + i, y + j, W)
            elif ch == "w":
                cv.put(x + i, y + j, K)


def fool_cap(cv, cx, top):
    """The durak's jester cap: three points with bells over a band."""
    base = top + 16
    for tip_x, tip_y in ((cx - 14, top + 5), (cx, top), (cx + 14, top + 5)):
        for y in range(tip_y, base):
            f = (y - tip_y) / (base - tip_y)
            half = int(1 + f * 6)
            for x in range(tip_x - half, tip_x + half + 1):
                edge = x in (tip_x - half, tip_x + half) or y == tip_y
                if cv.get(x, y) != W or edge:
                    cv.put(x, y, W if edge else D)
        for dx, dy in ((0, -2), (-1, -3), (0, -3), (1, -3), (0, -4)):
            cv.put(tip_x + dx, tip_y + dy, W)      # the bell
    cv.rect(cx - 19, base, 39, 4, W)
    for x in range(cx - 17, cx + 18, 4):
        cv.put(x, base + 1, D)
        cv.put(x + 1, base + 2, D)


def screens_full(sp, bake=True):
    """The win screens are per bot now (wins.py).
    bake=True: the mockups, names printed as the code will print them.
    bake=False: what export.py ships, those cells left black for the code."""
    import travel
    return [travel.dashboard(sp, "SEOUL", 0, bake), travel.dashboard(sp, None, 3, bake),
            outcome(False, bake=bake)]
