# The kernel, and a coder for it

```
make -C uttt/c run        1000 games, encoded, decoded, checked
make -C uttt/c asan       the same under ASan + UBSan
./uttt/c/build/uttt_test 50000 12345     more games, different seed

make -C uttt/c render     the board, to build/board.png, without Xcode
./uttt/c/build/uttt_render rulebook 54   just the door, blown up
make -C uttt/c rough-diff the pen, held against rough.js itself (needs node)
make -C uttt/c ios-smoke  every entry point Swift calls, without a Mac
make -C uttt/c analyse CODE=<code or link>   every ply held against quill
```

`uttt.c` is the rules and nothing else. `uttt_code.c` turns a
played game into bytes and back. `uttt_test.c` plays games with two bots,
round-trips every one, and reports what it cost.

The other half of the kernel is the DRAWING, which is here rather than in a
renderer because two phones looking at one bubble have to produce the same
sheet stroke for stroke - `uttt_pen.h` argues that at length.
`uttt_pen.c` is rough.js transcribed into C, `uttt_draw.c` is the board and
`uttt_rule.c` is the rulebook door, and all three emit filled polygons in a
unit square that a renderer only has to fill.
`rough_diff.mjs` is what keeps the transcription honest: it runs the vendored
rough.js the design document is drawn with and compares it sample by sample
with what the C emits, which is the only check that can see a drift that still
looks like a drawing.

## The answer to "how long can it get"

**81 plies, and it fits in 22 bytes.** That is every cell on the board filled,
which needs all nine blocks to end in a draw and none of them to be won on the
way - a block is closed the moment it is won, and its empty cells are then
frozen for the rest of the game.

The `stretch` bot finds it by refusing to close a block whenever it has another
option. The `uniform` bot, picking at random, never gets past about 78.

**The longest game is not the most expensive one.** 81 plies costs 22 bytes; the
priciest game in 100,000 was 25. Closing blocks early makes a game *shorter* and
*dearer* at once, because a closed block means the next player is sent nowhere
and gets a free choice over the whole sheet - a 40-way decision instead of a
9-way one. Length is cheap. Freedom is expensive.

## Measured, over 100,000 games

| | plies mean | p95 | max | bytes mean | p95 | max |
|---|---|---|---|---|---|---|
| uniform bot | 58.9 | 69 | 78 | 21.0 | 23 | 25 |
| stretch bot | 74.4 | 79 | **81** | 22.4 | 23 | 25 |

Ideal is 163.4 bits (20.4 bytes) for the uniform bot; the coder spends **0.57
bytes** more than that, which is the rounding up to a whole byte plus the
one-byte sentinel.

For scale: `log2(81!)` is 401 bits, 50 bytes - what an 81-move game would cost
if every move were a free choice over every empty cell. The forced-block rule
cuts that by more than half before anybody plays anything.

## How the coder works

The model is the rules. At every ply the kernel can enumerate the legal moves,
so the only thing worth storing is **which one was chosen** - an index into a
list both sides rebuild for themselves. Nine legal moves costs log2(9) = 3.17
bits. Two costs one. No frequency table, no probabilities to ship, nothing to
tune.

The code is one big number in a **mixed radix**, digit *k* in base *n_k*:

```
encode:  v = v * n + i      walked backwards
decode:  i = v % n;  v /= n  walked forwards
```

Backwards then forwards, so the decoder meets the digits in playing order -
which is the only order in which it can know *n*, since it has to replay the
game to find out.

## It was rANS first, and that is worth recording

rANS renormalises against `x_max = (L / n) << 8`, which is exact **only when n
divides L**. That is true for the power-of-two alphabets every reference
implementation uses, and false here, where *n* is the number of legal moves and
runs 1 to 81.

With `L = 2^16` and `n = 81`, `floor(L/n) * n` is **65529 - seven below the
floor the decoder renormalises against.** The encoder could leave the state just
under the window and the two sides would silently walk apart. It happened about
**twice in every eight thousand games**, which is exactly the rate that passes a
thousand-game test and fails in somebody's thread six months later.

Rounding the bound up fixed the floor and broke the ceiling instead: `ceil(L/n)
* n` can exceed `256L`, so the state no longer fit the flush.

The bignum has neither edge, is about fifteen lines, and is **two bytes
smaller** because it has no state to flush at all.

## The bots

```
make -C uttt/c ladder                          the seven named tiers, round robin, Elo
./uttt/c/build/uttt_ladder 200 8 quill@4000 fountain@5800     any entrants, head to head
LADDER_SEED=7 ./uttt/c/build/uttt_ladder ...   the same on a fresh seed stream
```

`uttt_bots.h` is the long version: what each engine is, and every measurement that shaped it.
The ladder prints each entrant's CPU time a move next to its Elo, because "the same budget" is a claim about time and not about the number after the `@`.
`make run` and `make asan` include `uttt_bots_test`: legal and deterministic from the seed, a side only ever re-roots its own tree, and fountain never loses to uniform play.

### quill never saturated - the ladder was sharing its tree

The tier table said quill@10 scored 42% against quill@4000, 400 times the thinking for 58%.
quill keeps its tree between moves, and the tree belonged to whoever searched last: quill@10 re-rooted the tree quill@4000 had just built and played its most visited move.
With a kept tree per side, quill@4000 takes 100 of 100 off quill@10.

### fountain, the experiment log

Every row is fountain against quill@4000, both colours on shared seeds, score for fountain and the Elo that score implies.
At 200 games the standard error is about 3.5 points (25 Elo), at 300 about 2.8, at 400 about 2.4, so read the direction and not the digit.
The stream is `LADDER_SEED`; the first stream's base reading turned out to be its lucky side, which is why later work moved to fresh streams.

| # | change | stream, games | score | Elo | ms/move (quill ~48) | kept |
|---|---|---|---|---|---|---|
| 0 | quill's tree copied, as a control | 0, 100 | 50% | 0 | 47 | - |
| 1 | playouts run to the end instead of 12 plies + `leaf_eval` (still biased) | 0, 200 | 56% | +45 | 93 | no, twice the time |
| 2 | ...and uniform, taking a game-winning move when there is one | 0, 200 | 64% | +101 | 47 | yes |
| 3 | 2 + take an offered block half the time | 0, 200 | 52% | +16 | 43 | no |
| 4 | 12-ply cutoff with the light policy | 0, 200 | 47% | -21 | 28 | no |
| 5 | node pool 250k -> 1M (on 2), 2M (on quill's leaf) | 0, 200 | 64%, 52% | +103, +12 | 48 | no, the pool is not the limit |
| 6 | exploration C 0.3 / 0.8 (quill's leaf), 0.4 / 0.7 (on 2) | 0, 200 | 44 / 38, 54 / 52% | | 47 | no, 0.5 stays |
| 7 | prior weight 0 / 3 / 6 / 10 (quill's is 1.5) | 0, 200 | 54 / 66 / 60 / 59% | +24 / +115 / +74 / +61 | 49 | 3 |
| 8 | FPU 0.45 / 0.7 | 0, 200 | 59 / 56% | | 49 | no |
| 9 | contempt: a draw worth 90 / 75 of 200 to fountain | 0, 200 | 61 / 60% | | 49 | no |
| 10 | a fixed 12 x budget iterations a move instead of budget x moves | 0, 200 | 64% | +96 | 74 | no |
| 11 | 2 + prior 3, re-read on a fresh stream | 7, 200 | 56% | +43 | 49 | the true base |
| 12 | implicit minimax backups of `leaf_eval`, alpha 0.2 / 0.4 | 7, 200 | 56 / 53% | +43 / +21 | 53 | no |
| 13 | light playouts cut at 24 / 12 plies, budget 6000 | 7, 200 | 59 / 51% | +63 / +5 | 57 / 45 | no |
| 14 | re-draw a random move that gives away the game | 7, 200 | 60% | +72 | 53 | yes, once it was cheap (19) |
| 15 | ...or any block, 2 / 6 re-draws | 7, 200 | 55 / 57% | | 74 / 105 | no |
| 16 | last good reply with forgetting (LGRF-1), always / half the time | 7, 200 | 60 / 60% | +72 / +72 | 41 / 43 | yes |
| 17 | tournament of 2 / 3 random moves by `score_move` | 7, 200 | 59 / 61% | +63 / +77 | 66 / 83 | no, the time |
| 18 | the recipe at budget 4000 / 8000 / 16000 | 7, 200 | 60 / 70 / 79% | +72 / +148 / +229 | 47 / 94 / 178 | about +75 a doubling: speed is the lever |
| 19 | speed: single-precision selection, proofs stop climbing when nothing changed | 7, 200 | 56% | +45 | 45 -> 41.5 | yes |
| 20 | speed: the playout in registers, threat masks, no divide in the RNG | 7, 200 | 60% | +72 | 47 -> 35 | yes |
| 21 | 20 at budget 5400 (equal time) | 11, 300 | 67% | +124 | 48.3 | yes |
| 22 | + any-block gift re-draws (2 / 6), + block preference 20% | 11, 300 | 67 / 60 / 68% | | 54 / 65 / 48 | no, level or slower |
| 23 | prior 3 / 2 / 4.5, C 0.4 / 0.6, at 5400 | 13, 300 | 65 / 69 / 70 / 62 / 68% | | 47 | flat; 3 and 0.5 stay |
| 24 | the clean rewrite (shipped code) at 5400 | 21, 400 | 66% | +112 | 45.9 | - |
| 25 | at 5800 (equal time) / prior decay 1/sqrt / FPU from the parent -0.1, -0.25 | 31, 300 | 68 / 69 / 68 / 68% | +127 / +140 / +132 / +130 | 49 | 5800; the rest level |
| 26 | allowance counts at least 18 legal moves, budget 2600 (vs 5800 plain, same games) | 41, 400 | 72% vs 68% | +160 vs +135 | 51.3 / 49.3 | yes |
| 27 | the shipped fountain@2500, confirmation | 51, 400 | 69% | +137 | 49.1 vs 49.1 | - |

Pooled over the two streams of the shipped recipe, 800 games: about 70.5%, +150 Elo, at the same CPU time a move as quill@4000.

What made it stronger, in order of size: playing the playout to the end (quill's twelve-ply cut with `leaf_eval` was a win at forty rollouts and a ceiling at four thousand), making every playout and every descent cheaper (at a fixed recipe each doubling of budget was worth about +75 Elo, and the speed work bought about 1.4x), a floor under the allowance so forced blocks get thought about, and a doubled prior weight.
Knowledge that cost time never paid for it: the tournament policy, small-block gift avoidance, implicit minimax and a cut-off leaf all read level or lost once their price was counted.
Transpositions were measured and are not worth a DAG here: under 1% of tree nodes with two visits or more repeat a position before the last few plies.
