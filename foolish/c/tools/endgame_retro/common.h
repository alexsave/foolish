// common.h - the small helpers every endgame_retro tool carried a copy of: who
// is still in a heads-up position, who acts, and a set of card ids as a mask.
// Include it AFTER cordite_sim.c (SimState, sim_should_act) - each tool pulls
// the simulator in by including its source. The tools own their positions
// tables and their interning (the table sizes differ by tool and are tuned to
// it), so those stay in the tool.
#ifndef RETRO_COMMON_H
#define RETRO_COMMON_H

#include <stdint.h>

// The first two seats still IN a hand (a, b; -1 when fewer).
static void two_in(const SimState *s, int *a, int *b) {
    *a = -1; *b = -1;
    for (int i = 0; i < s->num_players; i++)
        if (s->status_p[i] == PLAYER_STATUS_IN) { if (*a < 0) *a = i; else *b = i; }
}

// The seat to move: the defender if it has to act, else the first that does; -1 for none.
static int actor_of(const SimState *s) {
    if (sim_should_act(s, s->defender)) return s->defender;
    for (int i = 0; i < s->num_players; i++) if (sim_should_act(s, i)) return i;
    return -1;
}

// A list of card ids as a bit mask.
static uint64_t mask_of(const int *ids, int n) {
    uint64_t m = 0;
    for (int i = 0; i < n; i++) m |= 1ull << ids[i];
    return m;
}

#endif
