#!/usr/bin/env python3
# Localised UI mockups (design only, nothing live): the dashboard, the table
# (defending, pick up), the target prompt, the loss screen and the Moscow win
# screen, in en, ko, ru, ja, zh.  Text comes from loc/strings.py, glyphs from
# glyphs.FONT (Latin, Cyrillic) and loc/glyphs_*.txt (Hangul, kana, Hanzi).
# Lines are 8 px (en, ru, ko, ja: the English layout) or 16 px (zh: taller
# rails and board rows, the layout redone for them).
# Run: python3 loc/screens.py <lang>   -> mockups/28_ko.png, 29_ru.png, ...
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.dirname(HERE)
sys.path.insert(0, ART)
sys.path.insert(0, HERE)

import glyphs as G
import gen_art as A
import screens_full as SF
import travel
import wins
from strings import S

T, W, D, K = A.T, A.W, A.D, A.K
LATIN_SHAPE = {"А": "A", "В": "B", "Е": "E", "К": "K", "М": "M", "Н": "H", "О": "O",
               "Р": "P", "С": "C", "Т": "T", "Х": "X"}
LANG_SET = {"ko": "ko8", "ja": "ja8", "zh": "zh16"}
LINE = {"en": 8, "ru": 8, "ko": 8, "ja": 8, "zh": 16}


def load_set(name):
    out, cur = {}, None
    for line in open(os.path.join(HERE, "glyphs_%s.txt" % name)):
        line = line.rstrip("\n")
        if line.startswith("#") or not line:
            continue
        if line.startswith("U+"):
            cur = line.split(" ", 1)[1]
            out[cur] = []
        else:
            out[cur].append(line)
    return out


_SETS = {}


def glyph_set(name):
    if name not in _SETS:
        _SETS[name] = load_set(name)
    return _SETS[name]


def cell_of(ch, lang, h):
    """(rows, width) of one character's cell: wide glyphs from the language's
    set, everything else from our own 5x7 font (in an 8 px wide cell)."""
    if ch == " ":
        return [], 8 if h == 8 or lang != "zh" else 8
    s = LANG_SET.get(lang)
    if s and ch in glyph_set(s):
        rows = glyph_set(s)[ch]
        return rows, len(rows[0])
    rows = G.FONT[LATIN_SHAPE.get(ch, ch)]
    pad = (h - 8) // 2                               # centre the 8 px glyph in a 16 px line
    return [""] * (1 + pad) + [" " + r for r in rows], 8


def tokens(s):
    i = 0
    while i < len(s):
        if s[i] == "{":
            j = s.index("}", i)
            yield ("icon", s[i + 1:j])
            i = j + 1
        else:
            yield ("ch", s[i])
            i += 1


def width(s, lang):
    h = LINE[lang]
    return sum(8 if k == "icon" else cell_of(v, lang, h)[1] for k, v in tokens(s))


def text(cv, x, y, s, lang, fg, bg=None, flap=False):
    """Draw s at (x, y) in the language's line height; returns its width.
    bg fills each cell; flap draws a split-flap cell (dark flap, hinge line)."""
    h = LINE[lang]
    for kind, v in tokens(s):
        if kind == "icon":
            w = 8
            if bg is not None:
                cv.rect(x, y, w, h, bg)
            rows = G.SMALL[v] if v in G.SUITS else G.ICON[v]
            oy = y + (h - 8) // 2 + (1 if v in G.SUITS else 0)
            for j, r in enumerate(rows):
                for i, ch in enumerate(r):
                    if ch == "X":
                        cv.put(x + i + (1 if v in G.SUITS else 0), oy + j, fg if v not in ("BTN_A", "BTN_B") else fg)
                    elif ch == "w" and v in ("BTN_A", "BTN_B"):
                        cv.put(x + i, oy + j, bg if bg is not None else K)
                    elif ch == "o":
                        cv.put(x + i, oy + j, T)
                    elif ch == "t":
                        cv.put(x + i + (1 if v in G.SUITS else 0), oy + j, T)
            x += w
            continue
        rows, w = cell_of(v, lang, h)
        if flap:
            cv.rect(x, y, w, h, K)
            cv.rect(x, y, w - 1, h - 1, D)
        elif bg is not None:
            cv.rect(x, y, w, h, bg)
        for j, r in enumerate(rows):
            for i, c in enumerate(r):
                if c == "X":
                    cv.put(x + i, y + j, fg)
        if flap:
            hy = y + h // 2 - 1
            for xx in range(x, x + w - 1):
                if cv.get(xx, hy) == D:
                    cv.put(xx, hy, K)
        x += w
    return x


def big(cv, cx, y, s, lang, scale, fg, outline, shadow):
    """A title: the glyphs scaled up, outlined, with a drop shadow (as
    screens_full.big_text does for the Latin font), centred on cx."""
    h = LINE[lang]
    cells = []
    for kind, v in tokens(s):
        rows, w = cell_of(v, lang, h)
        cells.append((rows, w))
    total = sum(w for _, w in cells) * scale
    x = cx - total // 2
    layer = set()
    for rows, w in cells:
        for j, r in enumerate(rows):
            for i, c in enumerate(r):
                if c == "X":
                    for a in range(scale):
                        for b in range(scale):
                            layer.add((x + i * scale + a, y + j * scale + b))
        x += w * scale
    for (px, py) in layer:
        for dx, dy in ((2, 2), (1, 2), (2, 1)):
            if (px + dx, py + dy) not in layer:
                cv.put(px + dx, py + dy, shadow)
    for (px, py) in layer:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                q = (px + dx, py + dy)
                if q not in layer and 0 <= q[0] < 160 and 0 <= q[1] < 144 and cv.get(*q) != shadow:
                    cv.put(q[0], q[1], outline)
    for (px, py) in layer:
        cv.put(px, py, fg)


def cities(lang):
    t = S[lang]
    return [(t["miami"], "06:10", 1, "MIAMI"), (t["new_york"], "09:45", 2, "NEW YORK"),
            (t["seoul"], "17:20", 3, "SEOUL"), (t["moscow"], "23:55", 0, "MOSCOW")]


# ---- the dashboard -----------------------------------------------------------
def dashboard(lang, cont=True):
    t = S[lang]
    sp = A.sprite_bank()
    s = A.Screen("%s_dashboard" % lang, "Dashboard")
    cv = s.bg
    cv.rect(0, 0, 160, 144, T)
    for (x, y) in ((8, 6), (30, 12), (60, 4), (118, 8), (150, 14), (20, 30)):
        cv.put(x, y, W)
    for j in range(9):
        for i in range(9):
            if (i - 4) ** 2 + (j - 4) ** 2 <= 17:
                cv.put(140 + i, 2 + j, W)
    h = LINE[lang]
    if h == 8:
        big(cv, 80, 3, t["title"], lang, 3, K, W, D)
        text(cv, 80 - width(t["subtitle"], lang) // 2, 27, t["subtitle"], lang, K)
        SF.skyline(cv, 80)
        for x in range(2, 158, 4):
            y = travel.ROUTE_Y + (2 if (x // 4) % 6 in (2, 3) else 0)
            if cv.get(x, y) == T:
                cv.put(x, y, D)
        cv.rect(0, 80, 160, 64, K)
        rows = []
        if cont:
            cv.rect(8, 80, 152, 8, W)
            end = text(cv, 8, 80, t["continue"], lang, K, W)
            city_x = max(88, (end + 8 + 7) // 8 * 8)      # column 11, or later if the word is long
            for y in range(81, 87, 2):
                cv.put(city_x - 4, y, D)                 # the tear line
            text(cv, city_x, 80, t["moscow"], lang, K, W)
            rows.append(80)
        else:
            text(cv, 80 - width(t["pick_flight"], lang) // 2, 80, t["pick_flight"], lang, T)
        text(cv, 8, 88, t["departures"], lang, W)
        fly = "{BTN_A} " + t["fly"]
        text(cv, 160 - width(fly, lang), 88, fly, lang, W)
        for k, (name, when, pips, _) in enumerate(cities(lang)):
            y = (13 + k) * 8
            nx = text(cv, 8, y, name, lang, W, flap=True)
            while nx < 72:
                nx = text(cv, nx, y, " ", lang, W, flap=True)
            text(cv, 80, y, when, lang, W, flap=True)
            if pips:
                for p in range(pips):
                    cv.glyph(128 + p * 8 + 1, y + 1, G.SMALL["D"], W)
            else:
                bx = text(cv, 128, y, t["boss"], lang, W, flap=True)
                while bx < 160:
                    bx = text(cv, bx, y, " ", lang, W, flap=True)
            rows.append(y)
        cursor = 0
        s.spr(0, rows[cursor], sp["SP_POINTER"])
        s.spr(travel.CITY_X["MOSCOW"] - 4, travel.ROUTE_Y - 4, travel.plane_bank()["SP_PLANE"])
    else:
        # 16 px text: a shorter sky (the skyline stands on y = 64), the
        # ticket two tiles tall, the board's rows 16 px; DEPARTURES and FLY
        # move onto the ticket row's right when no game is open
        big(cv, 80, 1, t["title"], lang, 1, K, W, D)
        text(cv, 8, 1, t["subtitle"], lang, K)
        SF.skyline(cv, 64)
        cv.rect(0, 64, 160, 80, K)
        rows = []
        if cont:
            cv.rect(8, 64, 152, 16, W)
            for y in range(65, 79, 2):
                cv.put(80, y, D)
            text(cv, 8, 64, t["continue"], lang, K, W)
            text(cv, 88, 64, t["moscow"], lang, K, W)
            rows.append(68)
        else:
            text(cv, 8, 64, t["pick_flight"], lang, T)
            fly = "{BTN_A}" + t["fly"]
            text(cv, 160 - width(fly, lang), 64, fly, lang, W)
        for k, (name, when, pips, _) in enumerate(cities(lang)):
            y = 80 + k * 16
            nx = text(cv, 8, y, name, lang, W, flap=True)
            while nx < 56:
                nx = text(cv, nx, y, "　", lang, W, flap=True) if "　" in glyph_set("zh16") else text(cv, nx, y, " ", lang, W, flap=True)
            text(cv, 64, y, when, lang, W, flap=True)
            if pips:
                for p in range(pips):
                    cv.glyph(120 + p * 8 + 1, y + 5, G.SMALL["D"], W)
            else:
                text(cv, 120, y, t["boss"], lang, W, flap=True)
            rows.append(y + 4)
        s.spr(0, rows[0], sp["SP_POINTER"])
        s.spr(travel.CITY_X["MOSCOW"] - 4, 40 - 4, travel.plane_bank()["SP_PLANE"])
    return s


# ---- the table ---------------------------------------------------------------
def table(lang, target=False):
    t = S[lang]
    ui, sp = A.ui_bank(), A.sprite_bank()
    s = A.Screen("%s_%s" % (lang, "target" if target else "table"), "Table")
    rh = LINE[lang] // 8                         # rail height in tiles
    top_rail = range(0, rh)
    foot_rail = range(18 - rh, 18)
    bot_row = rh                                 # bot hand under the top rail
    hand_row = 13 if rh == 1 else 12
    A.wool_rows(s, range(rh, 18 - rh), ui)
    for ty in list(top_rail) + list(foot_rail):
        for tx in range(20):
            s.fill_tile(tx, ty, ui["CH_ "])
    cv = s.bg
    # top rail: who is thinking, the deck, the trump
    text(cv, 0, 0, t["moscow"] + " " + t["thinks"], lang, W, D)
    right = "{MINIDECK}18 {H}"
    text(cv, 160 - width(right, lang), 0, right, lang, W, D)
    for i in range(4):
        A.put_card(s, A.card_back(), 7 + i, bot_row)
    A.put_card(s, A.card_face("9", "H"), 0, bot_row + 3 if rh == 1 else bot_row + 3)
    A.put_card(s, A.card_back(), 1, bot_row + 3)
    y_b = 6 if rh == 1 else 7
    if target:
        pos = [(6, y_b), (10, y_b)]
        A.put_card(s, A.card_face("8", "C"), *pos[0])
        A.put_card(s, A.card_face("8", "S"), *pos[1])
    else:
        pos = [(4, y_b), (8, y_b), (12, y_b)]
        for (x, y), (a, c) in zip(pos, ((("K", "C"), ("A", "C")), (("K", "S"), None), (("A", "S"), None))):
            A.put_card(s, A.card_face(*a), x, y)
            if c:
                A.put_card(s, A.card_face(*c), x + 1, y + 1)
    hand = [("6", "D"), ("10", "C"), ("Q", "H"), ("K", "S"), ("A", "D"), ("7", "C")]
    cols = A.hand_columns(len(hand))
    for i, c in enumerate(hand):
        A.put_card(s, A.card_face(*c), cols[i], hand_row)
    fy = (18 - rh) * 8
    if target:
        left, right = "{LEFT}{RIGHT}" + t["target"], "{UP}" + t["cover"]
        place = (cols[2], hand_row)
        A.underline(s, sp, place, 2)
        A.arrow(s, sp, A.card_top_center(place, 2), A.card_bottom(*pos[1]))
        A.brackets(s, sp, *pos[1])
    else:
        left, right = t["defend"], "{DOWN}" + t["pick_up"]
    text(cv, 0, fy, left, lang, W, D)
    text(cv, 160 - width(right, lang), fy, right, lang, W, D)
    return s


# ---- the loss screen ----------------------------------------------------------
def lose(lang):
    t = S[lang]
    s = A.Screen("%s_lose" % lang, "Lose")
    cv = s.bg
    SF.lacquer(cv, 0, 144)
    for x in range(0, 160, 16):
        cv.glyph(x, 144 - 6, SF.VINE[::-1])
    h = LINE[lang]
    scale = 3 if h == 8 else 2
    if h == 8 and width(t["lose_big"], lang) * 3 > 156:
        scale = 2
    big(cv, 80, 12, t["lose_big"], lang, scale, W, K, D)
    yb = 12 + h * scale + 6
    if h == 8:
        text(cv, 80 - width(t["lose_1"], lang) // 2, yb, t["lose_1"], lang, W)
        text(cv, 80 - width(t["lose_2"], lang) // 2, yb + 10, t["lose_2"], lang, W)
    else:
        line = t["lose_1"] + t["lose_2"]
        text(cv, 80 - width(line, lang) // 2, yb, line, lang, W)
    SF.fool_cap(cv, 80, 70)
    x0 = (160 - 7 * 8 - 8) // 2
    for i in range(7):
        cv.blit(A.card_back(), x0 + i * 8, 94)
    if h == 8:
        r = "{BTN_A} " + t["rematch"]
        text(cv, 40, 120, r, lang, W)
        text(cv, 40, 129, "{BTN_B} " + t["home"], lang, W)
    else:
        r = "{BTN_A}" + t["rematch"] + " {BTN_B}" + t["home"]
        text(cv, 80 - width(r, lang) // 2, 122, r, lang, W)
    return s


# ---- the win screen (Moscow) ------------------------------------------------------
def win(lang):
    t = S[lang]
    s = wins.win_screen("MOSCOW", frame=2, bake=False)
    s.name = "%s_win" % lang
    cv = s.bg
    cv.rect(0, 2, 136, 21, K)                     # the English title goes
    cv.rect(0, 112, 160, 32, K)                    # the name, the line, the buttons go
    h = LINE[lang]
    big(cv, 72, 5 if h == 8 else 4, t["win_big"], lang, 2 if h == 8 else 1, W, K, D)
    if h == 8:
        text(cv, 80 - width(t["moscow"], lang) // 2, 112, t["moscow"], lang, W)
        text(cv, 80 - width(t["is_durak"], lang) // 2, 120, t["is_durak"], lang, T)
        r = "{BTN_A}" + t["rematch"] + " {BTN_B}" + t["home"]
        if width(r, lang) <= 144:
            text(cv, 80 - width(r, lang) // 2, 136, r, lang, W)
        else:                                        # long words: one button a line
            text(cv, 40, 128, "{BTN_A} " + t["rematch"], lang, W)
            text(cv, 40, 136, "{BTN_B} " + t["home"], lang, W)
    else:
        line = t["moscow"] + t["is_durak"]
        text(cv, 80 - width(line, lang) // 2, 112, line, lang, W)
        r = "{BTN_A}" + t["rematch"] + " {BTN_B}" + t["home"]
        text(cv, 80 - width(r, lang) // 2, 128, r, lang, W)
    return s


def screens(lang):
    return [dashboard(lang, True), table(lang), table(lang, target=True), lose(lang), win(lang)]


def unique_tiles(s):
    return len({s.bg.tile(x, y) for y in range(18) for x in range(20)})


def lang_sheet(lang, extra=None):
    from PIL import Image
    scr = screens(lang)
    ims = [x.compose().image(4) for x in scr]
    w, hh = ims[0].size
    gap = 16
    sheet = Image.new("RGB", (3 * w + 4 * gap, 2 * hh + 3 * gap + 200), (40, 40, 40))
    for k, im in enumerate(ims):
        sheet.paste(im, (gap + (k % 3) * (w + gap), gap + (k // 3) * (hh + gap)))
    strip_x, strip_y = gap + 2 * (w + gap), gap + hh + gap
    for k, x in enumerate(scr):                        # true size, as on the Game Boy
        sheet.paste(x.compose().image(1), (strip_x + (k % 3) * 168, strip_y + (k // 3) * 152))
    if extra is not None:
        sheet.paste(extra, (gap, 2 * hh + 3 * gap))
    return sheet, scr


def korean_compare():
    """The status bar's Korean in the three glyph sizes tried."""
    from PIL import Image
    line = S["ko"]["defend"] + " " + S["ko"]["pick_up"] + " " + S["ko"]["moscow"]
    cv = A.Canvas(160, 48, D)
    y = 0
    for name, h in (("ko8", 8), ("ko12", 16), ("ko16u", 16)):
        x = 0
        gs = glyph_set(name)
        for ch in line:
            if ch == " ":
                x += 8 if h == 8 else 8
                continue
            rows = gs[ch]
            for j, r in enumerate(rows):
                for i, c in enumerate(r):
                    if c == "X":
                        cv.put(x + i, y + j, W)
            x += len(rows[0])
        y += h
    return cv.image(4)


if __name__ == "__main__":
    lang = sys.argv[1]
    nums = {"ko": 28, "ru": 29, "ja": 30, "zh": 31, "en": 0}
    extra = korean_compare() if lang == "ko" else None
    sheet, scr = lang_sheet(lang, extra)
    p = os.path.join(ART, "mockups", "%02d_%s.png" % (nums[lang], lang))
    sheet.save(p)
    for x in scr:
        print("%-12s unique bg tiles %3d  sprites %2d" % (x.name, unique_tiles(x), len(x.sprites)))
    print(os.path.normpath(p))
