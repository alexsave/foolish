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
    BOT_SNIPER,       /* ...and a proved forced win, shortest, first       */
    BOT_COUNT
} UtttBot;

extern const char *UTTT_BOT_NAME[BOT_COUNT];

/* BOT_CRN and BOT_BIAS exist to answer "which of nib's three ideas is doing
 * the work". Bundling them and reporting the bundle is how a codebase ends up
 * carrying two that do nothing. */

/* AND `sniper` IS A PROOF, NOT A PREFERENCE - after one afternoon spent
 * learning it could not be a preference.
 *
 * THE VERSION THAT DID NOT WORK. nib with a winning rollout worth
 * 200 + (81 - plies) instead of a flat 2, so a faster win outranks a slower
 * one and never outranks a draw. Mean ply count of the games each bot won,
 * 120 a pairing:
 *
 *     opponent    nib      rollout-weight sniper
 *     random      44.2     43.8
 *     biro        38.4     37.2
 *     roller      44.8     44.2
 *     crn         44.7     44.9
 *     bias        52.7     52.3
 *
 * Half a ply, for two points of strength. The weight can only re-rank
 * candidates whose win COUNTS are equal, and at forty rollouts that is rare
 * - the ranking is dominated by whether a line wins at all, which is correct
 * and is exactly what keeps the weight from reaching anything. Winning
 * sooner is not a choice between two winning moves.
 *
 * THE VERSION THAT DOES. nib, plus `uttt_mate_in` before the rollouts: a
 * proved forced win, shortest first, or nothing. 60 games a pairing:
 *
 *     opponent    nib      sniper     sniper wins in / nib wins in
 *     roller      76.2%    81.7%      44.5 / 44.8
 *     crn         70.4%    75.8%      44.9 / 44.7
 *     bias        50.4%    68.3%      49.7 / 52.7
 *     nib           -      60.0%      50.7
 *
 * Stronger AND sooner, which is the part worth understanding: a search that
 * ends the game when it can see the end is not a stylistic choice, it is
 * simply better play. The rollouts never know they have a forced win; they
 * only know that a lot of games from here came out well.
 *
 * (Sixty games is about 1.6 sigma on that 60% - suggestive, not settled.) */

/* THE SHORTEST FORCED WIN for the side to move, in plies, or 0 if there is
 * none inside `nodes`. `out` receives the move when there is one.
 *
 * Exposed because it is the one thing in this file that is a FACT rather
 * than an opinion - it proves a line or says nothing - and because a test
 * can check it against the exact endgame solver, which is two independent
 * implementations of the same question. */
int uttt_mate_in(const UtttGame *g, long nodes, uint8_t *out);

/* Pick a move. `budget` is rollouts per candidate for the searching bots and
 * is ignored by the others. `rs` is the caller's RNG state, advanced. */
uint8_t uttt_bot_move(UtttBot bot, const UtttGame *g, int budget, uint64_t *rs);

#endif
