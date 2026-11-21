# Durak (foolish) on the Game Boy: the art

Everything the game draws, as tiles for the original Game Boy (DMG), with the screen layouts that use them.
The website is the reference: its card faces, its khokhloma card back, its wool table, the dark cursor underline and the cover/pass arrow of `KeyboardPlayMode.tsx`.

## Files

- `glyphs.py`: every shape as ASCII art (ranks, suits, pips, the khokhloma back, the font, the icons).
  This is the editable source of truth for the pixels.
- `gen_art.py`: draws the tile banks from `glyphs.py`, composes every screen mockup from those same tiles, and checks the hardware limits.
- `screens_full.py`: the full-screen pictures (dashboard, lose) and the city skyline.
- `travel.py`: the traveller dashboard (title + departures board + boarding pass).
- `wins.py`: the four per-bot win screens (each city at night, the fool's cap on its landmark) and the fireworks sprites.
- `export.py`: writes the indexed PNGs, runs GBDK's `png2asset` on them, decodes its bytes back and asserts they equal the drawn tiles, and writes `gbdk/art_tiles.h`.
- `png/`: the indexed 4-shade PNGs (palette in index order), one per bank and one per full screen.
  `cards.png`, `ui.png` and `sprites.png` are one tile wide so `png2asset` keeps their order; the `*_sheet.png` files are the same tiles 16 wide for looking at.
- `gbdk/`: the `png2asset` output (`cards.c/.h`, `ui.c/.h`, `sprites.c/.h`, `lacquer.c/.h` (the white-on-black font), `paper.c/.h` (the black-on-white font), `plane.c/.h`, `dashboard.c/.h`, `dashboard_new.c/.h`, `win_miami.c/.h`, `win_new_york.c/.h`, `win_seoul.c/.h`, `win_warsaw.c/.h`, `fireworks.c/.h`, `lose.c/.h`) and the generated `art_tiles.h`.
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
| 0..71 | `cards` | 72 | background cards, and the flying card as sprites |
| 72..132 | `ui` | 61 | wool, rail font, icons; font tiles also as sprite "chips" |
| 140..154 | `sprites` | 15 | underline, arrow, brackets, nudge, pointer, deal flip |

That is 133 background tiles of the 256 the background and window may use, which leaves room (the limit served: background and window share at most 256 tiles).

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
- A face costs 1 rank tile + 5 tiles that depend on the suit only, so all 36 faces and the back come from 72 tiles (36 faces alone would need 216) (the limit served: 256 background tiles).

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

The card back (`BACK_*`) needs `_u0`, `_u3` (the deck over the trump's black border) and `_u2` (a back over another back's dark grey ring, the bot hand).
`FACE_TR`, `PIPR_s`, `BACK_TR` and `BACK_BR` only ever sit over the table.
`gen_art.py` composes every mockup pixel by pixel and fails if any resulting tile is missing from the banks, so the rule above is checked, not just stated.

### The back

Pixel khokhloma, after the website's card back (black lacquer, gold, red berries), as one rich motif: a rowan sprig with a curled gold tendril, two spiral curls off the stem in the Khokhloma manner, two large veined leaves (8x8) and two round berries (5x5).
The edge is two rings: dark grey outermost (it parts the bot's overlapped backs), then black; the sprig has the whole 12x20 field inside them.
Each berry has a white body, a dark grey shaded underside and a black calyx, and is ringed in lacquer, drawn back to front, so the two read as separate fruit.
The sprig is built from stamps laid along a stem path in `sprig/sprig.py`, in three layouts (`hanging`, `diagonal`, `arching`); a minimal version of each (`hanging_min`, `diagonal_min`, `arching_min`: one leaf, one vine ending in a spiral curl, one 5x5 berry) sits beside them.
`BACK_LAYOUT` in `gen_art.py` picks one; it is `final_2b_framed` (Alex's picks: the diagonal minimal sprig 2B with option a's curl, option d's uneven one-pixel vine, the serrated veined leaf, the small shaded berry with its calyx, and an intricate small leaf with midrib, side veins and a serrated edge in the bottom-left corner, inside two rings, dark grey outermost and black inside, the motif grown into the freed pixel all round (the big curl, the full corner leaf); the grey edge gives the bot's overlapped backs their dividing line; `mockups/12_back_framed.png`, alone and overlapped).
The options that led there are kept as sheets: `06_back_rotations.png`, `07_back_vine_options.png`, `10_back_combo.png`.
Without the frame, overlapped backs (the bot hand) have no dividing line, so the repeating sprig is what separates them.

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

Unique background tiles each screen uses (all banks together are 133, so every table screen fits by construction):

| Screen | File | Unique BG tiles | Sprites | Max per line |
| --- | --- | --- | --- | --- |
| You attack | `mockups/t1_attack.png` | 56 | 2 | 2 |
| Bot defends, all covered, DONE | `mockups/t2_bot_defends.png` | 59 | 0 | 0 |
| Target mode, cover | `mockups/t3_target_cover.png` | 56 | 10 | 2 |
| Target mode, pass | `mockups/t3b_target_pass.png` | 60 | 12 | 4 |
| Pick up | `mockups/t4_pickup.png` | 47 | 0 | 0 |
| Big hand (22), two selected | `mockups/t5_big_hand.png` | 53 | 1 | 1 |
| Rearrange (SELECT held) | `mockups/t6_rearrange.png` | 57 | 4 | 4 |
| Dashboard, with CONTINUE | `mockups/d1_dashboard.png` | 174 (map shipped: 172) | 2 | 1 |
| Dashboard, no game open | `mockups/d1b_dashboard_new.png` | 175 | 2 | 1 |
| Win over Miami | `mockups/w_miami.png` | 191 (map shipped: 190) | 8 | 4 |
| Win over New York | `mockups/w_new_york.png` | 156 (152) | 12 | 6 |
| Win over Seoul | `mockups/w_seoul.png` | 132 (129) | 12 | 6 |
| Win over Warsaw | `mockups/w_warsaw.png` | 162 (160) | 12 | 6 |
| Lose (ДУРАК!) | `mockups/e2_lose.png` | 122 | 0 | 0 |

Contact sheets: `mockups/01_cards_sheet.png`, `02_table_layouts.png`, `03_dashboard_win_lose.png`, `04_fly.png`, `05_deal.png`.

## The full screens

The dashboard, win and lose screens are single pictures; `png2asset -map` dedupes each into a tileset and a 20x18 map (`<name>_tiles`, `<name>_map`, `<name>_TILE_COUNT`).
Load the tileset at tile 0 and the map at (0,0); reload the three banks when going back to the table.
Each is under 160 tiles because the sky, the lacquer and the black areas repeat (the limit served: a full-screen picture needs 360 tiles unless it repeats).

### Dashboard (title + bot picker): the traveller

After the blind designer's departures board, in our style (`travel.py`; `mockups/13_dashboard_travel.png`, with and without a game open).

- Rows 0-9: ДУРАК with FOOLISH under it, over the city skyline from Miami to Moscow; a dotted flight route runs across the sky behind the towers.
- Moscow is the real top bot, but the Game Boy is too weak to run it, so it stands in the skyline only and has no row on the board.
- Row 10: with a game open, CONTINUE is a boarding pass (a paper ticket with punched ends and a tear line); without one, "PICK A FLIGHT".
- Row 11: DEPARTURES, with the move (A FLY) on the right.
- Rows 13-16: a split-flap board, one row per bot in our order: MIAMI 06:10 (1 pip), NEW YORK 09:45 (2), SEOUL 17:20 (3), WARSAW 23:55 (BOSS); the later the flight, the harder the bot.
- Sprites: `SP_POINTER` at x = 0 on the selected row (the ticket is the first row when there is one), and the plane (`plane.c`) on the route at y = `ART_DASH_ROUTE_Y` over the selected city (x from `travel.CITY_X`: Miami 14, New York 44, Seoul 75, Warsaw 112), gliding between cities as the pointer moves.
- The CONTINUE city is printed by the code with the paper font (`paper.c`, black on white, the same characters as the rail font) loaded at `ART_PAPER_BASE` (200), on row `ART_DASH_CITY_ROW` (10) from column `ART_DASH_CITY_COL` (11); the shipped `dashboard` map leaves those cells as blank paper.
- VRAM while the dashboard is up: its tileset from 0 (at most 200 tiles, checked; 172 and 175 now), the paper font at 200, `SP_POINTER` copied to tile 250 and the plane at 251.

### Win and lose

- Win, one screen per bot (`win_miami`, `win_new_york`, `win_seoul`, `win_warsaw`; `mockups/08_win_cities.png`): "YOU WIN" over the beaten city at night, its own skyline with lit windows over the water, and the fool's cap (a two-horned jester cap with bells) on its tallest landmark: each drawn from reference photos (Wikimedia Commons) for true proportions and signature details: Miami Beach at night: the Brickell condo towers behind (slim glass towers with balcony bands on a shared row grid so their tiles repeat, sail, curved and lit-crown tops, two rising above the Freedom Tower), the Freedom Tower in front (arcaded base, tiered tower, belfry, domed cupola, lantern) with the art deco hotels and palms, then the beach (dithered sand, a striped lifeguard hut on stilts with its ramp and flag, two umbrellas), the surf and the moon's path on the water; New York's Empire State (podium, slim shaft of vertical piers, setbacks, the long mast) with the Chrysler building (terraced sunburst arches, needle) and a Brooklyn Bridge tower (twin pointed arches, cable fans); Seoul's N Seoul Tower (slim shaft, barrel pod, striped antenna) on wooded Namsan, Lotte World Tower (slender cone, split crown) and Namdaemun (stone gate base, two-tier roof with upturned eaves); Warsaw's Palace of Culture (wings, stepped tower with pinnacles, clock, long spire) with the Old Town, the Royal Castle's clock tower, Sigismund's Column and the new towers (`mockups/14_win_before_after.png`, and the current four in full in `mockups/15_win_after.png`).
  Under it the bot's name, then "IS THE DURAK".
  The shipped map leaves the name row black: the code loads `lacquer.c` (white on black, the same characters as the rail font) at `ART_LACQUER_BASE` (206) and prints the name on row `ART_WIN_NAME_ROW` (14), from column (20 - length) / 2, tile = 206 + index of the character in `ART_FONT_CHARS`.
  Each win tileset is at most 206 tiles (checked), so the tileset, the lacquer font and the fireworks fit one 256-tile space.
- Lose: "ДУРАК!" in Cyrillic, "YOU ARE THE FOOL", the fool's cap and the cards left in your hand.
- Both offer A rematch, B home.

## Motion

### Fireworks on the win screens (`mockups/09_fireworks_frames.png`)

Sprites, loaded from `fireworks.c` at `ART_FW_BASE` (252): `T_FW_SPARK`, `T_FW_BURST`, `T_FW_FADE` are the top-left quarter of a 16x16 burst (draw it four times with X and Y flips), `T_FW_SHELL` is a rising shell.
Bursts at fixed points per city (`wins.BURSTS`: two over Miami, whose sky is full of condos, three elsewhere), each one phase behind the last, in a 4-frame loop (`wins.SCHEDULE`): shell, spark, burst, fade; about 8 vsyncs per frame.
At most 12 sprites on screen and 6 on one line.
`gen_art.py` renders every frame of every city and fails if any firework pixel lands on anything but black sky or a star (mutation checked: a burst moved onto N Seoul Tower fails at the first covered pixel).

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
- `export.py` also prints the names into the shipped (blank) dashboard and win maps the way the code will and asserts the result equals the mockups; mutation checked by printing the win name one column off.
- The check ROM prints SEOUL on the dashboard and each bot's name on its win screen with the same rule.
- `gbdk/check` builds with GBDK's `lcc` and runs in SameBoy, showing the table mockups composed at runtime from the exported banks.
