/* Recursive Ultimate Tic-Tac-Toe: a 9-ary tree of depth D.
 *
 * THE SHIPPED GAME IS DEPTH 2 - nine blocks of nine cells (uttt.h) - and this
 * module generalises it to depth 2..5 WITHOUT touching it: the 9 x 9 kernel
 * stays the shipped code, byte for byte, and tests/uttt_big_test.c holds this
 * module's depth 2 to it move for move. Depth 5 is the 243 x 243 board of the
 * TestFlight-only big game (docs/BIG_BOARD.md): 9^5 = 59,049 leaves, whose
 * whole position travels as a picture in the bubble (shared/swift/BubbleDataKit)
 * - the cells here ARE that picture's symbols, 0 empty, 1 X, 2 O.
 *
 * THE RULES, stated once. A node is WON when three of its nine children in a
 * line (the rows, the columns, the two diagonals - uttt_mask_line's eight
 * lines) hold one mark; it is DRAWN when all nine children are decided and
 * none of the lines is; won or drawn is DECIDED, and decided is forever: no
 * leaf under a decided node is ever played again. The root decides the game.
 * A leaf's mark is its cell; a leaf is "decided" when it holds a mark.
 *
 * A MOVE is a leaf index 0..9^D-1 whose base-9 digits d1..dD run from the top
 * block (d1) to the cell inside its 3 x 3 (dD): at depth 2 that is the shipped
 * game's block*9 + cell exactly. The first move is anywhere. X moves first
 * (the joiner, as in uttt_msg.h).
 *
 * THE SEND RULE is a property of the game (`rule`, set by utb_init_rule; the
 * docs/BIG_BOARD_SEND_RULE.md debate). Let k be the level of the largest unit
 * the last move COMPLETED (decided): D when only the cell, D-1 its 3 x 3, and
 * so on up to 1, a child of the root.
 *   UTB_RULE_SHIFT (A, the default, the iMessage game's): the next move must
 *     land in the level-(D-1) block named by (d2..dD) - the first digit
 *     dropped. Every move, whatever it completed.
 *   UTB_RULE_CLIMB (B'): drop digit d(k-1) and keep the rest. An ordinary
 *     move (k = D) sends to d1..d(D-2) dD, the shipped rule inside the
 *     current level-(D-2) block; completing a 3 x 3 drops d(D-2) instead, so
 *     the completed block's position names the sibling one level up; k = 2
 *     is rule A's formula.
 *   UTB_RULE_CLIMB_FREE (B): an ordinary move as B'. After a completion
 *     (k < D) the target is the WHOLE level-(k-1) block d1..d(k-2) d(k): the
 *     completed unit's position names its parent's sibling, any open cell in
 *     it.
 *   For both climbs k = 1 has no level to climb to, and the cell's ordinary
 *   send stands (k = D): at depth 2 that is the shipped rule, so all three
 *   rules ARE the 9 x 9 game there; deeper, that target lies inside the
 *   decided level-1 node and relaxes to the root, anywhere.
 * Under every rule, if the target, or any node above it, is decided, it
 * RELAXES to the parent of the first decided node on its path from the
 * root; the root means anywhere.
 *
 * THE REGION IS DERIVED, never stored: utb_region walks the path of `last`
 * every time it is asked (a decided ancestor of the last move's cell was
 * decided BY that move, since play under a decided node is illegal, so k is
 * read off the board too), so there is one rule and no second copy of it to
 * drift (uttt.h on "two representations of one truth").
 *
 * NODES are numbered level-major: id 0 is the root; the level-L node with
 * base-9 prefix p (0 <= p < 9^L) has id (9^L - 1) / 8 + p; a leaf's ancestor
 * at level L has prefix mv / 9^(D-L), and a level-L node spans the 9^(D-L)
 * leaves from p * 9^(D-L). Leaves are not nodes: their status is their cell.
 *
 * NO HISTORY. 42,000 plies is a real game's length at depth 5 (random play),
 * so the position is the cells and the last move, which is all a bubble can
 * carry; undo is ONE level - a staged bubble is a draft and the only thing
 * ever taken back is this device's own newest move - and it recomputes the
 * statuses on the path, which is exact because a node's status is a pure
 * function of its children's and nothing above a legal move was decided.
 */
#ifndef UTTT_BIG_H
#define UTTT_BIG_H

#include "uttt.h"
#include <stdint.h>

/* TESTFLIGHT ONLY (docs/BIG_BOARD.md): every function declared here is HIDDEN,
 * so a framework that links the kernel never exports it, and a build whose
 * Swift never reaches the big game (every uti_big_* is UTI_UNEXPORTED)
 * dead-strips it. Exported, 43 utb_ functions stayed in the App Store build. */
#pragma GCC visibility push(hidden)

#define UTB_DEPTH_MIN   2
#define UTB_DEPTH_MAX   5
#define UTB_LEAVES_MAX  59049          /* 9^5                         */
#define UTB_NODES_MAX   7381           /* (9^5 - 1) / 8               */
#define UTB_NONE        (-1)           /* no last move: an empty board */
#define UTB_UNKNOWN     (-2)           /* prev: the history is not known */
#define UTB_ROOT        0              /* the region that means anywhere */

/* The send rules (above). SHIFT is 0, so a zeroed game, and every adopted
 * one, plays rule A. */
enum {
    UTB_RULE_SHIFT      = 0,           /* A: every digit shifts up            */
    UTB_RULE_CLIMB      = 1,           /* B': climb on completion, keep dD   */
    UTB_RULE_CLIMB_FREE = 2            /* B: climb on completion, free there  */
};

typedef struct {
    uint8_t  depth;                    /* 2..5                                   */
    uint8_t  turn;                     /* UTTT_X or UTTT_O                        */
    uint8_t  over;                     /* 0, or UTTT_X / UTTT_O / UTTT_DRAW       */
    uint8_t  rule;                     /* UTB_RULE_*: the send rule (the byte
                                          was padding, so the size is the same) */
    int32_t  last;                     /* the last move, or UTB_NONE              */
    int32_t  prev;                     /* the move before it, UTB_NONE when `last`
                                          was the first, UTB_UNKNOWN after adopt  */
    int32_t  n_plies;
    uint8_t  cell[UTB_LEAVES_MAX];     /* UTTT_OPEN / UTTT_X / UTTT_O, leaf-indexed */
    uint8_t  node[UTB_NODES_MAX];      /* UTTT_OPEN / UTTT_X / UTTT_O / UTTT_DRAW   */
} UtbGame;

/* 9^depth leaves and (9^depth - 1) / 8 internal nodes; 0 for a depth outside
 * UTB_DEPTH_MIN..UTB_DEPTH_MAX. */
int  utb_leaves(int depth);
int  utb_nodes(int depth);

/* The empty board at `depth`: X to move, anywhere, rule A. Returns 1, or 0
 * for a depth this module does not play (and then *g is untouched). */
int  utb_init(UtbGame *g, int depth);
/* The same under send rule `rule` (UTB_RULE_*); 0 for a rule that is not one,
 * *g untouched. The rule is the game's from then on: legal moves, play, undo
 * and the region all read it. */
int  utb_init_rule(UtbGame *g, int depth, int rule);

/* ---------------------------------------------------------------- the tree */

/* The level of node `id` (0 for the root), its base-9 prefix within that
 * level, the first leaf it spans and how many leaves it spans. -1 / 0 for an
 * id that is not a node of this game. */
int  utb_node_level(const UtbGame *g, int id);
int  utb_node_prefix(const UtbGame *g, int id);
int  utb_node_first(const UtbGame *g, int id);
int  utb_node_span(const UtbGame *g, int id);
/* The node id of leaf `mv`'s ancestor at `level` (1..depth-1), or the root for
 * level 0; -1 off the board. */
int  utb_ancestor(const UtbGame *g, int mv, int level);
/* The node id of the level-L node with prefix p, or -1. */
int  utb_node_id(const UtbGame *g, int level, int prefix);
/* A node's status: UTTT_OPEN, UTTT_X, UTTT_O or UTTT_DRAW; -1 off the tree. */
int  utb_node(const UtbGame *g, int id);
/* Is this node, or any node above it, decided: the whole subtree is closed. */
int  utb_closed(const UtbGame *g, int id);

/* ---------------------------------------------------------------- the rules */

/* WHERE THE NEXT MARK MUST GO: a node id (UTB_ROOT for anywhere), or -1 when
 * the game is over. The raw target is the game's send rule applied to the
 * last move (above); the answer is that block, or the parent of the first
 * decided node on the path down to it. */
int  utb_region(const UtbGame *g);

/* Is `mv` a legal move now: the game runs, the leaf is empty, nothing above
 * it is decided, and it lies in the region. */
int  utb_legal_at(const UtbGame *g, int mv);

/* Every legal move, ascending, into out[0..cap); returns the count (which may
 * exceed cap, in which case only the first cap were written). Walks the tree
 * and skips a decided subtree in one step, so late in a game it is cheap. */
int  utb_legal(const UtbGame *g, int32_t *out, int cap);

/* Apply a move. 1 if it was legal and was played, 0 and nothing changed
 * otherwise. Updates the statuses up the leaf's path, `over`, `turn`,
 * `prev`/`last` and `n_plies`. */
int  utb_play(UtbGame *g, int mv);

/* Take back the last move: its cell is cleared, the statuses on its path are
 * recomputed from their children, `last` becomes `prev` and `prev` becomes
 * UTB_UNKNOWN. 1, or 0 when there is no last move or `prev` is unknown
 * (the second undo in a row after an adopt). */
int  utb_undo(UtbGame *g);

/* THE BOARD AS A PICTURE GAVE IT: adopt 9^depth cells (each 0, 1 or 2) and the
 * last move, rebuilding every node status bottom-up. Refuses (0, *g untouched)
 * a cell value over 2; counts that no sequence of turns gives (X must hold as
 * many marks as O, or one more); a node with both an X line and an O line;
 * `last` not UTB_NONE on an empty board, or UTB_NONE on a non-empty one, or
 * off the board, or a cell that does not hold the mark that just moved. Turn
 * is derived from the counts, `over` from the root, `prev` is UTB_UNKNOWN,
 * and the rule is UTB_RULE_SHIFT (the bubble's game; a caller playing another
 * rule sets `rule` after). Returns 1 on success. */
int  utb_adopt(UtbGame *g, int depth, const uint8_t *cells, int last);

/* The marks on the board: n[0] empty, n[1] X, n[2] O. */
void utb_count(const UtbGame *g, int n[3]);

/* ------------------------------------------------------------- geometry */

/* The rectangle of node `id` (or of leaf `mv`) in the board's 0..1 square,
 * as x, y, w, h; row-major like the shipped game (digit 0 is the top left,
 * 8 the bottom right at every level). 1, or 0 and zeros for an id/mv off the
 * board. The root is the whole square. */
int  utb_node_rect(const UtbGame *g, int id, float r[4]);
int  utb_cell_rect(const UtbGame *g, int mv, float r[4]);

/* A point in the board's 0..1 square, to the leaf under it; -1 outside. The
 * exact inverse of utb_cell_rect: every point inside a cell's rect maps to it. */
int  utb_hit(const UtbGame *g, float u, float v);

#pragma GCC visibility pop

#endif
