# tt_divergence_viz

Interactive presentation of the cordite transposition-table divergence model:
`docs/tt-divergence.html`. Probability-of-divergence curves per bot / player
count, with the raw measured points overlaid, a 95% confidence band, and a
log-scale toggle.

## Bring it up locally

Just open the file — it is self-contained (no server, no external assets):

```
open docs/tt-divergence.html            # macOS
xdg-open docs/tt-divergence.html        # Linux
```

## Run your own measurements and watch it update

Each `measure` shards the games by seed across all cores, appends the results,
re-fits the curves + bands, and rewrites the HTML. Refresh the browser to see it
update.

```
# add 2000 games to octogen at 4 players, then rebuild
tools/tt_divergence_viz/generate.sh measure octogen 4 2000

# several player counts in one go
tools/tt_divergence_viz/generate.sh sweep octogen "2 3 4 5 6 7 8" 1500

# just rebuild the HTML from whatever is already in data/
tools/tt_divergence_viz/generate.sh
```

Keep calling `measure` for the same cell — the counts accumulate and the
confidence band tightens. Override the core count with `J=8`, the base seed with
`SEED0=...`.

## Why the results reconcile correctly

A game is a deterministic function of `(bot, opponent, player-count, seed)`. Each
cell file `data/W/<bot>_pc<pc>.gw` fixes bot + player-count, so records are keyed
by **seed** and deduped on it:

- Re-measuring a seed collapses to one record — overlap can't inflate the sample
  or falsely narrow the confidence interval.
- A seed's 5-player game and its 7-player game are different games in different
  files, never pooled.
- Accumulating two runs = the union of their seeds; you pool raw counts
  (numerator and denominator), you never average two rates.

`measure` auto-advances the seed range past what's already banked, so successive
runs are disjoint by construction; the dedup is the backstop if you pass an
overlapping `SEED0` by hand.

`W` per game is the largest key-set that had to coexist in the direct-mapped
table during that game (0 if the game never reached the endgame solver). The
divergence predictor is the survival function `P(W > 2^bits)`, an upper bound on
the per-game move-divergence rate — see the page's write-up.

## Files

| path | what |
|------|------|
| `generate.sh` | measure (seed-sharded, accumulating) + rebuild |
| `build.mjs` | render `data/*.json` → `docs/tt-divergence.html` |
| `ccdf.mjs` | `data/W/*.gw` → `data/ccdf.json` (per-seed → CCDF) |
| `data/W/*.gw` | accumulating per-game seed-keyed working sets |
| `data/measured.json` | hand-entered direct-divergence points (`tt_divergence.sh`) |
