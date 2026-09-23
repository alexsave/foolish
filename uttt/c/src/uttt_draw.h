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

/* One cell's mark, partially drawn - the animating stroke on its own. */
int uttt_draw_cell(UtttDL *d, int mark, int mv, int32_t seed, float t);

/* WHICH SQUARE A TOUCH LANDED ON: (u, v) in the board's 0..1 space, the one
 * uttt_draw_board draws in, to block*9+cell - or -1 off the board. Here
 * beside the drawing because it is the drawing's geometry read backwards; a
 * renderer that divided by three itself would be a second copy of where the
 * cells are. Legality is not asked: that is uttt_play's question. */
int uttt_hit(float u, float v);

/* One mark on its own, for the "you are" indicator. */
int uttt_draw_mark(UtttDL *d, int mark, int32_t seed, float calm);

/* The rulebook door - a hachured square with a book on it. Lives in
 * uttt_rule.c. Returns 0, or -1 if it ran out of room.
 *
 * It takes the size the button HAS, in points, because rough.js rounds a
 * hachure gap to a whole unit and the shape is therefore not scale-free; the
 * polygons still come back in 0..1 like everything else. */
int uttt_draw_rulebook(UtttDL *d, float w, float h);

/* ------------------------------------------------------------ the bubble */
/* MSMessageTemplateLayout bakes ONE image at insert - 300 by 195 points,
 * landscape, aspect 1.54 - and every device in the thread shows that same
 * image. The frame is not the board's shape and cannot be made into it: a
 * square tops out at 181 points after the padding and 119 are left over.
 *
 * Those 119 carry two lines, a headline and the block the opponent has been
 * sent to, and the split lives here with the strokes for the same reason the
 * strokes do - one number typed into a renderer is one number the other
 * surface gets wrong. See docs/UI.html, "Bubble 300x195", option 02.
 *
 * What the kernel does NOT own is where a baseline falls: that is the text
 * engine measuring a font the kernel has never seen. So the two lines come
 * back as ONE box, and the renderer stacks them inside it. */
typedef struct { float x, y, w, h; } UtttBox;

typedef struct {
    float    w, h;              /* 300 x 195, the frame Messages bakes      */
    UtttBox  board;             /* the square, as big as the frame allows   */
    UtttBox  text;              /* the two lines, stacked centred in here   */
    float    headline_pt;       /* both lines are bold                      */
    float    place_pt;
    float    lead;              /* points between the two lines             */
    uint32_t headline_rgba;
    uint32_t place_rgba;
} UtttBubble;

UtttBubble uttt_bubble(void);

/* The nine blocks, named, plus 9 for "anywhere". `spoken` picks the form a
 * sentence uses - the caption says "the bottom-middle board" where the place
 * line says "bottom middle". Never NULL. */
const char *uttt_place_name(int block, int spoken);

/* The app's own face - one hash, an X and an O - drawn with the app's own
 * pen and centred in a `w` by `h` frame in POINTS. Polygons come back in
 * 0..1 like everything else. This feeds a build-time tool, not the app: see
 * tools/icons.sh, which writes the PNGs the asset catalogues carry. */
int uttt_draw_icon(UtttDL *d, float w, float h);

/* THE RULES, in the kernel, for the same reason the nine block names are: it
 * is the one thing that knows what they are, and a second copy in a renderer
 * is a second rulebook. Six lines and a title; never NULL. */
int         uttt_rules_count(void);
const char *uttt_rules_line(int i);
const char *uttt_rules_title(void);

/* The sheet everything is drawn on: w*h pixels of RGBA, opaque. Here rather
 * than in a renderer because two phones have to be looking at the same piece
 * of paper - it had drifted into three copies before it moved. */
void uttt_paper(uint8_t *rgba, int w, int h);

#endif
