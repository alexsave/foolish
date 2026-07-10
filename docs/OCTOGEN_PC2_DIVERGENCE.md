# octogen · pc 2 — transposition-table divergence

Directly-measured fraction of games whose move sequence differs from the TT22 reference, by `CD_TT_BITS`, with a 95% Wilson interval. Seed-keyed and deduped, so this is poolable and keeps tightening as more seeds run. Model column is the baseline-free upper bound P(W > 2^bits) from the working-set distribution; latency is avg protagonist decision CPU time (CD_LAT pass).

Reference: TT22 · up to **720 seeds** per size.

| CD_TT_BITS | entries | bytes | games | diverged | divergence | 95% interval | model P(W>M) | avg decision |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| **TT8** | 256 | 4 KiB | 720 | 10 | 1.39% | 0.756% – 2.54% | 57.73% | — |
| **TT9** | 512 | 8 KiB | 720 | 5 | 0.694% | 0.297% – 1.62% | 45.27% | — |
| **TT10** | 1.024k | 16 KiB | 720 | 2 | 0.278% | 0.076% – 1.01% | 26.53% | — |
| **TT11** | 2.048k | 32 KiB | 720 | 1 | 0.139% | 0.025% – 0.783% | 8.20% | — |
| **TT12** | 4.096k | 64 KiB | 720 | 1 | 0.139% | 0.025% – 0.783% | 0.733% | — |
| **TT13** **◄ shipped** | 8.192k | 128 KiB | 720 | 1 | 0.139% | 0.025% – 0.783% | 0 | — |
| **TT14** | 16.384k | 256 KiB | 720 | 2 | 0.278% | 0.076% – 1.01% | 0 | — |
| **TT15** | 32.768k | 512 KiB | 720 | 2 | 0.278% | 0.076% – 1.01% | 0 | — |
| **TT16** | 65.536k | 1 MiB | 720 | 1 | 0.139% | 0.025% – 0.783% | 0 | — |
| **TT17** | 131.072k | 2 MiB | 720 | 1 | 0.139% | 0.025% – 0.783% | 0 | — |
| **TT18** | 262.144k | 4 MiB | 720 | 1 | 0.139% | 0.025% – 0.783% | 0 | — |

_“diverged” counts games where octogen played a different move sequence than at TT22. A zero row means none seen yet — the interval is the rule-of-three upper bound, not proof of zero._
