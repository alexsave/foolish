#!/usr/bin/env python3
"""The chosen design: layout h3 (rank 7 mm, heart 7 mm, gap 3 mm, the rotated twin), QR
4.4 cm at EC M, back B2. Brighton says "Останешься в дураках?", Washington Square "Play
Durak?". The headline is set as large as fits, on one line or two, whichever is larger;
the QR never shrinks. Writes out/final2/face_BR.png, face_WS.png, back.png and a preview.
"""

import os
import sys
import tempfile

from PIL import Image, ImageDraw

import final as fn
import make_cards as mc

OUT = os.path.join(mc.OUT, "final2")
MODULE = 18                 # 29 x 18 px = 522 px = 4.4 cm with the quiet zone
SUIT_H, GAP = 7.0, 3.0      # h3
URL_PX = 48
HEADS = {
    "BR": [["Останешься в дураках?"], ["Останешься<br>в дураках?"]],
    "WS": [["Play Durak?"], ["Play<br>Durak?"]],
}


def fit(tmp, code, q, lines):
    for px in range(64, 29, -2):
        img, why = fn.face(tmp, code, "A", 1.0, q, MODULE, lines, px, URL_PX, SUIT_H, GAP)
        if img is not None:
            return img, px
    return None, 0


def main():
    os.makedirs(OUT, exist_ok=True)
    failures, faces = 0, []
    with tempfile.TemporaryDirectory() as tmp:
        bk = fn.back(tmp)
        mc.save_print(bk, os.path.join(OUT, "back.png"))
        for code in ("BR", "WS"):
            payload = mc.PAYLOAD_FORMS["www"] + code
            q = mc.qr_symbol(payload, "m", 1)
            tries = [(fit(tmp, code, q, lines), lines) for lines in HEADS[code]]
            (img, px), lines = max(tries, key=lambda t: t[0][1])
            for (i, p), l in tries:
                print(f"     {code} {'two lines' if '<br>' in l[0] else 'one line'}: {p} px")
            path = os.path.join(OUT, f"face_{code}.png")
            mc.save_print(img, path)
            saved = Image.open(path)
            got = mc.decode(saved)
            share, sure = mc.shrink_sweep(saved, payload)
            ok = got == payload and sure <= mc.SURE_SCALE * mc.W
            failures += not ok
            how = "two lines" if "<br>" in lines[0] else "one line"
            print(f"{'OK  ' if ok else 'FAIL'} {path}: headline {how} at {px} px; decoded {got!r}; "
                  f"{share:.0%} of shrunk sizes read")
            faces.append((f"{code}: {lines[0].replace('<br>', ' / ')} ({how}, {px} px)", img))
    k = 0.5
    cw, ch = round((fn.R - fn.L) * k), round((fn.B - fn.T) * k)
    g = 30
    sheet = Image.new("RGB", (3 * cw + 4 * g, ch + 140), (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    d.text((g, 16), "Foolish promo cards, final: QR WWW.FOOLISH.CARDS/<BR|WS>, EC M, 4.4 cm. Shown as cut.",
           font=mc.font(mc.OSWALD, 24, "Medium"), fill=mc.INK)
    small = mc.font(mc.OSWALD, 17, "Regular")
    for i, (label, img) in enumerate(faces + [("back: B2, MPC's 4.06 mm corner", bk)]):
        x = g + i * (cw + g)
        sheet.paste(fn.trimmed_r(img, k), (x, 60))
        d.text((x, 60 + ch + 8), label, font=small, fill=(60, 60, 60))
    path = os.path.join(OUT, "preview.png")
    sheet.save(path)
    print(path)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
