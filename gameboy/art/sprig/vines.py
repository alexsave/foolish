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
opts = {"vines": sprig.VINE_OPTIONS, "combo": sprig.COMBO_OPTIONS,
        "final": {"final": sprig.final_2b},
        "framed": {"framed": sprig.final_2b_framed}}[which]
keys = list(opts)
out = A.Canvas(8 + len(keys) * cw, 46, A.T)
for k, key in enumerate(keys):
    x = 8 + k * cw
    if which == "framed":
        card = A.card_back("final_2b_framed")
    else:
        card = back(opts[key]())
    out.blit(card, x, 6)
    for i in range(5):
        out.blit(card, x + 24 + i * 8, 6)
    for i, t in enumerate(A.text_tiles(key.upper() if which == "vines" else "%d %s" % (k + 1, key.upper()), ui)):
        tc = A.Canvas(8, 8, None)
        tc.set_tile(0, 0, t)
        out.blit(tc, x + 8 * i, 34)
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mockups", {"vines": "07_back_vine_options.png", "combo": "10_back_combo.png", "final": "11_back_final.png", "framed": "12_back_framed.png"}[which])
out.image(4).save(p)
print(os.path.normpath(p))
