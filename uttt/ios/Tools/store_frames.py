"""Ultimate Tic-Tac-Toe's App Store frames, composed by the shared generator.

    python3 uttt/ios/Tools/store_frames.py [RAW_DIR] [OUT_DIR]

RAW_DIR holds one simulator screenshot per scene and appearance, named
NN_scene_{dark,light}.png (default ~/Downloads/uttt-store/raw); every frame is
written to OUT_DIR under the same name (default ~/Downloads/uttt-store), plus
a contact sheet. The scenes, their plies and how each was shot are in
uttt/docs/STORE_SHOTS.md.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "shared", "tools", "store"))
import market  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

# THE NAPKIN'S OWN COLOURS (owner, 2026-09-26: these four only). The inks are
# the kernel's (uttt/c/src/uttt_draw.c INK_O, INK_X), the paper is
# uttt_paper's .976 -> .948 ramp, coal is foolish's. Each: top, bottom, title.
X_BLUE = (37, 55, 107)
GROUNDS = {
    "ored":   ((168, 50, 31), (112, 33, 20), (255, 255, 255)),
    "xblue":  (X_BLUE, (22, 33, 66), (255, 255, 255)),
    "coal":   ((24, 20, 19), (12, 10, 9), (255, 255, 255)),
    "napkin": ((249, 248, 244), (242, 241, 237), X_BLUE),
}

# One size for the whole set; every title is two lines by a manual break.
SIZE = 124

# (file stem, title, ground) in listing order. The ground cycles
# O red / X blue / coal / napkin, against the theme's dark / light.
SCENES = [
    ("01_hero",  "Play Ultimate\nTic-Tac-Toe",    "ored"),
    ("02_empty", "Nine boards,\none big game",    "xblue"),
    ("03_picks", "Your move picks\ntheir board",  "coal"),
    ("04_send",  "Send your moves\nto the chat",  "napkin"),
    ("05_win",   "Win three boards\nin a row",    "ored"),
    ("06_rules", "Learn the rules\nin a minute",  "xblue"),
]


def main():
    raw = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/Downloads/uttt-store/raw")
    out = os.path.expanduser(sys.argv[2] if len(sys.argv) > 2 else "~/Downloads/uttt-store")
    os.makedirs(out, exist_ok=True)
    assert market.fit_size([t for _, t, _ in SCENES]) >= SIZE, "a title no longer fits two lines"
    made = []
    for stem, title, ground in SCENES:
        for theme in ("dark", "light"):
            name = f"{stem}_{theme}.png"
            market.frame(os.path.join(raw, name), title, os.path.join(out, name),
                         ground, SIZE, GROUNDS)
            made.append(os.path.join(out, name))
            print(name, ground)
    sheet(made, os.path.join(out, "contact_sheet.png"))


def sheet(paths, dest, w=330):
    """Dark row over light row, in listing order."""
    h = w * market.H // market.W
    pad, label = 16, 34
    cols = len(paths) // 2
    s = Image.new("RGB", (cols * (w + pad) + pad, 2 * (h + pad + label) + pad), (255, 255, 255))
    d = ImageDraw.Draw(s)
    for i, p in enumerate(paths):
        col, row = i // 2, i % 2
        x, y = pad + col * (w + pad), pad + row * (h + pad + label)
        d.text((x, y), os.path.basename(p), fill=(0, 0, 0), font=market.title_font(20))
        s.paste(Image.open(p).convert("RGB").resize((w, h), Image.LANCZOS), (x, y + label))
    s.save(dest)
    print("sheet", dest)


if __name__ == "__main__":
    main()
