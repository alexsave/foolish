# The win screens' fireworks against the art. scripts/win_fireworks.txt saves
# the game's screen for each city and each of the four firework frames; this
# renders the same frame with the art's own code (art/wins.py, bake=True: the
# name printed as the game prints it) and compares them pixel by pixel in every
# burst's box (wins.burst_box: the 16x16 burst and the rising shell, grown by
# the art's margin) and, where the art's sprites are only the fireworks, over
# the whole screen too. The New York train is the one sprite the game does not
# draw yet, so New York's whole-screen comparison skips the bridge's deck rows.
#   python3 -I tests/fwcheck.py            (from gameboy/, after the script ran)
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "art"))
from PIL import Image          # noqa: E402
import wins                    # noqa: E402

SHADE = {0: 1, 1: 0, 2: 2, 3: 3}      # the art's palette index -> the DMG shade
NAMES = {"MIAMI": "miami", "NEW YORK": "new_york", "SEOUL": "seoul", "MOSCOW": "moscow"}


def game_shades(path):
    im = Image.open(path).convert("RGB")
    out = {}
    for y in range(144):
        for x in range(160):
            r, g, b = im.getpixel((x, y))
            lum = (r + g + b) // 3
            out[(x, y)] = 0 if lum >= 192 else 1 if lum >= 128 else 2 if lum >= 64 else 3
    return out


def main():
    bad = 0
    for city in wins.CITIES:
        for f in range(4):
            g = game_shades(os.path.join(HERE, "..", "build", "fw", "%s_%d.png" % (NAMES[city], f)))
            s = wins.win_screen(city, frame=f, bake=True)
            art = s.compose()
            box = set()
            for (cx, cy) in wins.BURSTS[city]:
                box |= wins.burst_box(cx, cy)
            lit = 0
            wrong = []
            for (x, y) in sorted(box):
                if not (0 <= x < 160 and 0 <= y < 144):
                    continue
                a = SHADE[art.get(x, y)]
                if a != g[(x, y)]:
                    wrong.append((x, y, a, g[(x, y)]))
            # whole screen: everything but New York's train deck rows (sprites the game lacks)
            skip = set()
            if city == "NEW YORK":
                skip = {(x, y) for x in range(96, 160) for y in range(wins.MAN_DECK - 2, wins.MAN_DECK + 10)}
            whole = [(x, y) for y in range(144) for x in range(160)
                     if (x, y) not in skip and SHADE[art.get(x, y)] != g[(x, y)]]
            # a frame with a burst showing must really draw something in its boxes
            for (cx, cy), ph in zip(wins.BURSTS[city], wins.SCHEDULE[f]):
                if ph not in (None, "S"):
                    lit += sum(1 for (x, y) in wins.burst_box(cx, cy, 0)
                               if 0 <= x < 160 and 0 <= y < 144 and SHADE[art.get(x, y)] != 3)
            status = "ok" if not wrong and not whole else "FAIL"
            print("fwcheck %-8s frame %d: burst boxes %d px, %d lit by bursts, %d differ in the boxes, %d on the whole screen  %s"
                  % (city, f, len(box), lit, len(wrong), len(whole), status))
            if wrong:
                bad += 1
                print("    first: x=%d y=%d art shade %d, game %d" % wrong[0])
            elif whole:
                bad += 1
                print("    first whole-screen difference: x=%d y=%d" % whole[0])
    if bad:
        print("fwcheck FAILED: %d frames differ" % bad)
        sys.exit(1)
    print("fwcheck: all 16 frames equal the art")


main()
