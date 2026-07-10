# octogen deliberation explainer

Reconstructs a recorded game and dumps octogen's per-decision deliberation
(Monte-Carlo candidate scores + exact endgame-solver verdicts), then renders
the interactive page `docs/octogen-replay-explain.html`.

## Pipeline

1. **`../../tests/og_explain.c`** — native tool. Reconstructs the exact deal
   from a recorded game's public move stream (`deal.txt`, one line per seat:
   the 36-card deck order that reproduces the game), injects it, drives the
   recorded moves, and — with `OG_EXPLAIN=<file>` — dumps one JSONL record per
   octogen decision (hand, table, per-candidate `score/nsim`, solver verdicts,
   chosen move). Build it like the eval harness:
   `CORE=$(cd ../.. && make -s print-core); cc -O2 -ffast-math -Isrc $CORE tests/og_explain.c -o /tmp/og_explain -lm`
   then `OG_EXPLAIN=delib.jsonl CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 /tmp/og_explain`.
2. **`build_data.py`** — merges the deliberation JSONL with the decoded replay
   into `page_data.json` (one entry per log, octogen panels attached).
3. **`gen_html.py`** — renders `page_data.json` into the self-contained
   `docs/octogen-replay-explain.html`.

## Deal reconstruction note

The `game_seed` stored in the DB row for the reviewed game did NOT reproduce
its deal under the engine, so the deal here was reconstructed from the moves
(every card is revealed because the winner empties its hand). See the seed
plumbing investigation for why the stored seed diverges.

The instrumentation in `src/octogen_strategy.c` is runtime-env-gated
(`og_explain_on()`); the default build is behavior-neutral (SIG-identical when
`OG_EXPLAIN` is unset).
