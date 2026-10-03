# The 243 board's send rule: an open question (2026-10-03)

The 243 x 243 game in iMessage plays rule (A) below.
The owner expected rule (B).
The two cannot be told apart in the 9 x 9 game, which is why the question only appeared at depth 5.
Nothing is decided; this file is the debate, so it is not re-argued from scratch.

Since 2026-10-03 the kernel (`uttt/c/src/uttt_big.{c,h}`) carries all three rules below, (A), (B') and (B), as a property of a game (`UtbGame.rule`: `UTB_RULE_SHIFT`, `UTB_RULE_CLIMB`, `UTB_RULE_CLIMB_FREE`), and the arena at uttt.live/243 lets a visitor pick one ("Send" in its header; `&send=` in its replay link).
The iMessage app still plays (A): `utb_init` and `utb_adopt` give (A), and the codec, the wire and the picture are unchanged.
The first simulation of all three is at the end of this file.

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
As built, a completed unit with no level above it (k = 1: an 81 x 81 at depth 5, a 3 x 3 at depth 2) leaves the cell's ordinary send standing.
At depth 5 that target lies inside the decided 81 x 81 and relaxes to anywhere; at depth 2 it is the 9 x 9 game's own send, so all three rules are the 9 x 9 game there, which `uttt/c/tests/uttt_big_test.c` holds over 2,000 random games.

(B) has two sub-variants.
After a climb the opponent is either free inside the target block (**B**: the 9 x 9 `d1 d2 d4` after a 3 x 3, the 27 x 27 `d1 d3` after a 9 x 9, the 81 x 81 `d2` after a 27 x 27; up to 81, 729 or 6,561 choices) or still held to one 3 x 3 inside it (**B'**, 9 choices; the formula above).
Both are built.

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
   The kernel half of this is done (the rule is a field of the game); the codec half is not: a bubble carries no rule and is always (A).

## The first simulation (2026-10-03)

Whole depth-5 games under each rule, by uniform random play (20 games a rule) and by the arena's two greedy bots at the page's defaults (6 plies, budget 150,000; 4 games a rule), natively on this Mac.
`make -C uttt/c big-rules-sim ARGS="<rule 0|1|2> <random games> <bot games> [seed]"` (`uttt/c/tests/uttt_big_rules_sim.c`), seed 20261003.
"Decided" counts a grid that became won or drawn in its own right (one closed only because a grid above it was decided is not counted), averaged over the games; "jumps" is the share of plies whose 81 x 81 is not the previous ply's.

| | plies a game | X / O / drawn | 3 x 3 decided (won) | 9 x 9 | 27 x 27 | 81 x 81 | jumps |
|---|---|---|---|---|---|---|---|
| (A) random | 40,302 | 5 / 4 / 11 | 4,507 (4,379) | 529 (481) | 64.0 (50.8) | 8.0 (5.2) | 88.6% |
| (B') random | 42,225 | 0 / 2 / 18 | 5,132 (4,894) | 585 (485) | 71.1 (45.8) | 8.7 (3.0) | 0.1% |
| (B) random | 42,486 | 1 / 1 / 18 | 5,220 (4,977) | 604 (497) | 72.8 (45.2) | 8.7 (2.9) | 0.2% |
| (A) bots | 26,380 | 4 / 0 / 0 | 2,471 (2,470) | 425 (424) | 50.5 (49.0) | 7.0 (6.8) | 88.8% |
| (B') bots | 36,535 | 1 / 1 / 2 | 4,473 (4,368) | 495 (450) | 62.0 (50.0) | 8.2 (5.5) | 0.2% |
| (B) bots | 32,752 | 0 / 1 / 3 | 4,022 (3,962) | 490 (465) | 63.0 (54.8) | 8.2 (5.0) | 0.2% |

The bots' games took about 38 s each under (A), 57 s under (B') and 100 s under (B) (a completion under (B) hands the searcher a whole block, so more of its moves are the wide, capped kind).

What this suggests, and no more:

- LOCALITY IS REAL. Under both climbs play leaves its 81 x 81 on about 1 ply in 500 against 9 in 10 under (A); the arena's screenshots show the same thing (one corner of the board worked to completion while the rest stays empty, against an even scatter).
- NEITHER CLIMB SHORTENS A GAME. Every rule runs to roughly the whole board: random play fills about 40,000 to 42,500 cells under all three, and the bots took more plies under the climbs (33,000 to 36,500) than under (A) (26,000). The debate's estimate that (B) games "finish" in a few thousand plies is wrong for these players: winning the root still takes winning three 81 x 81s in a line, and the climbs do not make that cheaper.
- DRAWS. Random play draws most games under every rule, more under the climbs (18 of 20) than under (A) (11 of 20). The bots never drew under (A) in four games (X won all four) and drew 2 and 3 of 4 under (B') and (B). Four games is far too few to read a rate; the direction (more draws with locality) agrees with random play.
- UNDER (A) THE BOTS ALMOST NEVER DRAW A GRID (2,470 of 2,471 decided 3 x 3s won), and under the climbs more grids are drawn at every size: a local fight fills a grid before either side can line it up more often than a scattered one.
- Under (A) X won all four bot games; whether the first player's edge is structural under (A) is not known from four games.

NOT measured: humans; any bot but this one (it was tuned under (A)); a game-length or draw rate with more than 4 bot games a rule; whether (B)'s free choice is better or worse play than (B')'s 9-cell one.
