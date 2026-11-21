# The traveller dashboard (title + bot picker on one screen), after the
# blind designer's departures board, in our style:
#   rows 0-9   ДУРАК / FOOLISH over the Miami-to-Moscow skyline, a dotted
#              flight route behind the towers, a plane sprite over the
#              selected city
#   row 10     CONTINUE as a boarding pass (paper ticket); the city on it is
#              printed by the code with the paper font (black on white)
#   row 11     DEPARTURES
#   rows 12-15: a split-flap board, one row per bot, a departure
#              time (later = harder; Warsaw, the boss, leaves at midnight)
#   row 17     A FLY
# Moscow stands in the skyline only: it is the real top bot, too big for the
# Game Boy, so it has no row on the board.

import glyphs as G
from gen_art import Canvas, Screen, T, W, D, K, rail_glyph_tile
from screens_full import big_text, big_width, text, button, skyline

TICKET_ROW, TICKET_CITY_COL = 10, 11
BOARD_ROW = 12                       # first city row
PAPER_BASE, POINTER_TILE, PLANE_TILE = 200, 250, 251
TILESET_MAX = PAPER_BASE

BOARD = (("MIAMI", "06:10", 1), ("NEW YORK", "09:45", 2), ("SEOUL", "17:20", 3), ("WARSAW", "23:55", 0))
CITY_X = {"MIAMI": 14, "NEW YORK": 44, "SEOUL": 75, "WARSAW": 112}   # plane x over each city
ROUTE_Y = 40

PLANE = G.g("""
...w....
..wXw...
wwwXXw..
XXXXXXXw
wwwXXXww
..wXXw..
...ww...
........
""")
PLANE_ICON = G.g("""
...X....
...XX...
X..XXX..
XXXXXXXX
X..XXX..
...XX...
...X....
........
""")


def paper_font_bank():
    """Black on white: the boarding pass.  Same characters as the rail font."""
    from gen_art import FONT_CHARS
    bank = {}
    for ch in FONT_CHARS:
        bank["PP_" + ch] = tuple([W] * 64) if ch == " " else rail_glyph_tile(G.FONT[ch], fg=K, bg=W)
    return bank


def flap(cv, tx, ty, ch):
    """One split-flap cell: a dark grey flap, a black gap right and below,
    the hinge line across the flap, the letter white."""
    x0, y0 = tx * 8, ty * 8
    cv.rect(x0, y0, 8, 8, K)
    cv.rect(x0, y0, 7, 7, D)
    if ch != " ":
        cv.glyph(x0 + 1, y0, G.FONT[ch], W)
    for x in range(x0, x0 + 7):
        if cv.get(x, y0 + 3) == D:
            cv.put(x, y0 + 3, K)


def flaps(cv, tx, ty, s, width=None):
    s = s.ljust(width or len(s))
    for i, ch in enumerate(s):
        flap(cv, tx + i, ty, ch)


def sprite_tile(rows):
    cv = Canvas(8, 8, T)
    cv.glyph(0, 0, rows)
    return cv.tile(0, 0)


def dashboard(sp, cont="SEOUL", cursor=0, bake=True):
    s = Screen("d1_dashboard" if cont else "d1b_dashboard_new", "Dashboard")
    cv = s.bg
    cv.rect(0, 0, 160, 144, T)
    for (x, y) in ((8, 6), (30, 12), (60, 4), (118, 8), (150, 14), (20, 30)):
        cv.put(x, y, W)
    for j in range(9):                                  # the moon
        for i in range(9):
            if (i - 4) ** 2 + (j - 4) ** 2 <= 17:
                cv.put(140 + i, 2 + j, W)
    big_text(cv, (160 - big_width("ДУРАК")) // 2, 3, "ДУРАК")
    text(cv, (160 - 7 * 8) // 2, 27, "FOOLISH", K)
    # the flight route: a dotted line in the sky, hidden behind the towers
    skyline(cv, 80)
    for x in range(2, 158, 4):
        y = ROUTE_Y + (2 if (x // 4) % 6 in (2, 3) else 0)
        if cv.get(x, y) == T:
            cv.put(x, y, D)
    # the ground and the board below it
    cv.rect(0, 80, 160, 64, K)
    if cont:
        # the boarding pass: a paper ticket with punched ends
        cv.rect(8, 80, 152, 8, W)
        for (x, y) in ((8, 80), (8, 87), (159, 80), (159, 87)):
            cv.put(x, y, K)
        for y in range(81, 87, 2):
            cv.put(80, y, D)                            # the tear line
        pp = paper_font_bank()
        for i, ch in enumerate("CONTINUE"):
            cv.set_tile(1 + i, TICKET_ROW, pp["PP_" + ch])
        if bake:
            for i, ch in enumerate(cont):
                cv.set_tile(TICKET_CITY_COL + i, TICKET_ROW, pp["PP_" + ch])
        cv.rect(8, 80, 1, 1, K)
    else:
        text(cv, (160 - 13 * 8) // 2, 80, "PICK A FLIGHT", T)
    # DEPARTURES header
    text(cv, 8, 88, "DEPARTURES", W)
    cv.glyph(152, 88, PLANE_ICON, W)
    rows = [80] if cont else []
    for k, (name, when, pips) in enumerate(BOARD):
        ty = BOARD_ROW + k
        flaps(cv, 1, ty, name, 8)
        flaps(cv, 10, ty, when)
        if pips:
            for p in range(pips):
                cv.glyph(16 * 8 + p * 8 + 1, ty * 8 + 1, G.SMALL["D"], W)
        else:
            flaps(cv, 16, ty, "BOSS")
        rows.append(ty * 8)
    button(cv, 64, 136, "BTN_A")
    text(cv, 72, 136, " FLY", W)
    # sprites: the pointer at the selected row, the plane over its city
    s.spr(0, rows[cursor], sp["SP_POINTER"])
    sel = cont if (cont and cursor == 0) else BOARD[cursor - (1 if cont else 0)][0]
    s.spr(CITY_X[sel] - 4, ROUTE_Y - 4, sprite_tile(PLANE))
    s.notes.append("Up/down moves the pointer; the plane glides along the route to the city. START in a game comes back here; CONTINUE is the open game's boarding pass.")
    return s


def plane_bank():
    return {"SP_PLANE": sprite_tile(PLANE)}
