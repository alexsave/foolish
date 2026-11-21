# Durak (foolish) on the Game Boy: the art

Everything the game draws, as tiles for the original Game Boy (DMG), with the screen layouts that use them.
The website is the reference: its card faces, its khokhloma card back, its wool table, the dark cursor underline and the cover/pass arrow of `KeyboardPlayMode.tsx`.

## Files

- `glyphs.py`: every shape as ASCII art (ranks, suits, pips, the khokhloma back, the font, the icons).
  This is the editable source of truth for the pixels.
- `gen_art.py`: draws the tile banks from `glyphs.py`, composes every screen mockup from those same tiles, and checks the hardware limits.
- `screens_full.py`: the full-screen pictures (dashboard, win, lose) and the city skyline.
- `export.py`: writes the indexed PNGs, runs GBDK's `png2asset` on them, decodes its bytes back and asserts they equal the drawn tiles, and writes `gbdk/art_tiles.h`.
- `png/`: the indexed 4-shade PNGs (palette in index order), one per bank and one per full screen.
  `cards.png`, `ui.png` and `sprites.png` are one tile wide so `png2asset` keeps their order; the `*_sheet.png` files are the same tiles 16 wide for looking at.
- `gbdk/`: the `png2asset` output (`cards.c/.h`, `ui.c/.h`, `sprites.c/.h`, `dashboard.c/.h`, `dashboard_new.c/.h`, `win.c/.h`, `lose.c/.h`) and the generated `art_tiles.h`.
  All generated; do not edit them by hand.
- `gbdk/check/`: a check ROM that loads the banks the way the game will and shows every table mockup composed from them (A or right: next, left: previous, B: the full screens).
- `mockups/`: every screen at 4x with nearest neighbour (640x576), and contact sheets.

Regenerate everything:

```
cd gameboy/art
python3 gen_art.py && python3 export.py && make -C gbdk/check
```

`GBDK` defaults to `/Users/alex/Dev/gb-hello/toolchain/dl/gbdk`.

## Palette

One mapping for every layer: index 0 = table (light grey), 1 = paper (white), 2 = dark grey, 3 = black.

- `BGP = 0xE1` (background and window: the window shares BGP).
- `OBP0 = 0xE1` (all sprites).
- `OBP1` is unused.

Why 0 is the table and not white: sprite colour 0 is always transparent, and a card's round corner pixels are index 0.
So the same card tiles work on the background (the corner shows the table grey) and as sprites (the corner is see-through), with no second copy.
Nothing inside a card or a card back uses index 0, so a card flying as sprites has no holes.

Red suits (hearts, diamonds) are dark grey and black suits are black, but the four suits differ by shape first: hearts and diamonds never rely on the shade.

## VRAM

One 256-tile space at 0x8000, shared by the background, the window and the sprites (`LCDC` bit 4 = 1).
`art_tiles.h` names every tile by its VRAM index.

| Tiles | Bank | Count | Used by |
| --- | --- | --- | --- |
| 0..69 | `cards` | 70 | background cards, and the flying card as sprites |
| 70..130 | `ui` | 61 | wool, rail font, icons; font tiles also as sprite "chips" |
| 140..154 | `sprites` | 15 | underline, arrow, brackets, nudge, pointer, deal flip |

That is 131 background tiles of the 256 the background and window may use, which leaves room (the limit served: background and window share at most 256 tiles).

Load:

```
LCDC_REG |= LCDCF_BG8000;
BGP_REG = ART_BGP;  OBP0_REG = ART_OBP0;
set_bkg_data(ART_CARDS_BASE, ART_CARDS_COUNT, cards_tiles);
set_bkg_data(ART_UI_BASE, ART_UI_COUNT, ui_tiles);
set_sprite_data(ART_SPRITES_BASE, ART_SPRITES_COUNT, sprites_tiles);
SPRITES_8x8;
```

`gbdk/check/check.c` does exactly this and is the working example.

## Cards: 16x24, 2x3 tiles

A card is 2 tiles wide and 3 tall: big enough for a legible rank and suit, small enough that six battles fit on the 20x18 tile screen.

```
(0,0) RANK_r_uC    (1,0) FACE_TR
(0,1) SUIT_s       (1,1) PIPT_s
(0,2) PIPL_s_uC    (1,2) PIPR_s
```

- `RANK_r`: the rank (6, 7, 8, 9, 10, J, Q, K, A) at the top left, with the top-left round corner.
- `SUIT_s`: the small suit under it (s = S, H, C, D in the kernel's suit order: spade, heart, club, diamond).
- The big 7x7 pip sits low in the middle, across `PIPT_s`, `PIPL_s` and `PIPR_s`.
- A face costs 1 rank tile + 5 tiles that depend on the suit only, so all 36 faces come from 70 tiles instead of 216 (the limit served: 256 background tiles).

The left column alone (rank, small suit, half the pip) names the card, which is what an overlapped hand shows.

### The round-corner rule (the `_uC` variants)

A round-corner pixel shows whatever is under it.
Because cards are tile aligned, that pixel is the only thing that changes when a card overlaps another, so the rank tile and the bottom-left tile come in variants named by the colour under the corner:

| Corner | Under it | Variant |
| --- | --- | --- |
| top left | the table (nothing) | `_u0` |
| top left | the card to its left in the same row (its black top border) | `_u3` |
| top left | a card it covers one tile down and right, or a raised neighbour (white middle) | `_u1` |
| bottom left | the table (nothing) | `_u0` |
| bottom left | the card to its left in the same row (its black bottom border) | `_u3` |
| bottom left | a raised card's corner over its left neighbour's pip top: pixel (8,15) of that neighbour, which is white for a heart (`_u1`), dark grey for a diamond (`_u2`) and black for a spade or club (`_u3`) | `_u1`/`_u2`/`_u3` |

The card back (`BACK_*`) needs only `_u0` and `_u3` (bot hand overlap, the deck over the trump).
`FACE_TR`, `PIPR_s`, `BACK_TR` and `BACK_BR` only ever sit over the table.
`gen_art.py` composes every mockup pixel by pixel and fails if any resulting tile is missing from the banks, so the rule above is checked, not just stated.

### The back

Pixel khokhloma, after the website's card back: black lacquer, a dark grey frame, one gold sprig with its stem running across the card (the website lays its pattern on its side), two berry clusters and dark grey leaves.
Gold is the brightest colour in khokhloma, so gold and berries are white and the leaves dark grey.

### The pass slot

`PASS_*` (6 tiles) is the empty space target mode points at to pass: a dashed outline round a patch of plain table.

## The table screen

20x18 tiles, composed by the code from the three banks.

| Row | What |
| --- | --- |
| 0 | top rail: bot name, `IC_MINIBACK` + bot card count; right: `IC_MINIDECK` + deck count, trump suit icon (the suit stays after the deck runs out) |
| 1-3 | bot hand: backs one column apart, centred (18 at most; the rail has the true count) |
| 4-11 | table: trump at (0,4) under the deck at (1,4); discard pile at (0,8) |
| 12 | raise row: a selected card moves up one tile |
| 13-15 | your hand |
| 16 | underline row (wool); hidden-card tabs when the hand scrolls |
| 17 | foot rail: the status line and the move DOWN or UP makes (`{DOWN}DONE`, `{DOWN}PICK UP`, `{UP}PLAY`, `{UP}COVER`, `{UP}PASS`) |

There is no rail across the middle of the screen: the status lives in the foot rail, under the hand.
Rails are rows of `ui` font tiles (white on dark grey); everything else on the table is `WOOL` (one tile, repeated).
The foot rail can be the window layer (`WY = 136`) if that is easier to update; it uses the same BGP.

### Battles

Attack cards sit at `battle_layout(n)` in `gen_art.py`, centred on the screen: up to three battles in one row at table row 6 (centred vertically), four to six in two rows at rows 4 and 8 (three on top), each row centred, 4 columns per battle.
A cover card sits one tile right and one tile down from its attack (`_u1` rank variant), so the attack's rank and suit stay visible.
The pass slot is one battle step right of the last attack.

### Your hand

- Up to 10 cards: spread, 2 columns each, centred, every card whole.
- 11 to 19 cards: one column each (rank, suit and half a pip show), centred.
- 20 or more: 19 show, the hand scrolls with the cursor, and row 16 shows `{LEFT}n` and `n{RIGHT}` tabs for the hidden cards each side.
  The 36-card deck can leave a hand of 30, and this keeps every card one press away without shrinking the faces.
- A selected card (A toggles it) is raised one tile (row 12); several can be raised at once.

### Sprites on the table (8x8 mode, OBP0)

| Sprite | Tiles | Where |
| --- | --- | --- |
| Cursor underline | `SP_UL_ONE` under a one-column card; `SP_UL_LEFT` + `SP_UL_RIGHT` under a whole card | 1 px under the card's bottom (row 16) |
| Cover/pass arrow | `SP_DOT` every 6 px (cover) or every 10 px (pass, the website's dashed line), head `SP_HEAD_N` or `SP_HEAD_NE` (X flip for NW) | from the cursor card's top to just under the target |
| Target brackets | `SP_BRACKET`, flipped for the four corners | round the attack being covered |
| PASS label | the rail font tiles as sprites (a pill) | over the pass slot |
| Rearrange nudges | `SP_NUDGE`, X flip for the right one | beside the underline while SELECT is held |
| Flying card | the card's own six tiles from the `cards` bank | anywhere |

The most sprites any mockup puts on one scanline is 4 (target mode, pass), against the hardware's 10; the most on screen is 12 of 40.
A flying card is 2 sprites wide, so even two cards in flight stay at 4 per line (the limit served: 10 sprites per scanline).

## Controls the layouts show

- Left/right: move the cursor (the dark underline).
- A: toggle the cursor card in or out of the selection (raised).
- B: clear the selection, or back out of target mode.
- UP: play the selection or the cursor card; as defender with a choice, enter target mode (left/right moves the arrow between coverable attacks and, one step past the last, the pass slot; UP commits).
- DOWN: pick up (defender) or done (attacker), exactly when the foot rail shows it.
- SELECT held + left/right: slide the cursor card through the hand; the nudge arrows show the mode and the foot rail says `SELECT+{LEFT}{RIGHT} MOVE CARD`.
- START: back to the dashboard at once, like the website's back button (`BackButton.tsx` goes straight to `/dashboard`, no confirmation); the game stays and the dashboard offers CONTINUE.
  So there is no pause menu.

## Screen mockups and tile counts

Unique background tiles each screen uses (all banks together are 131, so every table screen fits by construction):

| Screen | File | Unique BG tiles | Sprites | Max per line |
| --- | --- | --- | --- | --- |
| You attack | `mockups/t1_attack.png` | 54 | 2 | 2 |
| Bot defends, all covered, DONE | `mockups/t2_bot_defends.png` | 57 | 0 | 0 |
| Target mode, cover | `mockups/t3_target_cover.png` | 54 | 10 | 2 |
| Target mode, pass | `mockups/t3b_target_pass.png` | 58 | 12 | 4 |
| Pick up | `mockups/t4_pickup.png` | 47 | 0 | 0 |
| Big hand (22), two selected | `mockups/t5_big_hand.png` | 53 | 1 | 1 |
| Rearrange (SELECT held) | `mockups/t6_rearrange.png` | 55 | 4 | 4 |
| Dashboard, with CONTINUE | `mockups/d1_dashboard.png` | 146 | 1 | 1 |
| Dashboard, no game open | `mockups/d1b_dashboard_new.png` | 139 | 1 | 1 |
| Win | `mockups/e1_win.png` | 97 | 0 | 0 |
| Lose (ДУРАК!) | `mockups/e2_lose.png` | 122 | 0 | 0 |

Contact sheets: `mockups/01_cards_sheet.png`, `02_table_layouts.png`, `03_dashboard_win_lose.png`, `04_fly.png`, `05_deal.png`.

## The full screens

The dashboard, win and lose screens are single pictures; `png2asset -map` dedupes each into a tileset and a 20x18 map (`<name>_tiles`, `<name>_map`, `<name>_TILE_COUNT`).
Load the tileset at tile 0 and the map at (0,0); reload the three banks when going back to the table.
Each is under 160 tiles because the sky, the lacquer and the black areas repeat (the limit served: a full-screen picture needs 360 tiles unless it repeats).

### Dashboard (title + bot picker)

- A city skyline from Miami to Moscow (palms, the Empire State, N Seoul Tower, the Palace of Culture in Warsaw, the Kremlin's Spasskaya tower) under the DURAK title.
- On black lacquer with a khokhloma vine edge: CONTINUE (when a game is open), then the bots Miami, New York, Seoul and Warsaw (the boss), with difficulty pips.
- The menu rows sit on the tile grid (CONTINUE at row 11, the bots at rows 13 to 16).
- The CONTINUE city is printed by the code with the rail font (load the `ui` bank at tile 160 while the dashboard is up) at row 11, columns 11 onwards; `dashboard_new` is the same screen with no CONTINUE row.
- The pointer is `SP_POINTER` at x = 6, on the selected row's y.

### Win and lose

- Win: "YOU WIN", "SEOUL IS THE DURAK" (the bot's name is baked into the mockup; four aces fan below).
- Lose: "ДУРАК!" in Cyrillic, "YOU ARE THE FOOL", the fool's cap and the cards left in your hand.
- Both offer A rematch, B home.

## Motion

### A card flying from the hand to the table (`mockups/04_fly.png`)

About 12 frames (0.2 s).
The card leaves its hand slot (the hand keeps the gap until it lands), and its 6 sprites (its own `cards` tiles) move in a straight line to the attack slot with a small lift (`y - 12 sin(pi t)`).
On landing it becomes background tiles, the battles re-centre for the new count, and the hand closes up.
The bot's cards fly the same way from its hand row, as backs, turning into faces on landing.

### The deal (`mockups/05_deal.png`)

Cards leave the deck one at a time, alternating bot and you, 6 each, about 8 frames per card.
A card to the bot flies as a back (6 sprites).
A card to you flips in flight: back (6 sprites), then `SP_NARROW_*` (the back at half width, 3 sprites), then `SP_EDGE_*` (on its edge, 3 sprites), then the face (6 sprites), landing in the hand.
At most one card is in flight, so the deal never passes 2 sprites per scanline.
Taking cards after a turn (pick up, or refilling from the deck) uses the same flight.

## Checks

- `gen_art.py` fails if any table screen has a background tile that is not in a bank, a sprite tile not in a bank (with flips), more than 40 sprites, or more than 10 on a scanline; it also checks that every face is one rank tile plus five suit tiles, and that the wool's corner pixels are index 0.
- `export.py` decodes `png2asset`'s 2bpp bytes back to pixels and asserts they equal the drawn tiles (banks) and the drawn screens (full maps, tile by tile).
  Mutation checked: corrupting one pixel of the PNG before conversion makes it fail with "png2asset bytes differ from the drawn tiles".
- `gbdk/check` builds with GBDK's `lcc` and runs in SameBoy, showing the table mockups composed at runtime from the exported banks.
