/* game_keep.h - the port's game as a host keeps it: canonical between operations, and kept
 * across loads as the kernel's state blob (README, Reload). Linked by the table and by the
 * native tests/blob_test.c, so the test checks the rule the table runs. */
#ifndef GAME_KEEP_H
#define GAME_KEEP_H
#include "game.h"
#include "view.h"

/* The room a blob of a two-seat game needs, with plenty to spare (a whole deck in hands, the
 * battles, the tail: well under 200 bytes). */
#define GAME_KEEP_CAP 1024

/* After every operation on the resident game (a move applied, a bot step, a deal): the board a
 * load of its own blob would hold (view.h state_canonical), so the next operation plays on the
 * row's board and not on dead state the blob does not carry (a seat's raw awaiting flag, the
 * log's count, an unflipped trump's stale card). */
void game_keep_canonical(Game *g);

/* The blob of g (state_blob_put, a zero clock): its length, or <= 0 when it cannot be written. */
int game_keep_put(const Game *g, unsigned char *out);

/* Adopts a stored blob into g when it reads as a two-seat game this table can play (format,
 * game_validate, two seats, playing or over); g is untouched otherwise. The seats' kinds are not
 * in the blob (the kernel's table_load takes them from its roster): seat 0 is the human, seat 1
 * the handwritten bot. Returns 1 when adopted. */
int game_keep_load(Game *g, const unsigned char *p, int len);

#endif
