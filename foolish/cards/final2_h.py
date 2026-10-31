#!/usr/bin/env python3
"""final2's faces at error correction H (version 2, 25 x 25): same h3 layout and headlines,
the QR held at no less than 4.4 cm with the quiet zone (33 x 16 px = 528 px = 4.47 cm),
plus a Brighton M-vs-H comparison at actual size with the QR at 3x."""

import os
import sys
import tempfile

from PIL import Image, ImageDraw

import final as fn
import final2 as f2
import make_cards as mc

MODULE_H = 16


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for code, lines in (("BR", ["Останешься<br>в дураках?"]), ("WS", ["Play Durak?"])):
            payload = mc.PAYLOAD_FORMS["www"] + code
            q = mc.qr_symbol(payload, "h", 2)
            for px in range(64, 29, -2):
                img, why = fn.face(tmp, code, "A", 1.0, q, MODULE_H, lines, px, f2.URL_PX, f2.SUIT_H, f2.GAP)
                if img is not None:
                    break
            path = os.path.join(f2.OUT, f"face_{code}_H.png")
            mc.save_print(img, path)
            saved = Image.open(path)
            got = mc.decode(saved)
            share, sure = mc.shrink_sweep(saved, payload)
            ok = got == payload and sure <= mc.SURE_SCALE * mc.W
            failures += not ok
            print(f"{'OK  ' if ok else 'FAIL'} {path}: headline {px} px; decoded {got!r}; {share:.0%} of shrunk sizes read")
    m, h = Image.open(os.path.join(f2.OUT, "face_BR.png")), Image.open(os.path.join(f2.OUT, "face_BR_H.png"))
    g, top = 40, 70
    z = 3
    def qr_crop(img, n, module):
        s = n * module
        x0, y0 = fn.CX - s // 2, fn.CY - s // 2
        return img.crop((x0, y0, x0 + s, y0 + s)).resize((s * z, s * z), Image.NEAREST)
    qm, qh = qr_crop(m, 29, f2.MODULE), qr_crop(h, 33, MODULE_H)
    colw = max(mc.W, qm.size[0], qh.size[0])
    sheet = Image.new("RGB", (2 * colw + 3 * g, top + mc.H + g + max(qm.size[1], qh.size[1]) + 60), (236, 236, 236))
    d = ImageDraw.Draw(sheet)
    d.text((g, 16), "Brighton face, EC M vs EC H. Top: actual size at 300 dpi (print at 100%). Bottom: the QR at 3x.",
           font=mc.font(mc.OSWALD, 26, "Medium"), fill=mc.INK)
    lab = mc.font(mc.OSWALD, 22, "Regular")
    for i, (img, qz, txt) in enumerate((
            (m, qm, f"EC M, version 1, 21 x 21 modules: {f2.MODULE} px = {f2.MODULE / mc.DPI * 25.4:.2f} mm per module, "
                    f"QR {29 * f2.MODULE / mc.DPI * 2.54:.2f} cm with quiet zone"),
            (h, qh, f"EC H, version 2, 25 x 25 modules: {MODULE_H} px = {MODULE_H / mc.DPI * 25.4:.2f} mm per module, "
                    f"QR {33 * MODULE_H / mc.DPI * 2.54:.2f} cm with quiet zone"))):
        x = g + i * (colw + g)
        sheet.paste(img, (x, top))
        d.text((x, top + mc.H + 6), txt, font=lab, fill=mc.INK)
        sheet.paste(qz, (x, top + mc.H + g + 10))
    path = os.path.join(f2.OUT, "compare_M_vs_H.png")
    sheet.save(path, dpi=(mc.DPI, mc.DPI))
    print(path)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
