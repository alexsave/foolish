#!/usr/bin/env python3
"""Round 2 on F1 (ace of hearts, EC M, QR fixed at F1's 4.4 cm): three heart-size and drop
steps for the corner index, by three Russian headlines, Brighton faces only, plus the B2
back. A headline that does not fit at its size is set smaller; the QR never shrinks.
Run with the same venv as make_cards.py.
"""

import os
import sys
import tempfile

from PIL import Image, ImageDraw

import final as fn
import make_cards as mc

OUT = os.path.join(mc.OUT, "round2")
CODE = "BR"
PAYLOAD = mc.PAYLOAD_FORMS["www"] + CODE
MODULE = 18          # F1's: 29 modules x 18 px = 522 px = 4.4 cm with the quiet zone, the minimum
HEAD_PX, URL_PX = 50, 48
# heart height and the gap from the rank's foot to the heart's top, in mm (the rank stays 7.0 mm)
HEARTS = [("h1", 5.0, 1.0), ("h2", 6.0, 2.0), ("h3", 7.0, 3.0)]
HEADLINES = [("t1", "Сыграем в дурака?"), ("t2", "Дураком станешь?"), ("t3", "Станешь дураком?")]


def main():
    os.makedirs(OUT, exist_ok=True)
    q = mc.qr_symbol(PAYLOAD, "m", 1)
    cm = q.symbol_size(border=4)[0] * MODULE / mc.DPI * 2.54
    faces, failures = {}, 0
    with tempfile.TemporaryDirectory() as tmp:
        bk = fn.back(tmp)
        mc.save_print(bk, os.path.join(OUT, "back_B2.png"))
        for hk, suit_h, gap in HEARTS:
            for tk, text in HEADLINES:
                name = f"{hk}{tk}"
                px = HEAD_PX
                while True:
                    img, why = fn.face(tmp, name, "A", 1.0, q, MODULE, [text], px, URL_PX, suit_h, gap)
                    if img is not None:
                        break
                    assert "index outside the safe area" not in why and "ink in the QR or its quiet zone" not in why, (name, why)
                    px -= 2
                    assert px >= 30, (name, why)
                path = os.path.join(OUT, f"{name}_face_BR.png")
                mc.save_print(img, path)
                saved = Image.open(path)
                got = mc.decode(saved)
                share, sure = mc.shrink_sweep(saved, PAYLOAD)
                ok = got == PAYLOAD and sure <= mc.SURE_SCALE * mc.W
                failures += not ok
                print(f"{'OK  ' if ok else 'FAIL'} {name}: heart {suit_h} mm, gap {gap} mm, '{text}' at {px} px; "
                      f"QR {cm:.1f} cm; decoded {got!r}; {share:.0%} of shrunk sizes read")
                faces[(hk, tk)] = (img, px)

    k = 0.42
    cw, ch = round((fn.R - fn.L) * k), round((fn.B - fn.T) * k)
    gap_, top, left = 24, 120, 200
    zoom = 2
    zw = 230 * zoom
    rows = len(HEARTS)
    W_ = left + len(HEADLINES) * (cw + gap_) + zw + 2 * gap_ + cw + gap_
    H_ = top + rows * (ch + 60) + 20
    sheet = Image.new("RGB", (W_, H_), (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    big, small = mc.font(mc.OSWALD, 24, "Medium"), mc.font(mc.OSWALD, 17, "Regular")
    d.text((gap_, 16), f"Foolish promo faces, round 2: F1 (ace of hearts), QR {PAYLOAD} at EC M, {cm:.1f} cm with quiet zone "
                       f"({MODULE} px modules) on every face. Shown as cut, one scale.", font=big, fill=mc.INK)
    for j, (tk, text) in enumerate(HEADLINES):
        d.text((left + j * (cw + gap_), top - 34), f"{tk}: {text}", font=mc.font(mc.OSWALD, 20, "Bold"), fill=mc.INK)
    d.text((left + len(HEADLINES) * (cw + gap_) + gap_, top - 34), "corner index, enlarged (blue: cut, pink: safe)", font=mc.font(mc.OSWALD, 20, "Bold"),
           fill=mc.INK)
    for i, (hk, suit_h, gp) in enumerate(HEARTS):
        y = top + i * (ch + 60)
        d.text((gap_, y + 10), hk, font=mc.font(mc.OSWALD, 30, "Bold"), fill=mc.INK)
        d.text((gap_, y + 50), f"heart {suit_h:g} mm", font=small, fill=(60, 60, 60))
        d.text((gap_, y + 70), f"gap {gp:g} mm", font=small, fill=(60, 60, 60))
        d.text((gap_, y + 90), "rank 7 mm", font=small, fill=(60, 60, 60))
        for j, (tk, _) in enumerate(HEADLINES):
            img, px = faces[(hk, tk)]
            x = left + j * (cw + gap_)
            sheet.paste(fn.trimmed_r(img, k), (x, y))
            d.text((x, y + ch + 6), f"{hk}{tk}  (headline {px} px)", font=small, fill=(60, 60, 60))
        img, _ = faces[(hk, "t1")]
        crop = img.crop((0, 0, 230, 330)).resize((230 * zoom, 330 * zoom), Image.LANCZOS)
        cd = ImageDraw.Draw(crop)
        s0 = mc.SAFE[0] * zoom
        cd.rectangle([fn.L * zoom, fn.T * zoom, 230 * zoom, 330 * zoom], outline=(0, 160, 255), width=2)
        cd.rectangle([s0, s0, 230 * zoom + 10, 330 * zoom + 10], outline=(230, 0, 160), width=2)
        crop = crop.resize((round(crop.size[0] * ch / crop.size[1]), ch), Image.LANCZOS)
        sheet.paste(crop, (left + len(HEADLINES) * (cw + gap_) + gap_, y))
    bx = left + len(HEADLINES) * (cw + gap_) + gap_ + round(230 * ch / 330) + gap_
    sheet.paste(fn.trimmed_r(bk, k), (bx, top))
    d.text((bx, top + ch + 6), "B2 back, MPC's 4.06 mm corner", font=small, fill=(60, 60, 60))
    path = os.path.join(OUT, "round2_sheet.png")
    sheet.save(path)
    print(path)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
