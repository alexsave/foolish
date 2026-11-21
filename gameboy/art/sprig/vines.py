#!/usr/bin/env python3
# 2B vine options a..e side by side at 4x, each alone and as the bot's
# overlapped stack, labelled.  None is live: BACK_LAYOUT in gen_art.py decides.
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gen_art as A
from sprig import sprig
from sprig.compare import back

ui = A.ui_bank()
cw = 16 + 8 + 16 + 4 * 8 + 8
which = sys.argv[1] if len(sys.argv) > 1 else "vines"
opts = sprig.VINE_OPTIONS if which == "vines" else sprig.COMBO_OPTIONS
keys = list(opts)
out = A.Canvas(8 + len(keys) * cw, 46, A.T)
for k, key in enumerate(keys):
    rows = opts[key]()
    x = 8 + k * cw
    out.blit(back(rows), x, 6)
    for i in range(5):
        out.blit(back(rows), x + 24 + i * 8, 6)
    for i, t in enumerate(A.text_tiles(key.upper() if which == "vines" else "%d %s" % (k + 1, key.upper()), ui)):
        tc = A.Canvas(8, 8, None)
        tc.set_tile(0, 0, t)
        out.blit(tc, x + 8 * i, 34)
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", "07_back_vine_options.png" if which == "vines" else "10_back_combo.png")
out.image(4).save(p)
print(os.path.normpath(p))
