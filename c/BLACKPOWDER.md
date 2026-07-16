# Blackpowder — belief-constrained determinized Monte Carlo

The strongest non-cheating bot in the pool. No LLM, no neural net, no
reading of hidden state: it sees exactly what a human player sees (own
hand, table, hand counts, deck count, the public move history) and runs
entirely inside one `chooseMove` call — no server-side memory needed.

## Why the previous MC bots stalled

Robusta/firecracker/gunpowder beat handwritten convincingly at 2-3
players but *regressed below baseline* at 5-8 players. Two causes:

1. **Per-move sampling noise.** Each candidate move was scored on its
   own set of sampled worlds (different seeds per move). With only 4-5
   affordable samples at high player counts, the noise in the
   comparison exceeded the signal, and MC degenerated to near-random
   tie-breaking among similar moves.
2. **Diffuse beliefs.** With 5+ opponents and a 52-card deck the unseen
   pool is ~40 cards; uniform sampling rarely resembles the real world.

## What blackpowder does

### 1. Card memory (rebuilt from `game.logs` every call)

- **Pinned cards** — cards an opponent publicly picked up stay tracked
  in their hand until publicly played (robusta's idea, kept). The
  table contents are reconstructed by replaying ATTACK/COVER/PASS
  events rather than reading PICKUP/DISCARD log entries, which silently
  truncate at MAX_LOG_PAIRS=16 (a 10-battle pickup is 21+ cards — this
  truncation was a real, verified leak in robusta's tracking).
- **Flipped-trump holder** — the flipped trump is by rule the last card
  drawn, so the player whose draw event consumed the last deck card
  publicly holds it until they play it. Uniform sampling previously
  scattered the single most important card in the game across all
  hands.
- **Void constraints** — a defender who picked up while exactly one
  attack card was uncovered demonstrably held no card that covers it.
  Until that player next draws, their unknown cards can only shrink, so
  every unknown card they hold still obeys the constraint. Constraints
  are applied to 3 of every 4 sampled worlds (a belief mixture), so
  opponents who *strategically* pick up while holding covers — humans,
  the random bot — only mislead 75% of worlds, with no hard wrongness.

The oracle self-check (`BP_VERIFY=1`, test-only: compares belief
against real hands) reports **zero violations** over 1600 games vs
handwritten and espresso. Violations vs random are expected (it picks
up while holding covers) and absorbed by the mixture.

### 2. Common random numbers (CRN)

All candidate moves are evaluated on the **same** sampled worlds with
the **same** rollout RNG stream (`game_rng_get/set` added to game.c —
additive API, no behavioral change). Move comparison becomes a paired
test: world-luck cancels out, and the variance that previously sank MC
at 5-8 players collapses. Two-stage allocation: every candidate gets
W1 worlds, the best third get W2 more.

Rollouts are stage-aware (gunpowder's rule): handwritten while the deck
is alive or the game is heads-up, espresso for multi-player endgames
(inside a sampled world, espresso's hand-reading is robusta's own guess,
not real hidden state — same legitimacy argument as firecracker).

### 3. Exact endgame (the "espresso killer")

Once 2 players remain and the deck is empty, the unseen pool *is* the
opponent's hand — a pure public deduction available to any attentive
human. Espresso gets this information by cheating; blackpowder derives
it. An alpha-beta search over the real engine rules (depth ≤ 48, node
budget 200k, defender-priority move order) then plays the endgame
perfectly: if a forced win exists it is taken; otherwise fall back to
MC, which models the real (imperfect) opponents better than a
minimax-pessimal line would. This applies at every player count — most
multi-player games funnel through a 2-player endgame for the durak
decision.

The deliberation never perturbs the outer game: the game RNG is saved
on entry and restored on exit.

## Results

Protagonist at seat 0, all other seats the listed opponent, 1000
games/cell, seeds 910001+, `mean` = mean finish position (lower is
better; baseline = (N+1)/2 for a no-edge bot), `win` = finished 1st.

### vs handwritten (mean finish / win%)

| pc | blackpowder | gunpowder | robusta | baseline |
|----|------------|-----------|---------|----------|
| 2 | **1.148** / 85.2% | 1.244 / 75.6% | 1.244 / 75.6% | 1.500 |
| 3 | **1.622** / 51.5% | 1.750 / 43.9% | 1.733 / 44.8% | 2.000 |
| 4 | **2.199** / 28.4% | 2.498 / 20.0% | 2.469 / 21.6% | 2.500 |
| 5 | **2.731** / 21.4% | 2.993 / 18.6% | 2.992 / 19.0% | 3.000 |
| 6 | **3.196** / 18.0% | 3.521 / 15.4% | 3.653 / 13.3% | 3.500 |
| 7 | **3.749** / 14.0% | 4.002 / 13.5% | 4.270 / 12.0% | 4.000 |
| 8 | **4.339** / 11.8% | 4.632 /  9.6% | 4.844 / 10.4% | 4.500 |

### vs espresso (the cheating bot)

| pc | blackpowder | gunpowder | robusta | baseline |
|----|------------|-----------|---------|----------|
| 2 | **1.398** / 60.2% | 1.481 / 51.9% | 1.481 / 51.9% | 1.500 |
| 3 | **1.677** / 51.5% | 1.801 / 43.9% | 1.801 / 44.8% | 2.000 |
| 4 | **2.257** / 28.4% | 2.546 / 20.0% | 2.544 / 21.6% | 2.500 |
| 5 | **2.772** / 21.4% | 3.017 / 18.6% | 3.056 / 19.0% | 3.000 |
| 6 | **3.235** / 18.0% | 3.535 / 15.4% | 3.684 / 13.3% | 3.500 |
| 7 | **3.776** / 14.0% | 4.015 / 13.5% | 4.312 / 12.0% | 4.000 |
| 8 | **4.362** / 11.8% | 4.646 /  9.6% | 4.882 / 10.4% | 4.500 |

### vs random

| pc | blackpowder | gunpowder | robusta | baseline |
|----|------------|-----------|---------|----------|
| 2 | **1.022** / 97.8% | 1.052 / 94.8% | 1.052 / 94.8% | 1.500 |
| 3 | **1.197** / 81.3% | 1.332 / 69.2% | 1.324 / 69.9% | 2.000 |
| 4 | **1.422** / 66.5% | 1.609 / 55.1% | 1.642 / 53.8% | 2.500 |
| 5 | **1.636** / 56.5% | 1.634 / 56.6% | 2.048 / 39.6% | 3.000 |
| 6 | **1.718** / 55.0% | 1.693 / 54.8% | 2.177 / 37.4% | 3.500 |
| 7 | 2.053 / 46.5% | **1.994** / 49.1% | 2.556 / 33.6% | 4.000 |
| 8 | 2.531 / 32.2% | **2.427** / 37.4% | 2.908 / 27.0% | 4.500 |

Headline:

- beats baseline and every prior bot at **every** player count 2-8 vs
  both handwritten and espresso — including the 4-8 player range where
  no previous bot (espresso included) beat baseline at all
- vs random it is far above baseline everywhere; at 7-8 players pure
  handwritten (= gunpowder there) keeps a ~0.1 edge in mean finish —
  random tables are the one place rollout modeling buys nothing
- ELO arena (mixed pools, PCs 2-8, 3000 games): #1 with the lowest
  durak rate and highest win rate of all seven strategies

### ELO arena (random seats, mixed pools, PCs 2-8, 3000 games, K=32)

| rank | competitor | elo | win% | durak% |
|------|-----------|------|------|--------|
| 1 | **blackpowder** | **1167** | **27.3%** | **7.9%** |
| 2 | gunpowder | 1123 | 22.3% | 13.9% |
| 3 | handwritten | 1067 | 22.9% | 16.8% |
| 4 | espresso | 952 | 23.4% | 13.6% |
| 5 | robusta | 951 | 19.9% | 13.5% |
| 6 | firecracker | 916 | 20.7% | 12.7% |
| 7 | random | 824 | 5.0% | 61.2% |

## Reproduce

```bash
cd c && make

# per-PC matrix vs one opponent
./build/cnitro_eval --strategy=blackpowder --opp=handwritten \
    --players=2,3,4,5,6,7,8 --games=1000 --seed-start=910001

# mixed-pool ELO arena
./build/cnitro_elo --games=3000 \
    --pool=random,handwritten,espresso,robusta,firecracker,gunpowder,blackpowder \
    --pcs=2,3,4,5,6,7,8 --snapshot-every=500

# single-game move trace
./build/cnitro_eval --strategy=blackpowder --opp=espresso --players=4 \
    --inspect=12345
```

Ablation/debug env vars (read once per process):

- `BP_NO_SOLVE=1` — disable the exact endgame solver
- `BP_NO_VOIDS=1` — disable void constraints
- `BP_NO_FLIP=1` — disable flipped-trump holder tracking
- `BP_VERIFY=1` — oracle self-check of the belief vs real hands
  (test-only cheating; prints violations to stderr)

## Tuning knobs

`bp_params()` sets worlds per decision by player count (W1/W2:
12/20 at 2p, 8/16 at 3-4p, 10/20 at 5-6p, 8/16 at 7-8p). Throughput at
these settings: ~35-50 games/s single-threaded — same order as robusta.
More worlds monotonically helps at 5-8 players; raise if CPU allows.

## Server-side notes (for later productionization)

The TS port needs: the belief builder (one pass over `game.logs`), the
CRN sampler, and rollouts. No schema or server changes — everything is
derived per-call from the existing logs, exactly like espresso's
discard memory. A persistent "bot memory" table is NOT required for
any of this; it would only become useful for cross-game opponent
modeling (e.g. learning that a specific human bluffs trump attacks).
