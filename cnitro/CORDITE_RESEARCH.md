# Cordite strength research — can more compute make it win more?

This is an honest write-up of a focused effort to make **cordite** (the deployed
belief-constrained determinized Monte-Carlo Durak bot) measurably stronger by
spending more compute per decision, and to speed it up so a bigger budget is
affordable inside the production ~2s/decision window.

Short version: **cordite is largely compute-saturated.** Throwing more sampled
worlds at it buys almost nothing against strong/equal opponents. There is no
10×/100× strength multiplier hiding in "more rollouts." The gains that *are*
real come from three narrow levers: (1) a TS engine speed-up that makes a bigger
budget fit in 2s, (2) giving **high player counts** more worlds (they were
variance-starved, not saturated), and (3) making the **rollout policy match the
real opponent** instead of one hardcoded policy — pursued as a separate bot,
`fulminate`.

## How it was measured

- **cnitro** (the C engine, `cnitro/`): fast batched self-play for sweeps.
  Knobs added for the study: `CD_W1/2/3` and `CD_WORLDMUL` (world budget),
  `CD_KEEP1/CD_KEEP2` (candidate-survival breadth), `CD_ROLLOUT` (force the
  rollout policy), `CD_LEAF`/`CD_NO_SOLVE` (leaf/endgame solver), and a
  `cordite_old` strategy = cordite at the pre-study budget, for head-to-head.
  OpenMP fans games across cores (`make OMP=1`).
- **TS** (the deployment target, `supabase/functions/_shared/common/strategies/`):
  parallel arenas under `offlinefun/localtest/` — `cordite_arena.ts` (hero vs a
  filler field) and `cordite_h2h.ts` (NEW seat-0 vs the **frozen origin/main
  cordite** in the other seats; "fair" win share is 1/n). The final word is
  always TS, since that is what ships.

A note on noise: win% at 80–240 games/cell has a standard error of ~1.5–3.5pp,
so single-cell win% deltas under ~5pp are weak evidence. **Mean finish position**
(lower is better) is the lower-variance signal and is weighted accordingly.

## Finding 1 — worlds saturate against strong opponents

Sweeping the world budget (1×→8×) against strong/equal opponents (cordite-mirror,
cordite_old, espresso) moved strength essentially not at all at pc2/pc4. In TS,
the cleanest demonstration (NEW bot only, scaled via `CD_WORLDMUL`, vs the frozen
old cordite at pc4, 160g):

| worlds | win% | mean finish | dec mean |
|-------:|-----:|------------:|---------:|
| 1× (deployed) | 28.1 | **2.313** | 132 ms |
| 4× | 23.1 | 2.513 | 470 ms |
| 8× | 24.4 | 2.494 | 870 ms |

More worlds at pc4 did **not** help (mean finish actually drifted back toward the
baseline 2.50). The C sweeps agreed: a "kitchen-sink" max config (huge worlds +
wide candidate survival + leaf-solve) vs `cordite_old` came out ~even (~48–51%).
Bias, not variance, is the limit at low player counts.

## Finding 2 — high player counts were variance-starved, not saturated

The opposite holds at pc6/pc8. With 6–8 way hidden state, the determinized
sample is too thin and the default budget actually finished **below** the 1/n
fair share. More worlds fixes it (TS, NEW-only `CD_WORLDMUL` vs old, pc6, 160g):

| worlds | win% | mean finish |
|-------:|-----:|------------:|
| 1× | 12.5 | 3.706 |
| 2× | 18.8 | 3.469 |
| 3× | 18.1 | 3.456 |
| 4× | 24.4 | 3.394 |

A methodology trap surfaced here: under the production `maxMillis=2000` cap the
number of worlds completed depends on wall-clock load, so capped multi-worker
runs are **non-deterministic** and a first "confirmation" looked like no gain.
Re-running uncapped (`MAXMS=10000`, full budget every decision → reproducible)
at higher N settled it — the shipped v2.4 budget is a genuine pc6 improvement,
and going beyond it does not help (deterministic, 300 games, vs old cordite):

| pc6 budget | win% | mean finish |
|------------|-----:|------------:|
| v2.3 (old, 1×) | 14.0 | 3.707 (below fair 16.7%/3.50) |
| **v2.4 (shipped, 2×)** | **19.3** | **3.440 (above fair)** |
| 4× (2× v2.4) | 18.7 | 3.580 (no further gain) |

The gain plateaus by ~2×; single-core p99 at 2× is ~1.2 s (well under the cap).
**Shipped (v2.4):** pc6/pc8 get a clean 2× the low-count world budget
(`[240,480,336]`/`[240,480,288]`), pc2/pc4 unchanged (saturated), `maxMillis`
2000 still caps the rare long decision gracefully. (The pc8 deterministic
confirmation was deprioritized — pc8 is rare in play and the direction matches
pc6; the cordite track was parked here as clearly diminishing.)

## Finding 3 — the one algorithmic lever: rollout-policy realism

Cordite evaluates every sampled world by rolling it out with a single hardcoded
policy. If that policy is a poor model of the actual opponent, every value
estimate is biased. The C study found this is the only thing that reliably moved
strength: using a **stronger** rollout policy (espresso) instead of the default
beat the default specifically against a different-style opponent (blackpowder:
+2pp win, better mean over 600g), while doing nothing in the cordite mirror.
This motivated `fulminate` (below) — model each opponent in-game and roll *them*
out accordingly.

## Finding 4 — the deployable win is the speed-up, not "more thinking"

The TS engine was made ~11–20% faster per decision and ~20% lighter on GC
(pooled rollout trials, an allocation-free fast rollout chooser, a cheaper
transposition-table fingerprint) — all **behavior-preserving** (a full-game
outcome fingerprint is bit-identical, `hash` unchanged). That headroom is what
makes the v2.4 budget affordable inside 2 s. The exact endgame solver (already in
production) dominates pc2 latency and is kept — it is the source of the strong
endgame play and is node-count bound, not allocation bound.

## Net: what shipped to cordite

- **v2.3** engine speed-up (behavior-identical) + ~3× worlds and `maxMillis`
  1500→2000, the freed budget partly spent on wider candidate survival.
- **v2.4** player-count-aware world budget: 2× at pc6/pc8 to remove the
  high-player-count regression; pc2/pc4 left saturated.

Honest bottom line: vs the previously-deployed cordite, the new cordite is a
**small** net improvement (a few points at pc2, ~even at pc4 with a better mean,
and pc6/pc8 lifted from *below* fair to *above* fair). It is not a 10×/100×
jump, because no such jump exists for this algorithm against strong play. The
larger upside is opponent modeling.

## The opponent-modeling direction: `fulminate`

`fulminate` = cordite + **in-game** opponent profiling (no cross-game learning,
to stay a general bot). It reads the current game's public log to (a) classify
each opponent seat — random / greedy / simple / strong — from rare-for-strong
tells and a trump-conservation rate, (b) reconstruct partial opponent hands
including *negative* inference (a beatable attack left uncovered implies no
cover), and (c) roll out each seat with **its** inferred policy. The engine hook
is default-off, so cordite stays bit-identical (fingerprint verified). Early
game with no logs it behaves like cordite and only commits to a "weak" label
under a high, sample-gated confidence bar.

Same-table A/B vs cordite (seat 0 = fulminate, seat 1 = cordite, rest = fillers),
Δwin = fulminate − cordite:

| field | pc4 Δwin | pc6 Δwin |
|-------|---------:|---------:|
| simple_heuristic (weak) | **+16.9** | **+13.8** |
| handwritten | +8.1 | −3.1 |
| espresso (strong) | −2.5 | −5.0 |

Big, real wins against exploitable opponents; roughly neutral vs strong fields
(the small negatives are within noise and being verified at higher N). This is
the most promising lever found — exploiting weak/human/random opponents harder
while playing tight against strong ones — and it is exactly where a bot facing
real humans stands to gain the most.

## Truncated rollouts + positional leaf eval — NEGATIVE result (do not re-run blind)

Hypothesis: truncate every playout after K plies and score the leaf with a
positional estimate (rank by hand size; variant orders card-count ties by
hand strength Σ(value + 20·trump)); rollout cost should drop several-fold at
small strength cost. Implemented behind CD_TRUNC=K / CD_TRUNC_EVAL (kept
default-off in an experiment branch, not merged) and swept K ∈ {12..64} ×
both evals, 400-game evals vs espresso and handwritten, 800-game finalist.

Rejected: the speed–strength frontier never crosses "within noise". K=12
buys ~3x but costs ~10% win rate at pc4; K=32 (best tradeoff, RANK+TRUMP)
still costs ~5% at pc4 (−4.8%, ≈2.8σ at 800 games) and −3% at pc6; by
K=48–64 the speedup decays to ~1.05–1.2x while pc4 stays depressed — the
bias from replacing the playout's opponent-modeling tail with a static
positional guess dominates long before the saved plies stop mattering. Only
pc2 tolerates truncation (its strength rides the exact endgame solver, not
rollouts). A future attempt needs a leaf that models table state and pickup
pressure, not card counts. Racing (CD_RACE, adopted) is the safe way to cut
rollout spend: it prunes redundant WORLDS on easy decisions instead of
biasing every playout.

## Distilling cordite into a linear policy — NEGATIVE result (tooling kept)

Hypothesis: a linear ranker over cheap per-move features (55 features:
state/move scalars plus "which move would handwritten/espresso/simple/
champion pick" oracle meta-features), trained Bradley-Terry-style on 74k
cordite(prod) self-play decisions, imitates most of cordite's moves in
microseconds; a score-margin gate (DL_TAU) defers uncertain decisions to
real cordite.

Rejected for production: held-out move-match ceilings at ~52% (best single
oracle matches cordite only 40%; cordite's close-call MC picks are
intrinsically hard for this model class), and strength tracks the deferral
rate monotonically — pure imitation (8µs/decision, 255x faster) loses to
plain espresso and finishes 3.23 vs cordite's 2.5 head-to-head; parity
needs >90% deferral, which is no speedup. The margin between "confident"
and "correct" is exactly where cordite's search earns its strength.

Kept in-tree as arena research (`--strategy=distilled`, DL_TAU knob,
`build/cnitro_distill` dumper + `tools/distill_train.py`): the pipeline is
reusable for a stronger model class (GBDT / small net) and the pure policy
is a fine µs-scale opponent where strength doesn't matter. Deliberately NOT
in bots.wasm or the production registry.
