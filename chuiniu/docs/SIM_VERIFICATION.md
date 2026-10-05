# Chui Niu - simulator verification

The record of playing Chui Niu inside Apple's Messages on a simulator, with the screenshot each step left in `chuiniu/docs/shots/` (the `tie_*` files; the `fake_*` ones are the scaffold's, from before the kernel was wired in).

## The setup

- 2026-09-27, iPhone 17e simulator `6E0A730D`, iOS 27.0, light appearance, the Debug build of `ChuiniuMessagesApp` installed with `foolish/ios/Tools/rig/rig.sh build` (`source chuiniu/ios/Tools/rig.env` first).
- One simulator plays both people in one thread (John Appleseed, 888): `rig.sh seat Alex` or `rig.sh seat Bo` writes `dev.seat`, and the Debug extension is then that person, with identity, nickname and seat records of its own (DECISIONS I15).
  Between two sides the rig kills the extension (`rig.sh killappex`), switches the person and taps the newest bubble, as a second phone would.
- Our board is not in the accessibility tree, so taps on it were placed from screenshots; Messages' own Send and `+` came from the rig.

## One round and one call, step by step

Each step is what was done, what was seen, and the shot.

1. Alex opens the extension from the `+` menu in an empty thread: the lobby ("1. Alex (You)", "Waiting for players") and the invitation staged in the input field, captioned "Alex wants a game of Chui Niu. Tap to join", its picture naming "Alex" with no "(You)". `tie_01_lobby_invite_staged.png`
2. Alex sends it; the drawer closes, as a drawer opened from `+` does. `tie_02_invite_sent.png`
3. Bo taps the invitation: the lobby opens expanded with the name field already holding "Bo" and Join lit. `tie_03_bo_offered_join.png`
4. Bo presses Join: in a DM the join fills the table and starts it (the kernel's join-and-start); Bo's table shows Alex's cup with 5 and Bo's own five dice, "Alex's turn". `tie_04_bo_joined_expanded.png`
5. Collapsed, the start bubble "Dice rolled. Alex bids first" is staged in the same session, and the invitation above it has become a caption line. `tie_05_start_staged.png`
6. Bo sends; Alex taps the start: Alex's own five dice, "Your turn: open the bidding", the picker on one 2 with Call dimmed (no call on nothing). `tie_06_alex_opening_menu.png`
7. Alex picks two 4s and presses Raise: after the kernel's staged beats the drawer collapses by itself and the bubble goes in, its picture showing "two 4s" and its caption "Alex bid two 4s"; the table under it still shows no bid (a staged move never changes the committed table) and the headline says "Send to bid two 4s". `tie_07_first_raise_staged.png`
8. Sent, as a bubble in the transcript. `tie_08_first_raise_sent.png`
9. Bo taps it: the other side's table with Bo's own dice (the same five as in step 4), "two 4s" on the plate, "Your turn: raise or call", the picker on the least raise (two 5s) and Call lit. `tie_09_bo_table_own_dice.png`
10. Bo raises to three 5s and it stages. `tie_10_second_raise_staged.png`
11. Alex taps it: "three 5s", raise or call. `tie_11_alex_sees_raise.png`
12. Alex presses Call: the bubble says "Alex calls three 5s" and nothing is revealed on Alex's own screen while it is staged (K8): same table, "Send to call three 5s". `tie_12_call_staged.png`
13. Alex sends: on Alex's still-open drawer the cups lift and the counting dice light one by one (`tie_13a_reveal_motion_cups_up.png`, taken mid-plan), then the settled reveal: "There were five", both 1s of Alex's and Bo's three 5s ringed, the rest dimmed, "Loses a die" on Alex, the outcome line "Alex calls. Three 5s was true, Alex loses a die", "Your turn: open the bidding" and Next round. `tie_13_call_sent_reveal_caller.png`
14. Bo taps the call bubble: the same reveal on Bo's side, played from cups down (`tie_14a_bo_reveal_motion_sheet.png`, eight frames 0.4s apart: cups down, cups up, the count, settled) to the outcome line and "Alex's turn". `tie_14_bo_sees_reveal.png`
15. Alex taps the call bubble and presses Next round: the next round's table, opened by the loser, Alex on four dice (1 3 4 5) with the opening menu, Bo's cup on 5. `tie_15_next_round_loser_opens.png`

## A whole game to a winner

A second game was played the same way to the end by a driver loop (the opener bids the picker's opening one 2, the other side calls, the loser opens the next round with Next round), eight rounds and 18 bubbles, all inside Messages.
Every sent link was decoded outside the simulator: the Debug extension writes it to the App Group as `dev.sent`, and `chuiniu/c/tests/cn_link_dump.c` (`make -C chuiniu/c build/cn_link_dump`) prints every seat's dice through the bridge's tests-only `CN_API_ALL` view, the bid, the call and the outcome line.
The whole log is `chuiniu/docs/SIM_GAME_LOG.txt`.

- It ended "Alex calls. One 2 was false, Bo loses a die. Alex wins", seat 0 the winner; the last screen shows "There were 0", "Loses a die" on Bo, that outcome line and "You win" for Alex. `tie_17_winner.png`
- THE DICE ON SCREEN ARE THE KERNEL'S. For each of the eight rounds, the opener's own dice (shot after Next round) and the caller's own dice (shot as the bid arrived) were set against the dump of the same link: all 16 rows agree die for die, from round 1 (Alex 1 2 2 2 5, Bo 1 1 3 5 6) to round 8 (Bo 6, Alex 3 6). `tie_18_game_dice_every_round.png` is the 16 rows cropped from those screens in that order, and `tie_16_last_round_alex_dice.png` is one of them whole.
- In the first game (steps 1 to 15) the dice were read off the screens only (Alex 1 1 2 4 6 and Bo 2 2 5 5 5, then Alex 1 3 4 5), since `dev.sent` did not exist yet; the reveal of step 13 shows both hands, and its count and loser agree with R2 and R3 by hand (two 1s and three 5s make five, at least three, so the caller lost).

## What was seen wrong, and fixed

- The compact drawer drew the other seat's cup over the bid plate, with its name under the grab handle: the table's ellipse needs about 280pt and the compact drawer leaves about 150pt above the picker. The table now has a short shape for that (DECISIONS I17), and its test measures short boards.
- The bubble picture of a lobby said "1. Alex (You)" to every phone; it now says the bare name (I18).
- On one opening of a call bubble the reveal stayed on its last animated frame, with no tally, outcome line or Next round, because a paused `TimelineView` keeps the frame it last drew; the settled reveal is now drawn outside the timeline once the kernel says done (the whole game above ran after that fix).

- In the compact drawer the reveal's names came out at 70% of their size (the loser row's stamp squeezed the row and the name's minimum scale gave way); the name no longer scales and the reveal is spaced to fit the compact drawer. `tie_19_reveal_names_fixed.png`, one more round played after the fix; the earlier reveal shots are from before it.

## The kernel's bubble, six seats and memory (package E2, 2026-10-05)

A private iPhone 17e simulator `C6058B4C` (iOS 27.0, 390 by 844 points, light), the Debug build installed with `rig.sh build`, a two-line session in the 888 thread, `dev.nick` Alex.

- `dev.fill` 6, then `rig.sh open`: one opening made a group lobby, sat Bo, Cy, Di, Ed and Fay, started it, and staged the start bubble.
  The bubble is the stage's own picture: six verdigris cups in a row with 5 on each crown, the bare names under them in small caps, no plate (no bid yet), caption "Dice rolled. Alex bids first". `e2_01_six_seats_start_bubble.png`
- Raise on one 2: after the staged beats the drawer collapsed and the bubble went in with the plate "one 2" and a 2 die, Alex's name lit and the others dim, caption "Alex bid one 2" (I18: the staged bid, bare names). `e2_02_raise_bubble_staged.png`, cropped `e2_03_bubble_crop.png`
- The picture is made at 2 pixels a point (600 by 390) on this 3x screen; the stage draws a still at 2 at most.

### Memory

`footprint` and `vmmap --summary` on the extension process, six seats.
The live table here is still the SwiftUI one, so the arena is taken only while a bubble is drawn.

| when | footprint | peak |
|---|---|---|
| compact (328 pt), after the start bubble | 37 MB | 55 MB |
| expanded (797 pt) | 37 MB | 55 MB |
| compact, after the raise's bubble | 38 MB | 59 MB |

The peak is the bubble's moment: the arena's touched pages, about 12 MB of textures and the 600 by 390 frame, then given back.
Nothing is near 120 MB.
Once the live table draws through the stage, a 2x frame of the tall drawer is up to 36 MB of the arena on top of the textures, plus each frame's copy out, so expect 90 to 100 MB there; that has to be measured again then.

### The drawer is measured

The Debug log prints the extension's bounds on every layout beside the drawer the stage was begun with.
Collapsed it was 390 by 328 and expanded 390 by 797, and the expand laid the view out at every height in between (348, 358, ... 669), so the hosting view follows the drawer and a screen reading its own size sees each one.
"stage none" on every line: no screen begins the table stage yet; when one does, the line shows whether it took the measured size.

### Seen wrong and left

- The cups' shadows in the bubble are stair-stepped, blocks of about two points (`e2_04_bubble_shadow_zoom.png`, nearest-neighbour zoom); the cups' own edges are clean. The shadow map's sampling is the renderer's (`cn_scene.c`), so it is reported to the kernel, not changed here.
- The felt under the bubble is the felt of the old table; the study's bubble sits on the grey plank table, which is not drawn anywhere yet.

## Not seen, or seen wrong and left

- "There were 0" and "There were one": the kernel's `REVEAL_COUNT` has one form for every count, so none reads as a digit and one reads as a plural; the words are the kernel's (K11, `cn_say.c`), so it is reported, not changed here.
- The caption line above the newest bubble reads "Dice rolled. Alex bids first" for every move: the simulator's own corrupted summaries of a short session (rig README point 12), not the product.
- A tapped bubble opens the drawer expanded; the collapse after a raise and the lobby's insert were seen, but a real phone was not used, and nothing was tried in dark appearance, at three or more seats, or with a typed nickname on a fresh device.
- The roll of a seat's own new dice runs on `RollBeats`, not the kernel's frame (I10); it was seen to play and settle, not measured.
