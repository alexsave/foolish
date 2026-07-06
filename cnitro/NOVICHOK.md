# Novichok — the cheating apex bot (research / benchmark only)

Named after something that should never be used in battle by any means.
Novichok is octogen (the strongest honest bot) with the honesty removed: it
reads every hidden hand, solves endgames on the true state, and exploits
the engine's RNG determinism to know, exactly, the cards the next refill
will deal. It exists to answer one question — *what eval values can the
session's strongest architecture achieve if it is allowed to cheat?* — and
it ended up answering three more interesting ones:

1. **"Knowing the deck order" is not one thing.** This engine has no
   pre-shuffled order to peek at — each draw picks a random index from the
   remaining deck at draw time, so the order is *created* by the RNG
   stream, entangled with every future move by every player. Order
   knowledge is only well-defined over spans with no intervening
   decisions.
2. **Where the order IS provable, using it wins everywhere.** Exact refill
   pinning (below) measured better-or-equal than the plain hands cheat in
   every cell, every field, every player count.
3. **Belief-averaging is opponent-model error smoothing.** At 3+ player MC
   tables, even the full cheat stays below honest octogen: sharp
   true-hand worlds re-run the same wrong prediction of strong opponents'
   moves in every rollout, while the honest bot's belief-spread worlds
   decorrelate that error and average it down.

Novichok is C-only, never registered in TS/production seeds, and (like
espresso's hand-peek) exists purely as a benchmark ceiling probe.

## What it cheats at

Starting from a verbatim octogen copy (`nv_` namespace):

1. **True-hand worlds** (`nv_sample_world`): every "sampled" world keeps
   the real hands from the live `Game` and only shuffles the deck. All
   belief machinery (pins, voids, rank floors, MC-tells) is bypassed —
   there is nothing left to infer.
2. **Exact root solves on truth**: the deck-empty endgame solver runs on a
   direct clone of the live state; within the solve window
   (`NV_SOLVE_CARDS=28`) novichok plays *provably* perfectly against the
   actual hands.
3. **Exact refill pinning** (`NV_PEEK=2`, the default): the game RNG is
   deterministic, and `refill_player_hands` runs *synchronously inside*
   the battle-ending move handlers — no other actor decides anything
   between our move and its refill. So for every candidate move that ends
   the battle, the exact refill cards are a deterministic function of the
   live RNG state, with zero prediction risk, against any opponent.
   Novichok probe-applies each candidate once on a clone with the live
   stream, records the drawn cards, and force-feeds that sequence to the
   sim's refill (`cd_sim_set_forced_draws`) in every sampled world: exact
   near-horizon knowledge, full shuffled-world smoothing beyond it.
   Heads-up this is devastating in principle — the pinned refill IS the
   opponent's exact next hand, so the next battle's worlds are all-true.

`NV_PEEK=1` is the failed first design, kept for the record: whole-game
trials that replicate the harness loop (same eligible-actor shuffle
consuming the live stream, RNG-neutral handwritten stand-ins, live-stream
draws), predicting the *entire* future draw order. The prediction is exact
only until the first move where a real opponent diverges from the
stand-in — see the findings.

## Headline results

All cells are paired same-deal runs, `novichok` (default config) as hero
vs `octogen` as control, seeds from 980001. `diff` is hero−control mean
placement: **negative = the cheat beats the honest apex.**

| tables (opp in every other seat) | pc | pairs | diff ± SE | win nv | win og |
|---|---|---|---|---|---|
| handwritten | 2 | 300 | **−0.073 ± 0.020** | 97.3% | 90.0% |
| handwritten | 3 | 200 | **−0.175 ± 0.056** | 78.0% | 64.0% |
| handwritten | 4 | 150 | **−0.213 ± 0.100** | 54.0% | 35.3% |
| handwritten | 6 | 150 | −0.093 ± 0.151 | 29.3% | 25.3% |
| handwritten | 8 | 150 | +0.167 ± 0.183 | 12.7% | 16.0% |
| espresso | 2 | 200 | −0.030 ± 0.040 | 83.5% | 80.5% |
| espresso | 4 | 150 | **−0.220 ± 0.107** | 54.0% | 35.3% |
| random | 2 | 200 | −0.005 ± 0.011 | 99.0% | 98.5% |
| random | 3 | 200 | −0.025 ± 0.038 | 87.0% | 84.0% |
| random | 4 | 200 | +0.035 ± 0.057 | 68.5% | 70.0% |
| random | 6 | 150 | +0.240 ± 0.106 | 48.0% | 61.3% |
| random | 8 | 150 | +0.160 ± 0.164 | 36.0% | 32.7% |
| cordite | 2 | 200 | −0.010 ± 0.048 | 63.5% | 62.5% |
| cordite | 3 | 150 | +0.173 ± 0.095 | 28.7% | 38.0% |
| cordite | 4 | 200 | +0.150 ± 0.103 | 15.5% | 20.0% |
| cordite | 6 | 150 | +0.267 ± 0.185 | 11.3% | 14.7% |
| cordite | 8 | 150 | +0.467 ± 0.207 | 8.7% | 8.7% |
| octogen | 2 | 100 | −0.050 ± 0.069 | 53.0% | 48.0% |
| octogen | 4 | 100 | +0.090 ± 0.171 | 22.0% | 26.0% |

Raw eval values (400 games each): vs handwritten heads-up **97.2% wins,
mean placement 1.028**; vs random heads-up **98.2% / 1.018** — the biggest
numbers any bot in this repo has posted.

Two regimes remain:

- **Heuristic fields**: the cheat is worth a lot (pc3 handwritten: 78.0%
  wins where honest octogen wins 64.0%). These games are decided by
  information, and novichok has all of it.
- **MC fields at 3+ players**: the cheat still *loses* to honesty
  (cordite pc3 +0.173, pc6 +0.267; octogen pc4 +0.090) — with strictly
  more information. Heads-up the pinned refill closes the gap to
  at-or-above par (cordite −0.010, octogen tables −0.050).

## Finding 1: exact refill pinning helps everywhere (the sound way to "know the order")

Effect of pinning on the same deals (hands-only cheat → hands + pinning),
all statistically clean A/Bs on identical seeds:

| cell | hands-only | + refill pinning |
|---|---|---|
| cordite pc2 | +0.035 (59.0%) | **−0.010 (63.5%)** |
| handwritten pc3 | −0.090 (69.5%) | **−0.175 (78.0%)** |
| handwritten pc4 | −0.163 (48.7%) | **−0.213 (54.0%)** |
| handwritten pc6 | −0.020 (24.0%) | **−0.093 (29.3%)** |
| espresso pc4 | −0.187 (52.0%) | **−0.220 (54.0%)** |
| octogen pc4 | +0.300 (16.0%) | **+0.090 (22.0%)** |
| cordite pc6 | +0.507 (9.3%) | **+0.267 (11.3%)** |
| octogen pc2 / cordite pc8 / hw pc8 / random | ≈ | ≈ (no change) |

Not a single cell measured worse. At cordite pc2 the per-pair dump shows
the mechanism directly: 89 of 200 deals changed outcome, 49 losses turned
into wins vs 40 the other way. Deterministic-draw knowledge is real and
exploitable — *when you only claim to know what is actually determined*.

## Finding 2: whole-game order prediction (NV_PEEK=1) has a split personality

The whole-game trial predicts every future draw correctly for exactly as
long as every seat's move prediction holds. That makes its value entirely
a function of opponent predictability, and the measurements split
perfectly along that line (all paired vs octogen, same seeds):

| field | peek-1 | vs hands-only |
|---|---|---|
| handwritten pc3 | **−0.325 (87.5%!)** | −0.090 — near-oracle |
| handwritten pc6 | **−0.280 (31.3%)** | −0.020 — near-oracle |
| handwritten pc8 | −0.207 (14.7%) | +0.147 — strong |
| cordite pc2 (3 trials) | +0.180 (44.7%) | +0.053 — poison |
| cordite pc2 (24 trials) | +0.127 (50.0%) | +0.053 — still poison |
| octogen pc2 | +0.220 (26.0%) | −0.050 — poison |
| espresso pc2 | +0.055 (75.0%) | −0.015 — broken (espresso consumes RNG) |

Against handwritten opponents the stand-in IS the opponent: predictions
hold, the trial replays the actual future, and peek-1 posts the most
lopsided multiplayer numbers of the whole project. Against MC opponents
the first unpredicted move desynchronizes the stream and every "known"
card after it is fiction — evaluated with the full confidence of a
handful of trials instead of a few hundred smoothing worlds. Refill
pinning (Finding 1) is the sound core of this idea with the fiction cut
out, which is why it replaced it as the default.

## Finding 3: belief-averaging is model-error smoothing

Why does a bot with perfect information still lose to the honest bot at
3+ player MC tables? Both bots predict opponents' *moves* with the same
handwritten rollout model, and against MC opponents that model is wrong
in correlated ways. Octogen evaluates each candidate across hundreds of
belief-sampled worlds; the move-model error partially decorrelates across
worlds and averages down. Novichok evaluates on the truth — so every
rollout repeats the *same* wrong prediction with full confidence.
Sharper posterior, same biased likelihood: the bias stops washing out,
and with more seats the compounding is worse. Heads-up, where one
opponent's moves are heavily constrained, the effect is small enough
that the refill pin flips the sign.

Corollary for honest-bot design: cordite/semtex/octogen's belief-world
spread should not be narrowed too aggressively even when inference is
confident — part of its value is regularization of the opponent model,
not information recovery. This retroactively explains hunt-2/hunt-4
nulls where tighter/truer beliefs failed to pay.

## Finding 4: predicting moves by SEARCH doesn't rescue the cheater (NV_REPLY)

If unpredicted moves are the problem, why not predict them better? Novichok
knows the exact hands, so the opponent's legal move set is fully known —
the one place a bot can predict moves by *searching* instead of guessing.
`NV_REPLY=1` does exactly that (octogen's hunt-4 reply tournament: the
first opponent decision in each playout is chosen by search over their
full legal reply set, best-for-them). For the honest bot this measured
null because "best reply vs a guessed hand" sharpens noise; on novichok's
TRUE worlds that objection vanishes. Measured anyway (same seeds, vs the
pinning default):

| cell | default (pin) | + reply search |
|---|---|---|
| cordite pc2 | −0.010 (63.5%) | −0.005 (63.0%) — null, 2x cost |
| octogen pc2 | −0.050 (53.0%) | −0.020 (50.0%) — null |
| cordite pc3 | +0.173 (28.7%) | +0.287 (30.0%) — null/worse |
| handwritten pc3 | −0.175 (78.0%) | **−0.085 (71.5%) — harmful** |

Three reasons, in increasing order of depth:

1. **Strong opponents are belief-rational, not truth-rational.** Cordite's
   actual move maximizes against *its belief*, not against the true hands.
   Heads-up its beliefs are good, so truth-best and belief-best usually
   coincide — but there the handwritten stand-in usually coincides too.
   The searched reply only differs where prediction was already hard, and
   there it predicts the wrong quantity.
2. **One ply of exactness doesn't survive the horizon.** Only the first
   opponent decision is searched; everything after reverts to the
   handwritten model, and the searched reply itself is *evaluated* by
   those model-driven playouts — the yardstick is still wrong.
3. **Against weak opponents it's paranoia.** Handwritten seats never play
   their best reply; defending against it forfeits real exploitation
   (the pc3 harm above — the classic paranoid-search failure).

The logical endpoint of this road is running the opponent's *literal
decision code* as the move model (their choice is deterministic given the
public state and their strategy RNG). That is a perfect oracle in
principle — and a compute explosion in practice: one cordite call costs
as much as a whole novichok decision, so predicting replies to every
candidate multiplies decision cost by ~candidates × replies. At equal
wall-clock that trade (better model, fewer worlds) is the same one that
measured negative in every form this session. `NV_REPLY` stays off by
default, documented as the measured boundary of "just predict more moves."

## Postmortem: the mislabeled sweep

An earlier revision of this document reported the hands-only cheat as
dramatically worse than octogen at MC tables (e.g. 26% vs 48% at octogen
tables pc2) and refill pinning as a multiplayer regression. Both were
artifacts of one bug: a default flip changed `nv_peek`'s static
initializer but not the `nv_env_int("NV_PEEK", 1)` fallback, so the
"hands-only" sweep silently ran with peek-1 trials on. Same-seed dump
diffs plus a determinism check caught it; every cell above was re-run
clean. (The lesson: a "measured" claim inherits every unverified
assumption about what was actually running.)

## Running it

```
make OMP=1 all
# headline cell: cheater vs the honest apex, same deals
./build/cnitro_eval --strategy=novichok --control=octogen --opp=cordite \
    --players=2 --games=200 --seed-start=980001
# knobs
NV_PEEK=2 ...   # exact refill pinning (default)
NV_PEEK=1 NV_PEEK_TRIALS=24 ...   # whole-game order trials (failed design)
NV_PEEK=0 ...   # hands-only cheat
NV_REPLY=1 NV_REPLY_CAP=6 ...     # true-state best-reply search (measured null)
NV_SOLVE_CARDS=28 ...             # exact-solve window (octogen's)
```

Novichok is deliberately absent from `bot_strategy.ts` and `seed.sql`: it
reads hidden state, which violates the production bots' legitimacy
contract (public info only). Its role is the one in this document — an
information-ceiling probe that measured both the value of certainty and
the value of humility.
