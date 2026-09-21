/* Eight bots, and a ladder to sort them.
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
    BOT_QUILL,        /* sniper's root, then a UCT tree instead of flat MC */
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
 * proved forced win, shortest first, or nothing. 300 games a pairing, 40
 * rollouts, both bots measured the same way:
 *
 *     opponent     nib     sniper      nib wins in / sniper wins in
 *     random      99.7%    100.0%       44.2 / 43.1
 *     biro       100.0%    100.0%       38.6 / 37.9
 *     roller      75.5%     79.7%       45.6 / 45.0
 *     crn         76.3%     77.8%       46.2 / 45.2
 *     bias        53.2%     64.8%       52.2 / 51.7
 *     nib            -      58.5%          -  / 51.6
 *
 * And from the other side, nib scored 40.0% against the sniper over its own
 * 300 - so 58.5% and 60.0%, measured independently. The standard error at
 * this sample is 2.8 points, which puts both around three sigma. THIS ONE IS
 * SETTLED, unlike the sixty-game read it replaces.
 *
 * Stronger AND about a ply sooner everywhere, and the part worth
 * understanding is that those are the same fact. A search that ends the game
 * when it can see the end is not a stylistic choice, it is better play. The
 * rollouts never know they have a forced win; they only know that a lot of
 * games from here came out well.
 *
 * SNIPER WAS THE TOP OF THE LADDER. Anything new is measured against it. */

/* AND `quill` IS THE SAME PLAYOUTS SPENT BY A TREE.
 *
 * UCT over the same biased playouts, at the same allowance - `budget` a
 * legal move, so a game costs what a sniper game costs - with the sniper's
 * mate search and exact endgame in front of it, unchanged. The tree keeps a
 * prior on every child from `score_move`, marks terminal children as facts
 * and lets the facts propagate (a node whose replies all lose is a loss, a
 * node with one winning move is a win), and answers with the most visited
 * child that is not a proved loss.
 *
 * WHY IT WORKS, in one line: flat Monte Carlo spends forty playouts on
 * every candidate including the nine it should have dismissed after five,
 * and forgets everything below the first ply; the tree spends them where
 * the game is still undecided and remembers.
 *
 * THE LADDER, 300 games a pairing, 40 rollouts, the same allowance for both
 * sides. Seconds are the bot's own thinking over its 300 games, next to its
 * opponent's - the tree costs what the flat search costs:
 *
 *     opponent    sniper    quill    quill wins in / they win in   quill s / opp s
 *     random      100.0%   100.0%         42.5 / -                  92.5 /   0.0
 *     biro        100.0%    97.5%         40.9 / 47.0               83.2 /   0.0
 *     roller       79.7%    89.3%         44.3 / 51.0               85.4 /  12.4
 *     crn          77.8%    87.7%         45.0 / 50.8               77.9 /  11.2
 *     bias         64.8%    79.7%         50.4 / 52.1               72.7 /  57.1
 *     nib          58.5%    76.8%         50.0 / 54.2               73.9 /  56.8
 *     sniper          -     64.5%         48.7 / 49.5               72.1 /  72.3
 *
 * That same pairing read 73.3% in the tuning sweep below and 62.5% in the
 * round robin, on different seed streams; the honest number is somewhere
 * in the high sixties, and every reading of it is past three sigma.
 *
 * CHECKED FROM OUTSIDE THE TUNING, on a fifth stream none of the work above
 * used: 200 games, quill 70.5%, 49.1s against 49.7s. Standard error 3.2 at
 * that sample, so six sigma, and the time is matched. That is the TOP of the
 * range rather than the bottom. Five readings across four streams now -
 * 62.5, 64.5, 66.8, 70.5, 73.3 - which is a wide band for a settled number
 * and a very clear answer to whether there is an edge at all. Anyone
 * re-tuning this should quote a range, not the reading they liked.
 *
 * And from the other side, sniper scored 33.2% over its own 300 against
 * quill - so quill at 66.8% measured independently, both well past three
 * sigma. In the round robin (`uttt_arena 100 40`) quill takes 62.5% off
 * sniper and 86.3% overall to sniper's 73.3%, at 124.0s of thinking to
 * sniper's 125.0s.
 *
 * THE ALLOWANCE CURVE, 300 games each, quill's rollouts against sniper's,
 * measured BEFORE the tree was kept between moves (next paragraph):
 *
 *     quill / sniper     20/20    40/40    80/80    120/120   120/40    40/120
 *     quill scores       64.5%    64.5%    75.8%    82.3%     88.3%     59.5%
 *
 * The tree's edge GROWS with the allowance, which is what a search that
 * concentrates should do and a flat one cannot; and at a third of sniper's
 * playouts it is still ahead.
 *
 * AND THE TREE IS KEPT BETWEEN MOVES. The subtree under the move we played
 * and the reply we got is next move's starting point, re-rooted by copying
 * it into the other of two pools. The allowance is unchanged; the tree
 * just starts a hundred-odd playouts full instead of empty. Same 300 games,
 * same thinking time:
 *
 *     quill / sniper        40/40    80/80    120/120   200/200   400/400
 *     fresh tree each move  64.5%    75.8%    82.3%     82.0%     90.8%
 *     tree kept             73.0%    79.2%    81.8%     85.8%     90.2%
 *
 * Worth most exactly where the allowance is smallest, which is where a
 * hundred remembered playouts are the biggest share of the budget. It is a
 * cache and nothing more: the tree it keeps is the tree it would have
 * built, and `uttt_tree_proof` always starts fresh so the test is of the
 * search and not of what happened to be remembered.
 *
 * TUNING, 300 games against sniper each, 40 rollouts, identical time. The
 * standard error is 2.8 points, so read the direction and not the digit:
 *
 *     C (exploration)   0.3    0.4    0.5    0.7    1.0    1.4
 *     vs sniper        57.8%  66.0%  72.0%  65.2%  58.8%  51.5%
 *
 *     prior weight, at C = 0.5     0.5    1.0    1.5    3.0
 *     vs sniper                   72.0%  64.7%  73.3%  66.8%
 *
 *     and at C = 0.7: prior 0 -> 59.0%, prior 1.5 -> 66.7%;
 *     first-play urgency 1.0 instead of 0.55 at C = 0.7 -> 58.5%,
 *     0.4 at C = 0.5 -> 66.0%; C = 0.3 with prior 1.5 -> 62.3%;
 *     C = 0.6 with prior 1.5 -> 67.3%.
 *
 * Less exploration is worth a lot: at a few hundred playouts the tree is
 * mostly frontier, and a wide search of a frontier is the flat search it
 * was meant to replace. C = 0.5 with the prior at 1.5 is what ships.
 *
 * TWO THINGS THAT DID NOT WORK, both measured at the shipped setting:
 *
 *   RAVE. All-moves-as-first statistics on every child, blended in with
 *   the usual beta. Equivalence 100 / 300 / 1000 -> 71.0% / 73.7% / 70.0%
 *   against 73.3% without it. Level, at every strength. The prior already
 *   orders the frontier, and in this game a cell played later in a line
 *   says little about the same cell played now - it is a different block's
 *   turn by then.
 *
 *   MATE-AVOIDANCE. `uttt_mate_in` run for the OTHER side after each
 *   candidate, and the candidates that lose by force struck from the tree.
 *   50 nodes a candidate -> 64.0% (at C = 0.5, prior 0.5, against 72.0%);
 *   300 -> 68.2% at 1.25x the time; 1000 -> 72.5% at 1.9x the time. The
 *   deeper proof costs real time and buys nothing the tree's own terminal
 *   marks were not already buying: a forced loss two plies down is a
 *   terminal grandchild, and the tree finds those for free.
 *
 * AND A WEAKNESS WORTH KNOWING. Against biro, the bare heuristic, the tree
 * scores 96% where the flat searches score 100%: fifteen draws and a loss
 * in 300. The tree steers into positions it has proved drawn rather than
 * ones the playouts merely like, and against a greedy opponent the flat
 * search's optimism was the better bet. */

/* THE SHORTEST FORCED WIN for the side to move, in plies, or 0 if there is
 * none inside `nodes`. `out` receives the move when there is one.
 *
 * Exposed because it is the one thing in this file that is a FACT rather
 * than an opinion - it proves a line or says nothing - and because a test
 * can check it against the exact endgame solver, which is two independent
 * implementations of the same question. */
int uttt_mate_in(const UtttGame *g, long nodes, uint8_t *out);

/* What quill's tree can PROVE about the side to move after `playouts`
 * playouts: +1 win, 0 draw, -1 loss, 2 nothing. A proof, never a guess,
 * which is what lets a test hold it against the exhaustive solver. */
int uttt_tree_proof(const UtttGame *g, long playouts, uint64_t *rs);

/* Pick a move. `budget` is rollouts per candidate for the searching bots and
 * is ignored by the others. `rs` is the caller's RNG state, advanced. */
uint8_t uttt_bot_move(UtttBot bot, const UtttGame *g, int budget, uint64_t *rs);

#endif
