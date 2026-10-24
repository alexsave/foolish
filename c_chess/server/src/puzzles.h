/* puzzles.h - the puzzles: a file of fixed records (puzzle.h, made by tools/puzzlegen) read into
 * memory at start (CC_PUZZLES), each with a Glicko-2 rating that people's attempts move, and each
 * player's puzzle rating beside the game rating. An attempt is one rating period for both: the
 * puzzle is the opponent, solved a win. The ratings are journaled (cc_puzzles, cc_players).
 *
 * The page solves on its own board (the kernel's cp_solve, as wasm), so a try costs the server
 * nothing; it asks for a puzzle and says how it went, once per puzzle handed out. */
#ifndef C_CHESS_PUZZLES_H
#define C_CHESS_PUZZLES_H

#include "cc_server.h"
#include "cbase/app.h"

extern const cb_route_list cc_puzzle_routes;

/* Reads the file (no file, or no path: no puzzles, and the routes answer 404). Before recovery. */
int  cc_puzzles_load(const char *path, char *err, size_t cap);
void cc_puzzles_free(void);
/* Recovery: a puzzle's journaled rating (ignored when the file no longer has it). */
void cc_puzzle_recovered(uint32_t id, const g2_player *r, uint32_t plays);
uint32_t cc_puzzles_count(void);
/* For the tests: whether the order by rating is in order. */
bool cc_puzzles_ordered(void);

/* ---- under cc_lock ---- */
/* The puzzle to hand p: near its puzzle rating, not among the last it saw; NULL when none. */
const cp_puzzle *cc_puzzle_pick(cc_player *p, uint32_t *plays, uint32_t seed);
/* p's answer to the puzzle it was handed: false when id is not that puzzle (then nothing changes). */
bool cc_puzzle_result(cc_player *p, uint32_t id, bool win, cw_puzzle_done *out);

#endif
