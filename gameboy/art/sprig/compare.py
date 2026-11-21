#!/usr/bin/env python3
# Render the sprig layouts side by side at 4x for Alex to pick:
# each as a lone back, and as an overlapped stack (the bot hand).
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gen_art as A
from sprig import sprig

def back(rows):
    cv = A.Canvas(16, 24, None)
    A.card_outline(cv, A.K)
    m = {"w": A.W, "o": A.D, "X": A.K}
    for j, r in enumerate(rows):
        for i, ch in enumerate(r):
            if ch in m:
                cv.put(1 + i, 1 + j, m[ch])
    return cv

def main():
    rows_of = [[n for n in sprig.LAYOUTS if not n.endswith('_min')], [n for n in sprig.LAYOUTS if n.endswith('_min')]]
    names = rows_of[0]
    cw = 16 + 8 + 16 + 5 * 8 + 16
    out = A.Canvas(8 + len(names) * (cw + 8), 104, A.T)
    ui = A.ui_bank()
    for v, names_v in enumerate(rows_of):
        oy = v * 52
        for k, n in enumerate(names_v):
            rows = sprig.LAYOUTS[n]()
            x = 8 + k * (cw + 8)
            out.blit(back(rows), x, oy + 8)
            for i in range(5):
                out.blit(back(rows), x + 24 + i * 8, oy + 8)
            label = "%d%s %s" % (k + 1, "B" if v else "A", n.replace("_min", "").upper())
            for i, t in enumerate(A.text_tiles(label, ui)):
                tc = A.Canvas(8, 8, None)
                tc.set_tile(0, 0, t)
                out.blit(tc, x + 8 * i, oy + 38)
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", "06_back_rotations.png")
    out.image(4).save(path)
    print(path)


if __name__ == "__main__":
    main()
