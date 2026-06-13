# Fulminate research notes (session continuation)

## Starting point (HEAD 011ab64)
Same-table A/B vs cordite, Δwin = fulminate − cordite (prior table, 160 games/cell):
| field | pc4 Δwin | pc6 Δwin |
|---|---|---|
| simple_heuristic (weak) | +16.9 | +13.8 |
| handwritten | +8.1 | -3.1 |
| espresso (strong) | -2.5 | -5.0 |

Open question: are the strong-field negatives noise or a real regression? (objective 1)

## Plan
1. High-N A/B with strong fillers (espresso, all-cordite field) at pc2/4/6, 400+ games/cell.
2. If regression real -> raise confidence/decision gate before deviating a seat from strong default.
3. Push exploitation edge vs weak opponents (negative inference, sharper archetypes/rollout).

## Results log

### Probe (direct mislabel measurement — low-variance signal)
fulminate_probe espresso (strong field), 40 games, CD_MAXMS=400, pc2:
- OLD gate (4ec99e6: decisions>=6, conf>=0.55, trump-ramp 0.22..0.42):
  espresso seats mislabelled non-HANDWRITTEN = **17.18%** (RANDOM=222/1292).
  measured espresso trump-rate mean 0.265, max 0.545 — the old ramp floor (0.22)
  put a STRONG bot squarely in the weak zone; the per-game high-trump TAIL
  cleared the 0.55 bar.
- NEW gate (ramp floor 0.40..0.60, base conf bar 0.70, minCards 14, pc-scaled):
  espresso mislabel = **0.30%** (RANDOM=4/1330). trump mean 0.242 -> ~0 score.
=> The profiler's root-cause mislabel of strong seats is essentially eliminated.
   The safe default (POL_HANDWRITTEN == cordite) now holds for strong fields.

### Strong-field A/B (espresso, 200g/cell, CD_MAXMS=400) — HIGH variance (~+-7pp)
Same-table paired dWin = fulminate(seat0) - cordite(seat1), pc4:
- OLD profiler (4ec99e6):              dWin -5.5
- FUL_OFF control (fulminate==cordite): dWin -12.5   <-- pure seat/harness noise
- NEW profiler (this branch):          dWin -14.5
The three are within one ~+-7pp std of each other: the seated A/B is dominated
by SEAT-POSITION asymmetry (fulminate is always seat 0), not by the profiler.
The FUL_OFF control (-12.5) proves even cordite-identical play loses ~10pp here,
so dWin "~0" is NOT achievable at this N/budget by any profiler change. The
low-variance probe (above) is the reliable signal; on it the fix clearly works.
(Full-budget runs at default maxMillis showed the documented small -2.5/-5.0 and
were too slow to repeat in-budget; 200g/CD_MAXMS=400 was used to stay in time.)


## Cordite fingerprint baseline (must not change)
`cordite_fingerprint.ts cordite handwritten 2,4,6 10` -> hash=3229187219
seq=[1,1,1,1,1,1,1,1,1,1,1,3,2,2,1,3,3,2,1,1,4,1,1,1,3,3,2,2,1,3]
(Verified identical at f51658d. Any core change must reproduce this.)
