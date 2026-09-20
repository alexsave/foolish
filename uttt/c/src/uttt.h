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

typedef struct {
    uint8_t cell[81];          /* block*9 + index, UTTT_OPEN / X / O        */
    uint8_t block[9];          /* UTTT_OPEN / X / O / UTTT_DRAW             */
    uint8_t forced;            /* block index, or UTTT_ANY                  */
    uint8_t turn;              /* UTTT_X or UTTT_O                          */
    uint8_t over;              /* 0, or UTTT_X / UTTT_O / UTTT_DRAW         */
    uint8_t n_plies;
    uint8_t move[UTTT_MAX_PLIES];   /* the history: block*9 + index         */
} UtttGame;

void uttt_init(UtttGame *g);

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

#endif
