# Foolish promo playing cards

Print-ready art for a small run of promo cards from MakePlayingCards (MPC): one back, one face per QR source.

## Make them

```sh
python3 -m venv /tmp/cards-venv
/tmp/cards-venv/bin/pip install -r foolish/cards/requirements.txt
/tmp/cards-venv/bin/python foolish/cards/make_cards.py              # all three layouts
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --layout B   # one layout
```

It writes `out/` and prints one line per face: every QR is decoded back from the saved PNG (full size and scaled to 40%) with OpenCV and must equal the exact payload, or the script exits 1.

## What to edit

The list at the top of `make_cards.py`: `SOURCES` (short path, name), `QR_PREFIX`, `QR_ERROR`, and the two headline lines.
The QR carries `HTTPS://FOOLISH.CARDS/<CODE>` in capitals so it stays in alphanumeric mode (version 2, 25 x 25 modules at Q).
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

- Back: `foolish/public/khokhloma-pattern.png`, 250 x 350 px, exactly a third of the cut size.
  It is upscaled 3x by `crisp_upscale` (the three inks clustered, their membership maps upscaled and thresholded for clean edges, the Lanczos colour kept inside each region with a light edge-keeping blur), and the bleed is the pattern mirrored past the cut line.
  No detail is invented; at arm's length it reads as print, at a loupe it is soft inside the leaves.
- Mark: `foolish/ios/Tools/icon-source/jester-d.png` (the app icon's jester-Д, 1024 px with alpha).
- Fonts: Oswald and Bebas Neue (the web app's `--font-family-primary` and `--font-family-display`), SIL OFL, in `fonts/`.
- Red `#B32929` is the web card back's red (`foolish/src/styles/variables.css`).
