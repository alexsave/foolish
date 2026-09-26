"""App Store marketing frames: a flat ground, a big title, a device mockup.

Layout copied from the reference the owner sent - a top iMessage app: a big
two-line title across the top, the screenshot below it in a phone body, and a
single flat brand colour behind both.

SHARED BY EVERY PRODUCT. Nothing here is Durak's except the defaults: the
grounds below and the CLI's automatic title size are foolish's, so its seven
listing frames (docs/appstore/screenshots) rebuild byte for byte. Another
product passes its own `grounds` and `size` to `frame()` - see
uttt/ios/Tools/store_frames.py.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 1320, 2868

# FOOLISH'S PALETTE, the default. A PALETTE, NOT ONE COLOUR. The reference sets vary the ground card to card -
# it keeps a row of ten from reading as one long block - so these alternate
# across the set. Every colour is either the owner's swatch or sampled from the
# product itself, so the set looks like the app rather than like a template.
#   red    the swatch the owner sent (103,25,13)
#   felt   the table, sampled straight off a board frame (17,53,37)
#   coal   the transcript's own near-black
GROUNDS = {
    "red":  ((103, 25, 13), (66, 15, 8),  (255, 255, 255)),
    "felt": ((17, 53, 37),  (9, 31, 21),  (255, 255, 255)),
    "coal": ((24, 20, 19),  (12, 10, 9),  (255, 255, 255)),
    "bone": ((243, 234, 222), (228, 214, 196), (103, 25, 13)),
}
BEZEL = (17, 17, 19)

# The phone shows WHOLE, with air under it. DEVICE_W is the width of the PNG,
# not of the phone: the asset carries 48px of transparent margin at the sides
# and 56px top and bottom, so the visible body is narrower and shorter than the
# numbers here. Every margin below is measured on the VISIBLE BODY - that
# transparent skirt is exactly why a title centred on the PNG's top edge still
# read as sitting too high.
DEVICE_W = 1200
VISIBLE_BOTTOM = 66   # air under the phone's actual bottom edge
SIDE_MARGIN = 65      # title column inset
# A nudge DOWN off dead-centre. Geometrically centred type read as sitting a
# touch high against the mass of the phone below it, so the block is dropped by
# 7% of the sky - optical centring, the same reason a page's type block sits
# above the middle and a title over an object sits below it.
TITLE_DROP = 0.035

def body_box(frame_img):
    """The opaque bounding box of the phone inside its transparent PNG."""
    return frame_img.split()[3].getbbox()

# Beside this file, so a checkout alone can rebuild every frame.
FRAME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "iphone16promax.png")
SCREEN_XY = (100, 100)

def screen_mask(frame_img):
    """The aperture's OWN shape, flood-filled from the centre.

    The window is rounded and a screenshot is square, so pasting the shot as a
    rectangle left four black corners sticking out past the phone's screen and
    sitting on the background. Masking with the aperture itself rounds them
    exactly as the hardware does - no guessed corner radius.
    """
    import numpy as np
    from collections import deque
    a = np.asarray(frame_img)
    alpha = a[:, :, 3]
    h, w = alpha.shape
    seen = np.zeros_like(alpha, bool)
    q = deque([(h // 2, w // 2)])
    seen[h // 2, w // 2] = True
    while q:
        yy, xx = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = yy + dy, xx + dx
            if 0 <= ny < h and 0 <= nx < w and not seen[ny, nx] and alpha[ny, nx] == 0:
                seen[ny, nx] = True
                q.append((ny, nx))
    return Image.fromarray((seen * 255).astype("uint8"), "L")

FONT = "/System/Library/Fonts/Supplemental/Futura.ttc"   # Futura Bold, owner's pick
FONT_INDEX = 2
FONT_VARIATION = None   # for variable faces (SF Pro) - the named instance

def title_font(size):
    f = ImageFont.truetype(FONT, size, index=FONT_INDEX)
    if FONT_VARIATION:
        f.set_variation_by_name(FONT_VARIATION)
    return f

def wrap(draw, text, font, maxw):
    """Greedy wrap, then BALANCE a two-line result.

    A greedy wrap leaves orphans - "Drag cards to / play", "Attack, defend, or /
    pass" - which read as a mistake at store size. When the text fits in two
    lines, pick the split that makes the WIDER line as narrow as possible; that
    is the same thing CSS `text-wrap: balance` does, and it also guarantees the
    result still fits the column.
    """
    words = text.split()
    lines, cur = [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=font) <= maxw:
            cur = t
        else:
            if cur: lines.append(cur)
            cur = w
    if cur: lines.append(cur)
    if len(lines) != 2:
        return lines
    best = None
    for i in range(1, len(words)):
        a, b = " ".join(words[:i]), " ".join(words[i:])
        wa, wb = draw.textlength(a, font=font), draw.textlength(b, font=font)
        if max(wa, wb) > maxw:
            continue
        if best is None or max(wa, wb) < best[0]:
            best = (max(wa, wb), [a, b])
    return best[1] if best else lines


def frame(shot_path, title, out_path, ground="red", size=None, grounds=None):
    """One frame. `grounds` maps a name to (top, bottom, title) RGB; it
    defaults to foolish's GROUNDS."""
    c_top, c_bot, c_text = (grounds or GROUNDS)[ground]
    bg = Image.new("RGB", (W, H), c_top)
    # A gentle vertical shade so a flat fill does not read as a print error.
    top = Image.new("RGB", (W, H), c_bot)
    mask = Image.linear_gradient("L").resize((W, H))
    bg = Image.composite(top, bg, mask)
    d = ImageDraw.Draw(bg)

    # THE DEVICE SITS ON THE CANVAS, NOT OFF IT, and it is placed by its
    # VISIBLE edges. The owner asked for air under the phone and for the title
    # centred between the phone's top and the top of the image; both of those
    # are about the body, not about the PNG's bounds.
    frame_img = Image.open(FRAME).convert("RGBA")
    target_w = DEVICE_W
    scale = target_w / frame_img.width
    dev_h = int(frame_img.height * scale)
    bx0, by0, bx1, by1 = body_box(frame_img)
    py = int(H - VISIBLE_BOTTOM - by1 * scale)
    sky = py + by0 * scale          # canvas top -> top of the phone's body

    # BIGGER TYPE, WRAPPED IF IT HAS TO BE. The one-line rule is retired at the
    # owner's call ("font size could be larger, if it means two rows so be it"),
    # so a title now wraps to the column width and the whole set still shares
    # one size - the caller measures every title first and passes the size that
    # fits them all in at most two lines.
    f = title_font(size or 132)
    line_h = int(f.size * 1.18)
    lines = []
    for para in title.split("\n"):
        lines += wrap(d, para, f, W - 2 * SIDE_MARGIN)

    # CENTRED ON THE INK, not on the line boxes. A font's ascender sits well
    # above its cap height, so centring the nominal block left every title
    # looking low; measuring the drawn glyphs puts the visual middle of the
    # words on the middle of the sky above the phone.
    pens = [i * line_h for i in range(len(lines))]
    boxes = [d.textbbox((0, p), l, font=f) for l, p in zip(lines, pens)]
    ink_top = min(bb[1] for bb in boxes)
    ink_bot = max(bb[3] for bb in boxes)
    off = (sky - (ink_bot - ink_top)) / 2 - ink_top + sky * TITLE_DROP
    for l, p in zip(lines, pens):
        wpx = d.textlength(l, font=f)
        d.text(((W - wpx) / 2, p + off), l, font=f, fill=c_text)

    shot_full = Image.open(shot_path).convert("RGB")

    # A REAL DEVICE FRAME, not a drawn one. Apple's own iPhone 16 Pro Max body
    # (weirdapps/mockups), whose screen window is transparent and measures
    # exactly 1320x2868 at offset (100,100) - the same pixels our shots are, so
    # the screenshot drops in at 1:1 with no resampling before the final scale.
    well = Image.new("RGBA", frame_img.size, (0, 0, 0, 0))
    m = screen_mask(frame_img)
    lay = Image.new("RGBA", frame_img.size, (0, 0, 0, 0))
    lay.paste(shot_full.convert("RGBA"), SCREEN_XY)
    well.paste(lay, (0, 0), m)
    dev = Image.alpha_composite(well, frame_img)
    dev = dev.resize((target_w, dev_h), Image.LANCZOS)
    px = (W - dev.width) // 2

    bg.paste(dev, (px, py), dev)
    bg.save(out_path, "PNG")
    return out_path

def fit_size(titles, start=160, floor=48):
    """The largest size at which EVERY title fits in at most two rows."""
    probe = ImageDraw.Draw(Image.new("RGB", (10, 10)))
    _fr = Image.open(FRAME).convert("RGBA")
    _s = DEVICE_W / _fr.width
    _b = body_box(_fr)
    sky = H - VISIBLE_BOTTOM - (_b[3] - _b[1]) * _s
    size = start
    while size > floor:
        f = title_font(size)
        # Rows are counted PER TITLE, not per paragraph - a title with an
        # explicit newline is two rows even though each half is one line.
        wrapped = [sum((wrap(probe, p, f, W - 2 * SIDE_MARGIN)
                        for p in t.split("\n")), [])
                   for t in titles]
        rows = max(len(w) for w in wrapped)
        fits_w = all(probe.textlength(l, font=f) <= W - 2 * SIDE_MARGIN
                     for w in wrapped for l in w)
        # Two rows of type must not crowd the phone: keep the block inside
        # two thirds of the sky so there is air over the title and under it.
        if rows <= 2 and fits_w and rows * int(size * 1.18) <= sky * 0.72:
            break
        size -= 2
    return size

if __name__ == "__main__":
    # foolish's listing: a JSON list of [shot, title, ground], its grounds.
    import json
    spec = json.load(open(sys.argv[1]))
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    size = fit_size([t for _, t, _ in spec])
    print(f"title size {size} (fits every line)")
    for i, (shot, title, ground) in enumerate(spec, 1):
        p = os.path.join(outdir, f"{i:02d}.png")
        frame(shot, title, p, ground, size)
        print(f"{i:02d}  {ground:5} {title}")
