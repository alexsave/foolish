# Fusion: octogen plus two Bot Cup ideas

Fusion is octogen with two changes.
First, kestrel's rollout noise: in its heuristic rollouts, each seat's decision is, with probability 20%, a uniform pick from a small sensible candidate set instead of the rollout policy's move.
Second, sapper's pickup inference at about 85%: a seat that picked up is assumed to hold nothing beating those cards in 6 of 7 sampled worlds (`og_void_mod` = 7), where octogen uses 3 of 4 (`og_void_mod` = 4).
Everything else is octogen's: the same belief, sampled worlds, world budgets, root endgame solve, exact leaf, LEAFBOOK, candidate race and trump-keep tax.
Registered as `fusion` / `fu`, `STRAT_FUSION = 24` (`src/strategy.h`).

Both levers are set by `fusion_strategy_choose` for the length of one decision and put back after it, so an octogen seat on the same thread keeps noise 0 and mixture 4.
`og_void_mod` is octogen's thread-local, latched from `OG_VOID_MOD` on a flag load, so fusion loads octogen's flags first and then swaps its own value in; otherwise the first decision's own flag load would overwrite it.
`FUSION_NOISE` (default 200) and `FUSION_VOID_MOD` (default 7) are read by fusion alone, so ablations need no second brain.

> Built, tested and mutation-checked on 2026-10-28 (rebased onto main after #445).
> The design below is unchanged from the first, uncompiled draft except for the void mixture.

## The idea and where it came from

The idea is kestrel's, an outside bot whose search is determinized Monte Carlo like octogen's.
Kestrel measured it as its single biggest gain: 1.71 vs 1.43 points per game against itself without the noise (par 1.5).
Its explanation: a rollout policy is an opponent model, and a search that always meets the same deterministic model over-fits that model's exact habits.
A small dose of sensible alternatives in every seat's rollout play keeps the root values from leaning on them.
Kestrel applies the noise to every seat, the searching seat included; noise for opponents only measured neutral or worse there.

## Design

### Where the noise sits

- **The knob** is a thread-local per-mille in the bitboard rollout engine, `cd_sim_set_noise` / `cd_sim_noise` (`src/cordite_sim.h`, `src/cordite_sim.c`).
  It is 0 by default.
  `fusion_strategy_choose` (`src/octogen_strategy.c`) sets it for the length of one octogen decision and puts 0 back.
  Octogen never touches it.
- **The bitboard rollouts** (the default path): every place a playout asks a seat for its policy move now asks the noise first.
  `sim_seat_move` is that one place for `cd_sim_playout_pol` and the reply playout's policy steps; `sim_hw_noisy_move` is the same for `cd_sim_playout` and `cd_sim_playout_leaf`.
  Octogen's default path is `cd_sim_playout_pol` (OG_BBLEAF defaults to 2, so the leaf flag is always on).
- **The struct rollout** (`og_simulate`, used under `OG_NO_FASTROLL`, `OG_LEAF`, `OG_DIFFTEST` or `OG_NO_WORLDSIM`): `cd_sim_noise_legal` builds a SimState from the determinized world, draws the same way, and maps the candidate onto the seat's lite menu by move kind and card set.
  One candidate builder serves both paths, so the two cannot disagree on the set.
- **The stream**: both draws (the gate, then the pick) are `game_random()`, the rollout's own stream, which `og_run_stages` reseeds to the same `sim_rng` for every root candidate in a world.
  So the common random numbers across candidates are intact: the same worlds and the same rollout streams for every root candidate.
  The two paths draw in the same order, so `OG_DIFFTEST` still compares like with like.
- **Never in an exact line**: the noise only replaces a policy choice.
  The root endgame solve, `cd_sim_solve*`, the in-rollout exact leaf and the reply tournament's enumeration never consult a policy, so solved lines are untouched.
  After a leaf solve aborts, the playout falls back to policy play, and that heuristic play takes the noise like any other.

### Why octogen at 0 stays byte-for-byte the same

Every draw site is behind `if (cd_noise_pm)`, and `cd_sim_noise_legal` returns before drawing when the knob is 0.
So at 0 not one extra `game_random()` is consumed, and octogen, cordite and every other brain on these playouts consume exactly the stream they always did.
The refactor that put the policy dispatch into `sim_seat_move` keeps the same branch order (LOOSE, MCDEF, else handwritten).
`CdNoiseStats.drawn` counts gate draws, so a test can prove a brain at 0 drew nothing.
The cost at 0 is one thread-local load per rollout ply in native builds.

### The candidate set, and how kestrel's list maps

Built in `sim_noise_cands` on the engine's existing move primitives (`sim_attack_group_core`, `sim_greedy_full_cover`, `sim_gen_pass`), not a new generator.
Identical candidates collapse to one, so the pick is uniform over distinct moves.
Every attack is capped at the defender's capacity and at `MAX_MOVE_CARDS`, so every candidate is a move on the lite menu.

| Seat | Kestrel's candidate | Here |
| --- | --- | --- |
| Leader | one card, per value | the lowest-score card of that value (non-trump first) |
| Leader | the whole group, per value | every card of that value, capped, cheapest first |
| Defender | cheapest full cover | `sim_greedy_full_cover`, the policy's own cover and the lite menu's one cover |
| Defender | cheapest cover without trumps | not built: in this engine it is always either impossible or the same move as the greedy cover (see the comment on `sim_noise_cands`) |
| Defender | pickup | always |
| Defender | each distinct pass | one single-card pass per matching card, from `sim_gen_pass` (so the podkidnoy rule and the next seat's capacity are the generator's) |
| Thrower | good (stop) | always |
| Thrower | lowest matching non-trump | 1 card |
| Thrower | all matching non-trumps | capped |
| Thrower | two of them | the two lowest non-trumps |
| Thrower | a lone trump | the lowest matching trump |
| Thrower | everything | every matching card, capped |

### What is NOT in any shipped module

- `FOOLISH_SEEDED_BOTS_ONLY` (set in `WASM_BOT_CFLAGS`, so in `web.wasm`, `oracle.wasm` and `oracle-mt.wasm`) turns `CD_ROLLOUT_NOISE` off.
  There the noise code is compiled out, the setters are inline no-ops, and `fusion_strategy_choose` is not defined, so anything that named it would fail to link rather than run octogen under fusion's name.
  `bot_roster.c` and the wasm source lists are untouched; no source list needed fusion added, because fusion lives in `octogen_strategy.c` and every list that has octogen has it.
- The native kernel (`KERNEL_SRC`: the arena tools, the tests, the kernel c_base links, the iOS xcframework) has the noise compiled in and dormant at 0.
  Fusion is reachable there only through `strategy_choose` / `parse_strategy`, which is the arena dispatch (`cnitro_eval`, `cnitro_elo`, the tests).
- **No roster row.**
  The roster (`src/bot_roster.c`) is one table compiled into every module, including the shipped ones, and read across the boundary as `BotRosterTable`; a row there would put fusion in the shipped roster even with seeded and offline both off.
  The roster also has no city or song field: a seat's city is an i18n string `bot.<key>` in all twenty-five languages.
  TODO(alex): fusion's city is "TBD" and its song is "TBD" (marked in `src/strategy.h`).
  When fusion earns a rung, the options are: a row with seeded=0 and offline=0 that the wasm builds mark unlinked (like espresso_prod and gunpowder under `FOOLISH_SEEDED_BOTS_ONLY`), plus the `bot.fusion` strings; or replace octogen's row's brain outright if fusion simply beats it everywhere.

## Ready to run

All commands from `foolish/c` in this worktree.
The Mac rules apply: one local executor at a time, no zsh word-splitting tricks, and `make OMP=1` needs libomp (`brew install libomp`).

### 1. Build

```sh
make build/cnitro_tests build/cnitro_eval build/cnitro_elo
make OMP=1 build/cnitro_eval build/cnitro_elo   # the parallel arena, for steps 4 and 5 (rebuilds the same targets)
```

The first build compiled clean with no warnings.

### 2. Tests

```sh
./build/cnitro_tests                 # the whole suite, including the four fusion tests below
FU_TRACE=1 ./build/cnitro_tests 2>&1 | grep 'fusion noise'   # the measured fire rates
make tests-wasm-caps                 # must still pass: octogen under the wasm size limits
```

The five new tests (`tests/tests.c`, after `test_bot_roster_choose_scopes_knobs`):

- `test_fusion_is_dispatchable_and_named`: id 24, `STRAT_COUNT` 25, the names parse, no roster row, `strategy_choose` returns a legal move.
- `test_rollout_noise_seam`: on one fixed world, at 0 nothing is drawn and a playout is its stream's; at 1000 the same streams play out differently, every playout ends, and the per-seat-policy playout takes the noise too.
- `test_octogen_is_fusion_at_zero`: over stretches of 2- and 3-seat games, on both rollout paths, octogen and fusion with `FUSION_NOISE=0` choose the same move and leave both RNG streams in the same state, and neither draws the noise.
- `test_fusion_rollouts_are_noisy`: fusion at 200 fires on both paths at 12-28% of drawn decisions, every struct-path pick is on the lite menu, octogen beside it draws nothing, and the knob is back at 0 afterwards.
- `test_fusion_void_mixture`: over 2- and 3-seat game stretches, fusion with the noise off is octogen at `OG_VOID_MOD=7` move for move and stream for stream, octogen right after a fusion call is octogen at 4, and somewhere the two mixtures part.

`test_octogen_is_fusion_at_zero` now sets `FUSION_VOID_MOD=4` beside `FUSION_NOISE=0`.

`test_podkidnoy_every_brain` (the `CD_PASS_PROBE` sweep) now also plays fusion, and its id check expects `STRAT_COUNT == 25`.

Then the mutation check, one mutation at a time, each written beside its test: break the code, confirm the named assertion goes red, restore by re-editing (never `git checkout --`).

### 3. Octogen is unchanged against main

No existing test pins an octogen decision to a value, so prove this branch's octogen is main's move for move:

```sh
BASE=/Users/alex/Dev/foolish/.claude/worktrees/fusion-base
git worktree add --detach $BASE c7e0a1a4d          # this branch's fork point on main
make -C $BASE/foolish/c build/cnitro_eval
for s in 200001 200002 200003; do for pc in 2 4; do
  $BASE/foolish/c/build/cnitro_eval --strategy=octogen --opp=octogen --players=$pc --inspect=$s > /tmp/og_main_${pc}_$s.txt
  ./build/cnitro_eval --strategy=octogen --opp=octogen --players=$pc --inspect=$s > /tmp/og_fusion_${pc}_$s.txt
  cmp /tmp/og_main_${pc}_$s.txt /tmp/og_fusion_${pc}_$s.txt
done; done
```

Every `cmp` must be silent (use the scratchpad instead of `/tmp` if the session has one).
Also run `scripts/wasm_build.sh` on both trees and compare the raw `web.wasm` and `oracle*.wasm`: they should be byte-identical or differ only by the `sim_seat_move` refactor's code layout; a difference is to be explained, not waved through.
Remove the base worktree afterwards (`git worktree remove $BASE`).

### 4. Paired same-deal comparison, fusion vs octogen

`--control` plays every seed twice on the same deal, the hero at seat 0 and then the control at seat 0, against the same opponents, and reports the paired finish delta (negative is better for the hero).

```sh
./build/cnitro_eval --strategy=fusion --control=octogen --opp=octogen  --players=2,4 --games=400 --seed-start=200001
./build/cnitro_eval --strategy=fusion --control=octogen --opp=cordite  --players=2,4 --games=400 --seed-start=300001
./build/cnitro_eval --strategy=fusion --control=octogen --opp=handwritten --players=2,4 --games=400 --seed-start=400001
```

The three fields matter because the octogen hunt-5 lesson is that a rollout change's sign is a property of the opponent: against handwritten the stock policy IS the opponent's decision function, so noise there is pure model error; against search-like opponents it may be the de-biasing kestrel saw.
A 0-delta sanity arm first: `FUSION_NOISE=0 ./build/cnitro_eval --strategy=fusion --control=octogen --opp=octogen --players=2,4 --games=200` must report every pair equal.

### 5. The dose sweep

The 20% was tuned on kestrel's weaker policy, so sweep it for octogen.
`FUSION_NOISE` is read by fusion alone, so the control stays plain octogen:

```sh
for pm in 50 100 200 300; do
  FUSION_NOISE=$pm ./build/cnitro_eval --strategy=fusion --control=octogen --opp=octogen --players=2,3,4,6 --games=400 --seed-start=500001
done
```

### 6. ELO

```sh
./build/cnitro_elo --games=2000 --pool=fusion,octogen,cordite,blackpowder,handwritten --pcs=2,3,4,5,6 --snapshot-every=200
```

### What to expect, and the risks

- **Kestrel's number does not transfer as is.** Its gain was measured against itself with a weaker rollout policy; octogen's handwritten rollouts plus its belief, exact leaf and root solve are a different regime.
  A null is a real possibility, and so is a small loss.
- **2 players may lose.** Heads-up octogen solves exactly more often (root solve to 28 cards, the exact leaf), so a larger share of its value comes from truth, and noise in the remaining rollouts is more likely to blur a sharp estimate than to de-bias it.
  Expect the best case at 3-4 players.
- **Against handwritten tables, expect harm.** The stock rollout policy is exactly a handwritten opponent's decision function, so noise there is pure model error (the hunt-5 sign rule in `OCTOGEN.md`).
- **Noise costs worlds' worth of signal.** At a fixed world budget, noisier rollouts mean noisier root estimates; if the sweep favours a dose, check whether it still holds when octogen's own budget is the binding constraint (the oracle at 6x is the probe).
- **The own-seat share.** Kestrel noised every seat including its own; if the seat-0 noise is what hurts, a variant that exempts the searching seat is one condition in `sim_seat_move`.
- **Wall clock.** Each fired decision builds a small candidate list, and noisy rollouts run somewhat longer (more pickups); at 20% this should be a few percent, and the paired run's `rate=` line shows it.

## First measurement (2026-10-28): the Bot Cup league

The serverless league (`cnitro_league_sim --accounts=7`, the gauntlet branch's 11 brains x 7 accounts, random table size 2-8) on seeds 100001..103000, with the 7 octogen accounts swapped for a variant and still labelled octogen, so every arm has the same tables, seats and deals.
Four arms ran concurrently, 2 workers each, so machine load hit every arm alike (the Bot Cup bots are time-budgeted, so a game is not reproducible run to run; the pairing is by table and deal).
The score is the octogen seats' average finish (place-1)/(n-1), lower is better; the delta is paired per seed against plain octogen, with a 95% interval.

| Table size | Games | octogen | fusion (noise + void 7) | noise only | void 7 only |
| --- | --- | --- | --- | --- | --- |
| all | 1115 | .382 | .355 (-.027 +/- .023) | .355 (-.027 +/- .023) | .376 (-.006 +/- .020) |
| 2p | 71 | .254 | .085 (-.169 +/- .104) | .155 (-.099 +/- .125) | .239 (-.014 +/- .108) |
| 3-5p | 420 | .383 | .350 (-.033 +/- .042) | .357 (-.026 +/- .041) | .382 (-.001 +/- .033) |
| 6-8p | 624 | .396 | .389 (-.007 +/- .027) | .376 (-.020 +/- .027) | .387 (-.009 +/- .025) |

The noise does the work; the void mixture at 7 is flat everywhere.
The other brains' deltas are all inside their intervals, so no single brain paid for the gain.
