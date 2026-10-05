# Package U - nothing on the compact table leaves the drawer

The Swift host package found, on a real simulator, the far cups' crowns and the held cups cut off by the compact drawer's top edge (`03_6seats_collapsed.png`, `04_4seats_collapsed_midroll.png`, a 328-point drawer).
This package measured where every body goes, found four separate causes, and fixed them in the layout.

## How it was measured

Every vertex of every body, as the stage draws it: the mesh placed by `cn_geom_emit`, seen from the eye by `cn_cam_project`, put on the board's place in the drawer, turned by `cn_cam_map` (the map the host paints with).
At rest, at my cup's full peek, and at every 60 Hz frame of every throw plus the half-frame between each pair (the host interpolates).
Drawers 281 (the iPhone 17e), 290, 300, 310, 323, 328, 334 and 340 points tall, 390 and 375 wide, 2 to 6 seats (six with a seat out, its cup lying), my turn, theirs and the reveal.

## What was wrong (before this package)

| Cause | Where it showed | How far out |
|---|---|---|
| The study's `rowSeats` places the row with no regard for the crown, which leans up the screen from the eye | the row's crowns at rest, drawers under 323 | 14 points past the top at 281, 4 at 310 |
| Every seat throws, and a held cup rises about four of its radii up the screen | every far cup in the row during the roll (screenshot 04) | 53 to 104 points past the top at every compact height; up to 35 past the left edge |
| Short or tall read the board left after the shelf, so a 340 drawer was a row on my turn and a 320-point ring on theirs | their turn, 300 to 340: the ring's far throws, and my own | far throws 123 past the top, mine 48 |
| On their turn the short board ran to the drawer's foot, my cup with it | their turn, 281 and 290: my throw comes toward me as it lands | 22 points past the bottom |
| My cup tipped for the peek swings its crown up the screen | 281 to 300, my turn and theirs | 23 points past the top at 281 |

The study itself clipped all of these: its canvas reaches only `pad` (40) above the board, and the collapsed screen it shows is 340 on my turn, the one size where the resting row clears the top (by 9 points).
Screenshot 03 (6 seats, 328) was inside by 15 points at rest; what looked clipped there is the drawer's rounded corners.

## What changed

All in `src/cn_lay.c` (rules) and `src/cn_lay.h` (the constants and the contract), with one export added to `src/cn_roll.c`.

1. **Short or tall reads the drawer.** The board is short when the board this drawer has with the picker up is under 280 (DECISIONS I17: "the compact drawer's table is a short board").
   A short board is that board on every screen: my turn, theirs and the reveal.
   Nothing on it moves when the turn comes round, and on their turn the picker's room is the room my throw comes down through.
   The study read the board after the shelf, so a 340 drawer flipped between the row and the ring every turn (docs_pkgB.md raised this as a design call).
2. **The row fits its crowns.** As big as the row allows (the study's), then a point smaller at a time until every crown in the row, standing or lying, is `CN_LAY_EDGE` (4 points) inside the drawer.
3. **My dice fit my peek.** On a short board my dice are the study's 16, then half a point smaller at a time until my cup, tipped as far as any peek tips it, stays inside (16 from 310 up, 13 at 281).
4. **Far cups stay down on a short board.** `cn_lay_throws` writes my throw alone there.
   A far cup held as a throw holds it needs about four radii of room above the row, and the row has one; at full reach the row's cups would have to shrink to a radius of 10 to 18.
   Their dice are hidden under them anyway.
   The study's `startRoll` follows (`canvas._short`).
5. **My throw is held as high as the drawer allows.** `L.my_reach` is the study's reach (1) when my held cup stays inside, else the largest that does (bisection), never under `CN_LAY_REACH_MIN` = .8 (at .7 the shaken crown dips 5 points into the table).
   It is fitted on this drawer's picker-up board, the tightest, so my throw is one throw on every screen and the reveal finds my dice where they landed.
   From 281 to 340 it is 1 (the dice fit, item 3, keeps my cup low enough); it bites under 255 and on tall boards from 400 to about 480.
   The fit reads the throw's own path: `cn_roll_cup_span` and `cn_roll_cup_pose` (new in `cn_roll.h`) give the cup's pose at any time without a bake, since the dice never move the cup.
   `bakeCup`'s path set-up moved into `cupPath`, shared by both; the bake's golden (seed 2026, fnv cf6066b3) is unchanged.

Result: from 281 to 340, 375 and 390 wide, 2 to 6 seats, every screen, the nearest any body comes to the drawer's edge is 4.47 points.

| Drawer | my dice | my cup R | row R (6 seats, 390) |
|---|---|---|---|
| 281 | 13 | 34.2 | 24.7 |
| 300 | 14.5 | 37.6 | 28.2 |
| 310 to 340 | 16 (the study's) | 40.9 | 30.2 (the study's) |

## Seam request (d): the brass rings

`CnStageHud` gains `die_d[6]` (each seat's die side on the glass, at its cup, through the painted turn) and `brass_r[6]` (the reveal's brass ring radius, .75 of that: past the die's corners at .71, inside half the dice's spacing at .85), filled from `CnLay.die_g` and `CnLay.brass_r` by one line in `cn_stage_begin`.
structgen carries them to Swift as `dieD` and `brassR`; nothing in `ios/cn_api*` or `layout.args` needed to change (the HUD is already a root).
**Swift is not edited (the E packages are done).** `RevealScreen.swift`'s `Rings` should read `hud.brassR[s]` in place of `Self.radius(pts, ...)`; that is the T package's file.

## Goldens

| Golden | Moved? | Why |
|---|---|---|
| `tools/study_layout.txt`, 390 by 340 on their turn, 2 to 6 seats | yes, now identical to my turn's | the study was wrong (the flip); `docs/UI.html`'s `screen()` reads the picker-up board now and the dump was regenerated with `tools/dump_study_layout.mjs` |
| every other case of the dump (45) | no, byte-identical | |
| `tests/cn_lay_test.c` `GOLD`, `GOLD_DICE`, `GOLD_CUPS` | the five 340-theirs cases | regenerated from the new dump (a converter that reproduces the old arrays byte for byte from the old dump) |
| `tests/cn_stage_test.c` 340-theirs row | yes, the row's numbers | the same |
| `tests/cn_cam_test.c` 340-theirs camera | kept, commented | still the camera over a 358 by 320 board |
| `docs/UI.html` `CN_ROLL_WASM_B64` | re-embedded (`make docs-roll`) | `cn_roll.c` changed |

The study also gained `cupInside` and the row's crown fit, so it and the kernel agree at any size; at the study's own sizes the fit changes nothing.
The peek fit (item 3) and the reach fit (item 5) are the kernel's only: the study never draws a drawer where they bite, and its wasm entry has no reach argument.

## Found, not fixed (other owners or out of scope)

- **The reveal's lift on a short board.** DECISIONS I23 tips every standing cup about the far edge of its mouth; in the row that swings each crown up past the top, 42 to 69 points at every compact height.
  The lift is the stage's (`cn_stage_objects`).
  Proposal: `cn_lay` gives a per-seat largest tip that stays inside, and the stage clamps the lift to it; or on a short board the row's cups lift about the near edge.
  The drawer test checks the reveal with its cups down and says so.
- **Tall boards, 400 and up.** The far seats' throws leave the drawer at every size, as in the study: 25 to 58 points past the sides, 30 to 110 past the top (the side seats' crowns are fitted to the screen's edge at rest, so any lift leans them out).
  The canvas clips them.
  A full fix moves the ring inward, which moves every tall golden.
- **400 is tall and 401 to 421 are short again.** `top_m` steps from 8 to 30 above 400 (the study's rule), so the picker-up board drops back under 280.
- **The throw starts under the table.** `cn_roll.c` starts the held cup `LOW` below its held height; in the first .35 s its crown is 14 points into the planks at full reach (21 at .8).
- 6 seats at 375 wide: the row's step limits R to 28.7, under the 30.2 the 390 width allows; fine, noted.

## Every test, seen red

Each mutation applied alone with an editor, the binary deleted first, then the source restored by re-editing and `git diff` checked clean.
Line numbers are the test files' as committed in 87cce511.

| Test | Mutation | Assertion that went red |
|---|---|---|
| nothing leaves the drawer (written before the fix) | the old layout (no fits, far throws, the flip) | `cn_lay_test.c:1615` "at rest every body inside" (146 red) and the throw's "every frame inside" |
| the same | the row's crown fit skipped (`if (1 \|\| ...)`) | `:1615` "390x281 n 2 mine: at rest every body inside (y -10.8..)" (105 red) |
| the same | the peek's dice fit skipped | `:1615` "y -23.2", and `:1660` "281: ... my dice made smaller (16.0)" |
| the same | far seats throw on a short board | `:1620` "a short board, my throw alone (2)" and `:1642` "seat 1's throw, every frame inside (y -49.8..)" (912 red) |
| short or tall | the board after the shelf decides | `:1352` golden "390x340 theirs n 2: board 320 short 0", `:1438` "340 on theirs: the same short board" |
| my throw's reach | `fit_reach` always 1 | `:1665` "240: held lower (1.000)", `:1673` "460 on theirs" |
| the same | the reach fitted on the board in hand, not the picker-up one | `:1673` "460 on theirs: the reach of 460 on mine", `:1676` |
| the same | `CN_LAY_REACH_MIN` .7 | `:1716` "the cup's lowest point -5.30, not into the table" |
| the cup without a bake | home from the slam, not past its shiver | `:1582` "every one of 219 frames, worst 1" |
| the brass rings | `CN_LAY_BRASS` .9 | `:1730` "the ring 15.26 clears the die's corners and not its neighbours'" |
| the same | every seat's die side mine | `:1729` "seat 1: the die's side on the glass 16.42 (12.13)" |
| the HUD carries them (`cn_stage_test.c`) | `brass_r` filled with `die_g` | `cn_stage_test.c:114` "a die's side and its brass ring at every seat (16.96, 16.96)" |
