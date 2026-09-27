#!/usr/bin/env python3
"""Contact sheets - the way these screenshots actually get reviewed.

The owner's verdict on the technique, Sep 2026: "these screenshots and sheets
have probably been the most useful thing you have come up with for QA testing,
allowing me to review animations and gameplay without downloading it".

Read the SHEETS, not the frames: six image reads instead of hundreds, each cell
labelled with the name or the timestamp that maps back to the original.

    sheet.py dir  <in-dir> <out.png> [--title T] [--cols N] [--dedupe]
        Tile a directory of stills. `--dedupe` drops a frame within 6 bits of
        the one before it (16x16 average hash), which is what collapses a
        4,000-frame burst to a couple of hundred distinct ones.

    sheet.py film <take-dir> [--first N] [--last N] [--cols N] [--rows N]
        Tile a `rig.sh film` take: one cell per COMPOSITED frame, stamped
        `index +offset (+delta)` from times.txt, with any delta outside
        12-22ms flagged - that is a beat the device dropped, and it is the
        whole reason the frames are placed in time rather than assumed to be
        1/60 apart.
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FLAG = (255, 210, 90)
DIM = (170, 170, 170)


def font(size):
    for p in ("/System/Library/Fonts/Menlo.ttc",
              "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
              "/System/Library/Fonts/Helvetica.ttc"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def ahash(path, side=16):
    """16x16 average hash as an int - cheap, and enough to tell "the same
    settled screen" from "the animation moved"."""
    im = Image.open(path).convert("L").resize((side, side), Image.LANCZOS)
    px = list(im.getdata())
    avg = sum(px) / len(px)
    bits = 0
    for i, v in enumerate(px):
        if v > avg:
            bits |= 1 << i
    return bits


def tile(cells, out, title=None, cols=8, cw=240, band=30):
    """cells: [(label, path, flagged)]"""
    if not cells:
        sys.exit("no frames")
    src = Image.open(cells[0][1])
    ch = int(src.height * cw / src.width)
    rows = (len(cells) + cols - 1) // cols
    head = 46 if title else 0
    sheet = Image.new("RGB", (cols * cw, head + rows * (ch + band)), (18, 18, 18))
    d = ImageDraw.Draw(sheet)
    if title:
        d.text((10, 12), title, fill=(255, 255, 255), font=font(22))
    for k, (label, path, flagged) in enumerate(cells):
        x, y = (k % cols) * cw, head + (k // cols) * (ch + band)
        im = Image.open(path).convert("RGB").resize((cw, ch), Image.LANCZOS)
        sheet.paste(im, (x, y + band))
        d.text((x + 5, y + 7), label[:38], fill=FLAG if flagged else DIM, font=font(15))
        d.rectangle([x, y, x + cw - 1, y + band + ch - 1], outline=(60, 60, 60))
    sheet.save(out)
    print(out, sheet.size)


def cmd_dir(a):
    files = sorted(f for f in os.listdir(a.indir) if f.lower().endswith(".png"))
    kept, prev = [], None
    for f in files:
        p = os.path.join(a.indir, f)
        if a.dedupe:
            h = ahash(p)
            if prev is not None and bin(h ^ prev).count("1") < 6:
                continue
            prev = h
        kept.append((os.path.splitext(f)[0], p, False))
    per = a.cols * a.rows
    if len(kept) <= per:
        tile(kept, a.out, a.title, a.cols)
        return
    base, ext = os.path.splitext(a.out)
    for i in range(0, len(kept), per):
        n = i // per + 1
        tile(kept[i:i + per], f"{base}{n:02d}{ext}",
             f"{a.title or ''} ({n})".strip(), a.cols)


def cmd_film(a):
    d = a.take
    files = sorted(f for f in os.listdir(d) if f.startswith("f") and f.endswith(".png"))
    tpath = os.path.join(d, "times.txt")
    times = [float(x) for x in open(tpath)] if os.path.exists(tpath) else []
    # A movie can carry one more frame than ffprobe listed a time for; hold the
    # last time rather than shifting every later stamp by a frame.
    times += [times[-1] if times else 0.0] * (len(files) - len(times))
    keep = list(range(a.first, min(a.last or len(files), len(files))))
    t0 = times[keep[0]] if keep else 0.0
    per = a.cols * a.rows
    for s in range((len(keep) + per - 1) // per):
        chunk = keep[s * per:(s + 1) * per]
        cells = []
        for i in chunk:
            dt = (times[i] - times[i - 1]) * 1000 if i else 0
            bad = not (i == keep[0] or 12 <= dt <= 22)
            cells.append((f"{i:04d} +{(times[i] - t0) * 1000:.0f}ms ({dt:+.0f})"
                          + (" !" if bad else ""),
                          os.path.join(d, files[i]), bad))
        tile(cells, os.path.join(d, f"sheet{s + 1:02d}.png"), None, a.cols)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("dir")
    p.add_argument("indir")
    p.add_argument("out")
    p.add_argument("--title")
    p.add_argument("--cols", type=int, default=6)
    p.add_argument("--rows", type=int, default=4)
    p.add_argument("--dedupe", action="store_true")
    p.set_defaults(fn=cmd_dir)

    p = sub.add_parser("film")
    p.add_argument("take")
    p.add_argument("--first", type=int, default=0)
    p.add_argument("--last", type=int, default=0)
    p.add_argument("--cols", type=int, default=8)
    p.add_argument("--rows", type=int, default=6)
    p.set_defaults(fn=cmd_film)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
