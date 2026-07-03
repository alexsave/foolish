# cnitro — pure-C Durak engine + bot arena

Self-contained C implementation of the Russian Durak engine and its
heuristic / Monte-Carlo bots. Native code so we can simulate and evaluate
millions of games without crossing the language boundary into the TS server.

**The kernel here IS the production rules engine.** `game.c` + `legal.c`
compile to WebAssembly (`make wasm`) and run every live move: the TS files in
`supabase/functions/_shared/actions/` are thin bridges over this code (see
`wasm/wasm_api.c` and `_shared/wasm/engine.ts`). The old TS rule
implementations were deleted after a differential harness proved the two
engines byte-identical across ~100k mirrored actions. Two behaviors are build
parameters because the deployments deliberately differ: the deck-size
boundary (production/WASM: 5+ players → 52 cards, matching the frozen replay
format; native research tools: 6+, per `card.h`) and the log/battle/move
capacities (`-DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LEGAL_MOVES=65536` for
production). The kernel fires `engine_snap_hook` at the exact points the old
TS handlers captured animation snapshots — a NULL no-op for native builds.

## What's here

Engine:
- `src/card.h`, `src/game.{h,c}` — cards, game state, the engine (deal,
  defender rotation, draws, eliminations, logs).
- `src/legal.{h,c}` — legal-move enumeration (`calculateLegalMoves`).
- `src/strategy.h` — the `STRAT_*` ids + `parse_strategy` name↔id mapping.
- `src/cli_util.h` — shared `--key=value` / int arg parsing for the mains.

Bots (weakest → strongest):
- `random`, `espresso`, `handwritten` — heuristics (espresso/handwritten are
  1v1-focused; espresso peeks at hands, so it's used only as a rollout policy).
- `robusta` — public-info Monte-Carlo; `firecracker` / `gunpowder` are
  robusta with different rollout policies.
- `blackpowder` — belief-constrained determinized MC + exact endgame.
- `cordite` — blackpowder's successor; ELO #1, beats every other bot at
  every player count. See `CORDITE.md` (and `BLACKPOWDER.md`).

Each strategy uses its own deterministic LCG (seeded per game) so a given
seed reproduces the same play run-to-run.

## Tools

```
make all          # builds the four binaries below into build/
make tests        # build + run the engine unit tests
```

- `cnitro_eval`   — protagonist (seat 0) vs `--opp` everywhere else; reports
  win-rate / mean finish position. e.g.
  `./build/cnitro_eval --strategy=cordite --opp=espresso --players=4 --games=500`
- `cnitro_elo`    — mixed-pool ELO arena. e.g.
  `./build/cnitro_elo --games=3000 --pcs=2,3,4,5,6,7,8 \`
  `    --pool=random,handwritten,espresso,robusta,firecracker,gunpowder,blackpowder,cordite`
- `cnitro_replay` — replay one game move-by-move from a seed.
- `cnitro_tests`  — engine smoke tests.

`bench_cordite.sh` runs the standard cordite benchmark suite.

## History

cnitro began as a pure-C port of the "nitro" transformer training pipeline
(and later a GRPO RL track). Both ML tracks plateaued below the handwritten
ceiling and were superseded by the heuristic/MC bots above (cordite), so the
NN/GRPO code was removed. Recover it from git history if ever needed.
