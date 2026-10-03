/* A bot for the recursive game (uttt_big.h): GREEDY LOOKAHEAD.
 *
 * WHY NOT ROLLOUTS. The 9 x 9 bots (uttt_bots.h) sample whole games, which
 * is right for 81 cells and impossible for 59,049: a depth-5 game is about
 * 40,000 plies, so one rollout is a whole game and a move would take seconds.
 * This bot looks a few plies ahead instead and counts what can be WON there.
 *
 * THE SCORE IS MATERIAL AND THREATS. Every node won is worth its size's
 * weight (below), the root's win is worth more than the whole rest of the
 * board, and every open two-in-a-row is worth two ninths of the win it
 * threatens. A line of moves is worth what the mover gains on it minus what
 * the opponent gains. A move that wins a 3 x 3 which completes a 9 x 9 wins
 * both, 9 + 81, and the threats it made or ended. A draw wins nothing.
 *
 * THE SEARCH is negamax with alpha-beta over that score, iterative deepening
 * from one ply to `plies`, and every move is a real utb_play: nothing here
 * knows the rules, only the arithmetic of what a play changed. The leaf is
 * not evaluated beyond what changed on the way to it, which is exact: the
 * score is a sum over grids and a play changes only the grids on its path.
 *
 * ONE PLY PAST THE HORIZON when the last ply decides a grid (`extend`): the
 * reply's best one-ply gain is taken off. Deciding a grid is what relaxes
 * the opponent's region, so it is the move whose price a fixed horizon hides.
 * The reply is not extended again, and the first iteration (the floor that
 * always finishes) is never extended.
 *
 * BOUNDED, so a move is always fast:
 *   - A NODE BUDGET (`budget`, in work units: a play is one, nine cells
 *     scanned for candidates is one, nine grids re-read for threats under a
 *     decided grid is one). The first iteration (one ply) always completes,
 *     so there is always a move; a deeper iteration that runs out is thrown
 *     away whole and the previous one's choice stands.
 *   - A CANDIDATE CAP per node (cap_root at the root, cap_node below, both
 *     derived from the plies and the budget by utb_bot_set). A region of nine
 *     cells is searched whole. A relaxed region can be thousands of cells,
 *     and then the candidates are every move that wins its 3 x 3 (the only
 *     moves that can score a win at once; when there are more than the cap,
 *     the ones whose play wins the most), and a uniform random sample of the
 *     rest up to the cap.
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
 * scale: a 9 x 9 is nine 3 x 3s' worth. In ninths, so that a threat (below)
 * has a whole number between a 3 x 3's win and a cell's. The game outweighs
 * every other node and every threat on the deepest board together (material
 * 4 * 59,049 = 236,196; threats at most 8 lines a grid, 5 * 104,976), so no
 * amount of either is ever traded for it. */
#define UTB_BOT_W3      9
#define UTB_BOT_W9      81
#define UTB_BOT_W27     729
#define UTB_BOT_W81     6561
#define UTB_BOT_WGAME   (1 << 22)

/* THREATS. An open line of a grid that holds two of a side's marks (won
 * children, or cells in a 3 x 3) and whose third child is still open is a
 * THREAT, worth two ninths of that grid's win: 2 in a 3 x 3, 18 in a 9 x 9,
 * 162 in a 27 x 27, 1,458 in an 81 x 81, and 2 * 6,561 = 13,122 in the
 * game's own grid. So a threat is worth less than the win it threatens and
 * more than a win one level down (a 9 x 9 threat, 18, is two 3 x 3s), and
 * taking a win (the win, less the threat it spends: seven ninths) is worth
 * more than making three new threats (six ninths) - the bot takes the win.
 * Only LIVE grids count - open, with nothing above them decided - so
 * deciding a grid takes every threat inside it off the board, both sides'.
 *
 * The score of a position for a side is its material minus the opponent's
 * plus its threats minus the opponent's (utb_bot_eval). The search keeps it
 * incrementally: a play re-reads only the grids on its leaf's path, and when
 * it decides a grid, the live grids under the highest one decided. */
#define UTB_BOT_THREAT_NUM 2           /* a threat is W * 2 / 9 */

/* THE DEFAULTS the 243 page opens at, and the bounds its control offers.
 * Measured natively and in Chrome: docs/BIG_BOARD.md's arena section and
 * tests/uttt_big_bot_test.c, which prints the speed every run. */
#define UTB_BOT_PLIES        6
#define UTB_BOT_BUDGET       150000
/* THE DEEPEST SETTING. Any depth is safe to ask for: the budget, not the
 * plies, bounds a move's work, and an iteration the budget cannot finish is
 * thrown away whole, so a depth out of reach plays the deepest one that
 * finished. 32 is past what the largest budget finishes on the 243 board
 * in the opening and middle game, and keeps the recursion small: a level of
 * the search holds 64 candidates and an undo record, so 32 of them are a few
 * KB of the wasm module's 128 KB stack. */
#define UTB_BOT_PLIES_MAX    32
#define UTB_BOT_BUDGET_MAX   1000000
#define UTB_BOT_CAP_ROOT     48
#define UTB_BOT_CAP_MAX      64
/* THE CAP BELOW THE ROOT SCALES with the plies and the budget
 * (utb_bot_cap_for): the widest that lets an alpha-beta tree of that depth
 * finish within the budget, between these two. A fixed cap starves one end:
 * at 8 plies a cap of 12 never finishes an iteration, and at 2 plies with a
 * large budget it leaves the budget unspent. */
#define UTB_BOT_CAP_NODE_MIN 3
#define UTB_BOT_CAP_NODE_MAX 32
#define UTB_BOT_NODE_COST    13

typedef struct {
    uint64_t rng;          /* the bot's own stream, never 0                      */
    int      plies;        /* the deepest iteration, 1..UTB_BOT_PLIES_MAX        */
    int      budget;       /* work units a move may spend past its first ply     */
    int      cap_root;     /* candidates at the root, 1..UTB_BOT_CAP_MAX         */
    int      cap_node;     /* ...and below it                                     */
    int      threats;      /* score threats as well as wins (1, the default)     */
    int      extend;       /* answer a horizon move that decides a grid (1)       */
    /* what the last move saw, for the page */
    int      value;        /* the chosen move's value at the deepest finished ply */
    int      depth;        /* that ply                                            */
    int      work;         /* work units the move spent                           */
} UtbBot;

/* The weight of winning node `id` of `g` (the game's for the root), and of
 * one threat in it. */
UTB_BOT_HIDDEN int  utb_bot_weight(const UtbGame *g, int id);
UTB_BOT_HIDDEN int  utb_bot_threat_weight(const UtbGame *g, int id);

/* The cap below the root for `plies` and `budget`, and the setter that takes
 * a depth and a budget (clamped to 1..UTB_BOT_PLIES_MAX and
 * 0..UTB_BOT_BUDGET_MAX) and derives both caps from them: the root's is four
 * times the node's, at least UTB_BOT_CAP_ROOT. The page's control calls it
 * between moves; the next move searches with it. */
UTB_BOT_HIDDEN int  utb_bot_cap_for(int plies, int budget);
UTB_BOT_HIDDEN void utb_bot_set(UtbBot *b, int plies, int budget);

/* A bot with the defaults above (threats and the extension on), its stream seeded from `seed` (any value,
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
 * that wins nothing, -1 for an illegal one. utb_bot_score is the search's
 * one-ply value of it: the gain and the change in both sides' threats, from
 * the mover's side (INT32_MIN for an illegal move); before the game's end it
 * is exactly utb_bot_eval after the move minus before. */
UTB_BOT_HIDDEN int  utb_bot_gain(UtbGame *g, int mv);
UTB_BOT_HIDDEN int  utb_bot_score(UtbGame *g, int mv);

/* The search's value of `mv` for the side to move at `plies` (1 is the gain
 * alone; 2 is the gain minus the opponent's best reply's; and so on), with
 * the bot's caps and no budget. INT32_MIN for an illegal move. */
UTB_BOT_HIDDEN int  utb_bot_value(UtbBot *b, UtbGame *g, int mv, int plies);

/* Everything `side` has won, by the weights (the game's included); its live
 * threats, weighted, minus the opponent's (0 once the game is over); and the
 * whole score: material minus the opponent's, plus those threats. */
UTB_BOT_HIDDEN int  utb_bot_material(const UtbGame *g, int side);
UTB_BOT_HIDDEN int  utb_bot_threats(const UtbGame *g, int side);
UTB_BOT_HIDDEN int  utb_bot_eval(const UtbGame *g, int side);

#endif
