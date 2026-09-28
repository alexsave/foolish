# Pick 'Em Up bots - the measured report

What the offline bots of `pickemup/c/src/pk_bot.h` are worth, measured by `pickemup/c/tools/pk_arena.c`.
The bots are not in the app (DECISION D63); this is a C-side capability for simulation and evaluation.
How the Monte Carlo bot imitates foolish's octogen is in `RULES_AND_KERNEL.md` section "Bot".

## How it was measured

```
make -C pickemup/c arena                           the five main line-ups, 2,000 games each, 8 workers
./pickemup/c/build/pk_arena 2000 8 248 x           the same plus the two extra line-ups (the tables below)
```

Run on 2026-09-27 on an 8-core Mac, about ten minutes of wall clock for the whole table (the five main line-ups alone, `make arena`, 6 minutes 42 seconds).

- A line-up is two agents seated alternately, A B A B, and every deal is played twice, once each way round.
  So each agent holds half the seats, and 50% is an even match at every table size.
- 2,000 games a line-up at each of 2, 4 and 8 players, from fixed seeds: a run is reproducible to the game.
- The 95% interval in these tables is the normal one, p plus or minus 1.96 times the square root of p(1-p)/n, over the 2,000 games; the arena has printed Wilson's since D66, which moves no conclusion here.
- "cards" is the mean number of cards an agent's seats still hold when the game ends.
- Every move of every game went through `pk_apply`, and none was refused.
- No game ended stuck (1.10) or at the long-game stop (1.11).

The agents:

| Agent | What it is |
|---|---|
| `random` | uniform over the whole turn menu, draw and pass included (D64): the baseline |
| `greedy` | the cheap rule of `pk_bot_greedy`: play if anything plays, stay in the suit held most, shed action cards, hit a seat on two cards or fewer, keep wilds; else draw once, then pass |
| `mc` | the Monte Carlo bot at its defaults: 48/64/64 worlds, 12-action greedy rollouts, soft voids in 3 of 4 worlds |
| `mc-rw` | `mc` with a wild's suit chosen at random (`PK_KNOB_RANDOM_WILD`): the wild-suit ablation |
| `mc-nb` | `mc` whose worlds ignore every void (`PK_KNOB_NO_BELIEF`): the belief ablation |

Every agent follows the same "Last card!" and "Caught you!" rule (D60), so the table measures the turn only.

## The bar: MC against random

| Players | MC wins [95% CI] | MC cards left | random cards left |
|---|---|---|---|
| 2 | 98.4% [97.8, 98.9] | 0.08 | 7.80 |
| 4 | 97.9% [97.3, 98.5] | 2.06 | 7.16 |
| 8 | 97.1% [96.4, 97.8] | 3.39 | 7.13 |

The interval is clear of 50% at every size, far beyond the bar (2 and 4 players).
The margin says more about the baseline than about MC: `greedy` beats `random` just as hard (97.8%, 97.7%, 97.7%), because a random bot draws with a playable card every time the draw comes up.

## MC against greedy

| Players | MC wins [95% CI] | MC cards left | greedy cards left |
|---|---|---|---|
| 2 | 55.5% [53.3, 57.7] | 1.96 | 2.16 |
| 4 | 54.5% [52.4, 56.7] | 3.06 | 2.99 |
| 8 | 51.7% [49.5, 53.9] | 3.79 | 3.60 |

MC's rollouts play greedy for every seat, so this is what search on top of the rollout policy adds.
It is clear of 50% at 2 and 4 players, and within noise at 8, where a 12-action horizon is a round and a half and the other seven seats decide most of what happens.

## What the wild's suit is worth (the ablation)

| Players | mc vs mc-rw [95% CI] | mc-rw vs random | mc-rw vs greedy [95% CI] |
|---|---|---|---|
| 2 | 55.5% [53.4, 57.7] | 97.9% | 51.6% [49.5, 53.8] |
| 4 | 49.4% [47.2, 51.5] | 97.3% | 49.4% [47.2, 51.5] |
| 8 | 50.4% [48.3, 52.6] | 97.9% | 52.2% [50.1, 54.4] |

At two players the suit decision alone is worth about five and a half points of win share: MC with it beats MC without it 55.5% to 44.5%, and without it MC's edge over greedy (55.5%) all but disappears (51.6%).
At four and eight players the ablation is inside the noise: with more seats between one wild and the next time it matters, the choice of suit is a small part of the game, and 2,000 games cannot see it.
Against random the suit choice is invisible, because random loses either way.

## What the belief is worth

| Players | mc vs mc-nb [95% CI] |
|---|---|
| 2 | 53.0% [50.9, 55.2] |
| 4 | 51.6% [49.4, 53.8] |
| 8 | 49.8% [47.6, 51.9] |

Sampling worlds that respect the voids (D59) is worth about three points at two players, just clear of 50%, and nothing measurable at four and eight.
The mechanism is visible in `tests/pk_bot_test.c`: at 400 real positions where the turn seat can play only a wild and the next seat, on two cards or fewer, has a trusted void, MC names a suit that seat is void in 135 times, and the same MC blind to the voids 91 times.
The whole-game effect is small because voids come from draws, which this game's rule D6 makes weak evidence, and because the greedy and MC opponents here draw only when stuck, which makes the evidence true but rarely decisive.

## The tuning that got here

Each row is `mc vs greedy`, both seat orders, the wild tax at 20 milli throughout (the knobs are the arena's `PK_W1`, `PK_W2`, `PK_W3`, `PK_DEPTH` and `PK_DRAW_KEEP` overrides).

| Worlds | Rollout | Draw tax (milli) | Games a size | 2 players | 4 players |
|---|---|---|---|---|---|
| 16/24/24 | to the end | none | 400 | 45.0% | 39.5% |
| 16/24/24 | 8 actions | none | 400 | 50.0% | 50.7% |
| 16/24/24 | 24 actions | none | 400 | 49.0% | 48.2% |
| 16/24/24 | to the end | 1000 | 400 | 47.8% | 43.2% |
| 48/64/64 | to the end | 1000 | 400 | 56.8% | 50.5% |
| 48/64/64 | to the end | 100 | 600 | 56.3% | 50.3% |
| 48/64/64 | 30 actions | 100 | 600 | 54.7% | 50.2% |
| 48/64/64 | 6 actions | 100 | 400 | 56.5% | 49.8% |
| 48/64/64 | 12 actions | 100 | 400 | 56.0% | 54.2% |

The last row is the defaults.

The first pass lost to greedy because it was noise-limited: at 16 worlds a candidate's value carried a standard error of about 0.06 against gaps of 0.02, so a draw with a playable card won the comparison about as often as a play.
Two things fixed it, both octogen's own tools: more worlds, and a selection tax on drawing while a play exists (`draw_keep`, in octogen's `OG_TRUMP_KEEP` milli units), which settles near-ties for the play without overriding a real gap.
The 12-action horizon was the best measured at four players and costs a third of a full rollout.

## What is not done

- The bot is basic: it does not model an opponent's policy (octogen's per-seat profiles), solve endgames exactly, or search an opponent's reply.
- At eight players MC is not measurably better than greedy.
- The wild ablation and the belief ablation are within noise at four and eight players; a larger run (20,000 games a line-up, about an hour) would say whether there is anything there.
