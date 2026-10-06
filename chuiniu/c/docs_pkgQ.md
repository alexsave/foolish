# Package Q - the crown's count in lining figures

## The bug

The count stamped on each cup's crown was baked from IM Fell English's default figures, which are old-style.
Fell's 3, 4, 5, 7 and 9 drop below the line and its 1 is a small capital I.
At 29 pt on a crown, and worse at 14 pt on a far crown, the 5 read as a long s.
The owner and an independent reviewer both saw it.

The study never showed it, by accident.
Its crown canvas (`TEX3.cupCrown`) asked for `'IM Fell English', serif` but drew before the web font had loaded, so the owner always saw the system serif's lining figures (Times on this Mac).
DECISIONS I27 already names the system serif's lining figures as the fallback for Fell's figures (the stepper uses them).

The die's pips are drilled discs, not glyphs, and nothing else in the pack is baked from a font, so the crown's count was the only place.

## The face: Libre Caslon Text

Candidates were fetched from google/fonts (all SIL OFL 1.1) and their digits inspected with fontTools:

| Face | Default figures | Notes |
|---|---|---|
| IM Fell English | old-style | no `lnum` feature at all |
| EB Garamond | lining | every digit is a composite glyph, which the baker's small glyf parser does not read |
| Cormorant Garamond | old-style | `lnum` only through GSUB |
| Sorts Mill Goudy | old-style | `lnum` only through GSUB |
| Libre Baskerville | lining | heavy and wide beside Fell |
| Crimson Pro | lining | light, a Garamond |
| Old Standard | lining | a Didone, too much contrast beside Fell |
| Libre Caslon Text | lining | chosen |

Libre Caslon Text was chosen because it is the closest of them to what the owner already saw and liked (the system serif's lining figures), it is an old-face like Fell so the two sit together, its default glyphs are lining (no GSUB needed), every digit is a simple glyph, and its regular weight matches Fell's stroke at the crown's sizes.
The variable font is committed unmodified (`tools/fonts/LibreCaslonText-wght.ttf`, 127 KB, google/fonts `ofl/librecaslontext/LibreCaslonText[wght].ttf`, version 2.000) with its licence `tools/fonts/OFL-LibreCaslonText.txt`.
The baker reads the glyf table, which holds the default instance, weight 400.
It is unmodified because the licence names Reserved Font Names, and a subset would have to be renamed.
Only the baked coverage masks ship; the font file does not go into the iOS bundle (Fell does, for the words).

Contact sheet (study as the owner saw it, the pack before, the pack now, the study now; at 29 pt and 14 pt and enlarged): `/private/tmp/claude-501/-Users-alex-Dev-foolish/f85a39ca-a5f0-450f-ad60-f80c97e535a9/scratchpad/contact_sheet_pkgQ.png`.

## Placement

The old bake used the canvas's `textBaseline 'middle'` (the middle of the em box from OS/2 typo metrics).
Fell's old-style figures are short, so that put them close to the anchor (128, 136).
Libre Caslon's metrics put that middle at .289 of the size while its figures' middle is at .386, so `middle` would have lifted every figure 11 px at 112 and 18 px at 184.
The rule now, in both the baker (`bake_glyph`) and the study (`cupCrown`): the baseline sits half the figure height below the anchor, the figure height being the 1's top (1544 of 2000 units, .386 of the size).
The study draws with `textBaseline 'alphabetic'` at `136 + numeral * .386`.
The shadow (+2, +4) and both fills are unchanged.

Against Chrome drawing the same face the same way, the baked masks' centroids agree to about half a pixel (the baked figure sits about .5 px lower) and Chrome's ink is about 8% heavier (its rasterizer's dilation).
The old Fell bake had the same two differences, a little larger (.7 px, 12%).

## The study

`docs/UI.html`, JavaScript only (the wasm base64 line is untouched):
- `NUMERAL_FACE = "'Libre Caslon Text', serif"` is one page-wide name in its own small script before the texture kernel, which also adds the Google Fonts stylesheet for the face. It has to be page-wide: the 2D badges live in one closure and `TEX3` in another (a first version declared it in the badges' closure and the layout tab went blank with a ReferenceError).
- `cupCrown` draws with it, centred as above.
- After the first `drawLayout()`, `document.fonts.load` of the face resets `TEX3`'s cache and draws the layout again, so the crowns drawn before the face arrived are replaced.
- The 2D count stamps (`badge`, the side view's and the top view's count) use the same name.

## The pack

`build/cn_tex.pack` grows from 344,944 to 431,232 bytes: lining figures are taller than Fell's old-style ones, so the masks are bigger.
It stays under a tenth of the 4 MiB budget.

## Goldens re-pinned (`tests/cn_tex_test.c`)

| Golden | Before | After |
|---|---|---|
| `GOLD_PACK_LEN` | 344944 | 431232 |
| `GOLD_PACK` | 0xacf5f72a | 0x507d6735 |
| `GOLD_CROWN7` | 0x65aa86fd | 0xb9737bbe |

The reason for each is the new figures; the side and the die goldens did not move.
`cn_scene_test` and `cn_stage_test` hold no crown hash and passed unchanged.
The new pack and goldens are the same under Apple clang -O2 and -O0 and GCC 16 (`gcc-16`), and the GCC 16 test holds the pack clang's `make tex` wrote.
GCC 16 also flagged a misleading indentation in `tg_stop` (two statements after a one-line `for`), fixed by splitting the line.

## Tests, each seen red

- `cn_tex_test` "numerals are lining figures": every digit's ink rows (coverage over half), in the anchor's coordinates, agree with the 0's top and bottom to 3% of the size.
  - Red first: written before the face changed, it failed on Fell (14 failures, e.g. "digit 5 at 112: ink rows -27..46 against the 0's -22..22").
  - Mutation: `base = mid * s - (digit & 1) * px * .1` in `bake_glyph` (odd digits lifted); the lining assertion failed for 1, 3, 5, 7, 9 ("digit 5 at 112: ink rows -56..32 against the 0's -43..43"). Restored by re-editing; 425 assertions, 0 failed.
- `cn_tex_test` "numeral lands inside the crown" (existing) now also guards the new centring rule.
  - Mutation: `mid = font_top(f, one) / 1.0` (the whole figure height instead of half); the centroid assertion failed for every digit ("digit 0 at 112: centred at 128.4, 181.6"). Restored by re-editing.
  - Before the centring rule existed, the canvas `middle` placement already failed it ("digit 7 at 184: centred at 130.1, 105.3"), which is how the lift was found.
