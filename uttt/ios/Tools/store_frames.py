"""Ultimate Tic-Tac-Toe's App Store frames, composed by the shared generator.

    python3 uttt/ios/Tools/store_frames.py [RAW_DIR] [OUT_DIR]

RAW_DIR holds one simulator screenshot per scene, named NN_scene_dark.png
(default ~/Downloads/uttt-store/raw): the set is DARK MODE ONLY (owner,
2026-09-26). Every frame is written to OUT_DIR under the same name (default
~/Downloads/uttt-store), plus a contact sheet. 04_send_dark.png is a copy of
the burst frame the owner picked (burst_send_dark/b08.png): the Send hint
bobs, so that frame is shot as a 16-shot burst. The scenes, their plies and how each was shot are in
uttt/docs/STORE_SHOTS.md.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "shared", "tools", "store"))
import market  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

# THE NAPKIN'S INKS AND FOOLISH'S COAL, these three only (owner, 2026-09-26;
# napkin white was dropped). The inks are the kernel's (uttt/c/src/uttt_draw.c
# INK_O, INK_X). Each: top, bottom, title.
GROUNDS = {
    "ored":   ((168, 50, 31), (112, 33, 20), (255, 255, 255)),
    "xblue":  ((37, 55, 107), (22, 33, 66), (255, 255, 255)),
    "coal":   ((24, 20, 19), (12, 10, 9), (255, 255, 255)),
}

# One size for the whole set; every title is two lines by a manual break.
SIZE = 124

# (file stem, title, ground) in listing order. The ground cycles coal /
# O red / X blue, so no two neighbours match and O's win lands on O red.
SCENES = [
    ("01_hero",  "Play Ultimate\nTic-Tac-Toe",    "coal"),
    ("02_empty", "Nine boards,\none big game",    "ored"),
    ("03_picks", "Your move picks\ntheir board",  "xblue"),
    ("04_send",  "Send your moves\nto the chat",  "coal"),
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
        name = f"{stem}_dark.png"
        market.frame(os.path.join(raw, name), title, os.path.join(out, name),
                     ground, SIZE, GROUNDS)
        made.append(os.path.join(out, name))
        print(name, ground)
    sheet(made, os.path.join(out, "contact_sheet.png"))


def sheet(paths, dest, w=330):
    """One row, in listing order."""
    h = w * market.H // market.W
    pad, label = 16, 34
    s = Image.new("RGB", (len(paths) * (w + pad) + pad, h + pad + label + pad), (255, 255, 255))
    d = ImageDraw.Draw(s)
    for i, p in enumerate(paths):
        x, y = pad + i * (w + pad), pad
        d.text((x, y), os.path.basename(p), fill=(0, 0, 0), font=market.title_font(20))
        s.paste(Image.open(p).convert("RGB").resize((w, h), Image.LANCZOS), (x, y + label))
    s.save(dest)
    print("sheet", dest)


if __name__ == "__main__":
    main()
