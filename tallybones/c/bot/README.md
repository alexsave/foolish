# tallybones/c/bot - the exact single-player solver

An internal simulation and evaluation tool.
It is not in the app, not in the lobby and not in any Swift.
Tallybones has no interaction between seats, so the bot maximises the expected value of its own final score and models no opponent.
It solves the game exactly by backward induction; no randomness is used anywhere in the solver.
DECISIONS.md T30 to T35 are the design and the numbers.

## Files

- `tb_bot.h`, `tb_bot.c`: the tables, the induction, the policy API (`tb_bot_choose`) and the simulation.
- `tb_solve.c`: the binary.
- `tb_bot_test.c`: every check, in eight groups (G1 to G8).
- `MUTATIONS.md`: the break that turns each check red.

Scoring is the kernel's `tb_score_of` (`../src/tb.c`), compiled in from this Makefile; nothing in `../src` or `../tests` is edited.

## Targets

- `make -C tallybones/c/bot run`: build and run every check with 200,000 simulated games (about 35 s on 8 cores).
- `make -C tallybones/c/bot asan`: the same under ASan and UBSan with 20,000 games (about 50 s).
- `make -C tallybones/c/bot solve`: `build/tb_solve play 200000`, the exact value, the simulated mean and a histogram.
- `build/tb_solve [-t THREADS] [play N [SEED]]`.

## Numbers

- Exact expected final score of optimal play: **245.870775**.
- The published optimum for the branded 13-category game without its extra five-alike bonus and joker rule is about 245.87; they agree to every digit quoted.
- 200,000 games by the policy: mean 245.9042, standard error 0.0890, so the exact value is 0.38 standard errors away.
- Building the table (8192 x 64 states): 8.4 s on one thread, 2.2 s on 8 (Apple silicon, -O2).
