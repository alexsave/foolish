# Pixel glyph data for the Game Boy art: every shape as ASCII art.
# 'X' = black (index 3), 'o' = dark grey (2), 'w' = white (1), '.' = clear.
# This file is the editable source of truth for the shapes; gen_art.py draws
# them into tiles, sheets and screens.

def g(s):
    rows = [r for r in s.strip("\n").split("\n")]
    return [r.strip() for r in rows]

# Card ranks, 5 wide (10 is 6 wide), 6 tall: top-left index of a card face.
RANKS = ["6", "7", "8", "9", "10", "J", "Q", "K", "A"]
RANK = {
"6": g("""
.XXX.
X....
XXXX.
X...X
X...X
.XXX.
"""),
"7": g("""
XXXXX
....X
...X.
..X..
..X..
..X..
"""),
"8": g("""
.XXX.
X...X
.XXX.
X...X
X...X
.XXX.
"""),
"9": g("""
.XXX.
X...X
X...X
.XXXX
....X
.XXX.
"""),
"10": g("""
X..XX.
X.X..X
X.X..X
X.X..X
X.X..X
X..XX.
"""),
"J": g("""
..XXX
...X.
...X.
...X.
X..X.
.XX..
"""),
"Q": g("""
.XXX.
X...X
X...X
X.X.X
X..X.
.XX.X
"""),
"K": g("""
X...X
X..X.
XXX..
X.X..
X..X.
X...X
"""),
"A": g("""
..X..
.X.X.
X...X
XXXXX
X...X
X...X
"""),
}

# Suit order is the kernel's (card_text.h CARD_SUIT_FACE): spade, heart, club, diamond.
SUITS = ["S", "H", "C", "D"]
RED = {"S": False, "H": True, "C": False, "D": True}

# Small suit, 5 wide and 5 or 6 tall, under the rank.
SMALL = {
"S": g("""
..X..
.XXX.
XXXXX
XXXXX
..X..
.XXX.
"""),
"H": g("""
.X.X.
XXXXX
XXXXX
.XXX.
..X..
"""),
"C": g("""
.XXX.
.XXX.
XXXXX
XXXXX
..X..
XXXXX
"""),
"D": g("""
..X..
.XXX.
XXXXX
.XXX.
..X..
"""),
}

# Big pip, 7x7, centred low on the card (card pixels x5..11, y15..21), the
# way a real card carries its centre pip.  Seven wide is the narrowest a
# club's three lobes stay apart from a spade's point.
PIP = {
"S": g("""
...X...
..XXX..
.XXXXX.
XXXXXXX
XXXXXXX
.X.X.X.
..XXX..
"""),
"H": g("""
.XX.XX.
XXXXXXX
XXXXXXX
XXXXXXX
.XXXXX.
..XXX..
...X...
"""),
"C": g("""
..XXX..
..XXX..
XX.X.XX
XXXXXXX
XX.X.XX
..XXX..
XXXXXXX
"""),
"D": g("""
...X...
..XXX..
.XXXXX.
XXXXXXX
.XXXXX.
..XXX..
...X...
"""),
}

# The card back's khokhloma sprig lives in sprig/sprig.py (stamps laid along
# a stem path, three layouts).

# UI font, 5x7 in an 8x8 tile at x1..5, y0..6.
FONT = {
"A": """
.XXX.
X...X
X...X
XXXXX
X...X
X...X
X...X""",
"B": """
XXXX.
X...X
X...X
XXXX.
X...X
X...X
XXXX.""",
"C": """
.XXX.
X...X
X....
X....
X....
X...X
.XXX.""",
"D": """
XXXX.
X...X
X...X
X...X
X...X
X...X
XXXX.""",
"E": """
XXXXX
X....
X....
XXXX.
X....
X....
XXXXX""",
"F": """
XXXXX
X....
X....
XXXX.
X....
X....
X....""",
"G": """
.XXX.
X...X
X....
X.XXX
X...X
X...X
.XXXX""",
"H": """
X...X
X...X
X...X
XXXXX
X...X
X...X
X...X""",
"I": """
.XXX.
..X..
..X..
..X..
..X..
..X..
.XXX.""",
"J": """
..XXX
...X.
...X.
...X.
...X.
X..X.
.XX..""",
"K": """
X...X
X..X.
X.X..
XX...
X.X..
X..X.
X...X""",
"L": """
X....
X....
X....
X....
X....
X....
XXXXX""",
"M": """
X...X
XX.XX
X.X.X
X.X.X
X...X
X...X
X...X""",
"N": """
X...X
X...X
XX..X
X.X.X
X..XX
X...X
X...X""",
"O": """
.XXX.
X...X
X...X
X...X
X...X
X...X
.XXX.""",
"P": """
XXXX.
X...X
X...X
XXXX.
X....
X....
X....""",
"Q": """
.XXX.
X...X
X...X
X...X
X.X.X
X..X.
.XX.X""",
"R": """
XXXX.
X...X
X...X
XXXX.
X.X..
X..X.
X...X""",
"S": """
.XXXX
X....
X....
.XXX.
....X
....X
XXXX.""",
"T": """
XXXXX
..X..
..X..
..X..
..X..
..X..
..X..""",
"U": """
X...X
X...X
X...X
X...X
X...X
X...X
.XXX.""",
"V": """
X...X
X...X
X...X
X...X
X...X
.X.X.
..X..""",
"W": """
X...X
X...X
X...X
X.X.X
X.X.X
X.X.X
.X.X.""",
"X": """
X...X
X...X
.X.X.
..X..
.X.X.
X...X
X...X""",
"Y": """
X...X
X...X
.X.X.
..X..
..X..
..X..
..X..""",
"Z": """
XXXXX
....X
...X.
..X..
.X...
X....
XXXXX""",
"0": """
.XXX.
X...X
X..XX
X.X.X
XX..X
X...X
.XXX.""",
"1": """
..X..
.XX..
..X..
..X..
..X..
..X..
.XXX.""",
"2": """
.XXX.
X...X
....X
...X.
..X..
.X...
XXXXX""",
"3": """
XXXXX
...X.
..X..
...X.
....X
X...X
.XXX.""",
"4": """
...X.
..XX.
.X.X.
X..X.
XXXXX
...X.
...X.""",
"5": """
XXXXX
X....
XXXX.
....X
....X
X...X
.XXX.""",
"6": """
..XX.
.X...
X....
XXXX.
X...X
X...X
.XXX.""",
"7": """
XXXXX
....X
...X.
..X..
.X...
.X...
.X...""",
"8": """
.XXX.
X...X
X...X
.XXX.
X...X
X...X
.XXX.""",
"9": """
.XXX.
X...X
X...X
.XXXX
....X
...X.
.XX..""",
".": """
.....
.....
.....
.....
.....
.XX..
.XX..""",
",": """
.....
.....
.....
.....
.XX..
..X..
.X...""",
"!": """
..X..
..X..
..X..
..X..
..X..
.....
..X..""",
"?": """
.XXX.
X...X
....X
...X.
..X..
.....
..X..""",
"-": """
.....
.....
.....
XXXXX
.....
.....
.....""",
":": """
.....
.XX..
.XX..
.....
.XX..
.XX..
.....""",
"'": """
..X..
..X..
.X...
.....
.....
.....
.....""",
# Cyrillic, for the lose screen's ДУРАК (A, K, P are the Latin shapes)
"Д": """
..XX.
.X.X.
.X.X.
.X.X.
.X.X.
XXXXX
X...X""",
"У": """
X...X
X...X
X...X
.XXXX
....X
...X.
.XX..""",
"Р": """
XXXX.
X...X
X...X
XXXX.
X....
X....
X....""",
"А": """
.XXX.
X...X
X...X
XXXXX
X...X
X...X
X...X""",
"К": """
X...X
X..X.
X.X..
XX...
X.X..
X..X.
X...X""",
"+": """
.....
..X..
..X..
XXXXX
..X..
..X..
.....""",
"/": """
....X
....X
...X.
..X..
.X...
X....
X....""",
}
FONT = {k: g(v) for k, v in FONT.items()}

# 8x8 UI icons that live in the font bank, drawn on the table grey.
ICON = {
# d-pad arrows, the website's arrow keys
"UP": g("""
...XX...
..XXXX..
.XXXXXX.
XXXXXXXX
...XX...
...XX...
...XX...
........
"""),
"DOWN": g("""
...XX...
...XX...
...XX...
XXXXXXXX
.XXXXXX.
..XXXX..
...XX...
........
"""),
"LEFT": g("""
...X....
..XX....
.XXXXXXX
XXXXXXXX
.XXXXXXX
..XX....
...X....
........
"""),
"RIGHT": g("""
....X...
....XX..
XXXXXXX.
XXXXXXXX
XXXXXXX.
....XX..
....X...
........
"""),
# round A and B buttons
"BTN_A": g("""
.XXXXX..
XXwwwXX.
XwXXXwX.
XwwwwwX.
XwXXXwX.
XXXXXXX.
.XXXXX..
........
"""),
"BTN_B": g("""
.XXXXX..
XwwwwXX.
XwXXXwX.
XwwwwXX.
XwXXXwX.
XwwwwXX.
.XXXXX..
........
"""),
# menu pointer
"POINT": g("""
.X......
.XX.....
.XXX....
.XXXX...
.XXX....
.XX.....
.X......
........
"""),
# a tiny card back: the bot card count and deck count icons
"MINIBACK": g("""
.XXXXX..
XoooooX.
XowwwoX.
XowowoX.
XowwwoX.
XoooooX.
.XXXXX..
........
"""),
# the deck: two backs, one on the other
"MINIDECK": g("""
..XXXXX.
.XoooooX
XXXXXXoX
XoooooXX
XowwwoXX
XowowoX.
XowwwoX.
XXXXXXX.
"""),
# 'x' between an icon and a count
"TIMES": g("""
........
........
.X...X..
..X.X...
...X....
..X.X...
.X...X..
........
"""),
}
