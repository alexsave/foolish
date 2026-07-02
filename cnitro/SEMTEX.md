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

Player-count aware: 12-card leaves at 3+ players; **8-card** leaves
heads-up. CORDITE.md records that exact leaves measured *worse* vs
imperfect opponents (CD_LEAF, struct solver, vs handwritten) — that result
reproduces here for *large* heads-up leaves, so pc2 keeps only small leaves
(which any opponent plays near-optimally anyway, and which still fix the
rollout's evaluation of them).

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
| cordite pc2 | **−0.135 ± 0.034** | **61.5%** | 48.0% | 122/68/210 |
| espresso pc2 | −0.050 ± 0.031 | 75.8% | 70.8% | 85/65/250 |
| blackpowder pc2 | −0.050 ± 0.042 | 62.7% | 57.7% | 86/71/143 |
| handwritten pc2 | −0.003 ± 0.024 | 88.0% | 87.8% | 45/44/311 |

The pc2 cordite-mirror result is 4σ on same-deal pairs: the small exact
rollout leaves + the extended solve window + the MC-tells stack into a
+13.5pp win-rate edge over cordite on identical deals.

### 3+ players — strict dominance, small margins

With the final defaults (no rollout leaves at 3+), semtex diverges from
cordite rarely at 3+ players, and when it does it is right: across the
2,400 paired games of the I-runs (cordite pc5/pc8, handwritten pc3/4/6,
espresso pc4) it finished **better in 11 pairs, worse in 0**, identical
in the rest. The earlier full-matrix run (12-card leaves at 3+ still on)
showed the same shape at pc3–pc8 vs every field — no cell significantly
negative, pc4/pc5 mildly positive vs cordite (−0.043 ± 0.042 /
−0.030 ± 0.034), espresso pc4 −0.050 ± 0.027.

### No rock-paper-scissors

Semtex was never significantly worse than cordite in any of the ~25
validated (field × player-count) cells — cordite, handwritten, espresso,
random, and blackpowder tables. The one lever family that *did* trade
cordite-mirror strength for heuristic-field weakness (globally disabling
floor/void inference) was cut for exactly that reason.

## Knobs (read once per process, `SX_*` — semtex only, never the cordite opponents)

- `SX_BBLEAF` — 2 (default) pc-aware exact rollout leaves; 1 = everywhere;
  0 = off. `SX_BBLEAF_CARDS` (12) / `SX_BBLEAF_CARDS2` (8, heads-up) /
  `SX_BBLEAF_BUDGET` (3000).
- `SX_SOLVE_CARDS` (24) — root endgame-solve ceiling; `SX_BB_WIN` (150000) /
  `SX_BB_AVOID` (100000) — solver node budgets.
- `SX_ADAPT` (1) — per-seat MC-tells; `SX_PROFILE` (0) — C weak-seat
  profiler + LOOSE rollouts.
- `SX_VOID_MOD` (4) / `SX_FLOOR_MOD` (2) — belief world-mixture.
- Cordite's ablation knobs are inherited under the `SX_` prefix
  (`SX_NO_SOLVE`, `SX_NO_VOIDS`, `SX_NO_FLOORS`, `SX_W1/2/3`, ...).

## Production TS port

`supabase/functions/_shared/strategies/semtex_strategy.ts` +
`SemtexOpts` hooks in `cordite_core.ts` (null opts ⇒ the engine is
bit-for-bit cordite; clean-tree fingerprint reproduced). Registered as
`semtex` / `semtex_max` in `bot_strategy.ts`, seeded in `seed.sql`
(Semtex 1–3, Semtex Max 1–3). TS-specific adaptations: rollout-leaf node
budget 600 (a TS solver node costs ~50x a bitboard node) and a fail-memo
so unresolvable leaves are attempted once per decision, not once per
world. Offline decision latency: p95 well under the 2 s bot-loop cap.
