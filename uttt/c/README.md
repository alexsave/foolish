# The kernel, and a coder for it

```
make -C uttt/c run        1000 games, encoded, decoded, checked
make -C uttt/c asan       the same under ASan + UBSan
./uttt/c/build/uttt_test 50000 12345     more games, different seed

make -C uttt/c render     the board, to build/board.png, without Xcode
./uttt/c/build/uttt_render rulebook 54   just the door, blown up
make -C uttt/c rough-diff the pen, held against rough.js itself (needs node)
make -C uttt/c ios-smoke  every entry point Swift calls, without a Mac
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
