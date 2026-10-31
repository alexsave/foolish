#!/usr/bin/env python3
"""Back variants for Alex to pick from: one contact sheet plus each back as its own
MPC-ready 822 x 1122 PNG. Every fern is fern.c with main's parameters (proven against the
e2e digest first); only the framing (zoom, centre, rotation, a top/bottom mirror) and the
frame around it vary. Run with the same venv as make_cards.py.
"""

import math
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw

import make_cards as mc

OUT = os.path.join(mc.OUT, "backs")
W, H = mc.W, mc.H
GOLD = (255, 215, 0)        # the fern's own primary, #ffd700 (DEFAULT_FERN_PARAMS.colors.primary)
WHITE = (255, 255, 255)
GAME_SCALE = 14 * (mc.TRIM[2] - mc.TRIM[0]) / 70   # the app's framing, 150 px per fern unit
BERRY = (2.803, 0.5296)     # circle1, the red bulbs

# name, frame width (px inside the cut), style, zoom, centre ("card" or "berry"), rotate 180, mirror top/bottom
VARIANTS = [
    ("B1", 15, "solid", 1.0, "card", False, False),    # the game's: 1 px of 50, 15 px of 750
    ("B2", 30, "solid", 1.0, "card", False, False),
    ("B3", 50, "solid", 1.0, "card", False, False),
    ("B4", 15, "gold", 1.0, "card", False, False),
    ("B5", 30, "gold", 1.8, "berry", False, False),
    ("B6", 15, "white", 1.0, "card", False, False),
    ("B7", 30, "white-gold", 1.0, "card", False, False),
    ("B8", 30, "solid", 0.6, "card", False, False),
    ("B9", 30, "solid", 1.0, "card", False, True),
    ("B10", 30, "gold", 1.0, "card", True, False),
    ("B11", 50, "gold", 0.6, "card", False, True),
]
WHITE_MARGIN = 24   # px of white inside the cut before the red, like a classic card's border
GOLD_LINE = 5       # px, at the red's inner edge
RADIUS = 75         # the game's 5 px corner on a 50 px card, at the cut


def fern(exe, tmp, zoom, centre):
    sc = GAME_SCALE * zoom
    cx, cy = W / 2, H / 2
    if centre == "berry":
        a = -67 * math.pi / 180
        rx = BERRY[0] * math.cos(a) - BERRY[1] * math.sin(a)
        ry = BERRY[0] * math.sin(a) + BERRY[1] * math.cos(a)
        cx, cy = W / 2 - rx * sc, H / 2 + ry * sc
    out = os.path.join(tmp, "f.rgba")
    # same tone as the app: points per pixel of fern, so the count grows with the zoom squared
    n = int(W * H * mc.FERN_DENSITY * zoom * zoom)
    subprocess.run([exe, "ios", str(W), str(H), str(n), "1", str(mc.FERN_ALPHA), str(sc), str(cx), str(cy), out],
                   check=True)
    return Image.frombytes("RGBA", (W, H), open(out, "rb").read()).convert("RGB")


def mask(inset, radius):
    """Antialiased rounded rectangle `inset` px inside the cut line."""
    s = mc.SS
    m = Image.new("L", (W * s, H * s), 0)
    x0, y0, x1, y1 = (v * s for v in mc.TRIM)
    ImageDraw.Draw(m).rounded_rectangle([x0 + inset * s, y0 + inset * s, x1 - inset * s - 1, y1 - inset * s - 1],
                                        radius=max(1, radius) * s, fill=255)
    return m.resize((W, H), Image.LANCZOS)


def frame(art, width, style):
    margin = WHITE_MARGIN if style.startswith("white") else 0
    base = Image.new("RGB", (W, H), WHITE if margin else mc.FRAME)
    if margin:  # red from the white margin inwards
        base = Image.composite(Image.new("RGB", (W, H), mc.FRAME), base, mask(margin, RADIUS - margin))
    inner = margin + width
    if style.endswith("gold"):
        base = Image.composite(Image.new("RGB", (W, H), GOLD), base, mask(inner - GOLD_LINE, RADIUS - inner + GOLD_LINE))
    return Image.composite(art, base, mask(inner, RADIUS - inner))


def main():
    os.makedirs(OUT, exist_ok=True)
    backs = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = mc.build_fern(tmp)
        for name, width, style, zoom, centre, rot, mirror in VARIANTS:
            art = fern(exe, tmp, zoom, centre)
            if rot:
                art = art.rotate(180)
            if mirror:  # the top half, reflected onto the bottom: a real card back reads the same both ways up
                top = art.crop((0, 0, W, H // 2))
                art.paste(top.transpose(Image.FLIP_TOP_BOTTOM), (0, H - H // 2))
            img = frame(art, width, style)
            path = os.path.join(OUT, f"{name}_back.png")
            mc.save_print(img, path)
            desc = (f"{ {15: 'thin (game)', 30: 'medium', 50: 'thick'}[width] } {style} frame, "
                    f"{'game framing' if zoom == 1 and centre == 'card' else ('berries x%.1f' % zoom if centre == 'berry' else 'zoomed out x%.1f' % zoom)}"
                    f"{', rotated 180' if rot else ''}{', mirrored top/bottom' if mirror else ''}")
            print(f"{name:4s} {desc}")
            backs.append((name, desc, img))

    from variants import trimmed  # the same as-cut rendering as the face sheet
    k = 0.42
    cw, ch = round(750 * k), round(1050 * k)
    cols, gap, top = 6, 24, 90
    rows = (len(backs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cw + gap) + gap, top + rows * (ch + 70) + 20), (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    d.text((gap, 18), "Foolish promo backs: the app's fern (fern.c, main's parameters), shown as cut, all at the same scale.",
           font=mc.font(mc.OSWALD, 26, "Medium"), fill=mc.INK)
    small = mc.font(mc.OSWALD, 17, "Regular")
    for i, (name, desc, img) in enumerate(backs):
        x, y = gap + (i % cols) * (cw + gap), top + (i // cols) * (ch + 70)
        sheet.paste(trimmed(img, k), (x, y))
        d.text((x, y + ch + 6), name, font=mc.font(mc.OSWALD, 24, "Bold"), fill=mc.INK)
        line, ly = "", y + ch + 34
        for w in desc.split(" "):
            if d.textlength(line + w, font=small) > cw:
                d.text((x, ly), line.strip(), font=small, fill=(60, 60, 60))
                line, ly = "", ly + 19
            line += w + " "
        d.text((x, ly), line.strip(), font=small, fill=(60, 60, 60))
    path = os.path.join(OUT, "backs_sheet.png")
    sheet.save(path)
    print(path)


if __name__ == "__main__":
    main()
