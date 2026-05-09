# cnitro — pure-C nitro training

Self-contained C port of the nitro NN training pipeline. Replaces the
Python/TS path with native code so we can train and evaluate without
crossing language boundaries.

What's here:

- `src/game.{h,c}` — Russian Durak engine (1v1). Direct port of
  `supabase/functions/_shared/{common_utils,actions/*}.ts`.
- `src/legal.{h,c}` — legal-move enumeration (`calculateLegalMoves`).
- `src/random_strategy.c`, `src/espresso_strategy.c` — bot strategies.
  Each uses its own deterministic LCG so seeded eval is repeatable.
- `src/tokenize.{h,c}` — token vocabulary + `tokenize` matching
  `nitro_nn.ts`.
- `src/nn.{h,c}` — 2-layer × 1-head pre-LN transformer
  (d_model=32, ff_dim=64), forward + backward + SGD.
- `src/nitro_strategy.{h,c}` — autoregressive NN-driven move selection
  (matches `neuralChooseMove` in `nitro_strategy.ts`).
- `src/main_collect.c` — self-play data collector (winner-only samples).
- `src/main_train.c` — SGD trainer.
- `src/main_eval.c` — eval driver vs random / espresso.
- `tests/tests.c` — internal smoke tests.
- `tests/cross_check.c` + `tests/cross_check_ts.ts` — token-sequence
  parity check between C and TS.

## Build

```
cd cnitro
make
```

Produces:

- `build/cnitro_collect`
- `build/cnitro_train`
- `build/cnitro_eval`
- `build/cnitro_tests`
- `build/cnitro_cross_check`

## Run

```
# 1. Collect games (winner moves only).
./build/cnitro_collect --from=1 --to=5000 --pairs=esp-rand,rand-rand --out=/tmp/corpus.bin

# 2. Train.
./build/cnitro_train --corpus=/tmp/corpus.bin --out=/tmp/weights.bin \
    --epochs=10 --batch=32 --lr=0.05

# 3. Evaluate.
./build/cnitro_eval --weights=/tmp/weights.bin --opp=random  --from=1 --to=1000
./build/cnitro_eval --weights=/tmp/weights.bin --opp=espresso --from=1 --to=1000
```

## Tests

```
make tests          # 16 internal asserts
./build/cnitro_cross_check
npx tsx tests/cross_check_ts.ts
# diff the two — token sequences should be byte-identical.
```

## Determinism

Two LCGs share the recurrence used by TS:

- `game_set_seed()` — drives `Math.random()`-equivalent calls in the
  game loop, deck draws, espresso branches.
- `random_strategy_set_seed()` — drives the random bot.

Both are seeded per-game in `cnitro_eval` and `cnitro_collect`, so a
given seed reproduces the same play sequence run-to-run.

## Sample format

Binary file written by `cnitro_collect`:

```
"NCOR" version(uint32) repeated{
    uint16 n_tokens
    uint16 target_action
    uint8  n_legal
    int32  tokens[n_tokens]
    uint8  legal[n_legal]
}
```

## Weights format

Binary file written by `cnitro_train` / read by `cnitro_eval`:

```
magic(uint32)=0x4E4E4E4E version(uint32)=1
uint32: VOCAB_SIZE D_MODEL FF_DIM N_LAYERS MAX_SEQ_LEN NUM_ACTIONS
sizeof(NNParams) bytes: weights, in struct order (see nn.h).
```

Architecture must match — load fails on any field mismatch.
