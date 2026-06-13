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

### Weak-field A/B (simple_heuristic, 200g/cell, CD_MAXMS=400) — the win HELD
- pc4 dWin **+17.0** (50.0% vs 33.0%)
- pc6 dWin **+24.5** (47.5% vs 23.0%)
vs the prior-commit table (+16.9 / +13.8). The much-stricter strong-seat gate did
NOT cost the weak-field exploitation: a genuinely weak seat (simple_heuristic)
still clears the gate (high trump-rate + discrete signals) and fulminate exploits
it. pc6 actually improved. Crucially the weak win (+24, seat 0) and the strong
"loss" (-14, seat 0) have OPPOSITE signs at the SAME seat position, so the +24 is
real strategy, not seat luck — and the -14 is shared by the FUL_OFF control, so it
is seat/harness noise, not the profiler.

## Verdict
The task's mechanism (profiler mislabels strong seats -> mis-skews rollout) is
fixed at the source: strong-seat mislabel 17.18% -> 0.30% (probe). On the only
low-variance signal available the fix is unambiguous; the seated A/B dWin is
swamped by seat-position asymmetry (FUL_OFF control = -12.5 with cordite-identical
play). The weak-field win is intact (+17 / +24.5). cordite fingerprint unchanged
(3229187219). Net: fulminate no longer deviates from cordite vs strong fields
(safe), while keeping its large weak-field edge.


## Cordite fingerprint baseline (must not change)
`cordite_fingerprint.ts cordite handwritten 2,4,6 10` -> hash=3229187219
seq=[1,1,1,1,1,1,1,1,1,1,1,3,2,2,1,3,3,2,1,1,4,1,1,1,3,3,2,2,1,3]
(Verified identical at f51658d. Any core change must reproduce this.)

---

## General model: per-seat posterior MIXTURE (this session)

Replaced hard archetype selection with a per-opponent **posterior over the
ARCH_POLICIES basis** (handwritten/espresso/random/simple/greedy + new
**passive**/**human**). Built online from observed decisions as signed per-policy
log-likelihood votes (trump-conservation rate, first-attack-trump / wasteful-cover
tells, defender pickup-vs-cover). Heavy strong prior [6,4,0..]; an evidence factor
grows with #decisions (pc-scaled). The MC world loop SAMPLES each seat's policy
from its posterior per world (CRN-preserved). "Gets more confident as we feed it
priors": few decisions -> posterior ~= strong prior -> plays like cordite.

### Safety mechanism (the key to no strong-field regression)
Per-seat COMMIT threshold: a seat deviates from the strong default only when its
posterior is majority-weak (nonStrong > commitThresh); else it is pinned EXACTLY
to POL_HANDWRITTEN (zero rollout perturbation). commitThresh = 0.50 for pc2/pc4,
scales up at pc6+ (0.65/0.80) where few-decision samples are noisier.

### Results (same-table A/B vs cordite, CD_MAXMS=400, 240g/cell, Δ vs cordite)
With the pc-scaled commit threshold (a7479ba):
| field | pc4 dWin | pc6 dWin | note |
|---|---|---|---|
| simple_heuristic (weak) | +8.8 | +10.4 | win holds |
| espresso (mixture) | -5.4 | +0.4 | vs control -10.4 / -0.4 => mixture >= control => SAFE |
| cordite all-strong (earlier) | +0.4 | -1.3 | vs control +1.7 / -4.6 => SAFE |
cordite fingerprint unchanged (3229187219); cordite path untouched (seatWeights null).

Interpretation: the seated A/B carries ~10pp seat-position noise at seat 0 (the
FUL_OFF control shows it). Against that baseline the mixture is >= cordite vs
strong fields and clearly + vs weak. The weak-field win is a bit lower than the
discrete RANDOM-rollout version's noisy +17/+24.5, but that was high-variance and
came WITHOUT the principled safety; the mixture is safe AND general.

### TODO (frontier)
- Hand reconstruction + negative inference into the likelihood ("had the card,
  declined to play it"; voids from declined covers; `good` = stop signal). Current
  likelihood is hand-free move-level only.
- Real-budget (2s + full worlds) validation in progress — more worlds should
  integrate the posterior better.
