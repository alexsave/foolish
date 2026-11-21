// playout - one random playout, as a Monte Carlo bot would run it: a fresh
// deal, then both seats play uniformly random moves off the LITE menu (foolish's
// rollout menu) until the game ends, with no repetition rule (a rollout's game
// carries none, as in foolish). Portable: tests/playout_host.c and
// tests/playout_rom.c run the same code, and must count the same moves.
#ifndef GD_PLAYOUT_H
#define GD_PLAYOUT_H
#include <stdint.h>
#define PLAYOUT_CAP 1000          // moves; a game past this is cut (and counted)
// One playout from the RNG's current state: its move count (PLAYOUT_CAP if cut).
uint16_t playout_one(void);
#define PLAYOUT_SEED 0x2468u
#define PLAYOUT_RUNS 64u
#endif
