/* The Swift-visible face of the kernel. Flat accessors, no structs crossing
 * the boundary, no byte layout on the far side.
 *
 * ONE RESIDENT GAME, static rather than malloc'd: an iMessage extension has no
 * good moment to free anything, and a fixed footprint is the only footprint
 * that can be reasoned about against its memory ceiling.
 *
 * The drawing half is the unusual part and it is deliberate. Swift does not
 * compute a single coordinate - it calls uti_draw() and then fills the
 * polygons the kernel hands back. See uttt_pen.h for why the geometry is
 * domain rather than rendering.
 */
#ifndef UTTT_API_H
#define UTTT_API_H

#include <stdint.h>

/* ------------------------------------------------------------ the game */
void uti_new(int32_t seed);
int  uti_play(int mv);                  /* 1 if legal and played        */
int  uti_legal(uint8_t *out);           /* out needs 81; returns count  */
int  uti_over(void);                    /* 0, or X / O / draw           */
int  uti_turn(void);
int  uti_forced(void);                  /* block, or 255 for anywhere   */
int  uti_n_plies(void);
int  uti_move_at(int i);
int  uti_block(int b);
int  uti_cell(int i);

/* the whole game, coded - and back */
int  uti_encode(uint8_t *out, int cap);
int  uti_decode(const uint8_t *buf, int n, int32_t seed);

/* nib, for solo play. budget is rollouts per candidate. */
int  uti_bot_move(int budget);

/* ---------------------------------------------------------- the drawing */
/* Rebuild the display list for the resident game. Returns polygon count.
 * active: block 0..8, 9 for anywhere, -1 for none.
 * last:   block*9+cell of the move being drawn, -1 for none. */
int  uti_draw(int active, int last, float mark_t, float meta_t);

/* One mark on its own, for the side indicator. */
int  uti_draw_mark(int mark, int32_t seed);

/* Parallel arrays describing the last uti_draw. Coordinates are 0..1. */
const float    *uti_points(void);       /* 2 floats a point             */
int             uti_point_count(void);
const int32_t  *uti_poly_first(void);
const int32_t  *uti_poly_n(void);
const uint32_t *uti_poly_rgba(void);

#endif
