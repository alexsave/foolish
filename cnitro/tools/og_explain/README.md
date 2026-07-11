# octogen deliberation explainer

Turns one recorded game into an interactive page that X-rays **octogen**'s
thinking at every one of its turns — the Monte-Carlo score of each candidate
move, the exact endgame-solver verdict once the deck is empty, and *what
octogen actually knows* about the hidden cards.

## One command

```sh
python3 cnitro/tools/og_explain/explain.py "<replay-url>" "<deal-seed>" [out.html]
```

Example (defaults to `docs/octogen-replay-explain.html`):

```sh
python3 cnitro/tools/og_explain/explain.py \
  "WWW.FOOLISH.CARDS/QDKZ2VYIB…" \
  aca4066e2bde5f37d99f9d1fdecbf416e0752bf903fa96e166da2c4f6a02ed72
```

That's it. **Nothing about the specific game is hardcoded** anywhere in the
pipeline: trump, who won, the fool, the flip card, the agree/differ tally,
which moves to flag, the flagged-move commentary, and the "octogen known state"
pool are all derived from the decoded replay and the engine's own deliberation
dump. Pass a different `(url, seed)` → you get a correct page for that game.

Requirements: `node` (with `tsx`, for the TypeScript replay codec) and a C
compiler. The analysis binary is built on demand via `make og_explain`.

## What the pipeline does (`explain.py` runs these in order)

1. **`decode_to_json.mjs`** — reuses the deployed replay codec
   (`urlToGame` + `decodeReplay`) to recover the public log stream + game meta
   from the share URL → `replay_decoded.json`.
2. **`make og_explain`** — builds `cnitro/tests/og_explain.c` with
   `-DOG_EXPLAIN_BUILD`, the flag that compiles the deliberation-dump
   instrumentation **into** octogen. That flag lives *only* in this make target,
   so every shipped native + wasm bot carries none of it (zero code-size cost).
3. **`make_moves.py`** turns the decoded logs into the recorded move stream;
   `og_explain <seed> moves.txt` deals the seed, replays the moves, and queries
   octogen at each of its turns, dumping one JSONL record per decision to the
   `OG_EXPLAIN` sink.
4. **`build_data.py`** merges the deliberation with the decoded replay into
   `page_data.json`, deriving all meta and computing the per-decision
   *hidden-pool* (`36 − hand − flip − discard − table`, asserted to equal
   `deck + opponent`).
5. **`gen_html.py`** renders `page_data.json` into the self-contained page.

## octogen known state

At each octogen turn the page shows the cards **hidden from octogen** right now:
the whole 36-card deck minus its hand, the flip, the discard pile, and the
table. That pool splits between the face-down deck and the opponent's hand
(`pool = deck + opponent`, exactly). When the deck empties, the pool collapses
onto the opponent's hand — the public deduction that lets the exact endgame
solver *prove* the line rather than sample it.

## Limitations

- **Two-player games.** The driver (`og_explain.c`) seats octogen at p1 vs one
  opponent. Multi-player replays aren't driven yet.
- **Needs the reproducing deal seed.** The deal is set from the 32-byte seed,
  then the recorded moves are replayed on top; the driver stops at the first
  move that doesn't apply. If a stored seed doesn't reproduce its deal, the
  deliberation will be partial (the page still renders what was captured).
- The recorded opponent may have been a *different* octogen configuration, so a
  "would differ" flag is usually a Monte-Carlo near-tie, not a blunder — each
  flagged panel shows both scores so you can judge.

The instrumentation in `src/octogen_strategy.c` is entirely behind
`#ifdef OG_EXPLAIN_BUILD`; the default build is behavior-neutral and carries no
trace of it.
