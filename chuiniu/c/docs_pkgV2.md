# Package V2 - the word, and nothing leaves the drawer anywhere

The owner's word for the call is Liar.
Package U fitted the compact table and listed four findings it did not fix; the theme package listed three reveal details.
This package fixed all of them in the kernel and the study, and proved each on a real Messages simulator.
The decisions are DECISIONS I29 and I30.

## The word (I30)

`BTN_CALL` is "Liar" (`i18n/strings_en.c`); the key keeps its name, so the bridge and the wire are untouched.
Every sentence the kernel composes with the call was checked against the study:

| Line | Kernel now | Study |
|---|---|---|
| the button | Liar | Liar (the blood plate) |
| the bubble's caption | Bo calls three 3s | Bo calls three 3s |
| the outcome | Bo calls. Three 3s was true, Bo loses a die | the same |
| my turn's headline | Your turn: raise or call Liar | none (dropped in round nineteen, I28) |
| only the call left | Your turn: call Liar | none |
| a staged call | Send to call Liar on four 3s | none |
| rule 4 | Instead of raising, call Liar on the last bid. ... | none |

The headlines are not drawn on the table any more (I28); they are kept in step with the button for any host that shows them.

## What changed

| Finding | Cause | Fix |
|---|---|---|
| U (a): the reveal's lift took the compact row 42 to 69 points past the top | the row's crowns are fitted to the top at rest; the lift swings each crown about two radii further up | the reveal on a short board is one row of every seat, mine first (`one_row`), as big and as high as lets every cup tip in full; my tipped cup no longer covers the middle seat |
| U (b): far throws left tall drawers by 25 to 110 points | a held cup rises about four of its radii up the screen | a far seat throws only when its held cup stays inside through its whole throw (`throw_mask`, read through `cn_roll_cup_pose`), else it stays down; 54 of 616 far seats still throw over the measured drawers |
| U (c): 400 tall, 401 to 421 short | `top_m` stepped from 8 to 30 at 400 | `top_m` grows a point a point from 400 to 422; one threshold, tested at every height from 281 to 900 |
| U (d): the throw's first .35 s put the cup under the table | the lift started 60 (times the reach) under the held height | the start is lowered at most to leave either rim 1 point (times the reach) over the planks |
| found here: tall drawers' side crowns 1 to 36 points past the sides from 760 up, their turn | the study fits the ring with its own map, not the painted one | the ring is fitted to the glass and off the plate |
| found here: my peek past the top from 400 to about 450 | the dice fit ran on short boards only | it runs on every board |
| found here: my throw past the top from 400 to 420 on their turn | the reach was fitted on the picker-up board only, and on a tall drawer each screen is another board | the least reach of the three screens |
| theme: "There were five" took two lines on the 160 plate | the tally is longer than a bid | the reveal's plate is 240 wide (the plate's frame is the kernel's; Swift's step-down stays the fallback) |
| theme: the bid plate over a cup on a short board | (already fixed by U) | proved by the HUD test at every height and seat count |
| theme: my cup over Di's name, six seats, compact | my crown leans up over the middle seat's name | on a short board my dice are made smaller until my standing cup stays off every row name's letters (16 from 316 up) |
| the compact reveal's outcome line over the loser's stamp | (seen on the simulator) | the row leaves its names, the stamp and the outcome line their room |
| rings from the dice's spacing | Swift derived a radius | `RevealScreen.Rings` reads `hud.brassR[s]` |

`cn_lay_lift_fit` clamps the stage's lift so a cup stays inside and off the plate the whole way up (the crown is highest a quarter of the way up, not at the full tip); the layout makes room for the full lift, so the clamp is the last word for dice a throw put elsewhere: 1278 of 1368 cups lift in full over the tested drawers, every cup on a short board.

`cn_lay_make` costs 0.3 ms on a short board, 1.5 ms tall and up to 7 ms at 400 (three reach fits); a reveal begin up to 4 ms (measured with `sample`: the trig in the rim loop and the ring fit were the cost; the rim's unit circle is now a table in `CnLay` and the ring steps by half the overshoot).

## Goldens moved

| Golden | Why |
|---|---|
| `tests/cn_roll_test.c` seed 2026: 227 frames, hand 52001, fnv cf6066b3 -> 233 frames, hand 34215, fnv 0a4c45d8 | the lift starts over the table; the dice's closest approach to the mouth grew from 13.1 to 15.5 (small cup 4.8 to 6.4), chi2 of the faces 8.9 on 6 faces |
| `tools/study_layout.txt`, `tests/cn_lay_test.c` GOLD arrays, `tests/cn_stage_test.c` GOLD | 430 by 830 on their turn at 4, 5 and 6 seats: the side seats drawn in 6, 9 and 3 points (the study's crowns left the glass); every other case byte-identical. The dump was regenerated with `tools/dump_study_layout.mjs` from the study's own new JS, and the arrays by a converter that first reproduced the old arrays byte for byte from the old dump |
| `tests/cn_stage_test.c` frame hashes 18318e6b, e41ce178 -> e41c2ebf, 33a371c9 | the lift, and at 375 by 541 the far cups stay down |
| `docs/UI.html` `CN_ROLL_WASM_B64` | NOT re-embedded (cn_roll.c changed, so `make docs-roll-check` fails until `make docs-roll`); checked in a copy: after `docs-roll` the check passes and the page loads with no errors and throws mine alone at 390 by 718 |

The study's JS gained `cupReach` (a box, with a throw's pose), `topMargin`, the ring's fit to the glass and the far throws' check in `startRoll`; the stage's reveal and the dice fits are the kernel's only (the study draws neither).

## Found, not fixed

- **Names on a tall board.** The ring places each far name with the study's pillar discs, which are not the painted pictures: over the measured tall drawers a cup covers a name's letters 453 times, 231 of them with every cup standing (my cup over the top seat's name at 400 to 584 with two or four seats; a side cup over its own name at six seats), the rest at the reveal, where a tipped cup leans over the name of the seat behind it. `cn_stage_test` asserts it on short boards only and prints the tall count. A fix moves every tall golden and the study's name rule.
- **Brass rings on thrown dice.** `brass_r` is .75 of a die, inside half the ring spacing (1.7 of a die); dice a throw left touching sit closer, and their rings overlap (Cy and Ed, six seats, expanded). The dice-spacing radius Swift had avoided that; the kernel could give a per-die radius from the landed spacing.
- **The compact reveal re-lays the board.** My cup moves into the row and my dice sit at their stations, not where my throw left them (I22's "the reveal finds my dice where they landed" holds on tall boards only); the cups jump at the call.
- **No tally on the compact reveal at three or more seats** unless the row leaves room under it (the simulator's compact drawer at four and six seats had none); the outcome line carries the count's result.

## Every test, seen red

Each mutation applied alone by editing, the test rebuilt and run, the source restored by re-editing and `git diff` checked.

| Test | Mutation | Assertion that went red |
|---|---|---|
| `cn_say_test` the button's word | `BTN_CALL` back to "Call" | `:105` "the call's button: Call" |
| `cn_lay_test` the cup never under the table | `c->low = LOW * sc` (the old start) | `:1719` "the cup's lowest point -14.01, never into the table" (12 red) |
| `cn_lay_test` one threshold | `top_margin` back to `H > 400 ? 30 : 8` | "410: tall ...", "short below 400 ... (3 flips, the last at 422)", "never shrinks (1 times)" |
| `cn_lay_test` nothing leaves the drawer (tall, rest) | the ring's glass fit skipped | "390x830 n 3 theirs: at rest every body inside (x -3.0..)", the 430x830 goldens |
| the same (throws) | every far seat throws | "390x400 n 2: seat 1's throw, every frame inside", "718, two seats: the top seat's cup stays down" (993 red) |
| the same (my reach) | the reach fitted on the picker-up board only | "390x400 n 2: seat 0's throw, every frame inside (reach 0.908 ... y -10.9)" |
| the same (my peek) | the dice fit on short boards only | "390x400 n 2 mine: at rest every body inside (y -43.6..)" |
| `cn_stage_test` the reveal's lift | no one row (the table's row at the reveal) | "on a short board every cup lifts as far as shows its dice", "nine cups in ten (798 of 1368)" |
| the same | `cn_lay_lift_fit` returns the full tip | HUD: "375x400 n 2 reveal: seat 0's cup clear of the plate" (90 red) |
| the same (written after: first it held only 0, and this mutant survived; it now holds CN_LAY_EDGE) | the lift's reach read at the full tip only | "390x300 n 2: every frame of the lift CN_LAY_EDGE inside" (11 red) |
| `cn_stage_test` the HUD and the names | my cup off the row's names skipped | "375x310 n 6 mine: seat 0's cup clear of seat 3's name" |
| the same | the short plate 30 wider | "375x281 n 4 mine: seat 3's cup clear of the plate", "seat 3's name clear of the plate" (108 red) |
| `cn_lay_test` the reveal's plate | 160 at the reveal | "718's reveal: the plate 160 wide" |
| the same | never beside the compact row | "340's reveal, two seats: the plate beside the row" |
| the same | the row's foot the board's, not over the outcome line | "the reveal's row leaves its names, the stamp and the outcome line their room" |
| `BridgeKernelTests` the rings | `Rings.radius` half the die's side | ":213 seat 0: the ring is the kernel's", ":214 past the die's corners" |
| `StageViewTests` the throw's clock (moved to 375 by 900, six seats) | no far seat throws | ":236 far cups throw at 375 by 900 and outlast mine", ":250" |

Two rules were deleted because no mutation of them turned anything red: the short plate's extra extent rule (right of the row's crowns and names), which the study's own `rowEnd` rule already satisfies at every tested height and seat count.

## The simulator

Real Messages, the DEBUG StageHarness, `chuiniu/ios/Tools/rig.env`: the compact reveal at 2, 4 and 6 seats (one row, every cup up, every die in sight, "There were four" on one line beside the two-seat row), the expanded reveal at 2, 4 and 6 ("There were eight" on one line on the 240 plate), and a throw compact and expanded (my held cup inside the drawer; at six seats expanded Cy's cup throws inside it too).
