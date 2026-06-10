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

### Rejected: exact leaf endgames inside rollouts

Solving small 2-player deck-empty endgames exactly *inside rollouts*
(legitimate — full info inside a sampled world) was implemented and
measured: it made cordite both ~10x slower and **weaker** (pc2 vs
handwritten: 1.150 → 1.240 mean). Modeling the actual imperfect opponent
(handwritten plays the endgame in the rollout) beats assuming perfect
play, and beats it at a fraction of the cost. Kept behind `CD_LEAF=1` as a
negative result worth remembering.

## Results

Protagonist at seat 0, all other seats the listed opponent,
seeds 910001+ (fresh-seed confirmations at 920001+), `mean` = mean finish
position (lower is better; baseline = (N+1)/2), `win` = finished 1st.

> Benchmark tables are written by `bench_cordite.sh` to
> `/tmp/cordite_bench/*.txt` — final 1000-game matrices below.

### vs blackpowder tables — the matchup that matters

Seat-0 has a measurable disadvantage in this harness, so the fair
comparison is against the **bp-vs-bp control** (blackpowder itself at
seat 0 of an all-blackpowder table), not the neutral baseline:

| setup (pc4, 400 games, seeds 920001+) | mean | win% |
|---|---|---|
| blackpowder @ blackpowder table (control) | 2.632 | 23.2% |
| **cordite @ blackpowder table** | **2.320** | **30.0%** |

Cordite beats blackpowder's own seat by 0.31 mean finish — and beats the
2.5 neutral baseline *on a hostile table*.

### vs handwritten (200 games, same seeds, pre-final-bench snapshot)

| pc | cordite | blackpowder |
|----|---------|-------------|
| 2 | **1.090** / 91.0% | 1.175 / 82.5% |
| 4 | **1.900** / 44.0% | 2.225 / 29.0% |
| 6 | **3.035** / 21.0% | 3.315 / 16.0% |
| 8 | **4.110** / 10.5% | 4.170 / 11.0% |

(Final 1000-game matrices vs handwritten / espresso / random and the
mixed-pool ELO arena: see `/tmp/cordite_bench/` outputs, summarized in the
session report.)

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
- `CD_LEAF=1` — re-enable exact leaf endgames in rollouts (measured worse)
- `CD_W1/CD_W2/CD_W3` — world-count overrides
- `CD_FULL_LOGS=1` — blackpowder-style full-log worlds (A/B; identical)
- `CD_NO_EARLYEXIT=1` / `CD_BP_SOLVE=1` — debug A/B switches used while
  isolating the tie-break bug
- `CD_VERIFY=1` — oracle self-check of the belief vs real hands (test-only)

## Server-side notes

Same TS-port surface as blackpowder: belief builder (one log pass), CRN
sampler, rollouts, endgame solver. No schema or server changes; no bot
memory table required. The "bot memory" idea from the kickoff brief was
explored and found unnecessary for everything cordite does — all state is
derivable per-call from `game.logs`; a persistent table would only pay for
cross-game opponent modeling (e.g. learning a specific human's bluffing
habits), which remains future work.
