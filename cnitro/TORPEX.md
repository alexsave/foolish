# Torpex — the value-net reboot of the ML track (negative result, kept)

Torpex was the attempt to reboot this project's ML lineage (nitro
transformer policy, GRPO RL, NEAT — all plateaued below handwritten) with
the modern division of labor: **keep semtex's search, learn the
evaluation**. The old tracks failed because the net had to *play* directly,
replacing search; torpex instead replaced only the handwritten-policy
rollout inside the determinized MC with a learned value
V(full-information world state, seat) → expected normalized finish,
trained on semtex self-play outcomes. The sampled worlds are full-info
states, so no information-set modeling is needed — on paper the cleanest
possible insertion point for learning.

## The pipeline (all committed, reusable)

- `src/main_gen.c` (`cnitro_gen`) — semtex self-play; snapshots every
  post-move state from every IN seat as 72-byte records (trump-rotated
  bitmasks + scalars), finish targets filled at game end.
  ~36k games / 4.2M records generated in ~1.5h on 4 cores.
- `train_torpex.py` — pure-numpy Adam/MSE MLP, on-the-fly bit unpacking,
  per-file record alignment (a killed generator leaves a partial trailing
  record; concatenating raw bytes misaligns everything after it — v0
  trained on garbage until fixed). Exports flat float32 weights.
- `src/torpex_value.{c,h}` — C inference (sparse-input GEMV), lazy-loads
  `$TORPEX_WEIGHTS`.
- `src/torpex_strategy.c` — semtex verbatim; `TX_VALUE=1` (default when
  weights load) evaluates each (world × candidate) with one forward pass
  instead of a playout. Without weights torpex IS semtex.

## Results (paired vs semtex control @ cordite tables)

| net | params | val-MSE (baseline 0.150) | pc3 Δ | pc4 Δ |
|---|---|---|---|---|
| v1: 378→256→64→1 | 123k | 0.114 | +0.230±0.068 | +0.473±0.083 |
| v2: +16 engineered features, 394→512→64→1, 8 ep | 300k | **0.102** | **+0.485±0.072** | **+0.635±0.102** |

Both clearly worse than rollouts — and **v2, the better predictor, plays
worse than v1**. That inversion is the finding:

## Why it loses (and why AlphaZero's analogy breaks here)

1. **The competition is not "predict the outcome", it is "rank two sibling
   states"** that differ by a single card played. The MC compares
   candidates within a shared world by small value differences. A smoother
   net with better *average* error is systematically worse at exactly
   those tiny tactical deltas, while a rollout — a mechanical simulation —
   responds to every card causally. Hence better val-MSE, worse play.
2. **The rollouts being replaced are already cheap, unbiased, and
   averaged.** The bitboard playout costs ~30μs and the MC averages
   hundreds per decision, annihilating its per-sample noise. AlphaZero's
   value nets won in Go because rollouts there were *biased and bad*;
   here they are the strong incumbent. A net must beat the averaged
   estimator's *bias* (small, since hunt-4 measured the rollout-policy
   bias as mostly unexploitable) with near-zero bias of its own —
   unreachable at CPU-trainable scale.
3. Combined with the historical nitro/GRPO/NEAT plateaus, the consistent
   lesson: **at this compute scale, learning loses to simulation in this
   game.** A genuine ML win would need GPU-scale training aimed at the
   discrimination task (e.g. a policy/value net over candidate *pairs*,
   or ReBeL/Student-of-Games-style belief-state training), not a better
   scalar evaluator.

Torpex stays registered (`torpex`/`tx`, C-only, research) with the full
pipeline, so a future GPU-scale attempt starts from working
data-generation, training, and inference plumbing rather than from
scratch. No weights are shipped; without them torpex plays exactly like
semtex. The strongest deployable bot remains **semtex** (SEMTEX.md);
the strongest research bot is **octogen** (OCTOGEN.md).
