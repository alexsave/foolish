#ifndef FOOLYARD_INVARIANT_H
#define FOOLYARD_INVARIANT_H

#include "types.h"
#include "game.h"

// Findings. The first three are sem_fuzz's kernel invariants, re-asserted here
// because the sim reaches states a straight-line fuzzer cannot: a move chosen
// against a view that is several versions old, arriving interleaved with a bot
// cycle. The rest only exist once there is a wire and a clock.
#define FIND_CONSERVATION 0   // an accepted move broke the physical deck
#define FIND_MUTATION     1   // a REJECTED move still changed the game
#define FIND_STALL        2   // a live game with a legal move went quiet
#define FIND_PHANTOM_LOSS 3   // a card left a client's hand with no move of its own
#define FIND_DUP_APPLIED  4   // the same (seat, seq) was applied twice
#define FIND_VIEW_REGRESS 5   // a client accepted a view older than one it held
#define FIND_QUEUE_FULL   6   // the per-game request backlog overflowed
#define FIND_SEAT_MISMATCH 7  // the server applied a move for the wrong seat
#define FIND_CROSS_DEAL   8   // a move decided in one game applied to the next
#define FIND_MOVE_LATE    9   // a move applied AFTER a newer one from the same seat
#define FIND_COUNT       10

typedef struct Findings {
    u64 count[FIND_COUNT];
    int printed[FIND_COUNT];   // per KIND, so a noisy finding cannot bury a rare one
} Findings;

struct World;

const char *inv_name(int kind);

// True if the board is a partition of the dealt deck. Lifted from
// server/impls/native/sem_fuzz.c (conservation_ok).
int inv_conservation(const Game *g, char *why, int whycap);

void inv_report(struct World *w, int kind, const char *fmt, ...);
void inv_sweep(struct World *w);
void inv_print(struct World *w);

#endif
