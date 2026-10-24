/* ai.h - the computer as a player: eight levels of Stockfish (engine.h), each a player of its own
 * with a fixed id, which moves when it is its turn in a game in play. Casual games only. */
#ifndef C_CHESS_AI_H
#define C_CHESS_AI_H

#include "cc_server.h"

#define CC_AI_LEVELS 8

/* Under cc_lock. The player of a level (1..8), made on first use; NULL for another level. */
cc_player *cc_ai_player(int level);
/* The level of a computer player, 0 for a person. */
int        cc_ai_level(const cc_player *p);
/* Under cc_lock, after any change to a game: when it is the computer's turn and no search is in
 * flight for this ply, one is queued; its answer is played when it arrives (if the game is still
 * at that ply). */
void       cc_ai_maybe_move(cc_live *l, int64_t now);
/* Searches asked and refused (a full queue), for /c/stats. */
void       cc_ai_counts(uint64_t *asked, uint64_t *refused);

#endif
