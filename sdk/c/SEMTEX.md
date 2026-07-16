# Semtex — cordite's successor

Semtex is cordite (belief-constrained determinized Monte Carlo, see
CORDITE.md) plus three levers that beat cordite head-to-head and one that
exploits weak opponents harder — with the explicit design constraint that
**no lever may trade strength against one opponent family for weakness
against another** (no rock-paper-scissors: semtex must be ≥ cordite against
*every* field, not just the cordite mirror). Same legitimacy contract:
public info only, no LLM, no reading hidden state, everything computed
inside one `chooseMove` call.

## The levers

### 1. Exact leaf endgames inside rollouts (bitboard solver)

Cordite finishes every rollout with handwritten policy play, including the
2-player deck-empty endgame — a phase its *own root solver* plays exactly.
Against opponents that also play endgames exactly (cordite itself, strong
humans), evaluating candidate moves under "both sides fumble the endgame"
biases every value estimate. Semtex resolves small 2-player deck-empty
positions inside rollouts with the fast bitboard solver
(`cd_sim_playout_leaf` / `cd_sim_playout_pol` in `cordite_sim.c`; sign-only
null window, one attempt per playout, unresolved solves fall back to policy
play). The transposition table persists across the worlds of a decision —
keys are exact 64-bit position fingerprints and values are depth-rebased,
so cross-world reuse is sound.

Final config: **small (8-card) leaves, heads-up only**. Loss analysis
(paired `--dump` + move-trace diffs on regression seeds) showed larger
(12-card) leaves at 3+ players inject "the endgame will be played
perfectly" into mid-game values — individually terrible calls (a trump-K
cover of a 6 with a pass available; passive pickups; held aces) that
exactly cancel the good calls (paired mirror delta ~0 at pc3-8 across two
seed sets) while costing ~3x wall-clock. CORDITE.md's negative CD_LEAF
result (exact leaves measured worse vs imperfect opponents) reproduces
here for large leaves; the small heads-up leaf is the part that
replicates as a win.

### 2. Extended exact root-solve window

Cordite's root endgame solver engages at ≤ 20 total cards. The bitboard
solver + TT resolves much larger endgames within budget, so semtex engages
at ≤ 24 cards with bigger node budgets (`SX_BB_WIN` 150k / `SX_BB_AVOID`
100k vs cordite's 20k/15k) — a window where semtex plays proven-optimal
moves while cordite is still sampling. This is a pure "more truth" lever:
it only ever takes proven wins and excludes proven losses (with cordite's
adverse-selection guard kept intact), so it cannot lose to anyone — 
measured bit-identical outcomes vs handwritten fields (400/400 equal
pairs) and better vs cordite and espresso fields.

### 3. Per-seat MC-tells (evidence-gated belief adaptivity)

Cordite's rank-floor and void inference model *heuristic-family* opponents
(lowest-first attackers, cover-if-you-can defenders). Against MC opponents
(cordite/blackpowder) and thinking humans those inferences corrupt the
sampled worlds. Turning them off globally is the classic
rock-paper-scissors trap (it tunes the bot to the cordite mirror and costs
strength against heuristic fields — measured, and it did not even replicate
across seed sets vs cordite). Semtex instead drops them **per seat, on
proof**:

- **Void contradiction**: a seat plays a card that an *active* void said it
  couldn't hold (and that wasn't publicly picked up) ⇒ it picked up
  strategically while holding cover. Heuristic-family defenders never do
  this; MC bots and humans do it all the time.
- **Declined attack**: a seat says GOOD while the defender has spare
  capacity, then later — before gaining any cards — plays a non-trump card
  whose value was on the table at GOOD time ⇒ it declined a legal
  attack. A handwritten-family attacker never declines a legal non-trump
  attack.

Either tell marks the seat "strategic" for the rest of the game (sticky —
the tell is about the player, not the current hand) and drops floors and
voids for that seat only. Heuristic opponents keep full inference pressure.

### 4. Weak-seat exploitation

- **C** (`SX_PROFILE`, default off — research knob): seats that burn trumps
  while the deck is alive at rates no strong bot exhibits are rolled out
  with a LOOSE random-ish model (`CD_POL_LOOSE`) instead of handwritten.
- **TS** (shipped): semtex wires fulminate's proven posterior-mixture
  profiler (`profileSeats` / `seatWeightsFromProfiles`) — the biggest
  measured lever against weak/human-like fields (+17 to +24.5pp win vs
  simple_heuristic at pc4/pc6, CORDITE_RESEARCH.md) — on top of the semtex
  core levers. The profiler's conservative gate (strong-seat mislabel rate
  0.30%) keeps strong seats on the cordite-identical default.

## Rejected / not shipped

- **Global NO_FLOORS / NO_VOIDS / soft void mixture** — won the cordite
  mirror on one seed set (up to −0.12 mean finish at pc4), failed to
  replicate on fresh seeds, and cost strength vs handwritten fields
  (floors-off pc3: +0.040 mean). Cut per the no-RPS rule; the MC-tells
  capture the defensible part adaptively.
- **Espresso rollout policy everywhere** (`SX_ROLLOUT=1`) — slightly better
  at pc2 vs cordite, clearly worse at pc4 (2.745 vs control 2.620). Cut.
- **Large heads-up exact leaves** (12+ cards at pc2) — flat vs cordite,
  small consistent cost vs handwritten (+0.030 mean, 88.0→85.0% win). The
  8-card version keeps the cordite gain without the handwritten cost.

## Evaluation methodology

`cnitro_eval --control=<strategy>` (added with this work) plays every seed
**twice on the same deal** — hero at seat 0, then the control bot at seat 0,
same opponents everywhere else — and reports the paired finish-position
delta with its standard error. Same-deal pairing cancels deal luck; a
structural change shows up in tens of pairs instead of hundreds of
independent games. All numbers below are semtex-vs-cordite-control paired
deltas (negative = semtex better; positions 1..N, lower is better).

## Results

All paired vs the cordite control on fresh seeds 930001 (400 pairs/cell,
300 for blackpowder), final shipped defaults. `Δ` = hero mean finish −
control mean finish (negative = semtex better), `±` = SE of the paired
delta, `b/w/e` = pairs where semtex finished better / worse / equal.

### Heads-up vs cordite — the decisive edge

| field | Δ mean finish | win% semtex | win% cordite | b/w/e |
|---|---|---|---|---|
| cordite pc2 | **−0.185 ± 0.034** | **66.5%** | 48.0% | 135/61/204 |
| blackpowder pc2 | −0.140 ± 0.039 | 71.7% | 57.7% | 92/50/158 |
| espresso pc2 | −0.050 ± 0.033 | 79.7% | 74.7% | 56/41/203 |
| handwritten pc2 | −0.033 ± 0.024 | 91.3% | 88.0% | 31/21/248 |

The pc2 cordite-mirror result is >5σ on same-deal pairs: the 6x world
budget + small exact rollout leaves + the extended solve window + the
MC-tells stack into a +18.5pp win-rate edge over cordite on identical
deals, confirmed at −0.168/−0.138 (+16.8/+13.7pp) on two more seed sets.

### The worlds finding (second loss-audit cycle)

An oracle audit of the 68 pc2 deals semtex (pre-6x) lost the mirror on:
a 6x-worlds probe disagreed with 27.4% of hero decisions, and PLAYING
those deals with 6x worlds won 66% of them, against a 10% rescue rate
for a same-budget noise perturbation — the pc2 losses were
**variance-limited, not model-limited**. The gain is entirely worlds
(wider candidate survival alone matched the old default), 2x/3x were not
reliable stops, and the "pc2 is world-saturated" conclusion in
CORDITE_RESEARCH.md does not transfer to the cordite mirror (it was
measured on weaker fields and on the TS engine, which already ran ~3x
the C budget). Because it is a variance fix, it helps against every
field (all four rows above improved). Baked as semtex's heads-up
budget: W1/W2/W3 = 192/336/336.

### 3+ players — strict dominance, small margins

With the final defaults (no rollout leaves at 3+), semtex diverges from
cordite rarely at 3+ players, and when it does it is right: across the
2,400 paired games of the I-runs (cordite pc5/pc8, handwritten pc3/4/6,
espresso pc4) it finished **better in 11 pairs, worse in 0**, identical
in the rest. The earlier full-matrix run (12-card leaves at 3+ still on)
showed the same shape at pc3–pc8 vs every field — no cell significantly
negative, pc4/pc5 mildly positive vs cordite (−0.043 ± 0.042 /
−0.030 ± 0.034), espresso pc4 −0.050 ± 0.027.

### ELO arena (random seats, mixed pools, PCs 2-8, 3000 games, K=32)

Two runs (seeds 1 / 777001), 9-bot pool = the CORDITE.md pool + semtex.
The K=32 sequential arena is a noisy instrument — game order alone moves
final ratings by >100 points (it even ranks blackpowder above cordite,
contradicting every direct head-to-head) — so the paired same-deal evals
above are the primary evidence; the arena is a sanity check:

| run | semtex | cordite | semtex win% / durak% | cordite win% / durak% |
|---|---|---|---|---|
| seed 1 | 1090 (#4) | 1094 (#3) | 26.6% / **8.3%** | 27.9% / 8.4% |
| seed 777001 | **1252 (#1)** | 1125 (#3) | **29.0%** / 8.2% | 26.8% / 7.9% |

Semtex ranks at-or-above cordite in both runs and never below any other
bot's tier.

### Hunt 3 — the 3+ player worlds finding

The same audit methodology at pc4/pc6: mirror losses there are also
compute-limited (oracle rescue 31%/23% vs noise 3%/0%), but the aggregate
mirror gain is small because opponent MC noise dominates MC-vs-MC
outcomes. The payoff surfaces against **heuristic fields** (the human
proxies): 6x worlds at pc3/pc4 is worth +5-8pp win vs the cordite control
(pc3 67.2% vs 59.5%, pc4 39.8% vs 34.8%, seeds 950001; attribution clean —
the default played 400/400 of those deals identically to cordite). pc2 is
saturated at its 6x (18x measured identical). Baked: C budgets at 3+
raised to 6x (pc5-8 = the ratios production TS cordite already ships).

Final fresh-seed matrix (960001, 400 pairs/cell, all levers baked):
every cordite-mirror cell negative — pc3 −0.105±0.057, pc4 −0.068±0.073,
pc5 −0.080±0.074, pc6 −0.085±0.108, pc8 −0.092±0.108 (jointly ~2.2σ, on
top of the pc2 −0.185 at 5σ) — and vs handwritten fields pc6 −0.218±0.082,
pc8 −0.120±0.087, pc4 −0.065±0.059, pc3 −0.028±0.043, pc5 +0.065±0.055
(the one ~1σ positive cell; within noise).

Supabase budget: base `semtex` TS params are unchanged (identical CPU
cost); `semtex_max` carries the full C-measured budget at every player
count (measured pc4 decision mean 104 ms / max 454 ms on a slow 4-core
dev box — far under the 2 s cap).

### No rock-paper-scissors

Semtex was never significantly worse than cordite in any of the ~25
validated (field × player-count) cells — cordite, handwritten, espresso,
random, and blackpowder tables. The one lever family that *did* trade
cordite-mirror strength for heuristic-field weakness (globally disabling
floor/void inference) was cut for exactly that reason.

## Knobs (read once per process, `SX_*` — semtex only, never the cordite opponents)

- `SX_BBLEAF` — 2 (default) pc-aware exact rollout leaves; 1 = everywhere;
  0 = off. `SX_BBLEAF_CARDS` (0 — 3+ leaves off by default) /
  `SX_BBLEAF_CARDS2` (8, heads-up) / `SX_BBLEAF_BUDGET` (3000).
- `SX_SOLVE_CARDS` (24) — root endgame-solve ceiling; `SX_BB_WIN` (150000) /
  `SX_BB_AVOID` (100000) — solver node budgets.
- `SX_ADAPT` (1) — per-seat MC-tells; `SX_PROFILE` (0) — C weak-seat
  profiler + LOOSE rollouts.
- `SX_VOID_MOD` (4) / `SX_FLOOR_MOD` (2) — belief world-mixture.
- Cordite's ablation knobs are inherited under the `SX_` prefix
  (`SX_NO_SOLVE`, `SX_NO_VOIDS`, `SX_NO_FLOORS`, `SX_W1/2/3`, ...).

## Production TS port

`supabase/functions/_shared/common/strategies/semtex_strategy.ts` +
`SemtexOpts` hooks in `cordite_core.ts` (null opts ⇒ the engine is
bit-for-bit cordite; clean-tree fingerprint reproduced). Registered as
`semtex` / `semtex_max` in `bot_strategy.ts`, seeded in `seed.sql`
(Semtex 1–3, Semtex Max 1–3). TS-specific adaptations: rollout-leaf node
budget 600 (a TS solver node costs ~50x a bitboard node) and a fail-memo
so unresolvable leaves are attempted once per decision, not once per
world.

TS validation (`cordite_arena.ts`, hero seat 0 vs all-cordite tables,
same seeds hero/control): pc2 semtex 64.0% / 1.360 vs control 63.0% /
1.370 (100 games — the TS harness gives seat 0 a large pc2 advantage,
compressing the visible edge; the C paired result is the reliable
measurement of the same algorithm), pc4 semtex 18.3% / 2.713 vs control
18.7% / 2.650 (300 games, within noise, matching the C paired ~0). The
unpaired TS cells are noisy (a 100-game control cell moved 28.0% -> 18.7%
at 300 games); strength conclusions come from the C paired harness,
which plays the identical algorithm. Decision latency (4-core dev box):
pc2 mean 422 ms / p99 1.28 s, pc4 mean 116 ms / p99 0.64 s -- inside the
2 s bot-loop cap.
