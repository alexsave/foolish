#!/usr/bin/env python3
"""Where to tap on a board, read off the rig's own marks in a screenshot.

    board.py hand  SHOT.png     ->  "HAND_Y x1 x2 ..."   hand row y and card centres (points)
    board.py table SHOT.png     ->  "x y x y ..."        the table pairs' centres (points)

`ui.py` finds the hand and the table by colour bands tuned to the COMPACT drawer.
Expanded, with one card in hand, it reads the opponent's fan at the top of the
board as the hand (hand_y 101), so a tap meant for the card lands on the seat
ring and the table tap after it is "That move isn't allowed." These two ask the
ruler instead (a DEBUG build with `rig.sh ruler on`):

  hand   the hand is bottom-anchored on the drawer, so its row is just above the
         ruler's green bottom bar; a card is anything that is not felt, 30pt+ wide.
  table  every table pair carries a coloured square at its centre (squares.py).
"""
import os
import sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# The measurement half of the rig lives in shared/rig/lib - it has no product
# knowledge, so it is shared. This file does, so it stays here and reaches.
SHLIB = os.path.join(HERE, "..", "..", "..", "..", "shared", "rig", "lib")
sys.path.insert(0, SHLIB)

import squares
import tween


def load(path):
    return np.asarray(Image.open(path).convert("RGB"))


def hand(a):
    s = 3 if a.shape[0] >= 2000 else 2
    box = tween.read_array(a) or {}
    if "bot_pt" not in box:
        return None, []
    y_pt = box["bot_pt"] - 40
    y = int(y_pt * s)
    band = a[max(0, y - 15):y + 15].astype(int)
    r, g, b = band[:, :, 0], band[:, :, 1], band[:, :, 2]
    felt = ((g - r > 12) & (g - b > 5) & (g < 110)).mean(axis=0) > 0.5
    card = ~felt
    card[:squares.STRIP_PX] = False
    xs, i = [], 0
    while i < len(card):
        if card[i]:
            j = i
            while j < len(card) and card[j]:
                j += 1
            if j - i > 30 * s:
                xs.append((i + j) // 2 // s)
            i = j
        else:
            i += 1
    return int(y_pt), xs


def table(a):
    return [(int(x), int(y)) for n, x, y in squares.squares_in(a) if n in squares.TABLE]


if __name__ == "__main__":
    what, path = sys.argv[1], sys.argv[2]
    a = load(path)
    if what == "hand":
        y, xs = hand(a)
        print(y if y is not None else -1, " ".join(str(x) for x in xs))
    elif what == "table":
        print(" ".join("%d %d" % p for p in table(a)))
    else:
        sys.exit("board.py hand|table SHOT.png")
