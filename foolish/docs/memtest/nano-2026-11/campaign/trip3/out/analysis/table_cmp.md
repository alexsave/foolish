| comparison | metric | X mean | Y mean | X - Y | % | range X | range Y | sets overlap? | outside spread? |
|---|---|--:|--:|--:|--:|--:|--:|---|---|
| M1 vs D: PR 496 on merged code | moves/s | 48,586 | 37,040 | +11,546 | +31.2% | 3,600 | 1,238 | no | YES |
| M1 vs D: PR 496 on merged code | CPU us/move | 30.85 | 37.64 | -6.79 | -18.0% | 1.63 | 1.04 | no | YES |
| M1 vs D: PR 496 on merged code | loop us/move | 19.76 | 20.33 | -0.57 | -2.8% | 1.50 | 0.41 | yes | no |
| M1 vs D: PR 496 on merged code | flusher us/move | 3.99 | 10.21 | -6.22 | -60.9% | 0.09 | 0.71 | no | YES |
| M1 vs D: PR 496 on merged code | p99 ms | 6.92 | 12.87 | -5.95 | -46.2% | 0.69 | 0.82 | no | YES |
| M1 vs D: PR 496 on merged code | push > 100 ms | 9.0 | 8.7 | +0.3 | +3.8% | 3.0 | 6.0 | yes | no |
| M1 vs D: PR 496 on merged code | RSS max kB | 30,172 | 30,683 | -511 | -1.7% | 1,420 | 68 | yes | no |
| M2 vs M1: PR 497 | moves/s | 50,451 | 48,586 | +1,865 | +3.8% | 1,057 | 3,600 | yes | no |
| M2 vs M1: PR 497 | CPU us/move | 29.63 | 30.85 | -1.22 | -3.9% | 0.96 | 1.63 | yes | no |
| M2 vs M1: PR 497 | loop us/move | 18.67 | 19.76 | -1.09 | -5.5% | 0.62 | 1.50 | yes | no |
| M2 vs M1: PR 497 | flusher us/move | 3.90 | 3.99 | -0.09 | -2.2% | 0.21 | 0.09 | yes | no |
| M2 vs M1: PR 497 | p99 ms | 6.72 | 6.92 | -0.19 | -2.8% | 0.05 | 0.69 | yes | no |
| M2 vs M1: PR 497 | push > 100 ms | 9.7 | 9.0 | +0.7 | +7.4% | 7.0 | 3.0 | yes | no |
| M2 vs M1: PR 497 | RSS max kB | 30,797 | 30,172 | +625 | +2.1% | 164 | 1,420 | yes | no |
| M3 vs M2: PR 498 | moves/s | 50,802 | 50,451 | +351 | +0.7% | 2,334 | 1,057 | yes | no |
| M3 vs M2: PR 498 | CPU us/move | 29.94 | 29.63 | +0.31 | +1.1% | 1.08 | 0.96 | yes | no |
| M3 vs M2: PR 498 | loop us/move | 18.79 | 18.67 | +0.12 | +0.6% | 0.85 | 0.62 | yes | no |
| M3 vs M2: PR 498 | flusher us/move | 4.03 | 3.90 | +0.12 | +3.2% | 0.19 | 0.21 | yes | no |
| M3 vs M2: PR 498 | p99 ms | 6.88 | 6.72 | +0.16 | +2.4% | 0.43 | 0.05 | yes | no |
| M3 vs M2: PR 498 | push > 100 ms | 10.0 | 9.7 | +0.3 | +3.4% | 5.0 | 7.0 | yes | no |
| M3 vs M2: PR 498 | RSS max kB | 30,840 | 30,797 | +43 | +0.1% | 308 | 164 | yes | no |
| M4 vs M3: PR 500 | moves/s | 51,249 | 50,802 | +447 | +0.9% | 2,128 | 2,334 | yes | no |
| M4 vs M3: PR 500 | CPU us/move | 29.48 | 29.94 | -0.46 | -1.5% | 0.41 | 1.08 | yes | no |
| M4 vs M3: PR 500 | loop us/move | 18.42 | 18.79 | -0.37 | -2.0% | 0.33 | 0.85 | yes | no |
| M4 vs M3: PR 500 | flusher us/move | 3.94 | 4.03 | -0.09 | -2.2% | 0.06 | 0.19 | yes | no |
| M4 vs M3: PR 500 | p99 ms | 6.56 | 6.88 | -0.33 | -4.7% | 0.29 | 0.43 | yes | no |
| M4 vs M3: PR 500 | push > 100 ms | 8.0 | 10.0 | -2.0 | -20.0% | 6.0 | 5.0 | yes | no |
| M4 vs M3: PR 500 | RSS max kB | 30,580 | 30,840 | -260 | -0.8% | 884 | 308 | yes | no |
| M3 vs D: everything merged | moves/s | 50,802 | 37,040 | +13,762 | +37.2% | 2,334 | 1,238 | no | YES |
| M3 vs D: everything merged | CPU us/move | 29.94 | 37.64 | -7.70 | -20.4% | 1.08 | 1.04 | no | YES |
| M3 vs D: everything merged | loop us/move | 18.79 | 20.33 | -1.54 | -7.6% | 0.85 | 0.41 | no | YES |
| M3 vs D: everything merged | flusher us/move | 4.03 | 10.21 | -6.19 | -60.6% | 0.19 | 0.71 | no | YES |
| M3 vs D: everything merged | p99 ms | 6.88 | 12.87 | -5.98 | -46.5% | 0.43 | 0.82 | no | YES |
| M3 vs D: everything merged | push > 100 ms | 10.0 | 8.7 | +1.3 | +15.4% | 5.0 | 6.0 | yes | no |
| M3 vs D: everything merged | RSS max kB | 30,840 | 30,683 | +157 | +0.5% | 308 | 68 | yes | no |
