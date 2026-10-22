/* pgn.h - a finished or running game as PGN text (an export format only: nothing reads PGN). */
#ifndef C_CHESS_PGN_H
#define C_CHESS_PGN_H

#include <stddef.h>
#include "game.h"

typedef struct {
    const char *white, *black;   /* names */
    const char *site;            /* the game's URL */
    const char *date;            /* "YYYY.MM.DD" */
    int white_elo, black_elo;    /* 0: not written */
    int base_s, inc_s;
    int status, winner;          /* enum cc_status, CH_WHITE / CH_BLACK / CC_NO_WINNER */
} cc_pgn_info;

/* "1-0", "0-1", "1/2-1/2", or "*" while the game runs. */
const char *cc_result_str(int status, int winner);
/* Writes the PGN of the moves into out (NUL-terminated); returns the length, or the length it
 * needed (>= cap) when out was too small. */
size_t cc_pgn(const cc_pgn_info *info, const ch_move *moves, int n, char *out, size_t cap);

#endif
