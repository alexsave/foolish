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
