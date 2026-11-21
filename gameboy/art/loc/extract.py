#!/usr/bin/env python3
# Extract, once, the glyphs each language's mockups use from free bitmap
# fonts into ASCII bitmaps (loc/glyphs_<set>.txt, committed, editable).
# The fonts themselves are not in the repo:
#   Galmuri 2.40.4 (Korean pixel font, SIL OFL 1.1, github.com/quiple/galmuri)
#   Misaki 2021-05-05 (8x8 Japanese, free: unlimited use, copy, distribute,
#     with or without modification; littlelimit.net/misaki.htm)
#   GNU Unifont 15.1.05 (16x16, GPLv2+ with the font embedding exception,
#     and SIL OFL 1.1; unifoundry.com)
# Run: python3 loc/extract.py <font dir> (galmuri/x/, misaki/x/, unifont/ under it)
import os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from strings import S

SETS = {
    # name: (font file, size, cell w for wide chars, cell h, reference char, top row in the cell, langs)
    "ko8": ("galmuri/x/Galmuri7.ttf", 8, 8, 8, "갈", 1, ["ko"]),
    "ko16u": ("unifont/unifont.otf", 16, 16, 16, "갈", 0, ["ko"]),
    "ko12": ("galmuri/x/Galmuri11.ttf", 12, 16, 16, "갈", 2, ["ko"]),
    "ja8": ("misaki/x/misaki_gothic_2nd.ttf", 8, 8, 8, "漢", 1, ["ja"]),
    "zh16": ("unifont/unifont.otf", 16, 16, 16, "汉", 0, ["zh"]),
}


def chars(langs):
    out = set()
    for l in langs:
        for v in S[l].values():
            out |= {c for c in v if ord(c) > 0x2000}
    return sorted(out)


def render(font, ch, w, h, ref_top, top):
    img = Image.new("L", (64, 64), 255)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((8, 8), ch, font=font, fill=0)
    px = img.load()
    rows = []
    y0 = ref_top - top
    for j in range(h):
        rows.append("".join("X" if px[8 + i, y0 + j] < 128 else "." for i in range(w)))
    ink_total = sum(1 for yy in range(64) for xx in range(64) if px[xx, yy] < 128)
    ink_cell = sum(r.count("X") for r in rows)
    assert ink_cell == ink_total, "%r does not fit its %dx%d cell" % (ch, w, h)
    return rows


def main(fdir):
    for name, (path, size, w, h, ref, top, langs) in SETS.items():
        font = ImageFont.truetype(os.path.join(fdir, path), size)
        ref_top = 64
        for ch in [ref] + chars(langs):             # the shared top row: the highest ink of the set
            img = Image.new("L", (64, 64), 255)
            d = ImageDraw.Draw(img)
            d.fontmode = "1"
            d.text((8, 8), ch, font=font, fill=0)
            ink = [y for y in range(64) for x in range(64) if img.getpixel((x, y)) < 128]
            if ink:
                ref_top = min(ref_top, min(ink))
        lines = ["# %s: from %s at %d px, %dx%d cells (see loc/extract.py for the licence)" % (name, os.path.basename(path), size, w, h)]
        for ch in chars(langs):
            lines.append("U+%04X %s" % (ord(ch), ch))
            lines += render(font, ch, w, h, ref_top, top)
        open(os.path.join(HERE, "glyphs_%s.txt" % name), "w").write("\n".join(lines) + "\n")
        print(name, len(chars(langs)), "glyphs")


if __name__ == "__main__":
    main(sys.argv[1])
