# Store shots: one game

Every App Store frame for Ultimate Tic-Tac-Toe shows a phase of ONE real, reachable game (owner, 2026-09-26).
A frame shows the board after one ply of this game, and nothing else.

- Replay code: `AAAAATPV4GTFLJSGT6H322GC7YR7STF6EZABI`
- Link: <https://uttt.live/AAAAATPV4GTFLJSGT6H322GC7YR7STF6EZABI>
- Drawing seed 77, the DEBUG seeded game's (`UtttDev.seed`), so the replay draws the same napkin as the frames.
- Result: X wins in 49 plies, on the diagonal (top-left, centre, bottom-right boards).
- We are X in every frame (captions on the right, the drawer seated at X).

## How it was made, and how to check it

`make -C uttt/c store-game` (`tools/uttt_store_game.c`) plays the first 25 plies, which are fixed:
the rig's 18-ply `devgame` opening and the seven moves the "Send your moves" frame was played with (ply 25 is that frame's staged draft).
It finishes the game with fountain for both seats (seed 1, 2000 rollouts), puts every ply through `uttt_play`, replays the whole list from scratch through the rules again, and reads the link back with `uttt_replay_read` to compare.
It prints the result, the link and the ply list below.

On the simulator the game's first 18 plies are the rig's `devgame` list:

```
rig.sh devgame 34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71
```

## Frames

| Frame | Title | Ply shown | Notes |
|---|---|---|---|
| Send | "Send your moves to the chat" | 25 (X, centre board, bottom-left square), staged and not sent | X wins the centre board. The transcript holds plies 19-24 in one session. |

The simulator draws each superseded line of a session with a neighbour's summary (rig README, iOS 26 and 27 notes).
The "Send" frame therefore sets the collapsed lines of plies 22 and 23 with the DEBUG-only `dev.caption`.
Only ply 24's line ("X to play", the kernel's own text) stays in frame; the lines above it are scrolled under the header.

## Plies

Moves are `block*9 + square`, both numbered row by row from the top left.

```
34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71,73,13,36,2,23,49,42,56,19,14,46,11,20,26,8,1,10,12,33,60,59,48,30,29,18,5,51,57,32,50,0
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
| 20 | O | 13 | top-middle | centre |
| 21 | X | 36 | centre | top-left |
| 22 | O | 2 | top-left | top-right |
| 23 | X | 23 | top-right | middle-right |
| 24 | O | 49 | middle-right | centre |
| 25 | X | 42 | centre | bottom-left |
| 26 | O | 56 | bottom-left | top-right |
| 27 | X | 19 | top-right | top-middle |
| 28 | O | 14 | top-middle | middle-right |
| 29 | X | 46 | middle-right | top-middle |
| 30 | O | 11 | top-middle | top-right |
| 31 | X | 20 | top-right | top-right |
| 32 | O | 26 | top-right | bottom-right |
| 33 | X | 8 | top-left | bottom-right |
| 34 | O | 1 | top-left | top-middle |
| 35 | X | 10 | top-middle | top-middle |
| 36 | O | 12 | top-middle | middle-left |
| 37 | X | 33 | middle-left | bottom-left |
| 38 | O | 60 | bottom-left | bottom-left |
| 39 | X | 59 | bottom-left | middle-right |
| 40 | O | 48 | middle-right | middle-left |
| 41 | X | 30 | middle-left | middle-left |
| 42 | O | 29 | middle-left | top-right |
| 43 | X | 18 | top-right | top-left |
| 44 | O | 5 | top-left | middle-right |
| 45 | X | 51 | middle-right | bottom-left |
| 46 | O | 57 | bottom-left | middle-left |
| 47 | X | 32 | middle-left | middle-right |
| 48 | O | 50 | middle-right | middle-right |
| 49 | X | 0 | top-left | top-left |
