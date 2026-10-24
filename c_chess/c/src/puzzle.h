/* puzzle.h - a puzzle: a position, the opponent's move that just went wrong (played for the
 * solver), and the line that punishes it, the solver's moves at the odd places. Fixed 128 bytes, the
 * same in the generator's file (tools/puzzlegen), the server's memory and the wire. */
#ifndef C_CHESS_PUZZLE_H
#define C_CHESS_PUZZLE_H

#include "chess.h"

#include <stdint.h>

#define CP_MOVES 12
#define PUZZLE_FILE_MAGIC "CCPZ0001"   /* a puzzle file: these 8 bytes, a u32 count, then the records */

typedef struct {
    char     fen[92];          /* the position before moves[0] */
    uint8_t  n;                /* moves in the line: moves[0] the opponent's, then solver, opponent, ... */
    uint8_t  themes;           /* CP_T_* */
    uint8_t  pad[2];
    uint16_t moves[CP_MOVES];
    int16_t  rating;
    uint16_t rd;
    uint32_t id;
} cp_puzzle;
_Static_assert(sizeof(cp_puzzle) == 128, "cp_puzzle");

enum { CP_T_MATE = 1, CP_T_ONEMOVE = 2, CP_T_SHORT = 4, CP_T_LONG = 8 };

/* Whether `played` solves the ply where `expected` is the line's move in position p: the same
 * move, or any move that mates at once (a puzzle accepts every mate). */
bool cp_accepts(const chess_pos *p, ch_move expected, ch_move played);

/* A solve in progress: the board the solver sees and how far along the line it is. */
typedef struct {
    cp_puzzle  p;
    chess_pos  pos;
    int        at;        /* the next line index the solver must match (odd), or p.n when solved */
    int        state;     /* CP_SOLVING, CP_SOLVED, CP_FAILED */
    ch_move    last[2];   /* the solver's move and the reply played for it, by cp_try */
} cp_solve;

enum { CP_SOLVING, CP_SOLVED, CP_FAILED };

/* Starts a solve: the position from the FEN with moves[0] played. -1 when the record is bad
 * (an unreadable FEN, an illegal move anywhere in the line). */
int  cp_begin(cp_solve *s, const cp_puzzle *p);
/* The solver plays m: CP_SOLVING (right, and the reply is played: last[1]), CP_SOLVED (right and
 * the line is done), CP_FAILED (wrong; the position is left before m). After the end, the state. */
int  cp_try(cp_solve *s, ch_move m);
/* Shows the line: plays its next move (the solver's or the reply) whatever the state, into
 * *played; false at the end. A failed solve shows the answer this way. */
bool cp_reveal(cp_solve *s, ch_move *played);

#endif
