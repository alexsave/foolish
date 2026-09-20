/* A whole board, as polygons. The only thing a renderer has to know.
 *
 * Coordinates are 0..1 on both axes, so a caller scales to whatever square it
 * has - 296 points in a collapsed drawer, 181 in a bubble, 370 in the play
 * surface - and nothing here cares which.
 */
#ifndef UTTT_DRAW_H
#define UTTT_DRAW_H

#include "uttt.h"
#include "uttt_pen.h"

typedef struct {
    int32_t seed;        /* the sheet's, from the first message's timestamp */
    int     active;      /* block to wash, -1 for none, 9 for "anywhere"    */
    int     last;        /* block*9+cell of the move just made, -1 for none */
    float   mark_t;      /* 0..1, how far the last mark has been drawn      */
    float   meta_t;      /* 0..1, the win line                              */
} UtttDrawOpts;

UtttDrawOpts uttt_draw_opts(int32_t seed);

/* Build the board. Returns 0, or -1 if it ran out of room. */
int uttt_draw_board(UtttDL *d, const UtttGame *g, const UtttDrawOpts *o);

/* One mark on its own, for the "you are" indicator. */
int uttt_draw_mark(UtttDL *d, int mark, int32_t seed, float calm);

#endif
