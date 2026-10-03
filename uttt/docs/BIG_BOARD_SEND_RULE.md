# The 243 board's send rule: an open question (2026-10-03)

The 243 x 243 game ships with rule (A) below.
The owner expected rule (B).
The two cannot be told apart in the 9 x 9 game, which is why the question only appeared at depth 5.
Nothing is decided; this file is the debate, so it is not re-argued from scratch.

## The two rules, precisely

A cell's address is five base-9 digits `d1 d2 d3 d4 d5`: which 81 x 81, which 27 x 27 inside it, which 9 x 9, which 3 x 3, which cell.

- **(A), as built** (`uttt/c/src/uttt_big.c`): the next forced 3 x 3 is `d2 d3 d4 d5`.
  Every digit shifts up one level, every move: your 27 x 27 becomes the opponent's 81 x 81, and so on.
- **(B), what the owner anticipated:** the next forced 3 x 3 is `d1 d2 d3 d5`, the standard rule, so play stays inside the 9 x 9.
  Only when a move COMPLETES a 3 x 3 does the send climb: the completed 3 x 3's position `d4` names the 9 x 9 in the same 27 x 27 (`d1 d2 d4 ...`).
  Completing a 9 x 9 sends to `d1 d3 ...`; completing a 27 x 27 sends to `d2 ...`.

They are one family.
Let k be the largest unit the move completes (k = 5 is the cell alone).
The send drops the digit above k and keeps the rest: k = 5 gives `d1 d2 d3 d5`, k = 4 gives `d1 d2 d4 d5`, k = 2 gives `d2 d3 d4 d5`, which is (A).
So (A) is (B) with the completion test deleted: every move is treated as if it had completed its whole 27 x 27.
At depth 2 a cell is the only thing a move can complete that has a level above it, so the 9 x 9 game is silent on the question.

(B) has two sub-variants.
After a climb the opponent is either free inside the target 9 x 9 (**B**, up to 81 choices) or still held to the 3 x 3 named by `d5` inside it (**B'**, 9 choices; the formula above).

## What exists in the wild

- The only written-down deeper rule found: a 2013 comment by Marc J on Ben Orlin's post, who played (3 x 3)^3 and (2 x 2)^4 with "playing in the sub-rectangle [x_1 ... x_n] after someone played in the square [x_0 ... x_n]".
  That is (A), the full shift.
  His verdict: "I do not recommend the 3 x 3^3 because it has 729 cases and it took 4+ hours to finish."
  (https://mathwithbaddrawings.com/2013/06/16/ultimate-tic-tac-toe/comment-page-1/)
- Arbitrary-depth implementations on GitHub mostly implement no send rule at all: ntgraff/infinite-tic-tac-toe lets the next player play anywhere; cobyj33/Recursive-Tic-Tac-Toe's interior `place()` is an empty stub; omermerm/Recursive-tic-tac-toe and tobast/rec-tictactoe describe only the depth-2 rule.
- A 27 x 27 "Mega Ultimate Tic-Tac-Toe" exists on itch.io (anugrah-davuri); its page refused the fetch, so its rule is unknown.

One precedent for (A), from one person who found it unplayable at depth 3.
No precedent for (B).
Nobody has published a depth-5 game under either rule.

## For (A)

- One rule, no cases: no completion test, no "largest completed unit", no sub-variant.
  Built, tested against the 9 x 9 kernel at depth 2, proven across two phones.
- Every ply is a 9-cell decision; no free 81- or 729-choice moves except after a relaxation, so the sender's choice always bites.
- The whole board is live from ply one; no region sits untouched.
- Real strategic depth of a strange kind: the forced location is a sliding window over the stream of cell digits, so the 9 x 9 played in four moves from now is decided by the next four digits, two of them yours.

## Against (A)

- The window is the problem: the hierarchy is cosmetic.
  The game is a string game in which each move marks the 5-gram of recent digits, and the grid of grids only draws 5-grams.
  Winning a 3 x 3 means revisiting the same 4-digit window three times against an opponent who controls half the digits.
  Random play took about 42,000 plies to decide a game, mostly draws; Marc J's four hours were at 729 cells and 243 x 243 has 81 times as many.
- Teleportation every ply: the camera jumps to a different 81 x 81 on every move; the pinch-to-zoom board and the rules fight each other; no local battle builds up.
- Nothing escalates: winning a 3 x 3 only closes one window; the levels above take no part in the sending.

## For (B)

- Locality: most of the time it is the 9 x 9 game people know, in one 9 x 9, camera parked, which fits a drawer-sized screen.
- The hierarchy means something: winning a 3 x 3 is a local gain and a send one level up; winning a 9 x 9 moves the theatre to another 27 x 27.
  At every level the nine siblings play Ultimate Tic-Tac-Toe against each other, driven by completions one level down.
- Games finish: 3 x 3s complete every 5 to 9 plies, 9 x 9s in roughly 40 to 60; winning needs 27 9 x 9s at the floor, about 1,350 plies, a few thousand realistically, with a reward every few moves.
- It costs nothing on the wire.
  The forced region is still derived from the board plus the last move: any decided unit containing the last move's cell was completed by that move, since play inside a decided unit is illegal.
  So (B) is a change to the region rule in `uttt_big.c` and its tests; the picture, the URL format, the codec, the UI and the flag stay.

## Against (B)

- More rule text: the completion test, "the largest unit completed wins", the climb, and a relaxation at each level (target 9 x 9 decided: relax to the 27 x 27, and so on).
- Camping: play can sit inside one 27 x 27 for hundreds of plies and the rest of the board is scenery, though that is how the 9 x 9 feels one level down.
- Pure (B) weakens the send: after winning a 3 x 3 the opponent gets a free 81-choice move in the target 9 x 9, which rewards the loser of the skirmish.
  (B') fixes this by keeping `d5`: the opponent lands in a specific 3 x 3 of the new 9 x 9, so every ply stays a 9-cell choice.
- Escalation timing becomes a weapon: the player completing a 3 x 3 chooses when the theatre moves; unexplored.
- Zero precedent, against one bad review of (A).

## Where this landed (the assistant's view, not a decision)

(B').
It keeps (A)'s real virtue, a 9-cell decision every ply, and adds locality and escalation.
It reduces to the 9 x 9 game at depth 2 and contains (A) as the special case "treat every move as completing everything", so nothing is thrown away by switching.

Before committing to anything:

1. Simulate both.
   The kernel already plays random depth-5 games (`uttt/c/tests/uttt_big_test.c`); a region-rule switch and a comparison of plies, draw rate, and how many 3 x 3s and 9 x 9s get decided is a small job.
2. Make it a per-game bit.
   The URL's flags byte has reserved bits (`uttt/c/src/uttt_big_msg.h`); a flag for "escalation rule" lets (A) games already in threads keep working while new games use (B'), with no transport change and no refusal of old bubbles.
