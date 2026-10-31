#!/usr/bin/env python3
"""Face variants for Alex to pick from: one contact sheet plus each face as its own
MPC-ready 822 x 1122 PNG, all for the Brighton (BR) source at error correction M.

Every variant keeps CardFace.tsx's default style (Georgia bold, #dc2626, white card, the
rank over the heart in a centred column, the same index turned 180 degrees bottom-right)
and varies only sizes and placement. Run with the same venv as make_cards.py.
"""

import html
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

import make_cards as mc

OUT = os.path.join(mc.OUT, "variants")
CODE, NAME, HEAD = mc.SOURCES[0]
PAYLOAD = mc.PAYLOAD_FORMS["www"] + CODE
ERROR = "m"

# Index sizes in print px: rank font (Georgia bold cap height is about 0.69 em, so 112 px
# is a 6.5 mm capital, a real poker index) and the heart's font under it.
INDEX = {"small": (112, 80), "medium": (160, 112)}
INSET = 40              # index inset from the cut line, about 3.4 mm, like a real card
MODULE = {"max": 15, "moderate": 12, "pips": 11}

#        name   rank  index     qr          pips   head      url
VARIANTS = [
    ("V1", "A", "small", "max", False, "centre", "centre"),
    ("V2", "A", "small", "moderate", False, "above", "centre"),
    ("V3", "A", "small", "max", False, "right", "left"),
    ("V4", "A", "medium", "max", False, "centre", "centre"),
    ("V5", "A", "medium", "moderate", False, "above", "left"),
    ("V6", "6", "small", "max", False, "centre", "centre"),
    ("V7", "6", "small", "pips", True, "centre", "centre"),
    ("V8", "6", "small", "pips", True, "centre", "left"),
    ("V9", "6", "small", "moderate", False, "right", "left"),
    ("V10", "6", "medium", "max", False, "centre", "left"),
]

CX, CY = mc.W // 2, (mc.TRIM[1] + mc.TRIM[3]) // 2
L, T, R, B = mc.TRIM


def page(rank, index, pips, head, url, qr_top):
    rf, sf = INDEX[index]
    ix = (f'<div>{rank}</div><div><span style="font-size:{sf}px">♥</span></div>')
    ixcss = (f"position:absolute;width:{rf * 0.6:.0f}px;font-size:{rf}px;line-height:{rf * 0.75:.0f}px;"
             "display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;")
    parts = [f'<div style="{ixcss}left:{L + INSET}px;top:{T + INSET}px">{ix}</div>',
             f'<div style="{ixcss}right:{mc.W - R + INSET}px;bottom:{mc.H - B + INSET}px;transform:rotate(180deg)">{ix}</div>']
    hs = 60 if index == "small" else 54
    lines = "<br>".join(html.escape(x) for x in HEAD)
    hcss = f"position:absolute;font-size:{hs}px;line-height:{hs * 1.15:.0f}px;"
    if head == "centre":
        parts.append(f'<div style="{hcss}left:0;right:0;top:{T + 60}px;text-align:center">{lines}</div>')
    elif head == "right":
        parts.append(f'<div style="{hcss}right:{mc.W - R + INSET}px;top:{T + INSET + 10}px;text-align:right">{lines}</div>')
    else:  # above the QR
        parts.append(f'<div style="{hcss}left:0;right:0;bottom:{mc.H - qr_top + 4}px;text-align:center">{lines}</div>')
    ucss = "position:absolute;font-size:52px;line-height:60px;color:#000000;"
    if url == "centre":
        parts.append(f'<div style="{ucss}left:0;right:0;bottom:{mc.H - B + 52}px;text-align:center">{mc.PRINTED_URL}</div>')
    else:
        parts.append(f'<div style="{ucss}left:{L + INSET}px;bottom:{mc.H - B + INSET}px">{mc.PRINTED_URL}</div>')
    if pips:  # the six's six pips, two columns beside the QR, the bottom row turned like a real card
        dx, dy = (130, 235) if index == "small" else (150, 205)  # clear of the bigger index
        for x in (L + dx, R - dx):
            for i, y in enumerate((CY - dy, CY, CY + dy)):
                rot = " rotate(180deg)" if i == 2 else ""
                parts.append(f'<div style="position:absolute;left:{x}px;top:{y}px;font-size:120px;line-height:1;'
                             f'transform:translate(-50%,-50%){rot}">♥</div>')
    return ("<!doctype html><html><head><meta charset='utf-8'><style>html,body{margin:0;width:%dpx;height:%dpx;"
            "overflow:hidden;background:#fff;font-family:Georgia,serif;font-weight:bold;color:#dc2626}</style></head>"
            "<body>%s</body></html>") % (mc.W, mc.H, "".join(parts))


def shoot(doc, tmp, name):
    p, s = os.path.join(tmp, name + ".html"), os.path.join(tmp, name + ".png")
    open(p, "w", encoding="utf-8").write(doc)
    subprocess.run(["nice", "-n", "10", mc.CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
                    "--no-default-browser-check", f"--user-data-dir={os.path.join(tmp, 'chrome')}",
                    "--hide-scrollbars", "--force-device-scale-factor=1", f"--window-size={mc.W},{mc.H}",
                    f"--screenshot={s}", "file://" + p], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return Image.open(s).convert("RGB")


def place_qr(layer, q, module):
    """Dark modules centred on the card; None if the QR or its quiet zone would touch ink."""
    n = q.symbol_size(border=4)[0]
    x0, y0 = CX - n * module // 2, CY - n * module // 2
    if not (np.asarray(layer.crop((x0, y0, x0 + n * module, y0 + n * module))) == 255).all():
        return None
    img = layer.copy()
    d = ImageDraw.Draw(img)
    for r, row in enumerate(q.matrix_iter(border=4)):
        for c, dark in enumerate(row):
            if dark:
                d.rectangle([x0 + c * module, y0 + r * module, x0 + (c + 1) * module - 1, y0 + (r + 1) * module - 1],
                            fill=(0, 0, 0))
    return img


def trimmed(img, k):
    """The card as cut, rounded like MPC's corners, scaled by k."""
    c = img.crop(mc.TRIM)
    m = Image.new("L", c.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, c.size[0] - 1, c.size[1] - 1], radius=38, fill=255)
    bg = Image.new("RGB", c.size, (200, 200, 200))
    return Image.composite(c, bg, m).resize((round(c.size[0] * k), round(c.size[1] * k)), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    q = mc.qr_symbol(PAYLOAD, ERROR, mc.QR_VERSIONS[("www", ERROR)])
    n = q.symbol_size(border=4)[0]
    faces, failures = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        back = mc.make_back(mc.build_fern(tmp), tmp, True)
        for name, rank, index, qr, pips, head, url in VARIANTS:
            module = MODULE[qr]
            while True:  # the largest module up to the variant's that leaves the QR on bare card
                layer = shoot(page(rank, index, pips, head, url, CY - n * module // 2), tmp, name)
                face = place_qr(layer, q, module)
                if face is not None:
                    break
                module -= 1
                assert module >= 6, name
            path = os.path.join(OUT, f"{name}_face_BR.png")
            mc.save_print(face, path)
            got = mc.decode(Image.open(path))
            share, sure = mc.shrink_sweep(Image.open(path), PAYLOAD)
            ok = got == PAYLOAD and sure <= mc.SURE_SCALE * mc.W
            failures += not ok
            cm = n * module / mc.DPI * 2.54
            desc = f"{'ace' if rank == 'A' else 'six'}, {index} index, QR {cm:.1f} cm{' + pips' if pips else ''}, head {head}, url {url}"
            print(f"{'OK  ' if ok else 'FAIL'} {name:4s} {desc}: {module}px modules, decoded {got!r}, {share:.0%} of shrunk sizes read")
            faces.append((name, desc, face))

    k = 0.42
    cw, ch = round(750 * k), round(1050 * k)
    cols, gap, top = 6, 24, 90
    rows = (len(faces) + 1 + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cw + gap) + gap, top + rows * (ch + 70) + 20), (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    d.text((gap, 18), f"Foolish promo faces for {CODE} ({NAME}), QR {PAYLOAD} at error correction M (version 1). "
           f"All cards at the same scale, shown as cut.", font=mc.font(mc.OSWALD, 26, "Medium"), fill=mc.INK)
    small = mc.font(mc.OSWALD, 17, "Regular")
    items = faces + [("BACK", "the fern in the game's frame", back)]
    for i, (name, desc, img) in enumerate(items):
        x, y = gap + (i % cols) * (cw + gap), top + (i // cols) * (ch + 70)
        sheet.paste(trimmed(img, k), (x, y))
        d.text((x, y + ch + 6), name, font=mc.font(mc.OSWALD, 24, "Bold"), fill=mc.INK)
        words, line, ly = desc.split(" "), "", y + ch + 34
        d.text((x, ly), desc, font=small, fill=(60, 60, 60)) if d.textlength(desc, font=small) <= cw else None
        if d.textlength(desc, font=small) > cw:
            for w in words:
                if d.textlength(line + w, font=small) > cw:
                    d.text((x, ly), line.strip(), font=small, fill=(60, 60, 60))
                    line, ly = "", ly + 19
                line += w + " "
            d.text((x, ly), line.strip(), font=small, fill=(60, 60, 60))
    path = os.path.join(OUT, "variants_sheet.png")
    sheet.save(path)
    print(path)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
