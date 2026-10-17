# cnitro — pure-C Durak engine + bot arena

Self-contained C implementation of the Russian Durak engine and its
heuristic / Monte-Carlo bots. Native code so we can simulate and evaluate
millions of games without crossing a language boundary.

**The kernel here IS the production rules engine.** The server (c_base, whose
Makefile reads this Makefile's source list) and the phones link it natively; the
browser runs it as WebAssembly (`make wasm-web`, `web.wasm`). Every host is a thin bridge over this code: see `wasm/wasm_api.c`,
and the web reaches it through `sdk/ts/table/`. The old TS rule
implementations were deleted after a differential harness proved the two
engines byte-identical across ~100k mirrored actions. The deck rule is
settled and hardcoded in `card.h`: 2..5 players → 36 cards, 6+ → 52,
everywhere (historical 5-player replays encoded under the old 5+ → 52 rule
no longer decode). Log/battle/move capacities remain build parameters
(`-DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 -DMAX_LEGAL_MOVES=65536` for
production). The kernel fires `engine_snap_hook` at the exact points the old
TS handlers captured animation snapshots — a NULL no-op for native builds.

**The bots here are ALSO the production bots.** The server links them natively
(c_base). In wasm they run only in the Oracle's modules (`make wasm-oracle`).
The e2e suites that need a server's half of a game take it from the table host
(`make table-host`, `tests/table_host.c`: the wasm entry files compiled
natively), through the C Table (`e2e/helpers/server_table.ts`).
The production bot names began as exact C
mirrors of TS strategies; the TS strategies and their move-for-move parity
suite are retired (docs/C_GAME_SHAPE_MIGRATION.md Phase 8), and the C is the
only implementation. Two bots exist in a `_prod` variant (`espresso_prod`,
`handwritten_prod`): the un-suffixed arena versions drifted slightly from
the TS originals and are frozen because cordite's rollout policy (and its
`cordite_sim.c` bitboard mirror) was tuned against them. `CD_BUDGET=prod|max`
selects the deployed cordite world/pruning budgets (the arena default stays
the C-tuned budget); `fulminate` is cordite + per-seat opponent profiling
(the TS design ported back, bit-for-bit cordite when profiling is off).

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
- `cordite` - blackpowder's successor. See `CORDITE.md` (and `BLACKPOWDER.md`).
- `octogen` - **the current apex and the top shipped tier** (`OCTOGEN.md`).
- `fulminate` — cordite + in-game per-seat opponent profiling (skews each
  profiled seat's rollout policy toward its best-fit archetype).
- `semtex`, `torpex` and `novichok` are retired and removed.
  Semtex was cordite's successor and octogen's predecessor, torpex was semtex's search with a learned value net (a measured negative result), and novichok was a cheating ceiling probe.
  Their sources, trainer and write-ups are in the git history, and their `STRAT_*` ids (18, 19, 22, 23) are retired in `src/strategy.h`, not reused.
- `astrolite`, `distilled` - research arms; see `CORDITE_RESEARCH.md`.
- Production TS mirrors: `simple_heuristic`, `champion`, `ultimate_champion`,
  `hacker`, `espresso_prod`, `handwritten_prod` — ported move for move from
  the retired TS bots; the TS originals and their parity suite are gone.

Each strategy uses its own deterministic LCG (seeded per game) so a given
seed reproduces the same play run-to-run.

## Tools

```
make all          # builds the binaries below into build/
make tests        # build + run the engine unit tests
```

- `cnitro_eval`   — protagonist (seat 0) vs `--opp` everywhere else; reports
  win-rate / mean finish position. e.g.
  `./build/cnitro_eval --strategy=cordite --opp=espresso --players=4 --games=500`
- `cnitro_elo`    — mixed-pool ELO arena. e.g.
  `./build/cnitro_elo --games=3000 --pcs=2,3,4,5,6,7,8 \`
  `    --pool=random,handwritten,espresso,robusta,firecracker,gunpowder,blackpowder,cordite`
- `cnitro_replay` — replay one game move-by-move from a seed.
  `./build/cnitro_replay --code="<share link or code>" [--from=N]` prints a share link's decoded
  log stream instead, with the seat names and move gaps its extras carry.
- `cnitro_tests`  — engine smoke tests.
- `cnitro_distill`, `cnitro_analyse` - the distillation arm (`CORDITE_RESEARCH.md`) and the
  post-game analyser (`docs/POST_GAME_ANALYSER.md`).

The shipped roster (`src/bot_roster.c`) is ten tiers ending at octogen:
`random, simple_heuristic, handwritten, espresso, robusta, firecracker,
gunpowder, blackpowder, cordite, octogen`. There is no `cordite_max` or
`octogen_max` - read `bot_roster.c`'s own comment for why "Max" was weaker.

`bench_cordite.sh` runs the standard cordite benchmark suite.

### Building the wasm on a Mac needs homebrew LLVM

The variable is `WASM_CC`, not `CC`, and it defaults to plain `clang` - which on
macOS is Apple clang and cannot target wasm32 at all:

```
make wasm-web WASM_CC=/opt/homebrew/opt/llvm/bin/clang
```

Builds are byte-reproducible for a pinned toolchain, and measurably so: a raw
module is byte-identical on macOS arm64, Linux arm64 and Linux x86_64 with
clang 22.1.8 + binaryen 130. So a different `build/web.wasm` means your
change moved it, not the machine. `scripts/wasm_build.sh --check` is the gate,
and `.gz` sizes are the one thing that does still vary by machine - the
compressor moves them by up to 576 B on identical input, which is why
`e2e/mem/wasm_memory.test.ts` pins the raw size instead.

Nothing here is committed. `sdk/ts/wasm/web.wasm.gz` and the two
`public/oracle*.wasm.gz` are gitignored build outputs, and CI builds them in the
lane that ships them (`scripts/wasm_build.sh`, `shared/scripts/ci_llvm.sh`).


### The arena fingerprint - a cheap "did bot behaviour drift?" check

`./build/cnitro_eval 2>/dev/null | tail -1` prints one deterministic line for
the default matchup.
Two builds that print the same line play the same games; a changed line means
some bot's decisions moved, which is either the point of your change or a bug
in it.
It is a diff, not a constant: the value is expected to move whenever a strategy
or the engine's RNG consumption changes, so record it before and after rather
than pinning it.

```
2   1.300   1.500   70.0%   140 60      # July 2026 (docs/CONSOLIDATION_PLAN.md, now retired)
2   1.315   1.500    68.5%  137 63      # 2026-09-18, origin/main @ 801c58a0
```

## History

cnitro began as a pure-C port of the "nitro" transformer training pipeline
(and later a GRPO RL track). Both ML tracks plateaued below the handwritten
ceiling and were superseded by the heuristic/MC bots above (cordite), so the
NN/GRPO code was removed. Recover it from git history if ever needed.
