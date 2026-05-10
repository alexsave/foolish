# cnitro handoff — resume here

State as of 2026-05-10. After laptop restart, this captures everything
needed to pick up the next iteration.

## Where we are

Pure-C transformer, 50K params (D_MODEL=64, 2 layers, 2 heads × 32 d_head,
FF_DIM=128), imitation-trained on 80K esp-vs-esp self-play games for 5
epochs (~2.3h compute, loss 1.59 → 0.20).

### Held-out eval (seeds 80001..82000, never trained on)

|  | win rate |
|---|---|
| nitro vs random | **96.2%** |
| nitro vs espresso | **40.4%** |

For comparison, the existing TS hand-tuned parametric nitro (weeks of
hill-climbing + 18 explicit Durak principles): 95.1% / 35.1%. The
imitation transformer matches/beats the hand-tuned baseline with no
coded rules.

### What the model learned (from inspector)

Independently, from imitation alone:
- Cheapest-cover (lowest same-suit beats higher same-suit at >70%)
- Multi-card pair attacks (74% on `[JS,JH]` over 19% on single `JH`)
- Strategic pass over cover (87.5% on `PASS [6H]` instead of 8 cover options)
- Trump conservation while deck is alive
- Pickup when forced (high confidence)

65% of decisions are >90% confidence (not flailing). Inspector even shows
the model predicting the *opponent's* next move when asked for raw
unmasked softmax — implicit belief over opponent state.

### Saved artifacts

- `/tmp/overnight_w.bin` — trained weights (binary, NNParams shape)
- `/tmp/overnight_corpus.bin` — 80K-game collected samples (~3.8M atomic)
- `cnitro/cout.txt` — training log (recentLoss + dt per ~1000 steps)

## Expert recommendation (agreed plan)

In order:

1. **Tokenizer refactor: variable-opponent seat encoding** (~1 day)
   - Build the multi-opponent encoding *now* as a strict superset where
     1v1 is the N=1 case. Don't bake "the opponent" into a singular slot.
   - Encode opponents as **seats to the right** of self: seat 1 = next
     defender / who attacks me next, seat 2 = after them, etc.
   - Walk `(bot_idx + k) mod num_players` skipping OUT players.
   - Tokens to add: `TOK_OPP_SEAT_1..7`. Vocab grows 72 → 79.
   - **History tokens also need the upgrade**: replace `TOK_PLAYER_OPP`
     with `TOK_PLAYER_OPP_SEAT_1..7` in move-history events, so logged
     moves can be attributed to a specific seat in 3+ player games.
   - Verify by retraining 1v1 on the new tokenizer and confirming we
     recover the 40.4% number.

2. **Deepen the teacher (espresso-N)** (~1-2 days)
   - Current `rollout_round` in `espresso_strategy.c` is greedy-both-sides
     for 5 iterations.
   - "Espresso-2" = same rollout but **opponent's response is recursive
     espresso (depth-1)** instead of greedy-lowest-cover.
   - "Espresso-3" = depth-2 recursion.
   - Regenerate corpus with new teacher, re-imitate.
   - **Success criterion**: imitation beats original espresso past 50%.
     If yes, validates policy-iteration ladder (espresso-N → model-N →
     MCTS-with-model-N-prior → model-N+1, i.e. AZ).
     If no, model is the bottleneck → scale.

3. **Scaling probe** (half-day)
   - One config: `D_MODEL=256, FF_DIM=1024, N_LAYERS=6, N_HEADS=8`
     (~5M params).
   - Same 80K corpus, retrain. Resume infra already exists.
   - One data point to know if imitation has saturated at 50K params.

4. **Train-time hidden-info masking** (half-day)
   - In `nn_forward_batch`, randomly zero the OPP-section tokens with
     prob=0.5 per sample during training.
   - Free regularizer + makes model deployable vs humans (no longer
     requires opp's hand at inference).
   - Do this BEFORE any RL work.

5. **AlphaZero** (defer until 1-3 done)
   - Search at composite-move level (not atomic-action level).
   - Policy head as MCTS prior; autoregressive head samples within nodes.
   - Add value head trained on MCTS rollout outcomes.

### Skip / defer

- Belief head as auxiliary loss — trunk already encodes belief
  implicitly (visible in unmasked softmax). Buys more once masking is on.
- Pure PPO self-play — credit assignment in Durak is hard; AZ's MCTS
  gives denser improvement signal. Don't burn cycles on PPO.

## Code state

All committed on `nitro` branch:

```
git log --oneline cnitro/
abc0c43 cnitro: ~1.4x backward speedup from profile-driven optimizations
371f624 cnitro: ~5x speedup via Accelerate cblas_sgemm + progress logging
c399422 cnitro: multi-head attention, quality filter, top-3 raw inspector
... (earlier)
```

Plus uncommitted: wall-time fix in `main_train.c` / `main_eval.c`
(`clock()` → `clock_gettime(CLOCK_MONOTONIC)`), and the batched training
path in `nn.c` + `main_train.c` (functions: `nn_forward_batch`,
`nn_accumulate_grads_batch`, `BatchedForwardCache`, `BatchedLayerCache`).

Build: `cd cnitro && make`. Tests: `./build/cnitro_tests` (16/16 pass).

## When you come back

Probably start with task **(1)** — tokenizer refactor for seat-based
multi-opponent encoding. Pure mechanical work in `tokenize.{h,c}`,
no training risk, unlocks everything downstream. After verifying 1v1
performance is unchanged on the new tokenizer, move to (2).

Run book: `cnitro/RUN.md`.
