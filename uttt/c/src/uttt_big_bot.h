/* A bot for the recursive game (uttt_big.h): GREEDY LOOKAHEAD.
 *
 * WHY NOT ROLLOUTS. The 9 x 9 bots (uttt_bots.h) sample whole games, which
 * is right for 81 cells and impossible for 59,049: a depth-5 game is about
 * 40,000 plies, so one rollout is a whole game and a move would take seconds.
 * This bot looks a few plies ahead instead and counts what can be WON there.
 *
 * THE SCORE IS MATERIAL. Every node won is worth its size's weight (below),
 * the root's win is worth more than the whole rest of the board, and a line
 * of moves is worth the weights the mover wins on it minus the weights the
 * opponent wins on it. A move that wins a 3 x 3 which completes a 9 x 9 wins
 * both, 1 + 9. A draw is worth nothing to either side.
 *
 * THE SEARCH is negamax with alpha-beta over that score, iterative deepening
 * from one ply to `plies`, and every move is a real utb_play: nothing here
 * knows the rules, only the arithmetic of what a play changed. Only moves can
 * win things, and the leaf is not evaluated beyond what was won on the way to
 * it, so "how many grids can this move win within the next N plies, minus
 * how many does it hand over" is exactly the value.
 *
 * BOUNDED, so a move is always fast:
 *   - A NODE BUDGET (`budget`, in work units: a play is one, nine cells
 *     scanned for candidates is one). The first iteration (one ply) always
 *     completes, so there is always a move; a deeper iteration that runs out
 *     is thrown away whole and the previous one's choice stands.
 *   - A CANDIDATE CAP per node (cap_root at the root, cap_node below). A
 *     region of nine cells is searched whole. A relaxed region can be
 *     thousands of cells, and then the candidates are every move that wins
 *     its 3 x 3 (the only moves that can score at once; when there are more
 *     than the cap, the ones whose play wins the most), and a uniform random
 *     sample of the rest up to the cap.
 *
 * TIES ARE THE BOT'S OWN DICE. Each bot has its own random stream, seeded
 * once; the root's candidates are shuffled by it before the search and the
 * first of the best is played, so two bots with this one rule still play two
 * different games. A game is a function of the two seeds.
 *
 * TESTFLIGHT / WEB ONLY, like uttt_big.h: hidden, and not in the Makefile's
 * SRC, so no app build links it.
 */
#ifndef UTTT_BIG_BOT_H
#define UTTT_BIG_BOT_H

#include "uttt_big.h"
#include <stdint.h>

/* Hidden one by one rather than by a pragma pair, as uttt_big_diag.h does: a
 * push/pop pair lints as mismatched in the editor. */
#define UTB_BOT_HIDDEN __attribute__((visibility("hidden")))

/* THE WEIGHTS, by the side of the grid won. Nine times per size, the owner's
 * scale: a 9 x 9 is nine 3 x 3s' worth. The game outweighs every other node
 * on the deepest board together (6561 + 729*9 + 81*81 + 9*729 = 26,244), so
 * no amount of material is ever traded for it. */
#define UTB_BOT_W3      1
#define UTB_BOT_W9      9
#define UTB_BOT_W27     81
#define UTB_BOT_W81     729
#define UTB_BOT_WGAME   (1 << 20)

/* THE DEFAULTS the 243 page plays at. Measured natively (-O3 -flto, an M-class
 * Mac), whole depth-5 games, the two settings against each other with the
 * seats alternated:
 *
 *     plies 2, budget 1500     169,000 moves/s
 *     plies 3, budget 1500      69,000 moves/s    beat plies 2: 2-0, 2 drawn
 *     plies 4, budget 4000      21,000 moves/s    beat plies 3: 4-1, 1 drawn
 *
 * Four plies is the stronger game and still a whole game in under two
 * seconds natively; tests/uttt_big_bot_test.c prints the speed every run. */
#define UTB_BOT_PLIES     4
#define UTB_BOT_BUDGET    4000
#define UTB_BOT_CAP_ROOT  48
#define UTB_BOT_CAP_NODE  12
#define UTB_BOT_PLIES_MAX 6
#define UTB_BOT_CAP_MAX   64

typedef struct {
    uint64_t rng;          /* the bot's own stream, never 0                      */
    int      plies;        /* the deepest iteration, 1..UTB_BOT_PLIES_MAX        */
    int      budget;       /* work units a move may spend past its first ply     */
    int      cap_root;     /* candidates at the root, 1..UTB_BOT_CAP_MAX         */
    int      cap_node;     /* ...and below it                                     */
    /* what the last move saw, for the page */
    int      value;        /* the chosen move's value at the deepest finished ply */
    int      depth;        /* that ply                                            */
    int      work;         /* work units the move spent                           */
} UtbBot;

/* The weight of winning node `id` of `g` (the game's for the root). */
UTB_BOT_HIDDEN int  utb_bot_weight(const UtbGame *g, int id);

/* A bot with the defaults above, its stream seeded from `seed` (any value,
 * 0 included). */
UTB_BOT_HIDDEN void utb_bot_init(UtbBot *b, uint64_t seed);

/* THE SEAT'S BOT FROM THE GAME'S ONE SEED: a page or a test holds one seed
 * for the whole game, and each seat (UTTT_X, UTTT_O) gets its own stream from
 * it, so the two bots never share dice and the seed names the game. */
UTB_BOT_HIDDEN void utb_bot_seat(UtbBot *b, uint64_t seed, int seat);

/* The bot's move for the side to play: a legal leaf, or -1 when the game is
 * over. `g` is searched in place and handed back exactly as it came. */
UTB_BOT_HIDDEN int  utb_bot_move(UtbBot *b, UtbGame *g);

/* What playing `mv` wins for the side to move, by the weights: 0 for a move
 * that wins nothing, -1 for an illegal one. */
UTB_BOT_HIDDEN int  utb_bot_gain(UtbGame *g, int mv);

/* The search's value of `mv` for the side to move at `plies` (1 is the gain
 * alone; 2 is the gain minus the opponent's best reply's; and so on), with
 * the bot's caps and no budget. INT32_MIN for an illegal move. */
UTB_BOT_HIDDEN int  utb_bot_value(UtbBot *b, UtbGame *g, int mv, int plies);

/* Everything `side` has won, by the weights (the game's included). */
UTB_BOT_HIDDEN int  utb_bot_material(const UtbGame *g, int side);

#endif
