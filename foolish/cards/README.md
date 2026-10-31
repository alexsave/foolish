# Foolish promo playing cards

Print-ready art for a small run of promo cards from MakePlayingCards (MPC): one back, one face per QR source.

## Make them

```sh
python3 -m venv /tmp/cards-venv
/tmp/cards-venv/bin/pip install -r foolish/cards/requirements.txt
/tmp/cards-venv/bin/python foolish/cards/make_cards.py              # both layouts, EC M into out/M/ and EC H into out/H/
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --ec m       # one error-correction level
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --layout B   # one layout
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --payload https   # HTTPS://FOOLISH.CARDS/<CODE>
/tmp/cards-venv/bin/python foolish/cards/make_cards.py --no-frame   # the back without the game's red frame
```

It needs `cc` (it builds `fern.c` into a temp dir) and Google Chrome, which it runs headless to draw the faces.
It first proves `fern.c` against `foolish/e2e/fern_texture.test.ts` (same random-draw count and the same pixel digest as the real `fernFractal.tsx`), or stops.
Then, for every face, it decodes the QR from the saved PNG at full size and at every even width down to 160 px, and measures the symbol contrast.
It exits 1 unless the full-size decode is the exact payload, every size from 60% up reads, and the contrast is at least 0.9.

## What to edit

The list at the top of `make_cards.py`: `SOURCES` (short path, name, headline lines) and `PAYLOAD_FORM`.
Brighton (BR) is Russian only, Washington Square (WS) English only; both print `foolish.cards`.
The QR carries `WWW.FOOLISH.CARDS/<CODE>` in capitals (the lobby QR's form), alphanumeric mode: error correction M is version 1 (21 x 21 modules), H is version 2 (25 x 25); the script asserts the version for each.
The bare `FOOLISH.CARDS/<CODE>` is not offered: a phone camera opened it as a web search.
Each code must contain a character outside the game-id alphabet (`0-9a-f`, `foolish/server/src/store/game_id.h`) and must not be 6 long, so it can never be a game id; the script asserts both.

## Files

One folder per error-correction level, `out/M/` and `out/H/`:

- `back.png` - the back.
- `face_<layout>_<name>_<CODE>.png` - a face per layout and source.
- `preview_<layout>_<name>_<CODE>.png` - back and face side by side at actual size (print at 100%), cut line solid blue, safe area dashed pink. Not for upload.

Every `back.png` and `face_*.png` is what MPC's uploader takes, and the script asserts it on the saved file: PNG, 8-bit RGB with no alpha, an embedded sRGB profile, 300 dpi, exactly 822 x 1122.
To upload one design: `back.png` as the back and one `face_<layout>_*_<CODE>.png` per source as the fronts.

## The faces

They are `foolish/src/components/GameDisplay/CardFace.tsx`, default theme, at 15x (the 50 px game card to the 750 px cut).
That is Georgia bold in `#dc2626` on `#ffffff`: the 20 px rank over the 14 px suit glyph in a 12 px index 4 px in from the corner, and the same index turned 180 degrees bottom-right.
Chrome lays out that markup with CSS zoom 15, so the glyphs are real fonts at print size.
Rank labels and the suit glyph are the kernel's (`foolish/c/cardtext/card_text.h`).
The QR replaces the game's centre suit: dark modules only, on the card itself, its quiet zone bare card (asserted).
The game's 2 px black face border is kept as a transparent border, so nothing moves but no black line sits on the cut.
Layout A is the ace of hearts, layout B the six of hearts (the lowest card of the 36-card Durak deck).
The headline sits top right and `foolish.cards` bottom left, in the face's Georgia bold.

## Print spec

MPC "Traditional Poker sized card (2.5" x 3.5")", template `poker-size.pdf` from https://www.makeplayingcards.com/pops/pc-temp-guide.html:
full bleed 2.74 x 3.74 in = 822 x 1122 px, cut 750 x 1050 px, safe 690 x 990 px, all at 300 dpi and centred (36 px bleed, then 30 px to the safe line).
MPC's other poker product ("Standard", 63 x 88 mm, `American-poker-size.pdf`) is 816 x 1110 px with bleed; change `W`, `H`, `TRIM` and `SAFE` for it.

## Sources

- Back: the app's own IFS fern, rendered natively at 822 x 1122 by `fern.c`, a C port of `foolish/src/utils/fernFractal.tsx` (constants, probabilities, colours, rotation, mirror and colour propagation verbatim).
  Its `web` mode reproduces the e2e test's digest of the real code bit for bit.
  Its `ios` mode is `shared/swift/Textures/FernCardBack.swift`, the owner's chosen look (1 px dots, 14 per pixel, alpha 0.20 over black), and matches `foolish/ios/FoolishKit/Resources/fern-back.jpg` (correlation 0.997 after a 2 px blur, the rest is JPEG).
  The print uses that look at the same density per pixel (12.9M points), framed to the cut size the way the app frames it to the card.
- Frame: `CardBack.tsx`'s, `#8B0000`, 1 px wide and 5 px round on the 50 px card, so its inner edge is 15 px inside the cut with a 60 px radius.
  Outside that inner edge it is red all the way through the bleed, so a drifting cut shows more or less red, never black.
- Fonts: Georgia (the game face's own, from the system, through Chrome); Oswald (SIL OFL, `fonts/`) only captions the preview sheets.
