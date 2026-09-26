# Store shots: one game

Every App Store frame for Ultimate Tic-Tac-Toe shows a phase of ONE real, reachable game (owner, 2026-09-26).
A frame shows the board after one ply of this game, and nothing else.

- Replay code: `AAAAATPW4GYDOIS33TKXTB7YI5CJZHMJ3GBISAI`
- Link: <https://uttt.live/AAAAATPW4GYDOIS33TKXTB7YI5CJZHMJ3GBISAI>
- Drawing seed 77, the DEBUG seeded game's (`UtttDev.seed`), so the replay draws the same napkin as the frames.
- Result: O wins in 50 plies, on the big board's anti-diagonal (top-right, centre, bottom-left boards). The owner asked for O's diagonal ("composes better").
- We are X in every frame but the win, which is shot from O's seat so it reads "You win"; the seat whose frame it is has its messages on the right and sits in the drawer.

## How it was made, and how to check it

`make -C uttt/c store-game` (`tools/uttt_store_game.c`) plays the rig's 18-ply `devgame` opening, then fountain for both seats: X on 5 rollouts, O on 2000, seed 44.
It puts every ply through `uttt_play`, replays the whole list from scratch through the rules again, reads the link back with `uttt_replay_read` to compare, and exits 1 unless O wins on a diagonal.
It prints the result, the link and the ply list below.

An earlier store game (X winning the main diagonal at ply 49) could not be bent into an O diagonal: X took the centre board at its ply 25, and both diagonals run through the centre.

On the simulator a frame's history is the rig's `devgame` with a prefix of the ply list, for example the first 34 plies:

```
rig.sh devgame 34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71,73,15,60,59,47,24,57,30,33,56,23,46,11,21,32,48
```

and the plies after the prefix are played on the simulator, each as a real message from alternating threads.

| Frames | `devgame` prefix | Played on the simulator |
|---|---|---|
| 01, 04 | 32 plies | 33-36 as real messages, then 37 sent (01) or, after a re-sent copy of 36, staged (04) |
| 03, 06 | 41 plies | 42 by O from the + menu; X opens it |
| 05 | 46 plies | 47-50, X from the other thread, O from the photographed one |
| 02 | none | O's invitation from the + menu with `dev.invite`; X opens it |

Ply 37 and not the more dramatic 39 for the Send frames: 39 wins X the middle-left board and sends O to a won board, so the whole napkin lights up as "play anywhere" and buries the marks.
The collapsed drawer's height follows the last keyboard the simulator showed, so it is not a constant: every tap on the collapsed board is placed from the board's own main lines in a fresh screenshot, and each dark / light pair is shot by the same script so its drawer sits at the same height.

## Frames

Six scenes, each shot in BOTH appearances, so a frame can move to any slot and take that slot's theme.
The listing alternates like foolish's (docs/appstore/screenshots/README.md): odd slots dark, even slots light.
One title size across the set, 124 px Futura Bold, every title two lines with a manual break.

| # | Title | Scene | Ply shown | Seat | Listing theme | Ground |
|---|---|---|---|---|---|---|
| 01 | "Play Ultimate / Tic-Tac-Toe" | The transcript with our SENT move on the right, the collapsed drawer under it | 37 (X, top-right board, top-left square), sent | X | dark | O red |
| 02 | "Nine boards, / one big game" | The empty napkin: O's invitation opened by X, expanded | 0 | X | light | X blue |
| 03 | "Your move picks / their board" | Expanded board, X to play in the top-middle board O's square sent them to | 42 (O) received | X | dark | coal |
| 04 | "Send your moves / to the chat" | The staged draft in the compose field, collapsed drawer | 37 (X), staged and not sent; O is sent to the top-left board | X | light | napkin |
| 05 | "Win three boards / in a row" | The transcript with O's winning move sent, the drawer reading "You win" | 50 (O wins the anti-diagonal), sent | O | dark | O red |
| 06 | "Learn the rules / in a minute" | The rules page, opened from the rulebook and not scrolled | over ply 42 | X | light | X blue |

The grounds are the napkin's own colours, taken from the kernel's draw code (`uttt/c/src/uttt_draw.c`), plus foolish's coal:

| Ground | Top | Bottom | Title | From |
|---|---|---|---|---|
| O red | (168,50,31) | (112,33,20) | white | `INK_O` 0xa8321f |
| X blue | (37,55,107) | (22,33,66) | white | `INK_X` 0x25376b |
| coal | (24,20,19) | (12,10,9) | white | foolish's `coal` |
| napkin | (249,248,244) | (242,241,237) | X blue | `uttt_paper`, .976 at the top to .948 at the bottom |

They cycle O red / X blue / coal / napkin, a four-cycle against the two-cycle of the theme, which also puts O's win on O's red.
`python3 uttt/ios/Tools/store_frames.py` composes the set, and a contact sheet, with `shared/tools/store/market.py`.

## Transcript frames on the simulator

The simulator draws each superseded line of a session with a neighbour's summary (rig README, iOS 26 and 27 notes): a collapsed line shows the summary of the NEXT message in the session.
Each move after the first is therefore staged with the DEBUG-only `dev.caption` set to the line the move BEFORE it has to read; the bubble's own caption stays the kernel's.
The "Send" frame also re-sends ply 36's board unchanged (`dev.restage`) to carry the last line, then stages ply 37 in the same session; that extra copy is scrolled behind the draft, leaving texts, then our line, then theirs.
The empty frame's invitation is a real one from the + menu, opened with the store game's seed by `dev.invite`.

## Plies

Moves are `block*9 + square`, both numbered row by row from the top left.

```
34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71,73,15,60,59,47,24,57,30,33,56,23,46,11,21,32,48,29,20,18,3,35,38,19,10,17,42,14,50,45,5,49,22
```

| Ply | Mark | Move | Board | Square |
|---|---|---|---|---|
| 1 | X | 34 | middle-left | bottom-middle |
| 2 | O | 67 | bottom-middle | centre |
| 3 | X | 44 | centre | bottom-right |
| 4 | O | 80 | bottom-right | bottom-right |
| 5 | X | 76 | bottom-right | centre |
| 6 | O | 43 | centre | bottom-middle |
| 7 | X | 69 | bottom-middle | bottom-left |
| 8 | O | 62 | bottom-left | bottom-right |
| 9 | X | 79 | bottom-right | bottom-middle |
| 10 | O | 63 | bottom-middle | top-left |
| 11 | X | 4 | top-left | centre |
| 12 | O | 40 | centre | centre |
| 13 | X | 39 | centre | middle-left |
| 14 | O | 31 | middle-left | centre |
| 15 | X | 37 | centre | top-middle |
| 16 | O | 16 | top-middle | bottom-middle |
| 17 | X | 70 | bottom-middle | bottom-middle |
| 18 | O | 71 | bottom-middle | bottom-right |
| 19 | X | 73 | bottom-right | top-middle |
| 20 | O | 15 | top-middle | bottom-left |
| 21 | X | 60 | bottom-left | bottom-left |
| 22 | O | 59 | bottom-left | middle-right |
| 23 | X | 47 | middle-right | top-right |
| 24 | O | 24 | top-right | bottom-left |
| 25 | X | 57 | bottom-left | middle-left |
| 26 | O | 30 | middle-left | middle-left |
| 27 | X | 33 | middle-left | bottom-left |
| 28 | O | 56 | bottom-left | top-right |
| 29 | X | 23 | top-right | middle-right |
| 30 | O | 46 | middle-right | top-middle |
| 31 | X | 11 | top-middle | top-right |
| 32 | O | 21 | top-right | middle-left |
| 33 | X | 32 | middle-left | middle-right |
| 34 | O | 48 | middle-right | middle-left |
| 35 | X | 29 | middle-left | top-right |
| 36 | O | 20 | top-right | top-right |
| 37 | X | 18 | top-right | top-left |
| 38 | O | 3 | top-left | middle-left |
| 39 | X | 35 | middle-left | bottom-right |
| 40 | O | 38 | centre | top-right |
| 41 | X | 19 | top-right | top-middle |
| 42 | O | 10 | top-middle | top-middle |
| 43 | X | 17 | top-middle | bottom-right |
| 44 | O | 42 | centre | bottom-left |
| 45 | X | 14 | top-middle | middle-right |
| 46 | O | 50 | middle-right | middle-right |
| 47 | X | 45 | middle-right | top-left |
| 48 | O | 5 | top-left | middle-right |
| 49 | X | 49 | middle-right | centre |
| 50 | O | 22 | top-right | centre |
