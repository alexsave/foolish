| comparison | metric | X mean | Y mean | X - Y | % | range X | range Y | sets overlap? | outside spread? |
|---|---|--:|--:|--:|--:|--:|--:|---|---|
| B vs A: P1 vs baseline (split) | moves/s | 50,280 | 37,086 | +13,195 | +35.6% | 1,595 | 1,618 | no | YES |
| B vs A: P1 vs baseline (split) | CPU us/move | 29.85 | 37.59 | -7.74 | -20.6% | 0.28 | 1.47 | no | YES |
| B vs A: P1 vs baseline (split) | loop us/move | 19.12 | 20.25 | -1.12 | -5.5% | 0.69 | 0.95 | no | YES |
| B vs A: P1 vs baseline (split) | flusher us/move | 3.86 | 10.27 | -6.41 | -62.4% | 0.11 | 0.65 | no | YES |
| B vs A: P1 vs baseline (split) | p99 ms | 6.68 | 12.62 | -5.94 | -47.1% | 0.25 | 1.18 | no | YES |
| C vs A: P3 vs baseline (split) | moves/s | 38,385 | 37,086 | +1,299 | +3.5% | 1,931 | 1,618 | yes | no |
| C vs A: P3 vs baseline (split) | CPU us/move | 36.68 | 37.59 | -0.91 | -2.4% | 1.49 | 1.47 | yes | no |
| C vs A: P3 vs baseline (split) | loop us/move | 19.39 | 20.25 | -0.86 | -4.2% | 1.01 | 0.95 | yes | no |
| C vs A: P3 vs baseline (split) | flusher us/move | 10.23 | 10.27 | -0.03 | -0.3% | 0.46 | 0.65 | yes | no |
| C vs A: P3 vs baseline (split) | p99 ms | 12.36 | 12.62 | -0.27 | -2.1% | 0.96 | 1.18 | yes | no |
| E vs A: ALL vs baseline (split) | moves/s | 49,502 | 37,086 | +12,417 | +33.5% | 2,089 | 1,618 | no | YES |
| E vs A: ALL vs baseline (split) | CPU us/move | 30.34 | 37.59 | -7.25 | -19.3% | 0.93 | 1.47 | no | YES |
| E vs A: ALL vs baseline (split) | loop us/move | 19.38 | 20.25 | -0.86 | -4.3% | 0.90 | 0.95 | yes | no |
| E vs A: ALL vs baseline (split) | flusher us/move | 3.96 | 10.27 | -6.31 | -61.4% | 0.12 | 0.65 | no | YES |
| E vs A: ALL vs baseline (split) | p99 ms | 6.88 | 12.62 | -5.74 | -45.5% | 0.38 | 1.18 | no | YES |
| N vs A: N1 vs baseline (split) | moves/s | 38,574 | 37,086 | +1,488 | +4.0% | 470 | 1,618 | no | no |
| N vs A: N1 vs baseline (split) | CPU us/move | 36.49 | 37.59 | -1.10 | -2.9% | 0.09 | 1.47 | no | no |
| N vs A: N1 vs baseline (split) | loop us/move | 19.41 | 20.25 | -0.84 | -4.1% | 0.27 | 0.95 | no | no |
| N vs A: N1 vs baseline (split) | flusher us/move | 10.02 | 10.27 | -0.24 | -2.4% | 0.09 | 0.65 | yes | no |
| N vs A: N1 vs baseline (split) | p99 ms | 12.09 | 12.62 | -0.54 | -4.3% | 0.44 | 1.18 | yes | no |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | moves/s | 47,304 | 37,086 | +10,219 | +27.6% | 4,538 | 1,618 | no | YES |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | CPU us/move | 34.20 | 37.59 | -3.39 | -9.0% | 2.28 | 1.47 | no | YES |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | loop us/move | 15.51 | 20.25 | -4.73 | -23.4% | 1.16 | 0.95 | no | YES |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | flusher us/move | 9.50 | 10.27 | -0.77 | -7.5% | 1.30 | 0.65 | yes | no |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | p99 ms | 9.93 | 12.62 | -2.69 | -21.3% | 1.62 | 1.18 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | moves/s | 60,178 | 47,304 | +12,874 | +27.2% | 1,428 | 4,538 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | CPU us/move | 28.91 | 34.20 | -5.29 | -15.5% | 0.98 | 2.28 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | loop us/move | 14.85 | 15.51 | -0.66 | -4.3% | 1.00 | 1.16 | yes | no |
| G vs F: the PRs under cpu1 (ALL vs D) | flusher us/move | 4.95 | 9.50 | -4.54 | -47.8% | 0.15 | 1.30 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | p99 ms | 6.57 | 9.93 | -3.36 | -33.9% | 0.62 | 1.62 | no | YES |
| G vs A: ALL cpu1 vs baseline split | moves/s | 60,178 | 37,086 | +23,092 | +62.3% | 1,428 | 1,618 | no | YES |
| G vs A: ALL cpu1 vs baseline split | CPU us/move | 28.91 | 37.59 | -8.68 | -23.1% | 0.98 | 1.47 | no | YES |
| G vs A: ALL cpu1 vs baseline split | loop us/move | 14.85 | 20.25 | -5.40 | -26.7% | 1.00 | 0.95 | no | YES |
| G vs A: ALL cpu1 vs baseline split | flusher us/move | 4.95 | 10.27 | -5.31 | -51.8% | 0.15 | 0.65 | no | YES |
| G vs A: ALL cpu1 vs baseline split | p99 ms | 6.57 | 12.62 | -6.06 | -48.0% | 0.62 | 1.18 | no | YES |
