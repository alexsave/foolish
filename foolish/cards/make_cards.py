#!/usr/bin/env python3
"""Foolish promo playing cards: print-ready PNGs for MakePlayingCards (MPC).

One back (the app's own IFS fern, rendered by fern.c) and one face per QR source,
in three face layouts. Every QR is decoded back from the final PNG and checked
against the exact payload. See README.md for the commands.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

import cv2
import numpy as np
import segno
from PIL import Image, ImageDraw, ImageFont

# ---- edit me -------------------------------------------------------------------------
# One card per source: (short path, name). The QR carries the payload form's prefix +
# short path, all upper-case so the QR stays in alphanumeric mode. The printed URL on
# the card is always the clean "foolish.cards".
# Every short path must hold at least one character outside the game-id alphabet
# (GAME_ID_ALPHABET below, from foolish/server/src/store/game_id.h) and must not be
# GAME_ID_LEN long, so it can never be read as a game id. main() asserts both.
SOURCES = [
    ("BR", "brighton"),   # Brighton Beach
    ("WS", "wsp"),        # Washington Square Park
]
# Payload forms: prefix, error correction, the QR version it must come out as (asserted).
# www is the lobby QR's form (foolish/src/components/Lobby.tsx); Alex's phone opened
# WWW.FOOLISH.CARDS/BR as a link, while the bare FOOLISH.CARDS/BR opened a web search.
PAYLOAD_FORMS = {
    "www": ("WWW.FOOLISH.CARDS/", "m", 1),
    "https": ("HTTPS://FOOLISH.CARDS/", "q", 2),
}
PAYLOAD_FORM = "www"
PRINTED_URL = "foolish.cards"
HEADLINE_RU = "Сыграем в дурака?"
HEADLINE_EN = "Play Durak"
# ---------------------------------------------------------------------------------------

GAME_ID_ALPHABET = "0123456789abcdef"
GAME_ID_LEN = 6

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)  # foolish/
OUT = os.path.join(HERE, "out")
FERN_C = os.path.join(HERE, "fern.c")
JESTER = os.path.join(REPO, "ios", "Tools", "icon-source", "jester-d.png")
OSWALD = os.path.join(HERE, "fonts", "Oswald.ttf")
BEBAS = os.path.join(HERE, "fonts", "BebasNeue-Regular.ttf")

# MPC "Traditional Poker sized card (2.5" x 3.5")" template, poker-size.pdf from
# https://www.makeplayingcards.com/pops/pc-temp-guide.html :
#   full bleed 2.74 x 3.74 in = 822 x 1122 px, cut 750 x 1050 px, safe 690 x 990 px, 300 dpi.
DPI = 300
W, H = 822, 1122
TRIM = (36, 36, 786, 1086)   # cut line, 36 px bleed each side
SAFE = (66, 66, 756, 1056)   # 30 px inside the cut line
CX = W // 2

RED = (179, 41, 41)          # --color-card-back / soviet red, foolish/src/styles/variables.css
INK = (12, 10, 10)
PAPER = (250, 247, 240)
WHITE = (255, 255, 255)

SS = 3  # supersampling for the drawn art (the QR is drawn afterwards, pixel exact)


def font(path, size, weight=None):
    f = ImageFont.truetype(path, size)
    if weight:
        f.set_variation_by_name(weight)
    return f


# ---- the back ---------------------------------------------------------------------------
# The app's card back is generated, not drawn: the IFS fern of fernFractal.tsx (gold
# spine, red and bronze bulbs, on black, under a thin dark-red frame in CardBack.tsx).
# fern.c is a port of it; build_fern() proves the port against the e2e test's digest of
# the real code before anything is rendered.

# foolish/e2e/fern_texture.test.ts: the title fern (1400 x 1960, 12M points, 2 px dots,
# scaleY 14 * 1400/70), off the seeded LCG, through the no-WebGL path (points computed
# twice), must paint exactly these pixels after exactly this many random draws.
WEB_DIGEST = "9c51cfc05121234948dfa83cf87b3214182c840c79583bacb02eda44cf8c07b6"
WEB_DRAWS = "50884422"

# FernCardBack.swift's look, the owner's round-7 pick: 1 px dots, 14 per pixel, alpha 0.20.
FERN_DENSITY = 14
FERN_ALPHA = 0.20
FRAME = (139, 0, 0)   # #8B0000, CardBack.tsx's border (1 px on a 50 px card)
FRAME_PX = 15         # the same 1/50 of the 750 px cut width
# Off by default: the fern runs through the bleed. A band this close to the cut line
# shows MPC's cut tolerance (about 1/16 in) as a visibly uneven border.


def build_fern(tmp):
    exe = os.path.join(tmp, "fern")
    subprocess.run(["cc", "-O2", "-ffp-contract=off", "-o", exe, FERN_C, "-lm"], check=True)
    out = os.path.join(tmp, "title.rgba")
    draws = subprocess.run([exe, "web", "1400", "1960", "12000000", "2", "280", "1", "2", out],
                           check=True, capture_output=True, text=True).stdout.strip()
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    ok = draws == WEB_DRAWS and digest == WEB_DIGEST
    print(f"{'OK  ' if ok else 'FAIL'} fern.c vs fern_texture.test.ts: draws {draws}, pixels {digest[:16]}")
    assert ok, "fern.c no longer draws the app's fern"
    return exe


def make_back(exe, tmp, frame):
    """The fern rendered natively over the whole bleed area, framed to the cut size
    exactly as the app frames it to the card (scale 14 * cut width / 70, centred)."""
    out = os.path.join(tmp, "back.rgba")
    scale = 14 * (TRIM[2] - TRIM[0]) / 70
    subprocess.run([exe, "ios", str(W), str(H), str(W * H * FERN_DENSITY), "1", str(FERN_ALPHA),
                    str(scale), str(W / 2), str(H / 2), out], check=True)
    img = Image.frombytes("RGBA", (W, H), open(out, "rb").read()).convert("RGB")
    if not frame:
        return img
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = TRIM
    for i in range(TRIM[0] + FRAME_PX):  # the frame: FRAME_PX inside the cut line, solid through the bleed
        d.rectangle([x0 + FRAME_PX - 1 - i, y0 + FRAME_PX - 1 - i, x1 - FRAME_PX + i, y1 - FRAME_PX + i], outline=FRAME)
    return img


# ---- face pieces ------------------------------------------------------------------------

def heart(draw, cx, cy, size, fill, flip=False):
    """A heart pip `size` px wide, centred on (cx, cy)."""
    t = np.linspace(0, 2 * np.pi, 400)
    x = 16 * np.sin(t) ** 3
    y = -(13 * np.cos(t) - 5 * np.cos(2 * t) - 2 * np.cos(3 * t) - np.cos(4 * t))
    y = y - (y.max() + y.min()) / 2
    k = size / 32.0
    if flip:
        y = -y
    draw.polygon([(cx + xi * k, cy + yi * k) for xi, yi in zip(x, y)], fill=fill)


def text_c(draw, cx, y, s, f, fill):
    """Text centred on cx, its ink top at y. Returns the ink bottom."""
    l, t, r, b = draw.textbbox((0, 0), s, font=f)
    draw.text((cx - (l + r) / 2, y - t), s, font=f, fill=fill)
    return y + (b - t)


def index(img, rank, size_px, pip_px, stacked=False, bottom_rank=None):
    """Corner indices: top-left, and the same rotated 180 degrees bottom-right.

    bottom_rank lets the two corners differ (the ace: Т for туз top-left, A bottom-right)."""
    s = SS
    box_w, box_h = 110 * s, (300 if stacked else 190) * s
    tiles = []
    for r in (rank, bottom_rank or rank):
        tile = Image.new("RGBA", (box_w, box_h), (0, 0, 0, 0))
        d = ImageDraw.Draw(tile)
        f = font(OSWALD, size_px * s, "Bold")
        cx = box_w // 2
        if stacked:
            y = 0
            for ch in r:
                y = text_c(d, cx, y, ch, f, RED) + 9 * s
        else:
            y = text_c(d, cx, 0, r, f, RED) + 12 * s
            heart(d, cx, y + pip_px * s * 0.45, pip_px * s, RED)
        tiles.append(tile)
    img.alpha_composite(tiles[0], ((SAFE[0] + 2) * s, (SAFE[1] + 4) * s))
    img.alpha_composite(tiles[1].rotate(180), (SAFE[2] * s - box_w - 2 * s, SAFE[3] * s - box_h - 4 * s))


def paste_mark(img, cx, top, size):
    j = Image.open(JESTER).convert("RGBA").resize((size * SS, size * SS), Image.LANCZOS)
    img.alpha_composite(j, (int(cx * SS - size * SS / 2), int(top * SS)))


def qr_symbol(payload, error, version):
    q = segno.make(payload, error=error, boost_error=False, micro=False)
    assert q.mode == "alphanumeric", (payload, q.mode)
    assert q.version == version, f"{payload}: version {q.version}, want {version}"
    return q


def qr_box(q, module):
    n = q.symbol_size(border=4)[0]
    return n * module


def draw_qr(img, q, x0, y0, module):
    """Pixel-exact QR with a 4-module white quiet zone, top-left at (x0, y0)."""
    d = ImageDraw.Draw(img)
    rows = list(q.matrix_iter(border=4))
    n = len(rows)
    d.rectangle([x0, y0, x0 + n * module - 1, y0 + n * module - 1], fill=WHITE)
    for r, row in enumerate(rows):
        for c, dark in enumerate(row):
            if dark:
                d.rectangle([x0 + c * module, y0 + r * module,
                             x0 + (c + 1) * module - 1, y0 + (r + 1) * module - 1], fill=INK)
    return n * module


def finish(art):
    return art.resize((W, H), Image.LANCZOS).convert("RGB")


def blank():
    return Image.new("RGBA", (W * SS, H * SS), PAPER + (255,))


# ---- the three face layouts ----------------------------------------------------------------

def face_ace(q):
    """A - the ace: Т (туз) top-left, A bottom-right, the QR as the one big pip."""
    module = 13 if q.version == 1 else 12
    art = blank()
    s = SS
    d = ImageDraw.Draw(art)
    index(art, "Т", 78, 52, bottom_rank="A")  # one card, both languages

    y = text_c(d, CX * s, 108 * s, HEADLINE_RU, font(OSWALD, 50 * s, "Bold"), RED)
    text_c(d, CX * s, y + 18 * s, HEADLINE_EN, font(OSWALD, 38 * s, "Medium"), INK)
    size = qr_box(q, module)
    qy = 292
    y = text_c(d, CX * s, (qy + size + 26) * s, PRINTED_URL, font(OSWALD, 52 * s, "SemiBold"), INK)
    mark = 150
    paste_mark(art, CX - 95, (y / s) + 36, mark)
    text_c(d, (CX + 60) * s, ((y / s) + 36 + mark / 2 - 26) * s, "FOOLISH", font(BEBAS, 64 * s), RED)
    img = finish(art)
    draw_qr(img, q, CX - size // 2, qy, module)
    return img


def face_six(q):
    """B - the six of hearts, the lowest card of the 36-card Durak deck; the QR sits between the pip columns."""
    module = 10 if q.version == 1 else 9
    art = blank()
    s = SS
    d = ImageDraw.Draw(art)
    index(art, "6", 78, 52)
    pip = 88
    xs = (196, 626)
    ys = (310, 590, 870)
    for x in xs:
        for i, y in enumerate(ys):
            heart(d, x * s, y * s, pip * s, RED, flip=(i == 2))
    y = text_c(d, CX * s, 104 * s, HEADLINE_RU, font(OSWALD, 50 * s, "Bold"), RED)
    text_c(d, CX * s, y + 18 * s, HEADLINE_EN, font(OSWALD, 38 * s, "Medium"), INK)
    size = qr_box(q, module)
    qy = 590 - size // 2
    y = text_c(d, CX * s, (qy + size + 22) * s, PRINTED_URL, font(OSWALD, 44 * s, "SemiBold"), INK)
    mark = 120
    paste_mark(art, CX, (y / s) + 30, mark)
    img = finish(art)
    draw_qr(img, q, CX - size // 2, qy, module)
    return img


def face_joker(q):
    """C - the jester: ДУРАК stacked down the corners like a joker's index, the app's jester-Д as the figure."""
    module = 11 if q.version == 1 else 10
    art = blank()
    s = SS
    d = ImageDraw.Draw(art)
    index(art, "ДУРАК", 46, 0, stacked=True)
    mark = 330
    paste_mark(art, CX, 70, mark)
    y = text_c(d, CX * s, (70 + mark + 6) * s, HEADLINE_RU, font(OSWALD, 52 * s, "Bold"), RED)
    y = text_c(d, CX * s, y + 16 * s, HEADLINE_EN, font(OSWALD, 38 * s, "Medium"), INK)
    size = qr_box(q, module)
    qy = int(y / s) + 26
    text_c(d, CX * s, (qy + size + 20) * s, PRINTED_URL, font(OSWALD, 44 * s, "SemiBold"), INK)
    img = finish(art)
    draw_qr(img, q, CX - size // 2, qy, module)
    return img


LAYOUTS = {
    "A": ("ace", face_ace),
    "B": ("six", face_six),
    "C": ("joker", face_joker),
}


# ---- preview sheet and checks -------------------------------------------------------------

def guides(img):
    """The card with its cut line (solid) and safe line (dashed) drawn on."""
    g = img.copy()
    d = ImageDraw.Draw(g)
    d.rectangle(TRIM, outline=(0, 160, 255), width=2)
    x0, y0, x1, y1 = SAFE
    dash = 12
    for x in range(x0, x1, dash * 2):
        d.line([(x, y0), (min(x + dash, x1), y0)], fill=(230, 0, 160), width=2)
        d.line([(x, y1), (min(x + dash, x1), y1)], fill=(230, 0, 160), width=2)
    for y in range(y0, y1, dash * 2):
        d.line([(x0, y), (x0, min(y + dash, y1))], fill=(230, 0, 160), width=2)
        d.line([(x1, y), (x1, min(y + dash, y1))], fill=(230, 0, 160), width=2)
    return g


def preview(back, face, title):
    gap, margin, head = 90, 60, 110
    sheet = Image.new("RGB", (2 * W + gap + 2 * margin, H + head + 2 * margin + 60), (235, 235, 235))
    d = ImageDraw.Draw(sheet)
    f = font(OSWALD, 34, "Medium")
    small = font(OSWALD, 24, "Regular")
    d.text((margin, margin - 10), title, font=f, fill=INK)
    d.text((margin, margin + 40),
           "Actual size at 300 dpi (print at 100%). Blue solid: cut line 2.5 x 3.5 in. "
           "Pink dashed: safe area. Outer edge: bleed 2.74 x 3.74 in.", font=small, fill=(60, 60, 60))
    y = margin + head
    sheet.paste(guides(back), (margin, y))
    sheet.paste(guides(face), (margin + W + gap, y))
    d.text((margin, y + H + 12), "BACK", font=small, fill=INK)
    d.text((margin + W + gap, y + H + 12), "FACE", font=small, fill=INK)
    return sheet


def decode(img):
    det = cv2.QRCodeDetector()
    a = cv2.cvtColor(np.asarray(img.convert("RGB")), cv2.COLOR_RGB2BGR)
    text, _, _ = det.detectAndDecode(a)
    return text


def check_short_path(code):
    assert len(code) != GAME_ID_LEN, f"{code}: a short path must not be {GAME_ID_LEN} long (a game id's length)"
    assert any(c.lower() not in GAME_ID_ALPHABET for c in code), \
        f"{code}: needs a character outside the game-id alphabet {GAME_ID_ALPHABET}"
    assert code == code.upper(), f"{code}: upper-case keeps the QR alphanumeric"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--layout", default="all", choices=["all"] + sorted(LAYOUTS))
    ap.add_argument("--payload", default=PAYLOAD_FORM, choices=sorted(PAYLOAD_FORMS))
    ap.add_argument("--frame", action="store_true", help="the app's #8B0000 card-back border on the back")
    args = ap.parse_args()
    layouts = sorted(LAYOUTS) if args.layout == "all" else [args.layout]
    prefix, error, version = PAYLOAD_FORMS[args.payload]
    os.makedirs(OUT, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        back = make_back(build_fern(tmp), tmp, args.frame)
    assert back.size == (W, H)
    back.save(os.path.join(OUT, "back.png"), dpi=(DPI, DPI))
    print(f"back.png  {W}x{H}")

    failures = 0
    for code, name in SOURCES:
        check_short_path(code)
        payload = prefix + code
        q = qr_symbol(payload, error, version)
        for key in layouts:
            label, fn = LAYOUTS[key]
            face = fn(q)
            assert face.size == (W, H)
            path = os.path.join(OUT, f"face_{key}_{label}_{code}.png")
            face.save(path, dpi=(DPI, DPI))
            # decode the saved file, full size and as a phone at arm's length might see it
            saved = Image.open(path)
            full = decode(saved)
            small = decode(saved.resize((W * 2 // 5, H * 2 // 5), Image.LANCZOS))
            ok = full == payload and small == payload
            failures += not ok
            print(f"{'OK  ' if ok else 'FAIL'} {os.path.basename(path):28s} {name:9s} "
                  f"QR v{q.version}-{error.upper()} want {payload!r} full={full!r} at40%={small!r}")
            if code == SOURCES[0][0]:
                preview(back, face, f"Foolish promo card, layout {key} ({label}), source {code}") \
                    .save(os.path.join(OUT, f"preview_{key}_{label}.png"), dpi=(DPI, DPI))
    if failures:
        print(f"{failures} QR check(s) failed")
        sys.exit(1)
    print("all QR checks passed")


if __name__ == "__main__":
    main()
