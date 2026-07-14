# Octogen information-hiding tax: pick up instead of partial-covering

**Experiment.** When octogen is defending and *cannot cover every card on the
table*, it sometimes still covers SOME of them before the inevitable pickup.
Because a pickup returns the defender's own cover cards to its hand **and logs
them** (`handle_pickup`, `game.c`: `log_add_card(l, b->defense)`), every card it
played to partially cover is now public — free knowledge for the memory-keeping
MC opponents (octogen pins observed cards to a seat). This experiment adds a rule
— *if you can't cover it all, pick up immediately* — and measures whether hiding
that information is worth anything.

Everything here is compiled ONLY under `-DOG_HIDE_UNCOVERABLE`; the shipped
`bots.wasm` / `rules.wasm` / `guards.wasm` are byte-identical without it (verified
by rebuilding the wasm with and without the change — same SHA-256).

## The rule

In `octogen_strategy_choose` (gated by `-DOG_HIDE_UNCOVERABLE`, active per-seat via
the `OG_HIDE_MASK` bitmask), right after the move is chosen:

> If this seat hides, and octogen is about to play a **cover** while **no full
> cover of all currently-uncovered table cards exists** in its legal set, replace
> the move with **pickup**.

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

## Harness

`cnitro/tools/hide_tax/hide_eval.c` self-plays octogen-vs-octogen (all seats
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

_(filled in from the paired runs; see the commit that adds this file)_

- **4-player** (seat 0 hides vs 3 normal octogens), N pairs: _TBD_
- **2-player** (hide-octogen vs normal octogen), N pairs: _TBD_

## Reproduce

Build commands above; the exact seeds are `seed0 .. seed0+count-1` mapped to
32-byte deals by the same xorshift the other `endgame_retro` tools use.
