// selfplay - see selfplay.c.
#ifndef GD_SELFPLAY_H
#define GD_SELFPLAY_H
#include <stdint.h>
// Play `games` seeded games; the CRC-16 of everything, and the moves played.
uint16_t selfplay(uint16_t seed, uint8_t games, uint16_t *moves_out);
// The run both builds make: the seed and the game count, in one place.
#define SELFPLAY_SEED  0x1234u
#define SELFPLAY_GAMES 12u
#define SELFPLAY_TRACE 64
extern uint16_t selfplay_trace[SELFPLAY_TRACE];
#define SELFPLAY_LOG 1024
extern uint8_t selfplay_log[SELFPLAY_LOG];
#endif
