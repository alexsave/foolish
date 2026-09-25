/* Nine bots, and a ladder to sort them.
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
 *   BIASED PLAYOUTS ARE THE WHOLE GAME - FOR THE FLAT SEARCH. `bias` - the
 *   same flat Monte Carlo with the heuristic steering three playout moves
 *   in four - beats plain rollouts decisively and is level with everything
 *   else stacked on top. A wide sample of PLAUSIBLE games is worth far more
 *   than a wide sample of random ones.
 *
 *   IN THE TREE IT IS A DIFFERENT TRADE, and the difference is per-second
 *   rather than per-rollout. Steering costs about 38% of the playout, and
 *   quill measured at both prices:
 *
 *       random vs biased, SAME rollouts, 2500 games   47.2%   -2.8 sigma
 *       random at 64 vs biased at 40, SAME time, 1200 53.8%   +2.6 sigma
 *
 *   So the bias earns its keep per rollout and loses it per second: the
 *   tree would rather have more plain playouts than fewer good ones,
 *   because it is already choosing WHERE to spend them and the flat search
 *   is not. Worth re-reading before anything is called "the whole game"
 *   again - which search is being talked about decides the answer.
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
    BOT_FOUNTAIN,     /* quill's tree, light playouts played to the end    */
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

/* AND `fountain` IS QUILL'S TREE, WITH THE LEAF QUILL GAVE UP.
 *
 * Same root (mate search, exact endgame), same pools, same expansion and
 * proofs, same kept tree per side, same answer. Two things differ:
 *
 *   THE PLAYOUT RUNS TO THE END, AND IT IS LIGHT. Uniform, except that a
 *   move that wins the game is always played, the last good reply to the
 *   previous move is replayed (LGRF-1: learned from each playout's winner,
 *   forgotten on a loss), and a random move that hands the opponent a
 *   game-winning block is re-drawn up to four times. quill's twelve-ply
 *   biased playout and `leaf_eval` won at forty rollouts because the tree
 *   was shallow; at four thousand the tree is deep and the leaf's bias is
 *   what is left, while an honest result is not biased at all.
 *
 *   IT IS CHEAP. The playout runs in registers with incremental threat
 *   masks (which blocks each side can take in one move), so both the win
 *   test and the gift test are one AND; selection is single precision with
 *   table reciprocals; a proof only walks up while it changes something.
 *   Together about 1.4x the iterations of the first version per second.
 *
 * WHY SPEED WAS THE LEVER: at a fixed recipe, doubling fountain's budget
 * was worth about +75 Elo against quill@4000 (60% -> 70% -> 79% at 4000,
 * 8000, 16000), while every change of knowledge - implicit minimax, a
 * tournament policy, block-win preference, small-block gift avoidance,
 * contempt, FPU from the parent, prior decay, C and the prior weight - read
 * level within noise or lost to its own cost. The full log is in
 * uttt/c/README.md.
 *
 * AND IT SPENDS ITS TIME ON NARROW MOVES. quill's allowance is budget x
 * legal moves, so a free choice of sixty squares gets twenty times what a
 * forced block of three gets. fountain counts at least eighteen moves,
 * which moves time onto the forced blocks where the game is usually
 * decided: at equal time over 400 games, 72% against 68% for the plain
 * product.
 *
 * AT EQUAL TIME, fountain@2500 thinks about as long a move as quill@4000
 * (the ladder prints both). The readings, each on a stream of its own:
 *
 *     plain product, fountain@5400, 400 games    66%   +112 Elo
 *     plain product, fountain@5800, 300 games    68%   +127
 *     plain product, fountain@5800, 400 games    68%   +135
 *     eighteen-move floor, @2600, 400 games      72%   +160
 *     eighteen-move floor, @2500, 400 games      69%   +137   49.1 ms each
 *
 * Pooled over the two floor streams, 800 games: about 70.5%, +150 Elo.
 * `budget` is playouts a legal move, like quill's, with the floor. */

/* THE HEURISTIC'S NUMBERS, so a tournament can play them against each other.
 *
 * `score_move` is the playout policy and the tree's prior, and its constants
 * were hand-picked and never measured. Making them settable lets one binary
 * play one set against another; the winner gets baked back in as literals so
 * the shipped code pays nothing for the flexibility. */
typedef struct {
    int win_block;      /* the move takes a block                          */
    int deny_block;     /* it takes a block they wanted                    */
    int decided_target; /* it sends them to a block already settled        */
    int closer;         /* per square they could close the target with     */
    int meta_gift;      /* ...and taking that target completes their line  */
    int bias_one_in;    /* a playout move is random one time in this many  */
    int leaf_cutoff;    /* the TREE's playouts stop after this many plies  */
    int cell_w[9];      /* where in a block, doubled: it picks the target  */
    int block_w[9];     /* which block                                     */
} UtttWeights;

/* THE TREE STOPS ITS PLAYOUTS EARLY, and it was the largest single win of
 * the three things tried after sniper.
 *
 * A playout used to be carried to a result. Now the tree's leaves stop after
 * `leaf_cutoff` plies and `leaf_eval` scores the position they reached,
 * still on the 0..200 scale, so a leaf reports a shade rather than a
 * verdict. Quill against the version that plays them out, same rollouts,
 * same everything else, 400 games:
 *
 *     55.6%   +2.3 sigma   and 47.6s against 86.4s
 *
 * Stronger AND 45% faster, which is not the usual shape of a result. The
 * reason it is not a trade is that a leaf in a TREE does not have to be
 * right, it has to RANK - the tree below it supplies the rest - and a
 * verdict from thirty plies of random play is a worse ranking than a count
 * of blocks from twelve. The flat bots get none of this and pass 0: with
 * one number a candidate and nothing underneath, a short playout is a guess
 * built on a guess. See `playout` for why the cut is an argument there and
 * not a weight read in place.
 *
 * This is the same shape as the bias result at the top of the file, and
 * they point the same way: the tree wants MORE and CHEAPER looks, the flat
 * search wants FEWER and BETTER ones.
 *
 * THE LADDER EITHER SIDE OF IT, 200 games a pairing at 400 rollouts. Read
 * the head to head above in preference to this: these are 200-game rows
 * against a moving opponent, worth about two points of standard error, and
 * only the nib column is outside it.
 *
 *     opponent   playouts run out   stopped at twelve
 *     bias             95.2%              93.8%
 *     nib              89.8%              93.8%
 *     sniper           92.5%              91.8%
 *
 * DROPPING SYMMETRIC DUPLICATES AT THE ROOT is proved move-equivalent - the
 * eight transforms are checked by replay over 20,000 games - and it spends a
 * fixed allowance on 15 real first moves instead of 81 with seven copies of
 * each. On against off, 4000 games apiece at 40 rollouts:
 *
 *     quill    50.6%   +0.7 sigma
 *     bias     50.6%   +0.8 sigma
 *
 * Neither is significant by itself and nobody should quote them as if they
 * were. Two independent bots landing on the same side of even is the whole
 * of the evidence, and it is weak on purpose: the reduction can only touch
 * the first ply or two, so there is very little there to find. It is kept
 * for being free and right rather than for the tenth of a point. */

/* THE ONE CONSTANT A TOURNAMENT MOVED, and what it cost to find out.
 *
 * `closer` was -45 and is -25. Two hill climbs on `bias`, sixty rounds each,
 * promoted the same parameter in the same direction and nothing else; a
 * third promoted nothing. Head to head against the old value:
 *
 *     bot      games   with -25    sigma
 *     bias      2000     53.9%      +3.5
 *     nib       1500     54.2%      +3.3
 *     sniper     600     55.2%      +2.5
 *     crn       1500     49.7%      -0.2     <- control: unbiased playouts,
 *     quill     2500     49.8%      -0.2        never calls score_move
 *
 * `crn` is why the rest is believable: its playouts are uniform, so it never
 * asks the heuristic anything, and it sat on fifty. The three that do ask
 * all moved together.
 *
 * AND QUILL DOES NOT CARE. Two more climbs, run on quill itself, promoted
 * NOTHING in a hundred mutations - one of them found this very change at
 * 56.8% and watched it fall to 49.7% on the confirmation match. Once UCT has
 * a few dozen visits on a node the counts swamp the prior, so a tree
 * consults the heuristic far less than a flat search that asks it for every
 * move of every rollout. The constants are worth tuning for the flat bots
 * and are not where the tree's strength is hiding.
 *
 * EVERY PROMOTION WAS CONFIRMED ON A SECOND, FRESH STREAM before being
 * taken. At three hundred games a coin flip clears a two-sigma bar one time
 * in forty, and sixty rounds would hand out a free promotion; asking twice
 * makes it one in sixteen hundred. Five candidates died at that gate,
 * including one that read 57.0% and then 44.5%. */
UtttWeights uttt_weights_default(void);
void        uttt_weights_set(const UtttWeights *w);

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

/* The bots' own depth-limited exact solver, exported so a test can hold it
 * against an exhaustive one. +1 win, 0 draw, -1 loss for the side to move,
 * and 2 when `depth` plies were not enough to settle it. */
int uttt_solve(const UtttGame *g, int depth);

/* How many empty squares the bots will still switch to exact play at. The
 * shipped value is the one in uttt_bots.c; this moves it so a test or a
 * head-to-head can hold two settings against each other. */
void uttt_solve_gate(int empties);

/* How many nodes exact play may spend on one move. */
void uttt_solve_budget(long nodes);

/* Whether the root drops moves that a symmetry makes duplicates of others. */
void uttt_root_symmetry(int on);

/* WHAT QUILL THINKS OF A POSITION: the expected score of the side to move,
 * 0..1 with a draw worth half, from a fresh tree of `playouts` playouts.
 * `proof` is +1 / 0 / -1 when the tree proved a win / draw / loss, 2 when
 * the number is an estimate. For the post-game analyser. */
double uttt_quill_value(const UtttGame *g, long playouts, uint64_t *rs,
                        int *proof);

/* Forget every cached proof and the kept tree, so what follows depends on
 * its arguments alone. The analyser calls it before each position. */
void uttt_bots_forget(void);

/* Pick a move. `budget` is rollouts per candidate for the searching bots and
 * is ignored by the others. `rs` is the caller's RNG state, advanced. */
uint8_t uttt_bot_move(UtttBot bot, const UtttGame *g, int budget, uint64_t *rs);

#endif
