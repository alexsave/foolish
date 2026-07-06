# Novichok — the cheating apex bot (research / benchmark only)

Named after something that should never be used in battle by any means.
Novichok is octogen (the strongest honest bot) with the honesty removed: it
reads every hidden hand, and optionally predicts the deck order itself from
the engine's RNG determinism. It exists to answer one question — *what eval
values can the session's strongest architecture achieve if it is allowed to
cheat?* — and the answer turned out to be the most interesting negative
result of the session:

> **Perfect information makes the bot BETTER against weak opponents and
> WORSE against strong MC opponents.** Belief-spread world sampling is not
> just an information-recovery device — it is model-error smoothing, and a
> cheater gives that smoothing up.

Novichok is C-only, never registered in TS/production seeds, and (like
espresso's hand-peek) exists purely as a benchmark ceiling probe.

## What it cheats at

Starting from a verbatim octogen copy (`nv_` namespace):

1. **True-hand worlds** (`nv_sample_world`): instead of building a belief
   (pins, voids, rank floors, MC-tells) and sampling hidden hands from it,
   every "sampled" world keeps the real hands from the live `Game` and only
   shuffles the deck. All belief machinery is bypassed (`nv_no_floors=1`,
   `nv_no_voids=1`, `NV_ADAPT=0` — there is nothing left to infer).
2. **Exact root solves on truth**: the deck-empty endgame solver runs on a
   direct `game_clone` of the live state instead of a deduced
   reconstruction — within the solve window (`NV_SOLVE_CARDS=28`) novichok
   plays *provably* perfectly against the actual hands.
3. **`NV_PEEK` (off by default — measured worse, see below)**: the game
   RNG is deterministic per seed and cordite-family bots consume zero net
   game-RNG (they save/restore around their own sampling). A trial that
   replicates the harness loop exactly — the same eligible-actor shuffle
   consuming the live RNG stream, RNG-neutral handwritten stand-ins for
   every seat, `draw_card` on the live stream — therefore draws the EXACT
   cards the real game will draw, for as long as the predicted moves match
   reality. `NV_PEEK=1` replaces the shuffled-world MC with an average over
   `NV_PEEK_TRIALS` such predicted-order trials (trial *t* pre-advances the
   stream by *t* draws to hedge prediction decay).

Everything else — the 3-stage MC, rollout policies, cheapest-first
tie-break, 8-card exact rollout leaves heads-up, octogen's 400k/250k
solver budgets and 24-card avoidance gate — is octogen, unchanged.

## Headline results

All cells are paired same-deal runs, `novichok` as hero vs `octogen` as
control, seeds from 980001. `diff` is hero−control mean placement:
**negative = the cheat helps, positive = the cheat hurts.**

| tables (opp in every other seat) | pc | pairs | diff ± SE | win nv | win og |
|---|---|---|---|---|---|
| handwritten | 2 | 300 | **−0.073 ± 0.020** | 97.3% | 90.0% |
| handwritten | 3 | 200 | **−0.325 ± 0.052** | 87.5% | 64.0% |
| handwritten | 4 | 150 | **−0.163 ± 0.067** | 48.7% | 37.7% |
| handwritten | 6 | 150 | −0.280 ± 0.151 | 31.3% | 25.3% |
| handwritten | 8 | 150 | −0.207 ± 0.186 | 14.7% | 16.0% |
| espresso | 2 | 200 | +0.055 ± 0.044 | 75.0% | 80.5% |
| espresso | 4 | 150 | **−0.300 ± 0.094** | 51.3% | 35.3% |
| random | 2 | 200 | +0.005 ± 0.013 | 98.0% | 98.5% |
| cordite | 2 | 200 | +0.035 ± 0.047 | 59.0% | 62.5% |
| cordite | 3 | 150 | **+0.287 ± 0.089** | 25.3% | 38.0% |
| cordite | 4 | 200 | +0.155 ± 0.100 | 15.0% | 20.0% |
| cordite | 6 | 150 | +0.347 ± 0.187 | 14.0% | 14.7% |
| cordite | 8 | 150 | −0.060 ± 0.227 | 16.0% | 8.7% |
| octogen | 2 | 100 | **+0.220 ± 0.066** | 26.0% | 48.0% |
| octogen | 4 | 100 | **+0.320 ± 0.155** | 17.0% | 26.0% |

Two clean regimes:

- **Heuristic fields (handwritten, espresso at 3+, random)**: the cheat is
  worth a lot. 97.3% heads-up wins vs handwritten (mean placement 1.027);
  at pc3 it wins 87.5% of games where octogen wins 64.0% — a 23.5pp jump
  from information alone. These are the biggest eval values any bot in
  this repo has posted.
- **MC fields (cordite, octogen tables)**: the cheat *hurts*. Against
  octogen tables heads-up the cheater wins 26% where the honest bot wins
  48% — significantly worse (3.3σ) *with strictly more information*.

## Finding 1: belief-averaging is model-error smoothing

Why does perfect information lose to honest play against strong opponents?
The rollout model predicting opponents' *moves* is the same handwritten
policy in both bots, and against MC opponents that model is wrong in
correlated ways. Octogen evaluates each candidate across hundreds of
belief-sampled worlds; the opponent-model error partially decorrelates
across worlds and averages down. Novichok evaluates on a point mass — the
true hands — so every rollout repeats the *same* wrong prediction with
full confidence. Sharper posterior, same biased likelihood: the bias stops
washing out.

Against handwritten opponents the rollout model is the opponent, there is
no model error to smooth, and true hands are pure profit — exactly the
split the table shows. (The wiring was verified by this split: a bug would
hurt everywhere.)

Corollary for honest-bot design (this is why the finding matters beyond
curiosity): cordite/semtex/octogen's belief-world spread should not be
narrowed too aggressively even when inference is confident — some of its
value is regularization of the opponent model, not just information
recovery. This retroactively explains hunt-2/hunt-4 nulls where
tighter/truer beliefs failed to pay.

## Finding 2: predicted deck order (NV_PEEK) is real but loses to variance

The RNG-determinism peek verifiably predicts the actual draw order (the
harness-replica trial draws the exact future cards while its move
predictions hold). It still measured worse than the plain hands-only
cheat — same 150 seeds, paired vs octogen at cordite tables, pc2:

| variant | diff ± SE | win nv | win og |
|---|---|---|---|
| hands-only (peek off) | +0.053 ± 0.054 | 57.3% | 62.7% |
| NV_PEEK, 3 trials | +0.180 ± 0.058 | 44.7% | 62.7% |
| NV_PEEK, 24 trials | +0.127 ± 0.055 | 50.0% | 62.7% |

The prediction is exact only until the first move where the real opponent
diverges from the trial's stand-in policy; after that the "known" order is
just one random order — but the bot still commits to a handful of trials
instead of a few hundred shuffled worlds. More trials (24) recover some
averaging but never reach the shuffled-world baseline. Deck-order
knowledge is worth less than deck-order *variance reduction*, at least at
this rollout-model quality. `NV_PEEK=0` is the default.

## Running it

```
make OMP=1 all
# headline cell: cheater vs the honest apex, same deals
./build/cnitro_eval --strategy=novichok --control=octogen --opp=handwritten \
    --players=3 --games=200 --seed-start=980001
# knobs
NV_PEEK=1 NV_PEEK_TRIALS=24 ...   # deck-order prediction (off by default)
NV_SOLVE_CARDS=28 ...              # exact-solve window (octogen's)
```

Novichok is deliberately absent from `bot_strategy.ts` and `seed.sql`:
it reads hidden state, which violates the production bots' legitimacy
contract (public info only). Its role is the one in this document — an
information-ceiling probe that ended up measuring the value of humility.
