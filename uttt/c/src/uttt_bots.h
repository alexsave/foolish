/* Four bots, and a ladder to sort them.
 *
 * Borrowed wholesale from foolish, which learned all of it the hard way:
 *
 *   ROLLOUTS, not a deep search. Ultimate Tic-Tac-Toe has no evaluation
 *   function anybody trusts, and foolish's experience is that a shallow search
 *   over a bad eval loses to a wide sample of honest games.
 *
 *   COMMON RANDOM NUMBERS. blackpowder's trick: give every candidate move THE
 *   SAME random stream instead of an independent one, so the difference
 *   between two moves is the moves and not the dice.
 *
 *   IT DOES ALMOST NOTHING HERE, and that is worth keeping rather than
 *   quietly deleting. Measured, `crn` and `roller` are level. blackpowder
 *   plays a HIDDEN-information game where each rollout also samples a belief
 *   about an unseen hand, so its variance between candidates is enormous and
 *   CRN cancels most of it. Ultimate Tic-Tac-Toe is perfect information -
 *   there is nothing to sample but the playout itself - so the variance is
 *   small and what the playout DOES matters far more than which dice it used.
 *   A technique that was the largest single win in one game is a rounding
 *   error in another, and the only way to know which is to run it.
 *
 *   BIASED PLAYOUTS ARE THE WHOLE GAME. `bias` - the same flat Monte Carlo
 *   with the heuristic steering three playout moves in four - beats plain
 *   rollouts decisively and is level with everything else stacked on top.
 *   A wide sample of PLAUSIBLE games is worth far more than a wide sample of
 *   random ones.
 *
 *   AN EXACT ENDGAME. Below a handful of empty cells, stop guessing and solve
 *   it. A rollout that could have been a proof is a rollout wasted - though
 *   see above: it does not show up in the results either.
 *
 *   TIE-BREAKS ARE A DECISION. cordite beat blackpowder on an inverted
 *   tie-break and nothing else, so ours is named and tested rather than
 *   whatever qsort happened to do.
 */
#ifndef UTTT_BOTS_H
#define UTTT_BOTS_H

#include "uttt.h"

typedef enum {
    BOT_RANDOM = 0,   /* uniform over legal moves - the floor              */
    BOT_BIRO,         /* heuristic only, no search                         */
    BOT_ROLLER,       /* flat Monte Carlo, independent streams             */
    BOT_CRN,          /* ...the same, with common random numbers ONLY      */
    BOT_BIAS,         /* ...the same, with biased playouts ONLY            */
    BOT_NIB,          /* CRN + biased playouts + exact endgame             */
    BOT_SNIPER,       /* ...and a rollout worth more the sooner it wins    */
                      /* MEASURED, AND IT DOES NOT WORK - see below.       */
    BOT_COUNT
} UtttBot;

extern const char *UTTT_BOT_NAME[BOT_COUNT];

/* BOT_CRN and BOT_BIAS exist to answer "which of nib's three ideas is doing
 * the work". Bundling them and reporting the bundle is how a codebase ends up
 * carrying two that do nothing. */

/* AND `sniper` IS A NEGATIVE RESULT, kept for the same reason.
 *
 * It is nib with one change: a rollout that wins is worth 200 + (81 - plies)
 * instead of a flat 2, so a faster win outranks a slower one while never
 * outranking a draw. The idea was a bot that beats you in as few moves as it
 * can. 120 games a pairing at 40 rollouts, mean ply count of the games each
 * one WON:
 *
 *     opponent    nib      sniper
 *     random      44.2     43.8
 *     biro        38.4     37.2
 *     roller      44.8     44.2
 *     crn         44.7     44.9
 *     bias        52.7     52.3
 *
 * HALF A PLY, for about two points of strength (73.3% against nib's 75.5%
 * across the ladder, and nib takes the head-to-head 55.8%). The bonus only
 * re-ranks candidates whose win COUNTS are equal, and at forty rollouts that
 * is rare - the ranking is dominated by whether a line wins at all, which is
 * correct and is why the bonus cannot reach it.
 *
 * Winning sooner is not a choice between two winning moves. It needs a
 * forced-win search that prefers the SHALLOWEST mate, which is a different
 * thing from a rollout weight - the exact endgame below is already that
 * search, and it only runs under eleven empty cells. Anybody tempted to try
 * the rollout weight again should read this table first. */

/* Pick a move. `budget` is rollouts per candidate for the searching bots and
 * is ignored by the others. `rs` is the caller's RNG state, advanced. */
uint8_t uttt_bot_move(UtttBot bot, const UtttGame *g, int budget, uint64_t *rs);

#endif
