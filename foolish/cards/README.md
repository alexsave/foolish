# Foolish promo playing cards

Print-ready art for a small run of promo cards from MakePlayingCards (MPC): one back, one face per QR source.

## Make them

```sh
python3 -m venv /tmp/cards-venv
/tmp/cards-venv/bin/pip install -r foolish/cards/requirements.txt
/tmp/cards-venv/bin/python foolish/cards/make_cards.py              # all three layouts
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --layout B   # one layout
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --payload https   # HTTPS://FOOLISH.CARDS/<CODE>, v2 at Q
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --frame      # the app's #8B0000 border on the back
```

It needs `cc` (it builds `fern.c` into a temp dir).
It first proves `fern.c` against `foolish/e2e/fern_texture.test.ts` (same random-draw count and the same pixel digest as the real `fernFractal.tsx`), or stops.
It writes `out/` and prints one line per face: every QR is decoded back from the saved PNG (full size and scaled to 40%) with OpenCV and must equal the exact payload, or the script exits 1.

## What to edit

The list at the top of `make_cards.py`: `SOURCES` (short path, name), `PAYLOAD_FORM`, and the two headline lines.
The QR carries `WWW.FOOLISH.CARDS/<CODE>` in capitals (the lobby QR's form), alphanumeric mode, error correction M, version 1 (21 x 21 modules); the script asserts the version.
The bare `FOOLISH.CARDS/<CODE>` is not offered: a phone camera opened it as a web search.
The printed URL is always `foolish.cards`.
Each code must contain a character outside the game-id alphabet (`0-9a-f`, `foolish/server/src/store/game_id.h`) and must not be 6 long, so it can never be a game id; the script asserts both.

## Files

- `out/back.png` - the back, 822 x 1122 px at 300 dpi.
- `out/face_<layout>_<name>_<CODE>.png` - a face per layout and source.
- `out/preview_<layout>_<name>.png` - back and face side by side at actual size (300 dpi, print at 100%), cut line solid blue, safe area dashed pink.

Layouts: A ace (Т top-left, A bottom-right, the QR as the one pip), B six of hearts (the lowest card of the 36-card Durak deck, QR between the pip columns), C jester (ДУРАК stacked down the corners like a joker's index, the app's jester-Д as the figure).

## Print spec

MPC "Traditional Poker sized card (2.5" x 3.5")", template `poker-size.pdf` from https://www.makeplayingcards.com/pops/pc-temp-guide.html:
full bleed 2.74 x 3.74 in = 822 x 1122 px, cut 750 x 1050 px, safe 690 x 990 px, all at 300 dpi and centred (36 px bleed, then 30 px to the safe line).
MPC's other poker product ("Standard", 63 x 88 mm, `American-poker-size.pdf`) is 816 x 1110 px with bleed; change `W`, `H`, `TRIM` and `SAFE` for it.

## Sources

- Back: the app's own IFS fern, rendered natively at 822 x 1122 by `fern.c`, a C port of `foolish/src/utils/fernFractal.tsx` (constants, probabilities, colours, rotation, mirror and colour propagation verbatim).
  Its `web` mode reproduces the e2e test's digest of the real code bit for bit; its `ios` mode is `shared/swift/Textures/FernCardBack.swift`, the owner's chosen look (1 px dots, 14 per pixel, alpha 0.20 over black), which matches `foolish/ios/FoolishKit/Resources/fern-back.jpg` (correlation 0.997 after a 2 px blur, the rest is JPEG).
  The print uses that look at the same density per pixel (12.9M points), framed to the cut size the way the app frames it to the card, and drawn through the bleed.
- Mark: `foolish/ios/Tools/icon-source/jester-d.png` (the app icon's jester-Д, 1024 px with alpha).
- Fonts: Oswald and Bebas Neue (the web app's `--font-family-primary` and `--font-family-display`), SIL OFL, in `fonts/`.
- Red `#B32929` is the web card back's red (`foolish/src/styles/variables.css`).
