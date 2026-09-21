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

/* Take back the last move. 1 if there was one. A STAGED BUBBLE IS A DRAFT -
 * a player who taps the wrong square taps another one instead - so this is
 * the product's own undo, not a debugging convenience. */
int  uti_undo(void);
int  uti_over(void);                    /* 0, or X / O / draw           */
int  uti_turn(void);
int  uti_forced(void);                  /* block, or 255 for anywhere   */
int  uti_n_plies(void);
int  uti_move_at(int i);
int  uti_block(int b);
int  uti_cell(int i);

/* Where the next mark must go: block 0..8, 9 for anywhere, -1 when the game
 * is over. The board's wash and the bubble's place line are the same question
 * and it is answered once, here - uti_forced() is the raw rule and says
 * nothing about a block that has already been decided. */
int  uti_active(void);

/* the whole game, coded - and back */
int  uti_encode(uint8_t *out, int cap);
int  uti_decode(const uint8_t *buf, int n, int32_t seed);

/* NO BOT CROSSES THIS BOUNDARY. `uttt/c/src/uttt_bots.c` still holds all six
 * of them and the arena still runs them - they are how we learned that a real
 * game codes to about twenty-two bytes, which needed thousands of plausible
 * games and no human. But this app is two people doing something to each
 * other in a thread, and an opponent that is always available and never loses
 * interest is the opposite of that. The bots are research; they do not ship.
 */

/* ---------------------------------------------------------- the drawing */
/* Rebuild the display list for the resident game. Returns polygon count.
 * active: block 0..8, 9 for anywhere, -1 for none.
 * last:   block*9+cell of the move being drawn, -1 for none. */
int  uti_draw(int active, int last, float mark_t, float meta_t);

/* Non-zero if the LAST uti_draw() ran out of buffer. A display list that
 * fills up does not fail - it stops appending and the board comes back with
 * marks missing - so this is the only way anyone finds out. */
int  uti_draw_overflow(void);

/* JUST the mark being drawn, so an animation does not rebuild the board.
 * 14,000 polygons is fine once and not fine sixty times a second: the caller
 * caches uti_draw()'s output as an image and composites this on top. */
int  uti_draw_one(int mv, float t);

/* One mark on its own, for the side indicator. */
int  uti_draw_mark(int mark, int32_t seed);

/* The rulebook door - the one button on the expanded sheet, a hachured square
 * with a book on it. Takes the size the button HAS, IN POINTS, because
 * rough.js rounds a hachure gap to a whole unit and the shape is therefore
 * not scale-free; the polygons still come back in 0..1 like everything else,
 * so the caller fills them exactly as it fills the board. Sized for a button:
 * past about 125 points the fill runs out of strokes and comes back short,
 * the same way the board does. */
int  uti_draw_rulebook(float w, float h);

/* The sheet itself. Fills w*h RGBA bytes with the napkin - crossed cellulose
 * over a warm near-white. Here rather than in the renderer for the same reason
 * the marks are: both phones have to be looking at the same piece of paper. */
void uti_paper(uint8_t *rgba, int w, int h);

/* ----------------------------------------------------------- the bubble */
/* The transcript image is 300x195 points, landscape, and baked at insert -
 * see uttt_draw.h for why the split is the kernel's and where it stops. Flat
 * accessors because no struct crosses this boundary.
 *
 * Coordinates are POINTS inside that frame, not 0..1: the frame is the one
 * place in the app whose size is fixed by somebody else. */
void  uti_bubble_size(float *w, float *h);
void  uti_bubble_board(float *x, float *y, float *side);
void  uti_bubble_text(float *x, float *y, float *w, float *h);

/* line: 0 the headline, 1 the place. */
float uti_bubble_type(int line);
uint32_t uti_bubble_ink(int line);      /* 0xRRGGBBAA */
float uti_bubble_lead(void);            /* points between the two lines */

/* A block's name. 0..8, or 9 for "anywhere". spoken: 0 for the place line
 * ("bottom middle"), 1 for a sentence ("the bottom-middle board"). */
const char *uti_place_name(int block, int spoken);

/* The rulebook's text. Six lines and a title - see uttt_draw.h. */
int         uti_rules_count(void);
const char *uti_rules_line(int i);
const char *uti_rules_title(void);

/* Parallel arrays describing the last uti_draw. Coordinates are 0..1. */
const float    *uti_points(void);       /* 2 floats a point             */
int             uti_point_count(void);
const int32_t  *uti_poly_first(void);
const int32_t  *uti_poly_n(void);
const uint32_t *uti_poly_rgba(void);

#endif
