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
(appended below as runs complete)

## Cordite fingerprint baseline (must not change)
`cordite_fingerprint.ts cordite handwritten 2,4,6 10` -> hash=3229187219
seq=[1,1,1,1,1,1,1,1,1,1,1,3,2,2,1,3,3,2,1,1,4,1,1,1,3,3,2,2,1,3]
(Verified identical at f51658d. Any core change must reproduce this.)
