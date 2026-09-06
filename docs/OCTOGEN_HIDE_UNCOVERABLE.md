# Octogen information-hiding rule: pick up instead of partial-covering

**Status: SHIPPED (always on).** When octogen is defending and *cannot cover every
card on the table*, it used to still cover SOME of them before the inevitable
pickup. Because a pickup returns the defender's own cover cards to its hand **and
logs them** (`handle_pickup`, `game.c`: `log_add_card(l, b->defense)`), every card
it played to partially cover became public — free knowledge for the memory-keeping
MC opponents (octogen pins observed cards to a seat). The rule below closes that
leak, and a paired A/B (below) measured a real, significant win.

The rule ships unconditionally in `bots.wasm` (server) and `oracle.wasm` (client
analyzer), and there is no longer any way to turn it off.
`rules.wasm` / `guards.wasm` do not embed octogen and are unaffected.

The A/B scaffolding that produced the numbers below - the `-DOG_HIDE_UNCOVERABLE`
macro, the `OG_HIDE_MASK` per-seat gate, the fire counter, and the `hide_eval`
harness in `c/tools/hide_tax/` - was deleted with the rest of the dead C build
flags once the rule shipped.
No build target set the macro, so nothing that ships changed when it went.
Restore it from git history to re-run the A/B.

## The rule

In `octogen_strategy_choose`, right after the move is chosen:

> If octogen is about to play a **cover** while **no full cover of all
> currently-uncovered table cards exists** in its legal set, replace the move with
> **pickup**.

Detection is exact and local: count the uncovered battles `U`; if no legal
`MOVE_COVER` covers all `U` of them (`n_cards == U`), any cover octogen picks is a
partial one it can't complete, so it will pick up anyway — take the pickup now and
reveal nothing. Full successful defenses, passes, and already-chosen pickups are
untouched. (This is deliberately conservative: it never aborts a defense octogen
*could* complete, only doomed partial covers.)

Note the rule is weakly card-neutral by construction — covers you play and then
pick up return to your hand (net zero cards) — so any measured effect is almost
purely the information tax, plus the second-order effect that covering adds ranks
to the table and can invite more throw-ins into your pickup.

## Harness (deleted - restore it from history to re-run)

Everything in this section describes code that is no longer in the tree.
It is kept because it is how the numbers below were produced.

`c/tools/hide_tax/hide_eval.c` self-plays octogen-vs-octogen (all seats
octogen — the opponents keep perfect memory and exploit any leak) over a seed
range and prints seat-0's finish position per seed. Seat 0 hides iff bit 0 of
`OG_HIDE_MASK` is set. Run it twice over the SAME seeds and join with `analyze.py`
for a paired comparison (same deals → variance-reduced).

```sh
NOCS="<core srcs minus cordite_sim.c>"; CORE="$NOCS src/cordite_sim.c"
clang -O3 -ffast-math -Isrc -DCD_TT_BITS=20 -DOG_HIDE_UNCOVERABLE \
  $CORE tools/hide_tax/hide_eval.c -o build/hide_eval -lm
OG_HIDE_MASK=1 ./build/hide_eval <seed0> <count> <players> 1 > hide.txt 2> hide.err
OG_HIDE_MASK=0 ./build/hide_eval <seed0> <count> <players> 1 > ctrl.txt 2> ctrl.err
python3 tools/hide_tax/analyze.py hide.txt --ctrl ctrl.txt
```

The `fires=` field in the stderr summary counts how often the rule actually
overrode a partial cover (≈1.2×/game at a 4-player table — the rule is well
exercised, not a corner case).

## Results

Paired by seed (same deals played with seat 0 hiding vs not). Seat 0 is octogen;
opponents are normal octogen with full memory. Lower finish position = better.

**4-player** (seat 0 hides vs 3 normal octogens), 5,999 paired games, rule fired
~1.37×/game:

| metric | hide | normal | delta | SE | z |
|---|---|---|---|---|---|
| win rate (seat 0 finishes 1st) | 23.97% | 22.59% | **+1.38%** | 0.41% | **+3.36** |
| mean finish position | 2.514 | 2.575 | **−0.061** | 0.012 | **−4.92** |

Both significant. Hiding the doomed-defense cover cards makes octogen win ~1.4
points more often and finish ~0.06 places higher against memory-keeping MC
opponents. Per-seed: better in 1227, worse in 1010, equal in 3762.

**2-player** (hide-octogen vs one normal octogen), 6,000 paired games: _TBD (run
in progress)_.

**Reading.** The effect is real but modest, which fits the mechanism: the rule is
card-neutral, so it only ever changes what opponents *know*, and the leak matters
only in the fraction of rounds that (a) reach an uncoverable multi-card table and
(b) whose revealed cards actually inform a later opponent decision. The upside is
free (never costs cards or tempo), so it is a strictly-worth-taking information
tax — larger with more opponents watching (4p > 2p expected).

## Reproduce

Build commands above; the exact seeds are `seed0 .. seed0+count-1` mapped to
32-byte deals by the same xorshift the other `endgame_retro` tools use.
