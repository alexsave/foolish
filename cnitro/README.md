# cnitro — pure-C Durak engine + bot arena

Self-contained C port of the Russian Durak engine and its heuristic /
Monte-Carlo bots. Native code so we can simulate and evaluate millions of
games without crossing the language boundary into the TS server.

The engine and legal-move enumeration mirror
`supabase/functions/_shared/{common_utils,actions/*}.ts` exactly, so a game
played here is a legal game on the production server (the replay codec on the
TS side round-trips C-played games unchanged).

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
- `cordite` — blackpowder's successor; beats every other bot at
  every player count. See `CORDITE.md` (and `BLACKPOWDER.md`).
- `semtex` — cordite's successor; strictly dominates cordite (never a
  significantly worse cell vs any field, +18.5pp win heads-up on
  same-deal pairs): exact rollout-leaf endgames heads-up, extended exact
  root-solve window, per-seat MC-tells, measured-knee world budgets.
  The strongest deployed bot. See `SEMTEX.md`.
- `octogen` — semtex's successor (C-only): a wider exact-solve window
  makes it provably never worse than semtex and strictly better in the
  rare deep-endgame deals; also the documented map of hunt-4's
  measured-null opponent-model levers. See `OCTOGEN.md`.
- `torpex` — the value-net reboot of the ML track (research): full
  gen/train/infer pipeline for a learned rollout replacement, and the
  measured negative result explaining why simulation beats learning at
  CPU scale in this game. Without weights it plays exactly like semtex.
  See `TORPEX.md`.
- `astrolite` — cordite + defender card-management levers (research;
  roughly cordite-equal).
- `novichok` — octogen allowed to cheat (research/benchmark, C-only,
  never deployed): true-hand worlds, exact root solves on the real state,
  and exact refill pinning from the engine's RNG determinism (the sound
  slice of "knowing the deck order" — better-or-equal in every measured
  cell). Posts the repo's biggest eval values vs heuristic fields; at 3+
  player MC tables even the full cheat stays below honest octogen —
  belief-averaging is opponent-model error smoothing, not just
  information recovery. See `NOVICHOK.md`.

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
  - `--control=<strategy>` plays every seed twice on the same deal (hero
    vs control at seat 0) and reports the paired finish delta ± SE —
    same-deal pairing cancels deal luck, so far fewer games reach
    significance. `--dump=<file>` logs per-pair outcomes for loss
    analysis (`analyze_losses.sh` replays and diffs regression seeds).
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
