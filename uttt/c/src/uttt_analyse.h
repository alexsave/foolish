/* The post-game analyser: every ply of a finished game held against quill.
 *
 *     ./uttt/c/build/uttt_analyse <code or replay link> [playouts]
 *
 * NOT PART OF THE APP. It is built into the tool and its test only (see the
 * Makefile's `analyse`), never into SRC, so nothing the phones link grows.
 *
 * For each ply, every legal move is played and the position after it is
 * handed to quill (`uttt_quill_value`) for the OPPONENT; one minus that is
 * what the move is worth to the mover, with a draw counted as half a win.
 * Where a proof exists it replaces the estimate: the game ending, the
 * opponent's forced win (`uttt_mate_in`), the depth-limited exact solver,
 * or quill's own tree proving the position. A mover who had a proved forced
 * win and played a move that is not proved to keep it has THROWN it, and
 * that is a blunder whatever the estimate says.
 *
 * Every number is a function of the game and the playout count alone: each
 * position gets its own seeded stream and a forgotten solver table, so two
 * runs print the same bytes and a prefix of a game analyses the same way
 * the whole game does. */
#ifndef UTTT_ANALYSE_H
#define UTTT_ANALYSE_H

#include "uttt.h"
#include <stdio.h>

typedef enum {
    UA_BEST = 0,      /* cost <= 2 points                                   */
    UA_GOOD,          /* <= 5                                               */
    UA_INACCURACY,    /* <= 10                                              */
    UA_MISTAKE,       /* <= 20                                              */
    UA_BLUNDER,       /* > 20, or a proved forced win thrown away           */
    UA_ONLY,          /* the only legal move: not a decision, not counted   */
    UA_LABELS
} UtttLabel;

extern const char *UTTT_LABEL_NAME[UA_LABELS];

enum {
    UA_TAG_GIFT  = 1,   /* sends them to a decided board: a free move       */
    UA_TAG_TAKES = 2,   /* wins a small board                               */
    UA_TAG_HANDS = 4,   /* they can win a small board with their reply      */
    UA_TAG_THROWN = 8,  /* a proved forced win, gone                        */
    UA_TAG_PROOF = 16,  /* the cost is proved, not estimated                */
};

typedef struct {
    uint8_t mover;          /* UTTT_X or UTTT_O                              */
    uint8_t mv;             /* block*9 + cell                                */
    uint8_t best;           /* the move quill rated highest                  */
    uint8_t sent;           /* 0..8 the board they must play, 9 anywhere,
                               255 the game ended                            */
    uint8_t n_legal;
    uint8_t label;          /* UtttLabel                                     */
    uint8_t tags;
    uint8_t best_proved, played_proved;
    double  p_best;         /* the mover's expected score, best move         */
    double  p_played;       /* ...and the move played                        */
    double  cost;           /* p_best - p_played                             */
    double  x_after;        /* X's expected score after the ply              */
} UtttNote;

/* Analyse every ply of `g`. `out` needs g->n_plies entries. `progress`, if
 * not NULL, is called after each ply. Returns the number of plies. */
int uttt_analyse(const UtttGame *g, long playouts, UtttNote *out,
                 void (*progress)(int ply, int n_plies));

/* The report: per ply, per player, the turning points and X's curve. */
void uttt_analyse_print(FILE *f, const UtttGame *g, const UtttNote *notes,
                        long playouts);

#endif
