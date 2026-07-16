# Cordite — belief-constrained determinized Monte Carlo, v2

The successor to blackpowder (as smokeless powder was to black powder).
Same legitimacy contract: **no LLM, no cheating** — it sees exactly what a
human sees (own hand, table, hand counts, deck count, the public move
history) and computes everything inside one `chooseMove` call. No
server-side memory needed.

## What changed vs blackpowder

### 1. The tie-break inversion fix (the single biggest win)

Blackpowder's two-stage MC prunes candidates by dropping the worst mean
finish repeatedly. With few worlds and small-integer finish positions,
candidate scores tie *constantly* — and the prune loop dropped the
**first** tied candidate. Candidates are deliberately ranked
cheapest-first (low cards before trumps), so on ties the pruning
systematically discarded the cheap move and kept the expensive one:
a quiet, pervasive trump-burning bias present in blackpowder and inherited
by cordite's first draft. Found by diffing a move trace at a divergent
defender decision (covering a 6♣ with Q♠-trump instead of 8♣) after every
feature ablation failed to explain a persistent ~0.1 mean-finish deficit.

Cordite drops the **last** tied candidate instead, preserving cheap-first
order through pruning. This one-line fix improved *every* matchup at
*every* player count (e.g. vs handwritten pc4: 2.080 → 1.900 mean finish;
vs blackpowder tables pc4: 2.72 → 2.32).

### 2. Compact worlds + early rollout exit (≈2-3x sampling budget)

- Sampled worlds keep only the `LOG_DISCARD` entries of the real log — the
  only log type any rollout policy reads (espresso's discard memory).
  Hot-loop trial clones shrink ~8x. Verified decision-identical to
  full-log worlds (`CD_FULL_LOGS=1` A/B: bit-identical results).
- A rollout returns the moment our seat is eliminated — the finish
  position is already determined; the long tail of other players'
  endgames is skipped. At 5-8 players this is a large saving.

The freed CPU is reinvested in worlds: W1/W2 = 16/28 at 2p, 14/28 at 3-4p,
**20/40 at 5-8p** (vs blackpowder's 8-12/16-20), plus a third stage where
the top-2 candidates duel on W3 more shared worlds (28/24). Throughput
stays in the same order as blackpowder (~20-30 games/s at pc4 vs ~48).

### 3. Loss-avoiding endgame solver

Blackpowder's exact 2-player endgame solver only *takes* forced wins.
Cordite adds a second pass when no win exists: each root move is
classified with a null window (sign only, maximal pruning), and
proven-losing moves are excluded from the MC stage — but **only when at
least one move is proven non-losing**. The unguarded version restricted
MC to "moves the solver failed to read" whenever everything was lost —
adverse selection that measurably cost ~10 points of win rate vs
blackpowder at pc2 before the guard was added.

### 4. Rank-floor inference + per-player trust

A *single-card, non-trump first attack* by a lowest-first opponent
(handwritten family — espresso defers to handwritten at 3+ players in)
reveals their lowest non-trump rank: a max-cards-then-lowest-sum attacker
holding anything lower would have led it. The floor is applied as a soft
constraint in half the sampled worlds (`CD_FLOOR_MOD`), expires when the
player gains cards (draw/pickup), and is dropped per-player on
contradiction or after any trump lead while the deck is alive (lowest-first
players almost never do that; random and MC-style players do). Oracle
self-check (`CD_VERIFY=1`): zero violations over hundreds of games vs
handwritten and espresso.

Void constraints are kept exactly as in blackpowder (3-of-4 world
mixture), *not* gated by trump leads — a trump lead can be forced, and the
mixture already absorbs violators.

### 6. Compact bitboard rollout engine (v2.1 — ~4x faster rollout)

The 44 KB `Game` struct and its linear-scan / array-shift ops
(`hand_remove_card` is O(hand)+shift; `should_bot_act` loops battles; etc.)
made each rollout ply ~320 ns, and a rollout runs a full playout for every
(world × candidate). `cordite_sim.{c,h}` replaces the rollout with a compact
bitboard engine:

- A `SimState` is ~200 bytes: each player's hand is a `uint64` bitmask over
  card-ids `id = suit*13 + (value-1)` (0..51), plus precomputed
  `VALUE_MASK[v]` / `SUIT_MASK[s]` and the trump suit. `hand_remove =
  h &= ~(1ull<<id)` is O(1); "cards of value v" is `h & VALUE_MASK[v]`;
  counts/lowest-card are `popcount`/`ctz`. Clone/copy is one `memcpy`.
- The rollout rules (attack/cover/pass/pickup/good + refill + elimination)
  and the rollout **policy** are reimplemented directly on the bitboard
  state — the policy move is *computed* (decision tree over bit masks), no
  `LegalMoves` list is ever materialized. The effective rollout policy is
  always **handwritten**: `cd_rollout_for` only returns espresso when the
  deck is dead **and** 3+ players are in, but espresso defers to handwritten
  at 3+ in, so its 1v1 path is never reached. Only handwritten is ported.
- Each sampled determinized **world** is converted to a `SimState` once
  (`cd_sim_from_game`); each candidate then clones the SimState, applies its
  move with `cd_sim_apply_root_move`, and plays out on bitboards. The struct
  path is kept for `CD_NO_FASTROLL` / `CD_LEAF` (the exact leaf solver lives
  there) / `CD_DIFFTEST`.

**Correctness.** Two difftests (`tests/sim_difftest.c` move-by-move playout
equivalence; `tests/apply_difftest.c` root-move application) show **zero**
genuine rule divergences over thousands of games at every player count
(`np 2..8`). The only differences are benign interchangeable
equal-value / equal-trumpness card swaps (handwritten breaks ties by hand
array order, which a bitmask can't preserve; the game value is identical).
`cnitro_tests` stays 14/14.

**Speed.** Rollout in isolation (`CD_NO_SOLVE`, eval wall-clock) is ~4x
faster (pc4: 6.74 s → 1.55 s / 200 games; pc2: 8.65 s → 2.25 s). With the
exact endgame solver on (the solver runs on the struct and is now the
dominant cost, especially at pc2), the whole-eval speedup at the *old*
budget is ~2.5x (pc4 14.4 s → 5.7 s / 300 games).

**Spent on strength (the v2.1 budget).** The freed CPU buys ~2x the world
budget — W1/W2/W3 = 32/56/56 at 2p, 28/56/56 at 3-4p, 40/80/56 at 5-6p,
40/80/48 at 7-8p (≈2x the v2 numbers below). At this budget cordite is both
faster than the old slow+1x (pc4 14.3 s → 8.7 s, pc6 26.5 s → 14.7 s per
300 games) **and** stronger (vs handwritten, 400 games, seeds 910001):
pc2 87.9% → 88.0%, pc5 24.3% → 24.8%, pc6 21.6% → 22.5%, pc8 11.6% →
13.2% win rate; mean finish at or below the published 1000-game baseline at
every player count. Doubling again to 3x was *worse* (pc6 2.873 → 2.962,
pc8 4.223 → 4.272) — the budget stops at 2x. Knobs: `CD_NO_FASTROLL=1`
forces the struct rollout; `CD_DIFFTEST=1` runs both engines per rollout and
reports the divergence rate.

### 7. Bitboard exact endgame solver (v2.2 — the solver was the new top cost)

Once the rollout was on bitboards, the **exact 2-player endgame solver**
(`cd_try_endgame_solve` → `cd_solve`) became the dominant cost — it still ran
on the 44 KB `Game` struct, cloning a Game and re-enumerating
`calculate_legal_moves` (combinations + cover blow-up) at every minimax node.
Profiled share of total eval wall-clock (cordite vs handwritten, 300 games):
**pc2 ≈ 69%** (28.6 s with the solver vs 8.9 s `CD_NO_SOLVE`), pc4 ≈ 45%,
falling to ≈14% at pc6 (2-player endgames are rare while many players remain).
gprof confirmed `cd_try_endgame_solve` at ~40% of total, almost all in the
struct `cd_apply`/`handle_*`/`calculate_legal_moves` it drives.

`cordite_sim.c` now carries a second engine: an exact minimax solver
(`cd_sim_solve`) on the compact `SimState`. A node clones with a `*child = *s`
memcpy (~200 B) instead of a Game clone; the **full** legal-move set is
enumerated with bit ops (subset masks for same-value attacks/passes,
combination-of-battles × per-card covers for the defender — the same SET the
struct enumerates, so the minimax value is identical); and a **transposition
table** (64K entries, full-64-bit key) memoizes resolved subtrees — these
endgames transpose heavily (move orderings converge). The TT stores **EXACT
values only**: a fail-soft alpha-beta result is the true game value solely when
it lands strictly inside the original window (`alpha0 < best < beta0`);
fail-low/fail-high results are bounds and are not memoized. Stored values are
depth-relative (mate-distance re-basing), so a position reached at a different
depth reads back correctly.

**Correctness (it is EXACT — a wrong solver = wrong play).**
`tests/solver_difftest.c` plays real games and, at every deck-empty 2-player
node (the regime the solver runs in), resolves the full game value from each
IN player's perspective with **both** engines (wide window, large budget) and
asserts bit-identical values. **Zero mismatches** over thousands of
fully-resolved positions at np 2/3/4. (An early version had sign-correct but
depth-off-by-1 values; the bug was the TT storing alpha-beta *bound* values as
exact — fixed by the strict-window exactness test above; with the TT disabled
the two engines already agreed exactly, which localized it.) The bitboard
solver also resolves *more* positions within budget than the struct (cheaper
nodes + TT); since every resolved value is provably the true value, this is
strictly more exact information, not a different answer. `cnitro_tests` stays
14/14; the rollout difftests stay 0 REAL divergence.

**Speed.** Same strength, less wall-clock (cordite vs handwritten, 400 games,
seeds 910001; `CD_NO_BBSOLVE=1` is the old struct solver in the same binary):

| pc | struct solver | bitboard solver | speedup | mean / win |
|----|---------------|-----------------|---------|------------|
| 2 | 37.9 s | **13.9 s** | **2.74x** | 1.120 / 88.0% (identical) |
| 3 | 20.2 s | **11.9 s** | 1.70x | 1.545 / 56.2% (identical) |
| 4 | 15.2 s | **10.1 s** | 1.51x | 2.028 / 34.8% (≈, within noise) |
| 6 | 24.6 s | **22.5 s** | 1.10x | 2.873 / 22.5% (identical) |
| 8 | 20.0 s | **17.1 s** | 1.17x | 4.223 / 13.2% (identical) |

vs espresso (500 games, seed 930001) strength holds within noise: pc2
1.288 → 1.292, pc3 1.538 (identical), pc4 2.098 → 2.100. The gain is largest
exactly where the solver dominated (pc2, ~2.7x); at high
player counts the rollout dominates and the solver share — hence the gain — is
small. The bitboard solver needs a much smaller **node** budget than the
struct (each node is far cheaper and the TT converges fast): `CD_BB_WIN`
(default 20000) / `CD_BB_AVOID` (default 15000) replace the struct's shared
200K/150K; at this budget strength matches the struct's exactly while
wall-clock is minimized (a 60K budget gave bit-identical strength but no
faster, confirming 20K already resolves every decisive endgame). Knobs:
`CD_NO_BBSOLVE=1` reverts to the struct solver (A/B); `CD_BB_WIN` /
`CD_BB_AVOID` tune the per-pass node budgets.

**Spending the freed CPU.** The solver speedup frees the most wall-clock at
low player counts (where it dominated) — at pc2 the whole eval drops from
37.9 s to 13.9 s / 400 games, leaving headroom under the old budget. Re-investing
it in more sampled worlds at pc2 (W1/W2/W3 64/112/112, ~2x) was **not** a robust
win: vs handwritten it was seed-dependent (seed 910001 88.0% → 93.0%, but seed
920001 90.2% → 90.0% — flat), and vs espresso it helped (+3% win) but
inconsistently. As with the v2.1 budget study, more worlds past the tuned point
is noise, not signal, so the world budgets are **left unchanged** and the win is
banked as throughput. The solver budget itself stays small (`CD_BB_WIN` 20000)
because a larger one gave bit-identical strength at higher cost.

The TS port (`cordite_core.ts`) keeps its compact-array `SimGame` solver (the
bitboard engine is C-only — V8 would need `BigInt`), but **does** get the
portable algorithmic win: the same EXACT-only, depth-rebased **transposition
table** (a `Map` keyed on a string fingerprint of the two hands + table +
roles). On the latency-bounded TS path a faster solve frees wall-clock for
more world sampling; play is unchanged (exact memoization cannot change the
solver's value). Offline harness still plays correctly (pc2/pc4 vs
handwritten) with decision times well inside the cap.

### Rejected: exact leaf endgames inside rollouts

Solving small 2-player deck-empty endgames exactly *inside rollouts*
(legitimate — full info inside a sampled world) was implemented and
measured: it made cordite both ~10x slower and **weaker** (pc2 vs
handwritten: 1.150 → 1.240 mean). Modeling the actual imperfect opponent
(handwritten plays the endgame in the rollout) beats assuming perfect
play, and beats it at a fraction of the cost. Kept behind `CD_LEAF=1` as a
negative result worth remembering.

### 6. Direct rollout chooser (faster Monte Carlo, identical play)

The rollout used to call `calculate_legal_moves_lite` (full combination
enumeration) every ply, then let the policy pick ONE structured move. Since
the rollout policy is handwritten almost everywhere (deck alive, heads-up, or
espresso deferring to handwritten at 3+ in — i.e. every case except the
espresso 1v1 deck-empty endgame), `handwritten_rollout_choose` now computes
that one move *directly*: the most-non-trump-cards / lowest-value first attack,
the lowest-summed-score max-cards regular attack (emitting GOOD directly when
no non-trump attack exists), and the pass-then-greedy-cover-then-pickup
defender order. It reproduces handwritten's exact tie-breaks **and** draws
`game_random()` in the same spots (the GOOD branch), so the whole rollout is
bit-for-bit unchanged. Trump-gated attacks and the espresso 1v1 endgame defer
to the slow path (the reference). Verified by `CD_DIFFTEST=1` (per-move
fast-vs-slow comparison: 0 mismatches over ~1000 games at pc 2–8 vs every
opponent) and by whole-eval bit-identity to `CD_NO_FASTROLL=1`. Throughput
(cordite vs handwritten, 200 games): **pc2 +10%, pc4 +20%, pc6 +30%** — the win
grows with player count as enumeration dominates more decisions; `combinations_
attack` calls fall ~40% and `handwritten_strategy_choose` calls ~68%. A
transposition table was **not** pursued: rollout states rarely repeat and the
empty-deck 2-player endgame is already solved exactly — the win is purely
cutting per-ply enumeration, confirmed by the profile. `CD_NO_FASTROLL=1`
restores the enumerate-then-pick path for A/B.

The TS port (`cordite_core.ts`) is identical except the regular-attack branch
defers to the slow path for hands with ≥7 valid cards: the TS enumerator caps
attacks at `MAX_SOLVE_MOVES` (96) and truncates large hands before the biggest
combo, which the C enumerator does not. TS throughput gain is smaller (~2–3%)
because the JS rollout is allocation-bound rather than enumeration-bound; the
benefit there is more worlds sampled inside the 1.5–1.9 s wall-clock cap.

### Rejected: fool-risk (last-place) objective

cordite ranks candidates by **mean** finishing position. Since the only real
loss in Durak is finishing last (the fool), a risk-averse objective
`mean + λ·P(finish == N)` was added (per-candidate fool-rate tracked alongside
the mean; same cheapest-first tie-break) and A/B'd at λ ∈ {0.1, 0.25, 0.5, 1,
2} across pc 4/6/8 vs handwritten, espresso, and random (250–400 games each).
It was **worse or flat on every metric in every config**, and — counter to its
purpose — often *raised* the actual fool-rate vs strong opponents (pc4 vs
espresso 400g: λ=0 → mean 2.080/win 35.8%/fool 10.3%; λ=1 → 2.172/32.8%/12.8%).
The mean objective already weights the fool maximally (it is the worst position,
N, dominating the average); an extra penalty double-counts it and pushes the bot
into passive "safe-now" moves that end up worse positioned against the real,
imperfect opponent. **Not kept** — cordite stays pure-mean. Negative result
worth remembering.

## Results

Protagonist at seat 0, all other seats the listed opponent, seeds 910001+,
`mean` = mean finish position (lower is better; baseline = (N+1)/2),
`win` = finished 1st. Blackpowder columns are its published 1000-game
numbers from BLACKPOWDER.md.

### vs handwritten (1000 games/pc)

| pc | cordite | blackpowder | baseline |
|----|---------|-------------|----------|
| 2 | **1.121** / 87.9% | 1.148 / 85.2% | 1.500 |
| 3 | **1.553** / 56.0% | 1.622 / 51.5% | 2.000 |
| 4 | **2.039** / 35.5% | 2.199 / 28.4% | 2.500 |
| 5 | **2.584** / 24.3% | 2.731 / 21.4% | 3.000 |
| 6 | **2.960** / 21.6% | 3.196 / 18.0% | 3.500 |
| 7 | **3.539** / 15.2% | 3.749 / 14.0% | 4.000 |
| 8 | **4.177** / 11.6% | 4.339 / 11.8% | 4.500 |

### vs espresso (the cheating bot, 1000 games/pc)

| pc | cordite | blackpowder | baseline |
|----|---------|-------------|----------|
| 2 | **1.310** / 69.0% | 1.398 / 60.2% | 1.500 |
| 3 | **1.616** / 56.0% | 1.677 / 51.5% | 2.000 |
| 4 | **2.083** / 35.5% | 2.257 / 28.4% | 2.500 |
| 5 | **2.616** / 24.3% | 2.772 / 21.4% | 3.000 |
| 6 | **2.988** / 21.6% | 3.235 / 18.0% | 3.500 |
| 7 | **3.559** / 15.2% | 3.776 / 14.0% | 4.000 |
| 8 | **4.190** / 11.6% | 4.362 / 11.8% | 4.500 |

### vs random (1000 games/pc)

| pc | cordite | blackpowder | baseline |
|----|---------|-------------|----------|
| 2 | **1.019** / 98.1% | 1.022 / 97.8% | 1.500 |
| 3 | **1.131** / 87.3% | 1.197 / 81.3% | 2.000 |
| 4 | **1.331** / 73.5% | 1.422 / 66.5% | 2.500 |
| 5 | **1.575** / 61.4% | 1.636 / 56.5% | 3.000 |
| 6 | 1.753 / 51.1% | **1.718** / 55.0% | 3.500 |
| 7 | **1.878** / 51.5% | 2.053 / 46.5% | 4.000 |
| 8 | **2.330** / 39.8% | 2.531 / 32.2% | 4.500 |

(pc6 vs random is the single cell blackpowder keeps, by 0.035 ≈ 1σ; at
pc7-8 cordite also beats gunpowder/handwritten, which had beaten
blackpowder there.)

### vs blackpowder tables — the matchup that matters (400 games/pc)

Seat 0 has a measurable disadvantage in this harness, so the fair
comparison is the **bp-vs-bp control** (blackpowder itself at seat 0 of an
all-blackpowder table), not the neutral baseline:

| pc | cordite @ bp table | control: bp @ bp table |
|----|--------------------|------------------------|
| 2 | **1.450** / 55.0% | 1.522 / 47.8% |
| 3 | **1.910** / 36.2% | 2.127 / 27.5% |
| 4 | **2.447** / 28.8% | 2.635 / 21.8% |
| 5 | **2.830** / 21.8% | 3.150 / 15.0% |
| 6 | **3.502** / 13.8% | 3.717 / 12.8% |
| 7 | **4.050** / 10.8% | 4.080 / 11.5% |
| 8 | **4.470** / 12.2% | 4.800 /  9.8% |

Cordite outscores blackpowder's own seat at every player count (pc7 is
the narrowest at -0.03 mean), confirmed at pc4 on fresh seeds 920001+
(2.320/30.0% vs control 2.632/23.2%).

### ELO arena (random seats, mixed pools, PCs 2-8, 3000 games, K=32)

| rank | competitor | elo | win% | durak% |
|------|-----------|------|------|--------|
| 1 | **cordite** | **1194** | **28.2%** | **6.4%** |
| 2 | handwritten | 1146 | 21.0% | 17.6% |
| 3 | blackpowder | 1047 | 24.5% | 9.2% |
| 4 | firecracker | 1026 | 17.6% | 15.0% |
| 5 | gunpowder | 985 | 22.0% | 16.8% |
| 6 | robusta | 974 | 19.6% | 14.4% |
| 7 | espresso | 948 | 21.9% | 15.1% |
| 8 | random | 680 | 3.9% | 64.3% |

Cordite takes #1 with the highest win rate and the lowest durak rate;
3000 arena games ran in 222s (~13.5 games/s mixed).

## Reproduce

```bash
cd cnitro && make

# full benchmark suite (writes /tmp/cordite_bench/*.txt)
./bench_cordite.sh /tmp/cordite_bench

# single matchup
./build/cnitro_eval --strategy=cordite --opp=blackpowder \
    --players=2,3,4,5,6,7,8 --games=400 --seed-start=910001

# single-game move trace
./build/cnitro_eval --strategy=cordite --opp=espresso --players=4 --inspect=12345
```

## Knobs / ablation env vars (read once per process)

- `CD_NO_SOLVE` / `CD_NO_VOIDS` / `CD_NO_FLIP` — as in blackpowder
- `CD_NO_FLOORS` — disable rank-floor inference; `CD_FLOOR_MOD=k` — floors
  in 1/k of worlds (default 2)
- `CD_NO_AVOID` — disable the loss-avoiding solver pass
- `CD_LEAF=1` — re-enable exact leaf endgames in rollouts (measured worse);
  also forces the struct rollout path (the leaf solver lives there)
- `CD_NO_FASTROLL=1` — use the struct rollout instead of the bitboard engine
- `CD_DIFFTEST=1` — run both rollout engines per call and report the
  fast-vs-slow divergence rate at exit (keeps the struct result)
- `CD_NO_BBSOLVE=1` — use the struct endgame solver instead of the bitboard
  one (A/B; same result, slower). `CD_BB_WIN` / `CD_BB_AVOID` — node budgets
  for the bitboard solver's win-hunt / loss-avoidance passes (default
  20000 / 15000)
- `CD_W1/CD_W2/CD_W3` — world-count overrides
- `CD_FULL_LOGS=1` — blackpowder-style full-log worlds (A/B; identical)
- `CD_NO_EARLYEXIT=1` / `CD_BP_SOLVE=1` — debug A/B switches used while
  isolating the tie-break bug
- `CD_NO_FASTROLL=1` — disable the direct rollout chooser, reverting to
  enumerate-then-pick (A/B; bit-identical, just slower)
- `CD_DIFFTEST=1` — assert the direct rollout chooser matches the slow path on
  every accepted decision; aborts on divergence (test-only). The TS port uses
  `globalThis.CD_NO_FASTROLL=true` for the same A/B.
- `CD_VERIFY=1` — oracle self-check of the belief vs real hands (test-only)

## Production TS port (cordite + cordite_max)

Shipped in `supabase/functions/_shared/common/strategies/`:

- `cordite_core.ts` — transliteration of this C code: compact int-card sim
  engine, handwritten + espresso rollout policies, belief builder, CRN
  sampler, endgame solver, staged MC. Reads only public info from the real
  `Game` (own hand + logs; opponent draws are masked server-side anyway).
  The TS draw log conveniently exposes the flipped card identity when
  drawn, so flip-pinning is direct rather than inferred.
- `cordite_strategy.ts` — `BotStrategy` adapter; registered in
  `bot_strategy.ts` as `cordite` and `cordite_max`, seeded in
  `supabase/seed.sql` (Cordite 1-3, Cordite Max).
- Offline harness: `offlinefun/localtest/cordite_eval.ts`
  (`npx tsx offlinefun/localtest/cordite_eval.ts cordite_max espresso 4,8 20`).

Validation: TS cordite vs handwritten pc2 = 1.133 mean / 87% win over 30
offline games (C benchmark: 1.121 / 87.9%).

**v2.1 (bitboard rollout) and the TS port.** The compact bitboard rollout
engine (section 6) is a C-only win: it relies on native 64-bit `popcount` /
`ctz` / bitmask ops. In V8 the equivalent would need `BigInt`, which is
*slower* than the small-array `SimGame` the TS port already uses, so the
engine is deliberately **not** ported — the TS rollout stays on its compact
int-card arrays. The portable win is the **budget**: `CORDITE_PARAMS` in
`cordite_core.ts` was raised to the v2.1 2x counts (32/56/56 at 2p,
28/56/56 at 3-4p, 40/80/56|48 at 5-8p). The TS path is wall-clock bounded
(`maxMillis`), so the larger counts only take effect when time allows and
the latency cap is unchanged.

**World budgets.** `cordite` uses the C-tuned defaults. `cordite_max` uses
W1/W2/W3 = 40/80/56 at every player count — the highest tier with a
*measured* strength gain (C evals, 300 games/pc vs handwritten):
default → 40/80/56 improved mean finish and win rate at pc 4/6/8
(2.050→1.990, 2.910→2.767, 4.183→4.157); doubling again to 80/160/112 was
NOT reliably better (pc6 regressed, pc4 flat), so the budget stops there
even though wall-clock allows far more. Measured TS decision times for
cordite_max: mean ~15 ms, p95 ~60 ms, max ~200 ms — comfortably inside
the 2 s/decision target; a 1.9 s wall-clock cap in `cordite_core.ts`
guarantees the bot loop can never stall on a pathological position.

## Server-side notes

Same TS-port surface as blackpowder: belief builder (one log pass), CRN
sampler, rollouts, endgame solver. No schema or server changes; no bot
memory table required. The "bot memory" idea from the kickoff brief was
explored and found unnecessary for everything cordite does — all state is
derivable per-call from `game.logs`; a persistent table would only pay for
cross-game opponent modeling (e.g. learning a specific human's bluffing
habits), which remains future work.
