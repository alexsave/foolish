#!/usr/bin/env python3
"""The final pick sheet: V4 (the game's ace of hearts) with Bicycle-proportioned corner
indices, the headline right above the QR and foolish.cards right below it, in a few close
variants, each with its Brighton (Russian) and Washington Square (English) face, plus the
B2 back cut to MakePlayingCards' real corner radius. Every file is MPC-ready and every QR
is decoded from the saved PNG. Run with the same venv as make_cards.py.
"""

import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import make_cards as mc
from variants import shoot

OUT = os.path.join(mc.OUT, "final")
W, H = mc.W, mc.H
L, T, R, B = mc.TRIM
CX, CY = W // 2, (T + B) // 2
MM = mc.DPI / 25.4  # print px per mm

# ---- the corner index, in Bicycle's proportions ---------------------------------------
# Bicycle (USPCC standard index) on the 88.9 x 63.5 mm poker card, as ratios of the card:
#   rank height 7.0 mm (0.079 of the card height), index suit 5.0 mm tall (0.056),
#   rank top 4.4 mm below the top edge (0.050), gap rank to suit 1.0 mm (0.011),
#   index centre line 5.6 mm in from the side (0.088 of the card width).
# Georgia's bold A is wider than Bicycle's, so its centre moves in until the whole glyph
# clears MPC's safe line; the heights are Bicycle's.
RANK_H, SUIT_H, TOP, GAP, CENTRE = 7.0, 5.0, 4.4, 1.0, 5.6
# Georgia bold in Chrome, measured: ink height per em and ink top below the line box top
# (line-height 1), for the A and the U+2665 heart; ink width per em of the A.
A_INK, A_TOP, A_WIDE = 0.705, 0.14, 0.815
HEART_INK, HEART_TOP = 0.615, 0.24
SIX_INK, SIX_TOP, SIX_WIDE = 0.73, 0.13, 0.56

# MakePlayingCards' cut corner: the cut line in their own poker-size.pdf template, rendered
# at 1200 dpi and fitted with a circle, has a 4.06 mm (0.16 in) radius: 48 px at 300 dpi.
# Bicycle's is 1/8 in (3.2 mm); the frame follows what MPC will actually cut.
CUT_RADIUS = 48
FRAME_W = 30  # B2: the medium frame


def index_html(rank, scale, suit_h=SUIT_H, gap=GAP):
    ink, top, wide = (A_INK, A_TOP, A_WIDE) if rank == "A" else (SIX_INK, SIX_TOP, SIX_WIDE)
    rf = RANK_H * MM * scale / ink
    sf = suit_h * MM * scale / HEART_INK
    cx = max(CENTRE * MM * scale, (mc.SAFE[0] - L) + wide * rf / 2 + 4)  # clear of the safe line
    rank_top = TOP * MM * scale
    suit_top = rank_top + RANK_H * MM * scale + gap * MM * scale
    css = "position:absolute;line-height:1;transform:translateX(-50%);white-space:nowrap;"
    one = (f'<div style="{css}left:{cx:.1f}px;top:{rank_top - top * rf:.1f}px;font-size:{rf:.1f}px">{rank}</div>'
           f'<div style="{css}left:{cx:.1f}px;top:{suit_top - HEART_TOP * sf:.1f}px;font-size:{sf:.1f}px">♥</div>')
    box = f"position:absolute;left:{L}px;top:{T}px;width:{R - L}px;height:{B - T}px;"
    return (f'<div style="{box}">{one}</div>'
            f'<div style="{box}transform:rotate(180deg)">{one}</div>')


def text_html(head, qr_top, qr_bottom, head_px, url_px, gap=10):
    lines = " ".join(head)
    return (f'<div style="position:absolute;left:0;right:0;bottom:{H - qr_top + gap}px;text-align:center;'
            f'font-size:{head_px}px;line-height:1.2">{lines}</div>'
            f'<div style="position:absolute;left:0;right:0;top:{qr_bottom + gap}px;text-align:center;'
            f'font-size:{url_px}px;line-height:1.2;color:#000">{mc.PRINTED_URL}</div>')


def doc(body):
    return ("<!doctype html><html><head><meta charset='utf-8'><style>html,body{margin:0;width:%dpx;height:%dpx;"
            "overflow:hidden;background:#fff;font-family:Georgia,serif;font-weight:bold;color:#dc2626}</style></head>"
            "<body>%s</body></html>") % (W, H, body)


def ink(img):
    return np.asarray(img.convert("L")) < 250


def face(tmp, name, rank, scale, q, module, head, head_px, url_px, suit_h=SUIT_H, gap=GAP):
    n = q.symbol_size(border=4)[0]
    size = n * module
    x0, y0 = CX - size // 2, CY - size // 2
    idx = shoot(doc(index_html(rank, scale, suit_h, gap)), tmp, name + "_i")
    txt = shoot(doc(text_html(head, y0, y0 + size, head_px, url_px)), tmp, name + "_t")
    a, b = ink(idx), ink(txt)
    box = np.zeros_like(a)
    box[y0:y0 + size, x0:x0 + size] = True
    # the indices with 30 px of air around them (about 2.5 mm)
    pad = np.asarray(Image.fromarray(a.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(61))) > 0
    problems = []
    if (pad & b).any():
        problems.append("text touches an index")
    if ((a | b) & box).any():
        problems.append("ink in the QR or its quiet zone")
    ia = np.nonzero(a)
    if ia[1].min() < mc.SAFE[0] or ia[1].max() > mc.SAFE[2] or ia[0].min() < mc.SAFE[1] or ia[0].max() > mc.SAFE[3]:
        problems.append("index outside the safe area")
    tb = np.nonzero(b)
    if tb[1].min() < mc.SAFE[0] or tb[1].max() > mc.SAFE[2] or tb[0].min() < mc.SAFE[1] or tb[0].max() > mc.SAFE[3]:
        problems.append("text outside the safe area")
    if problems:
        return None, problems
    layer = Image.fromarray(np.minimum(np.asarray(idx), np.asarray(txt)))
    out = layer.copy()
    d = ImageDraw.Draw(out)
    for r, row in enumerate(q.matrix_iter(border=4)):
        for c, dark in enumerate(row):
            if dark:
                d.rectangle([x0 + c * module, y0 + r * module, x0 + (c + 1) * module - 1, y0 + (r + 1) * module - 1],
                            fill=(0, 0, 0))
    return out, []


def back(tmp):
    exe = mc.build_fern(tmp)
    out = os.path.join(tmp, "b.rgba")
    subprocess.run([exe, "ios", str(W), str(H), str(W * H * mc.FERN_DENSITY), "1", str(mc.FERN_ALPHA),
                    str(14 * (R - L) / 70), str(W / 2), str(H / 2), out], check=True)
    art = Image.frombytes("RGBA", (W, H), open(out, "rb").read()).convert("RGB")
    s = mc.SS
    m = Image.new("L", (W * s, H * s), 0)
    ImageDraw.Draw(m).rounded_rectangle([(L + FRAME_W) * s, (T + FRAME_W) * s, (R - FRAME_W) * s - 1, (B - FRAME_W) * s - 1],
                                        radius=(CUT_RADIUS - FRAME_W) * s, fill=255)
    return Image.composite(art, Image.new("RGB", (W, H), mc.FRAME), m.resize((W, H), Image.LANCZOS))


#   name   ec   index scale  module cap  headline px  url px   what differs
# Each variant's QR is the largest module up to its cap at which the layout still holds
# (the Brighton headline is the longer one, so it sets the size both twins use).
VARIANTS = [
    ("F1", "m", 1.0, 24, 50, 48, "Bicycle index, QR as big as fits"),
    ("F2", "m", 1.0, 14, 58, 52, "Bicycle index, smaller QR, more air, bigger text"),
    ("F3", "m", 1.1, 24, 50, 48, "index 10% over Bicycle, QR as big as fits"),
    ("F4", "m", 1.0, 24, 44, 44, "Bicycle index, smaller text, so a bigger QR"),
    ("F5", "m", 1.1, 14, 58, 52, "index 10% over Bicycle, smaller QR, more air"),
    ("F6", "h", 1.0, 24, 50, 48, "as F1 at error correction H (version 2)"),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    failures, sheet_items = 0, []
    with tempfile.TemporaryDirectory() as tmp:
        bk = back(tmp)
        mc.save_print(bk, os.path.join(OUT, "back_B2.png"))
        for name, ec, scale, module, head_px, url_px, what in VARIANTS:
            m = module
            for code, _, head in mc.SOURCES:  # BR first: its longer headline sets the size both twins use
                payload = mc.PAYLOAD_FORMS["www"] + code
                q = mc.qr_symbol(payload, ec, mc.QR_VERSIONS[("www", ec)])
                while True:
                    img, why = face(tmp, f"{name}_{code}", "A", scale, q, m, head, head_px, url_px)
                    if img is not None:
                        break
                    assert code == "BR", (name, code, why)
                    m -= 1
                path = os.path.join(OUT, f"{name}_face_{code}.png")
                mc.save_print(img, path)
                saved = Image.open(path)
                got = mc.decode(saved)
                share, sure = mc.shrink_sweep(saved, payload)
                ok = got == payload and sure <= mc.SURE_SCALE * W
                failures += not ok
                cm = q.symbol_size(border=4)[0] * m / mc.DPI * 2.54
                print(f"{'OK  ' if ok else 'FAIL'} {name} {code} QR v{q.version}-{ec.upper()} {m}px modules, "
                      f"{cm:.1f} cm with quiet zone; decoded {got!r}; {share:.0%} of shrunk sizes read")
                if code == "BR":
                    sheet_items.append((name, f"{what}; QR {cm:.1f} cm, {m} px modules, EC {ec.upper()}", img))

    # the sheet: every BR face and the back as cut, at one scale, and the back's corner at 6x
    k = 0.42
    cw, ch = round((R - L) * k), round((B - T) * k)
    items = sheet_items + [("B2", f"back: medium frame, MPC cut corner r = {CUT_RADIUS} px (4.1 mm)", bk)]
    cols, gap, top = 4, 24, 90
    rows = (len(items) + cols - 1) // cols
    zoom = 6
    zw = 120 * zoom
    sheet = Image.new("RGB", (cols * (cw + gap) + gap + zw + gap, max(top + rows * (ch + 92) + 20, top + zw + 80)),
                      (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    d.text((gap, 18), "Foolish promo cards, final picks (BR faces shown; each has a WS English twin). "
                      "Shown as cut, all at one scale. QR: WWW.FOOLISH.CARDS/BR.", font=mc.font(mc.OSWALD, 26, "Medium"),
           fill=mc.INK)
    small = mc.font(mc.OSWALD, 17, "Regular")
    for i, (name, desc, img) in enumerate(items):
        x, y = gap + (i % cols) * (cw + gap), top + (i // cols) * (ch + 92)
        sheet.paste(trimmed_r(img, k), (x, y))
        d.text((x, y + ch + 6), name, font=mc.font(mc.OSWALD, 24, "Bold"), fill=mc.INK)
        line, ly = "", y + ch + 34
        for w_ in desc.split(" "):
            if d.textlength(line + w_, font=small) > cw:
                d.text((x, ly), line.strip(), font=small, fill=(60, 60, 60))
                line, ly = "", ly + 19
            line += w_ + " "
        d.text((x, ly), line.strip(), font=small, fill=(60, 60, 60))
    # the back's top-left corner at 6x, with MPC's cut line drawn on
    zx = cols * (cw + gap) + gap
    corner = bk.crop((0, 0, 120, 120)).resize((zw, zw), Image.NEAREST)
    zd = ImageDraw.Draw(corner)
    c0 = L * zoom
    r = CUT_RADIUS * zoom
    zd.line([(c0 + r, c0), (zw, c0)], fill=(0, 200, 255), width=3)
    zd.line([(c0, c0 + r), (c0, zw)], fill=(0, 200, 255), width=3)
    zd.arc([c0, c0, c0 + 2 * r, c0 + 2 * r], 180, 270, fill=(0, 200, 255), width=3)
    sheet.paste(corner, (zx, top))
    d.text((zx, top + zw + 8), f"B2 corner at 6x. Blue: MPC's cut line, radius {CUT_RADIUS} px = 4.1 mm "
                               f"(Bicycle: 1/8 in = 3.2 mm).", font=small, fill=(60, 60, 60))
    d.text((zx, top + zw + 30), f"Frame {FRAME_W} px; the fern's edge is concentric, radius {CUT_RADIUS - FRAME_W} px.",
           font=small, fill=(60, 60, 60))
    path = os.path.join(OUT, "final_sheet.png")
    sheet.save(path)
    print(path)
    sys.exit(1 if failures else 0)


def trimmed_r(img, k):
    """As cut, with MPC's real corner radius."""
    c = img.crop(mc.TRIM)
    m = Image.new("L", c.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, c.size[0] - 1, c.size[1] - 1], radius=CUT_RADIUS, fill=255)
    return Image.composite(c, Image.new("RGB", c.size, (200, 200, 200)), m).resize(
        (round(c.size[0] * k), round(c.size[1] * k)), Image.LANCZOS)


if __name__ == "__main__":
    main()
