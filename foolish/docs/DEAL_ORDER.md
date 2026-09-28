# The deal order, and the format break it caused

*Round 16. The kernel dealt the end-of-bout refill in the wrong order, in two
separate ways, from the beginning. Fixing it changed every game the engine
plays, which retired every replay code cut before it.*

## The rule

When a bout ends - covered and everyone said good, or the defender picked up,
or a clean cover emptied the defender's hand - everybody draws back up to six,
and the order is:

1. the bout's **first attacker**,
2. then the rest of the table **clockwise, skipping the defender**,
3. then the **defender, last**.

Always.
Deep stock or shallow, empty defender hand or full, two players or eight.
The face-up trump under the talon is therefore the last card dealt in the game.

This is the standard rule and not a house variant.
[pagat.com](https://www.pagat.com/beating/podkidnoy_durak.html): "the attacker
replenishes first, then the other players who joined in the attack, in
clockwise order, and finally the defender."
The owner's family plays it that way, which is how the bug was found.
So does this app's own help screen, in all three languages
(`ios.rules.round.b`): "Players then draw starting from the first attacker,
going clockwise through all attackers... Then the defender draws."
The rules text shipped correct and the engine did not.

## What was wrong

Two things, in `game.c refill_player_hands`:

**The defender used to draw FIRST when their hand was empty.**
A special case at the top of the refill, guarded on `hand_count == 0`, which is
the state a clean cover leaves the defender in.
Exactly backwards, and it is the case that matters most: the defender who ends
a bout with their last card is the one racing to go out, and this handed them
the top of the talon.

**Otherwise the defender drew in their natural rotation slot.**
The walk simply started at `first_attacker` and went round, taking the defender
wherever they happened to fall.
At two players that is indistinguishable from the correct order, which is
probably why it survived; at three or more it is wrong every time.

Neither showed up in play, because the wrong order still deals a perfectly
legal game.
It only shows up when the talon runs short, and then only as "why did *I* get
nothing?".

## Why nothing caught it

Every test in the tree checked that the pieces agreed with each other, and they
did: the engine, the replay model and the bitboard rollout all dealt in the same
wrong order, so every round-trip, difftest and parity check stayed green.
A rule nobody else implements cannot be caught by cross-checking implementations
of it.
It needs a test that names the expected order and reads the cards, which is what
`tests.c test_deal_order` now is: four cases, and each one fails against the old
code.

## The format break

A replay code is a game, not a document.
The same bytes under the fixed rules deal different cards to different seats, so
every code cut before the fix describes a game this kernel would no longer play
- and a v6-family code is replayed by *rebuilding the deal and running the real
engine over it* (`replay_steps.c` pass 2, and every in-flight iMessage chain),
so the divergence is not theoretical.

Both format lines were therefore renumbered together (`replay.h`):

| line | was | is | carries |
|---|---|---|---|
| retrodiction (public DRAW logs, hands recovered once the fool is known) | 5 | **9** | unchanged wire |
| inline reveals (hidden-state-lossless, mid-game cut, pass-mode bit, forced-opening bit) | 6, 7, 8 | **10** | unchanged wire |

The wires did not change.
The numbers exist so that a pre-fix code is **rejected loudly** -
`REPLAY_EVERSION`, with the version in the error detail - instead of quietly
decoding into a game that never happened.

Because the numbers no longer sort by family, the "is this the inline-reveal
line" test is an explicit membership check (`fmt_inline_reveals`), never
`format >= 6`.

**There is no migration and there will not be one.**
An old code cannot be re-read, because re-reading it means dealing its cards,
and dealing its cards is the thing that was wrong.
What that costs, concretely:

- **Share links and saved replays from before the fix** stop opening.
- **iMessage games in flight across the update** stop replaying, the same cost
  the owner accepted when the FMSG header grew a send clock ("a build that only
  knows format 2 rejects a format-3 bubble outright").
- **The tutorial's frozen code** was re-cut, which is a one-line change and a
  supported operation: `npx tsx tests/gen_tutorial_game.ts`.

## Where it lives

Three implementations, and they must agree or the codec desyncs:

- `c/src/game.c` `refill_player_hands` - the rule itself.
- `c/src/replay.c` `refill` - the replay model. Order is load-bearing here in a
  way it is nowhere else: the inline-reveal line codes each drawn card at the
  moment it is dealt, so walking the table differently hands the right cards to
  the wrong seats and desyncs the arithmetic stream from that point on.
- `c/src/cordite_sim.c` `sim_refill` - the bitboard rollout the Monte-Carlo bots
  search with.

`tests.c test_deal_order` pins the first, the encode/decode round-trip
(`tests.c`, `replay_v6_test.c`) pins that the first two agree, and
`apply_difftest` pins the third against the first.
All three were mutation-checked by restoring the old order one file at a time.
