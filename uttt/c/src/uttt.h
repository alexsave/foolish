/* Ultimate Tic-Tac-Toe - the rules, and nothing else.
 *
 * Nine blocks of nine cells. Your move inside a block decides which block the
 * opponent must play in next; if that block is already decided they may play
 * anywhere. Win three blocks in a line.
 *
 * A block is DECIDED when it is won or when it is full. A decided block is
 * closed forever - its remaining empty cells can never be played, which is the
 * rule that makes the maximum game length an interesting question.
 */
#ifndef UTTT_H
#define UTTT_H

#include <stdint.h>

#define UTTT_X      1
#define UTTT_O      2
#define UTTT_DRAW   3          /* a block that filled without a line */
#define UTTT_OPEN   0

#define UTTT_ANY    255        /* "you may play in any open block" */
#define UTTT_MAX_PLIES 81

/* THE POSITION IS BITS, AND ONLY BITS.
 *
 * It used to be bytes - `cell[81]` and `block[9]` - with the bitboards added
 * beside them for speed, and the comment there said they were "derived, not
 * authoritative". That is two representations of one truth, which is this
 * codebase's favourite bug: they cannot disagree today because one function
 * writes both, and that is exactly the kind of guarantee that stops being
 * true the first time somebody adds a second writer.
 *
 * So the bytes are gone and `uttt_cell` and `uttt_block` compute them. Nine
 * bits a block, one word per mark: cm[X-1][b] is which cells of block b are
 * X's, bm[X-1] is which blocks are X's, bdrawn is which filled without a
 * line, live is which are still open.
 *
 * 218 bytes to 128, which a search copies per node. */
typedef struct {
    uint16_t cm[2][9];         /* cells of each block, per mark            */
    uint16_t bm[2];            /* blocks won, per mark                     */
    uint16_t bdrawn;           /* blocks that filled without a line        */
    uint16_t live;             /* blocks still open                        */

    uint8_t forced;            /* block index, or UTTT_ANY                  */
    uint8_t turn;              /* UTTT_X or UTTT_O                          */
    uint8_t over;              /* 0, or UTTT_X / UTTT_O / UTTT_DRAW         */
    uint8_t n_plies;

    uint8_t move[UTTT_MAX_PLIES];   /* the history: block*9 + index         */
} UtttGame;

/* What is in a square: UTTT_OPEN, UTTT_X or UTTT_O. */
static inline uint8_t uttt_cell(const UtttGame *g, int i)
{
    int b = i / 9, c = i % 9;
    if ((g->cm[0][b] >> c) & 1u) return UTTT_X;
    if ((g->cm[1][b] >> c) & 1u) return UTTT_O;
    return UTTT_OPEN;
}

/* What became of a block: UTTT_OPEN, UTTT_X, UTTT_O or UTTT_DRAW. */
static inline uint8_t uttt_block(const UtttGame *g, int b)
{
    if ((g->bm[0] >> b) & 1u)   return UTTT_X;
    if ((g->bm[1] >> b) & 1u)   return UTTT_O;
    if ((g->bdrawn >> b) & 1u)  return UTTT_DRAW;
    return UTTT_OPEN;
}

/* 218 BYTES, AND EIGHTY-TWO OF THEM ARE A HISTORY NO SEARCH READS. That
 * looks like an obvious waste - a tree copies a game per node - and it was
 * worth 2.3%, measured, which is not enough to carry a struct whose `move`
 * array is silently garbage in the ten places a search would have used the
 * cheap copy.
 *
 * The padding experiment that suggested otherwise was measuring something
 * else: eighty spare bytes appended to the struct cost 7%, but skipping
 * eighty real bytes in ten copies saved 2.3%. The difference is that the
 * copies are not where the time goes - `score_move` and `uttt_play` are, and
 * they touch a handful of fields rather than the whole thing.
 *
 * `cell` and `block` are the other ninety bytes, and they are now derivable
 * from `cm` and `bm`. Removing them would be a real tidy-up - two
 * representations of one truth is this codebase's favourite bug - but on
 * this evidence it is a tidy-up, not a speed-up, and it would touch the
 * coder, the renderer, the iOS bridge and four tests to get there. */

void uttt_init(UtttGame *g);

/* THE LEGAL MOVES AS TWO MASKS, for callers that would rather walk bits
 * than a list. `uttt_legal` is written in terms of these, so there is one
 * definition of what is legal and not two that can drift.
 *
 * A playout picks one move and throws the rest away, so building an
 * eighty-one byte list for it is eighty bytes of waste per step - and a
 * playout is where a Monte Carlo bot spends its life. */
unsigned uttt_legal_blocks(const UtttGame *g);   /* 9 bits, 0 when none */
unsigned uttt_open_cells(const UtttGame *g, int b);

/* Every legal move, as block*9+index. Returns the count, 0 when the game is
 * over. THE ORDER IS PART OF THE FORMAT - the coder stores an index into this
 * list, so encoder and decoder must walk it identically. Ascending, always. */
int  uttt_legal(const UtttGame *g, uint8_t *out);

/* Apply a move. Returns 1 if it was legal and was played, 0 otherwise. */
int  uttt_play(UtttGame *g, uint8_t mv);

/* Take back the last ply. Returns 1 if there was one to take back.
 *
 * BY REPLAYING, not by unwinding. A block closes when it is won or full and
 * is closed FOREVER, and `over` and `forced` are derived from the whole
 * position rather than from the last move - so an undo that tried to reverse
 * each of those in turn would be a second, subtly different set of rules, and
 * the two would drift. Eighty-one plies replay in microseconds; correctness
 * is worth more than that. The history is already in `move[]`.
 *
 * It exists because a staged bubble is a DRAFT: a player who taps the wrong
 * square must be able to tap another one before they send. */
int  uttt_undo(UtttGame *g);

/* Three in a line for `mark` over nine slots; slots may hold UTTT_DRAW. */
int  uttt_line(const uint8_t *nine, uint8_t mark);

/* The same question asked of a nine-bit mask, which is how the hot paths ask
 * it. One lookup. */
int  uttt_mask_line(unsigned mask);

/* WHICH EMPTY SQUARES WOULD COMPLETE A LINE for a player holding `mask` -
 * itself a nine-bit mask, and never including a square they already hold.
 *
 * It is the same table read the other way round, and it turns "can they
 * close this block" from a walk over nine squares into one AND against the
 * squares that are still empty. */
unsigned uttt_mask_wins(unsigned mask);

/* The i-th of the eight lines, as a nine-bit mask. */
unsigned uttt_line_mask(int i);

#endif
