/* arena.h - an arena's rules, lichess's: a time control, a start and a duration; while it runs the
 * server pairs its free players with each other, and each game scores points. A win scores 2, a
 * draw 1, a loss 0. After two wins in a row a player is on fire: a win scores 4 and a draw 2 until a
 * loss or a draw ends the streak. A draw before CC_ARENA_DRAW_PLIES plies scores nothing (an agreed
 * draw at once is not a way to bank points). No berserk.
 *
 * The kernel's because the browser shows the same rules (the fire mark, an arena's name as typed). */
#ifndef C_CHESS_ARENA_H
#define C_CHESS_ARENA_H

#include <stdbool.h>
#include <stddef.h>

enum { CC_ARENA_LOSS = 0, CC_ARENA_DRAW = 1, CC_ARENA_WIN = 2 };          /* a game's result, for one side */
enum { CC_ARENA_UPCOMING = 0, CC_ARENA_RUNNING = 1, CC_ARENA_FINISHED = 2 };   /* an arena's status */

#define CC_ARENA_FIRE 2              /* wins in a row after which a game scores double */
#define CC_ARENA_DRAW_PLIES 20       /* a draw shorter than this scores nothing */
#define CC_ARENA_NAME_MIN 3
#define CC_ARENA_NAME_MAX 30
#define CC_ARENA_DURATION_MIN 20     /* seconds (short, so a test runs a whole arena) */
#define CC_ARENA_DURATION_MAX 43200  /* 12 hours */
#define CC_ARENA_START_MAX 86400     /* an arena starts at most a day ahead */

/* The points a game scores for a side: its result (CC_ARENA_WIN ..), its streak before the game
 * (wins in a row), and the game's plies. */
int  cc_arena_points(int result, int streak, int plies);
/* The streak after a game (it stops counting at 255). */
int  cc_arena_streak(int result, int streak);
/* Whether a streak makes the next game score double. */
bool cc_arena_on_fire(int streak);
/* An arena's name: n bytes of letters, digits, space, '-' and '_', CC_ARENA_NAME_MIN to
 * CC_ARENA_NAME_MAX of them, no space at either end. */
bool cc_arena_name_ok(const char *s, size_t n);

#endif
