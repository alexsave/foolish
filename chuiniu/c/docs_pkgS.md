# Package S - the independent verifier's findings, fixed

The independent verifier set the simulator's captures beside the study (`chuiniu/docs/UI.html`) and listed eight things that looked wrong.
This package fixed each in the kernel where it is geometry, proved each on the `cnpolish` simulator inside Messages, and saw every new test red under a deliberate break.

## What each finding was, and what changed

| # | Finding | Cause | Fix |
|---|---|---|---|
| 1 | Reveal rings about a die off the dice | the ring centres were right (`tools/cn_glass_shot.c` lays the HUD's `die_x` on the turned frame: every ring on its die); a 3-point glow ring on a 7-point die ate the die, and the study glows the die's square, not a circle | the HUD gives each die's up face on the glass (`die_q`: four corners, grown by the study's .07), `RevealScreen.Rings` strokes it as a rounded quad in the glow; the dice that do not count are drowned by the lift in the picture itself (`CnStageIn.count_mask`, tint and shade toward the study's `#0b2e32` wash); `brass_r` and `CN_LAY_BRASS` are deleted |
| 2 | Expanded six seats: lifted cups hide the far seats' dice | on the ring a seat sits behind another (Cy behind Bo, the top seat behind my cup), and a cup tipped to show its own dice swings its crown over the dice behind it | the reveal on a tall board is the study's list in rows (`cn_lay.c` `list_rows`): rows of `per` seats, mine first, each row as high as keeps its tipped cups off every die, name and stamp of the rows behind; the most rows that give the biggest cups (six seats at 390 by 718: two rows of three); no cup throws at the reveal, as on a short board |
| 3 | "Loses a die" off the left edge, over the next name | centred on its name in the turned layer, never clamped | the stamp is on the glass (`StageUIView.stampFrame`): centred under its name's block, moved inside the drawer by `CN_STAGE_EDGE`, then under any other name it would cover; the layout leaves it its whole room (`CN_LAY_STAMP_FOOT` 40 to 52: it sat over the next row's crown) |
| 4 | Compact throw: the plate over half my held cup; names over the cup | the plate is flat on the glass over the picture; the names are decals since package N and the held cup hides them (seen at four and six seats) | `cn_lay` says when my throw's held cup passes under the plate (`plate_throw`), and `TableScreen` stands the plate down until my dice rest |
| 5 | Compact plate at four seats in two lines | the room right of the row was 102 points | `cn_lay` `row_step` (and the study's `rowStep`): when the plate would be under 140 the row's step narrows to leave it 140, as long as the step stays 60; every four-seat bid is then one line |
| 6 | A plank's end (a black bar) behind the stepper and the chips | the tile's phase is the study's, and an end crosses the table every half tile | the HUD gives the tile's phase (`planks_y`): the study's, moved the least that keeps every end off the shelf (12 of 216 tested tables move); `CN_TEX_PLANK_*` is shared by `cn_texgen` and the stage |
| 7 | The outcome line one colour; the end not reached | the line was one string | the kernel gives the loser's and the winner's clauses (`CN_API_W_OUTCOME_LOSS`, `_WIN`; `CAP_CALL_TRUE` now composes `{loss}` from `CAP_LOSES`), `RevealScreen.styled` sets them in blood and the glow; the outcome line sits at the kernel's rect (`outcome`, under the list's last row on the glass); `dev.harness "2 over"` plays a game to its winner |
| 8 | Package N's Swift tests never seen red | the run was cut off | done now (below), and one survived: the letters' ink; the picture test now compares the letters' rows |

Found on the way: the compact reveal compared the stamp's foot (flat, turned with the names) with the outcome line (on the glass); `foot_glass` puts both on the glass.

## Goldens moved

| Golden | Why |
|---|---|
| `tools/study_layout.txt`, `tests/cn_lay_test.c` GOLD, GOLD_DICE, GOLD_CUPS: 390 by 340 at four seats, my turn and theirs | the row narrowed for a 140 plate (R 36 to 29.67, the seats at 41.67, 109, 176.33, the plate 210 wide 140). Dumped from the study's own new JS (`tools/dump_study_layout.mjs`); the arrays by a converter that first reproduced all 840 dice and 200 cup rows byte for byte from the old dump. Every other case byte-identical |
| frame hashes, the throw's golden | none moved |

## Tests, each seen red

Each mutation applied alone by editing, the binary deleted (make's same-second trap), run, restored by re-editing, `git diff` checked.

| Test | Mutation | Assertion that went red |
|---|---|---|
| `cn_stage_test` every die's face seen past every cup (a ray from the eye to each top face's centre and corners, an independent inverse of the cup's pose) | the reveal on the ring again (`list = 0`) | ":659 375x400 n 2: seat 1's die 0 hidden by seat 0's cup", and "every cup lifts in full (1278 of 1368)", "seat 0's cup clear of seat 1's name" |
| the same | the list's rows not kept off the rows behind | ":659 375x400 n 6: seat 1's die 0 hidden by seat 4's cup" |
| `cn_stage_test` the face quads | the face at the die's centre, not its top | "every die's face quad is centred on its top (2245 of 2304 off)" |
| the same | corners in bow-tie order | "its sides ... (2304 faces off)", "its corners in turn round it (4608 not)" |
| the same, the drowned | no drowning | "every die that does not count drowned", "no die counting, the picture is darker (12973203 against 12973203)" |
| `cn_stage_test` the plate and my throw | `plate_throw` always 0 | ":695 375x281 n 2 mine 0: my cup passes under the plate, the HUD says not" |
| `cn_stage_test` the planks' ends | the study's phase always | ":731 375x281 screen 0: a plank's end under the shelf" (12 tables) |
| `cn_stage_test` the reveal's cups and stamps | the list's stamp foot back to 40 | "375x541 n 5 reveal: seat 3's cup clear of seat 0's stamp" |
| `cn_stage_test` the outcome line | its top flat, not through the turn | "375x584 n 4: seat 0's stamp clear of the outcome line" (95 red) |
| `cn_lay_test` the short plate | the row never narrowed | "375x281 n 4: a plate up to four seats", the 102 plates |
| `cn_say_test` the outcome's clauses | the loss clause filled from `CAP_OUT` | ":122 the loser's clause: Bo is out", ":144" |
| the same | a missing clause returns the whole line | ":124 no winner's clause before the end" |
| `StageViewTests` the stamp | no clamp | ":626 pushed right ... (-30 against 4)", ":630" |
| the same | no stacking | ":634 off the other name", ":635" |
| `BridgeKernelTests` the reveal | the blood never applied | ":235 the loser's clause alone in blood" |
| the same | `Rings.face` ignores the seat | ":220 seat 1 die 0: the face is the die's" |
| `StageViewTests` every four-seat bid one line | the kernel's 102 plate (`row_step` not narrowing, in the xcframework) | ":645" (the three-letter bids), ":638 375x300: four seats have a plate" |
| `StageViewTests` the plate stands down | `plateShown` always true | ":658 the plate stands down while my throw is in the air" |
| `StageViewTests` the planks' phase | `tileTop` ignores the HUD | ":672 laid where the kernel says" |
| package N `testANameIsHandedToTheStageOnlyWhenItChanges` | no change check (every name handed over each time) | ":603 the same names: nothing to hand over", ":604 (6, 3)", ":610 (9, 5)", ":612 (12, 6)" |
| the same | the view never hands names to the stage | ":601 three names handed over (0)", ":603", ":610", ":612", ":614" |
| package N `testANamesPictureIsItsLettersAndItsBar` | the bar drawn off the turn too | ":567 no bar off the turn (822 against 0)" |
| the same | one ink for every name | SURVIVED; the test gained ":565 the turn's letters are brighter than the dim ones", which then went red |

## The simulator

`cnpolish` (iPhone 17 Pro, iOS 27.0), the Debug build by the rig, `dev.harness` for each table; shots in `chuiniu/c/build/shots/` (not committed).

- Six seats, compact reveal: one row, each counting die's face in the glow, the rest drowned, the stamp under Bo inside the drawer, the outcome line with "Bo loses a die" in blood (`s6_reveal_compact.png`).
- Six seats, expanded: two rows of three, every die in sight, the stamp clear of the row in front (`s6_reveal_expanded2.png`). Four seats expanded: two rows of two (`s4_reveal_expanded.png`).
- Two seats compact, my throw: the plate stands down while the cup is in the air and is back at rest (`t2_*`); four and six seats: the held cup hides the row's names behind it (`t4_*`, `t6_*`).
- Four seats compact: "three 6s" on one line on the 140 plate; the plank's end above the shelf, not behind the stepper (`s4_table_compact.png`).
- The end, two seats: "Bo calls. One 2 was false, Alex loses a die. Bo wins", the loss in blood and the win in the glow, compact and expanded (`s2_end_*`).

## Still wrong, or not done

- The study's end screen has a New game plate; ours has no button at the end (the kernel offers none).
- "There were 0" (the kernel's tally words, reported before).
- `dev.harness "6 bid"` leaves the lobby (the harness's round of raises fails at six seats); `6`, `6 call` and `4 bid` work.
- The tall list re-lays the board at the call (the cups move from the ring into rows), as the short board's row already did; the dice lie at their stations, not where the throw left them.
- The plank's end may now sit just above the shelf (the least move); a calmer place would be mid-way between, a design call.
