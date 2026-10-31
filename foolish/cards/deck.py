#!/usr/bin/env python3
"""The 36-card Durak deck (6 to A in four suits) in the final2 layout: h3 corner index with
each card's own rank and suit, the QR at EC H (16 px modules, 4.47 cm with the quiet zone),
headline right above it, foolish.cards right below, the B2 back for all.

Spades and hearts carry the Brighton face (Russian headline, WWW.FOOLISH.CARDS/BR);
diamonds and clubs the Washington Square face (English, WWW.FOOLISH.CARDS/WS).

Ranks and suits are the game's default theme: the kernel's labels and glyphs
(foolish/c/cardtext/card_text.h; the labels are not localized, so every card says
6 7 8 9 10 J Q K A), CardFace.tsx's Georgia bold, red #dc2626 for hearts and diamonds,
black #000000 for spades and clubs.
"""

import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import final as fn
import make_cards as mc
from variants import shoot

OUT = os.path.join(mc.OUT, "deck")
MM = fn.MM
L, T, R, B = mc.TRIM
CX, CY = fn.CX, fn.CY
MODULE = 16                                 # EC H, version 2: 33 x 16 px = 528 px = 4.47 cm
RANK_H, SUIT_H, TOP, GAP = 7.0, 7.0, 4.4, 3.0   # h3, mm
RED, BLACK = "#dc2626", "#000000"
RANKS = ["6", "7", "8", "9", "10", "J", "Q", "K", "A"]
# deck order: spades, hearts, diamonds, clubs; (letter, glyph, ink, face)
SUITS = [("S", "♠", BLACK, "BR"), ("H", "♥", RED, "BR"),
         ("D", "♦", RED, "WS"), ("C", "♣", BLACK, "WS")]
FACES = {"BR": ["Останешься<br>в дураках?"], "WS": ["Play Durak?"]}
URL_PX = 48

# Georgia bold in Chrome, measured at line-height 1: ink top, ink bottom, ink left and ink
# width, per em. Georgia's figures are old-style: 7 and 9 hang below the line, and 1 and 0
# stand only x-height tall.
INK = {
    "A": (0.14, 0.845, -0.03, 0.815), "K": (0.15, 0.845, 0.03, 0.82), "Q": (0.13, 1.045, 0.035, 0.75),
    "J": (0.15, 0.865, 0.005, 0.565), "6": (0.13, 0.86, 0.05, 0.56), "7": (0.315, 1.025, 0.04, 0.52),
    "8": (0.13, 0.865, 0.05, 0.575), "9": (0.30, 1.03, 0.04, 0.56),
    "10": (0.30, 0.86, 0.055, 1.005),      # with the game's own letter-spacing -0.08em
    "♠": (0.245, 0.845, 0.025, 0.48), "♥": (0.24, 0.855, 0.03, 0.535),
    "♣": (0.24, 0.845, 0.025, 0.605), "♦": (0.245, 0.855, 0.03, 0.45),
}
# Q's tail hangs 0.2 em below its cap. Sized by the cap alone it would end 0.9 mm above
# the suit; instead its whole ink, tail included, is 8.4 mm, so the cap is 6.6 mm and the
# tail stops halfway into the 3 mm gap.
FULL = {"Q": 8.4}

# Every rank is 7 mm of ink tall with its top on the same line, so the corners line up
# across the deck. One font size for the letters and 6 to 9 (each is about 0.71 em of ink);
# "10", whose old-style figures are only x-height, is set larger to the same 7 mm and then
# condensed horizontally, the way real cards narrow their 10, to TEN_WIDE of the K.
TEN_WIDE = 1.15
K_W = INK["K"][3] * RANK_H * MM / (INK["K"][1] - INK["K"][0])
TEN_W = TEN_WIDE * K_W
# One centre line for every index on every card: as far in as the widest rank (the
# condensed 10) needs to clear MPC's safe line by 4 px.
AXIS = (mc.SAFE[0] - L) + TEN_W / 2 + 4


def glyph(text, colour, ink_top_px, height_px, cx, wide=None, extra=""):
    t, b, left, w = INK[text]
    fs = (FULL[text] * MM if text in FULL else height_px) / (b - t)
    width = w * fs
    sx = 1.0 if wide is None else min(1.0, wide / width)
    ink_cx = (left + w / 2) * fs
    return (f'<div style="position:absolute;line-height:1;white-space:nowrap;color:{colour};font-size:{fs:.2f}px;'
            f'left:{cx - ink_cx:.2f}px;top:{ink_top_px - t * fs:.2f}px;transform-origin:{ink_cx:.2f}px 0;'
            f'transform:scaleX({sx:.4f});{extra}">{text}</div>')


def index_html(rank, suit, colour):
    rank_top = TOP * MM
    suit_top = rank_top + (RANK_H + GAP) * MM
    one = (glyph(rank, colour, rank_top, RANK_H * MM, AXIS,
                 wide=TEN_W if rank == "10" else None,
                 extra="letter-spacing:-0.08em;" if rank == "10" else "")
           + glyph(suit, colour, suit_top, SUIT_H * MM, AXIS))
    box = f"position:absolute;left:{L}px;top:{T}px;width:{R - L}px;height:{B - T}px;"
    return f'<div style="{box}">{one}</div><div style="{box}transform:rotate(180deg)">{one}</div>'


def text_html(head, colour, qr_top, qr_bottom, head_px, gap=10):
    return (f'<div style="position:absolute;left:0;right:0;bottom:{mc.H - qr_top + gap}px;text-align:center;'
            f'font-size:{head_px}px;line-height:1.2;color:{colour}">{" ".join(head)}</div>'
            f'<div style="position:absolute;left:0;right:0;top:{qr_bottom + gap}px;text-align:center;'
            f'font-size:{URL_PX}px;line-height:1.2;color:#000">{mc.PRINTED_URL}</div>')


def inside_safe(mask):
    ys, xs = np.nonzero(mask)
    return xs.min() >= mc.SAFE[0] and xs.max() <= mc.SAFE[2] and ys.min() >= mc.SAFE[1] and ys.max() <= mc.SAFE[3]


def render(tmp, name, rank, suit, colour, q, head, head_px, idx_cache):
    n = q.symbol_size(border=4)[0]
    size = n * MODULE
    x0, y0 = CX - size // 2, CY - size // 2
    if name not in idx_cache:
        idx_cache[name] = shoot(fn.doc(index_html(rank, suit, colour)), tmp, name + "_i")
    idx = idx_cache[name]
    txt = shoot(fn.doc(text_html(head, RED, y0, y0 + size, head_px)), tmp, name + "_t")
    a, b = fn.ink(idx), fn.ink(txt)
    box = np.zeros_like(a)
    box[y0:y0 + size, x0:x0 + size] = True
    pad = np.asarray(Image.fromarray(a.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(61))) > 0
    assert inside_safe(a), f"{name}: the index leaves the safe area"
    assert not (a & box).any(), f"{name}: the index reaches the QR"
    if (pad & b).any() or ((b & box).any()) or not inside_safe(b):
        return None
    out = Image.fromarray(np.minimum(np.asarray(idx), np.asarray(txt)))
    d = ImageDraw.Draw(out)
    for r, row in enumerate(q.matrix_iter(border=4)):
        for c, dark in enumerate(row):
            if dark:
                d.rectangle([x0 + c * MODULE, y0 + r * MODULE, x0 + (c + 1) * MODULE - 1, y0 + (r + 1) * MODULE - 1],
                            fill=(0, 0, 0))
    return out


def rank_suit_gap(idx):
    """Clear px between the rank's ink and the suit's in the top-left index."""
    a = fn.ink(idx)[:, :mc.W // 2][: mc.H // 2]
    rows = np.nonzero(a.any(1))[0]
    runs = np.split(rows, np.nonzero(np.diff(rows) > 1)[0] + 1)
    return runs[1][0] - runs[0][-1] - 1 if len(runs) > 1 else 0


def main():
    os.makedirs(OUT, exist_ok=True)
    cards = [(s, r) for s in SUITS for r in RANKS]
    idx_cache, failures = {}, 0
    with tempfile.TemporaryDirectory() as tmp:
        bk = fn.back(tmp)
        mc.save_print(bk, os.path.join(OUT, "back.png"))
        # one headline size per face: the largest that fits on every card that carries it
        head_px = {}
        for face_code in FACES:
            px = 64
            for (letter, g, colour, fc), rank in cards:
                if fc != face_code:
                    continue
                q = mc.qr_symbol(mc.PAYLOAD_FORMS["www"] + fc, "h", 2)
                while render(tmp, f"{rank}{letter}", rank, g, colour, q, FACES[fc], px, idx_cache) is None:
                    px -= 2
                    assert px >= 30
            head_px[face_code] = px
            print(f"     {face_code} headline {px} px on every card")
        faces = []
        for i, ((letter, g, colour, fc), rank) in enumerate(cards, 1):
            payload = mc.PAYLOAD_FORMS["www"] + fc
            q = mc.qr_symbol(payload, "h", 2)
            name = f"{rank}{letter}"
            img = render(tmp, name, rank, g, colour, q, FACES[fc], head_px[fc], idx_cache)
            gap = rank_suit_gap(idx_cache[name])
            path = os.path.join(OUT, f"{i:02d}_{name}.png")
            mc.save_print(img, path)
            got = mc.decode(Image.open(path))
            ok = got == payload and gap >= 8
            failures += not ok
            print(f"{'OK  ' if ok else 'FAIL'} {os.path.basename(path):10s} decoded {got!r}; rank to suit {gap} px")
            faces.append((f"{i:02d} {name}", img))

    # the sheet: 36 faces and the back as cut, one scale, and the 9/10/J of hearts corners at 3x
    k = 0.2
    cw, ch = round((R - L) * k), round((B - T) * k)
    g_, top = 14, 64
    cols = 9
    zoom = 3
    crop_w, crop_h = 200, 330
    inset_h = crop_h * zoom
    sheet_w = cols * (cw + g_) + g_
    sheet_h = top + 4 * (ch + 30) + 40 + (inset_h + 50)
    sheet = Image.new("RGB", (max(sheet_w, 6 * (crop_w * zoom // 2 + g_) + cw + 3 * g_), sheet_h), (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    d.text((g_, 14), "Foolish 36-card Durak deck. Spades and hearts: Brighton (WWW.FOOLISH.CARDS/BR); diamonds and clubs: "
                     "Washington Square (WWW.FOOLISH.CARDS/WS). QR EC H, 4.47 cm. Shown as cut, one scale.",
           font=mc.font(mc.OSWALD, 22, "Medium"), fill=mc.INK)
    small = mc.font(mc.OSWALD, 15, "Regular")
    for i, (label, img) in enumerate(faces):
        x, y = g_ + (i % cols) * (cw + g_), top + (i // cols) * (ch + 30)
        sheet.paste(fn.trimmed_r(img, k), (x, y))
        d.text((x, y + ch + 4), label, font=small, fill=(60, 60, 60))
    y = top + 4 * (ch + 30) + 30
    d.text((g_, y - 6), "Corner indices at 1.5x of print: 9, 10 and J of hearts, top-left and the rotated bottom-right. "
                        "Blue: cut line; pink: MPC's safe line.", font=mc.font(mc.OSWALD, 18, "Medium"), fill=mc.INK)
    x = g_
    for name in ("9H", "10H", "JH"):
        img = next(im for lab, im in faces if lab.endswith(" " + name))
        for corner in ("tl", "br"):
            c = img.crop((0, 0, crop_w, crop_h)) if corner == "tl" else img.crop((mc.W - crop_w, mc.H - crop_h, mc.W, mc.H))
            ox, oy = (0, 0) if corner == "tl" else (mc.W - crop_w, mc.H - crop_h)
            cd = ImageDraw.Draw(c)
            cd.rectangle([L - ox, T - oy, R - ox, B - oy], outline=(0, 160, 255), width=1)
            cd.rectangle([mc.SAFE[0] - ox, mc.SAFE[1] - oy, mc.SAFE[2] - ox, mc.SAFE[3] - oy], outline=(230, 0, 160), width=1)
            c = c.resize((crop_w * zoom // 2, crop_h * zoom // 2), Image.LANCZOS)
            sheet.paste(c, (x, y + 24))
            d.text((x, y + 24 + c.size[1] + 4), f"{name} {'top-left' if corner == 'tl' else 'bottom-right'}", font=small,
                   fill=(60, 60, 60))
            x += c.size[0] + g_
    sheet.paste(fn.trimmed_r(bk, k * 1.5), (x + g_, y + 24))
    d.text((x + g_, y + 24 + round((B - T) * k * 1.5) + 4), "back (B2)", font=small, fill=(60, 60, 60))
    sheet = sheet.crop((0, 0, sheet.size[0], y + 24 + max(crop_h * zoom // 2, round((B - T) * k * 1.5)) + 30))
    path = os.path.join(OUT, "deck_sheet.png")
    sheet.save(path)
    print(path)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
