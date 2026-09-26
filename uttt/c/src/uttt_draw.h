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
    float   fall_t;      /* 0..1, the big mark of the block `last` won      */
    float   reach;       /* how far the four main lines run past the board,
                            as a fraction of the pen's own 13.5% overshoot;
                            UTTT_REACH by default, everywhere               */
} UtttDrawOpts;

/* UI.html's main lines run 5% of the board past it (hashIn(..., S * .05)),
 * in every frame it draws - bubble, collapsed and expanded alike. The pen's
 * own overshoot is 13.5%, which ran the drawer's lines ~55 points off both
 * sides of the sheet; the design stops them just past the board. */
#define UTTT_REACH (.05f / .135f)

UtttDrawOpts uttt_draw_opts(int32_t seed);

/* The ink a mark is drawn in, 0xRRGGBBAA: O red for UTTT_O, X blue for
 * anything else. The one owner of the two colours outside the pen. */
uint32_t uttt_mark_ink(int mark);

/* The page's ink, #1d1b16: type that is not a side's. */
#define UTTT_INK 0x1d1b16ffu

/* Build the board. Returns 0, or -1 if it ran out of room. */
int uttt_draw_board(UtttDL *d, const UtttGame *g, const UtttDrawOpts *o);

/* THE LAST MOVE'S HEAVY MARK ON ITS OWN, drawn to `t`. A board drawn with
 * `last` set and mark_t 0 is every stroke but this one, so a renderer can
 * cache that and draw only this over it while it moves. -1 on no moves. */
int uttt_draw_last(UtttDL *d, const UtttGame *g, int32_t seed, float t);

/* THE SETTLEMENT OF THE LAST MOVE ON ITS OWN: the big mark of the block it
 * won, drawn to `fall_t`, and the win line of the game it ended, to
 * `line_t`. A board drawn with `last` set and fall_t and meta_t 0 is every
 * stroke but these and the last mark. -1 on no moves. */
int uttt_draw_settle(UtttDL *d, const UtttGame *g, int32_t seed, float fall_t, float line_t);

/* THE PROMISE: the outline round `block` (0..8, 9 the sheet) in the
 * highlighter's rect and colour, drawn round to `t`. 0, or -1 when the
 * display list ran out; nothing for a block of -1. */
int uttt_draw_outline(UtttDL *d, int block, int32_t seed, float t);

/* One cell's mark, partially drawn - the animating stroke on its own. */
int uttt_draw_cell(UtttDL *d, int mark, int mv, int32_t seed, float t);

/* WHICH SQUARE A TOUCH LANDED ON: (u, v) in the board's 0..1 space, the one
 * uttt_draw_board draws in, to block*9+cell - or -1 off the board. Here
 * beside the drawing because it is the drawing's geometry read backwards; a
 * renderer that divided by three itself would be a second copy of where the
 * cells are. Legality is not asked: that is uttt_play's question. */
int uttt_hit(float u, float v);

/* The square `mv` covers in the board's unit square, as x, y, w, h - the
 * rectangle uttt_hit maps back to `mv`, from the same BL and CE, so the
 * host can place one accessibility element per square without a number of
 * its own. 0 for an `mv` off the board. */
int uttt_cell_rect(int mv, float r[4]);

/* One mark on its own: UTTT_MARK_SIDE of the unit square, drawn with
 * uttt_mark_seed(mark, seed). `board` 0 for the "you are" indicator's pen.
 * For the headline's mark ("Waiting on O") it is the board's side over this
 * mark's frame, both in points, and the mark is gone over twice with
 * strokes as many points wide as the board's last mark's (owner,
 * 2026-09-25). */
int uttt_draw_mark(UtttDL *d, int mark, int32_t seed, float board);

#define UTTT_MARK_SIDE .88f
/* How far off its own median radius an O's ink may stray, as a fraction. */
#define UTTT_O_RING    .15f

/* 1 when the O drawn with `seed` at side `s` (of the unit square) keeps every
 * sample of its ink within UTTT_O_RING of its own radius - no tail cutting a
 * chord across it. */
int uttt_o_in_ring(int32_t seed, float s);

/* The seed uttt_draw_mark actually draws `mark` with: `seed` for an X; for
 * an O the first of a fixed walk from `seed` that stays a ring. */
int32_t uttt_mark_seed(int mark, int32_t seed);

/* The rulebook door - a hachured square with a book on it. Lives in
 * uttt_rule.c. Returns 0, or -1 if it ran out of room.
 *
 * It takes the size the button HAS, in points, because rough.js rounds a
 * hachure gap to a whole unit and the shape is therefore not scale-free; the
 * polygons still come back in 0..1 like everything else. */
int uttt_draw_rulebook(UtttDL *d, float w, float h);

/* The Again door - a hachured bar in the rulebook's pen, IN POINTS (the
 * width buys more hachure at the same gap). uttt_rule.c. 0, or -1 when it
 * ran out of room; a bar up to about 430 by 60 points fits. */
int uttt_draw_door(UtttDL *d, float w, float h);

/* ------------------------------------------------------------ the bubble */
/* MSMessageTemplateLayout bakes ONE image at insert - 300 by 195 points,
 * landscape, aspect 1.54 - and every device in the thread shows that same
 * image. The frame is not the board's shape and cannot be made into it: a
 * square tops out at 181 points after the padding and 119 are left over.
 *
 * ONLY A FINISHED GAME'S BUBBLE CARRIES WORDS (owner, 2026-09-23): the
 * winner's mark and "wins" (or "A draw") over "N moves", in a column left of
 * the board. Every other bubble is the board alone, centred in the frame -
 * the caption under the image ("Sent to the top-left board", "New game?")
 * says the rest. The split lives here with the strokes for the same reason
 * the strokes do - one number typed into a renderer is one number the other
 * surface gets wrong. See docs/UI.html, "Bubble 300x195", option 02.
 *
 * MESSAGES STAMPS THE APP'S BADGE into the frame's top-left corner, about
 * UTTT_BUBBLE_BADGE_W by UTTT_BUBBLE_BADGE_H points, over whatever is there;
 * no board line may run under it.
 *
 * What the kernel does NOT own is where a baseline falls: that is the text
 * engine measuring a font the kernel has never seen. So the two lines come
 * back as ONE box, and the renderer stacks them inside it. */
typedef struct { float x, y, w, h; } UtttBox;

typedef struct {
    float    w, h;              /* 300 x 195, the frame Messages bakes      */
    int32_t  words;             /* 1: a finished game, the two lines drawn  */
    UtttBox  board;             /* the square, as big as the frame allows   */
    UtttBox  text;              /* the two lines, stacked centred in here;
                                   all 0 when there are no words            */
    float    headline_pt;       /* both lines are bold                      */
    float    place_pt;
    float    lead;              /* points between the two lines             */
    float    reach;             /* UtttDrawOpts.reach for the bubble's board */
    uint32_t headline_rgba;
    uint32_t place_rgba;
} UtttBubble;

#define UTTT_BUBBLE_BADGE_W 31.f
#define UTTT_BUBBLE_BADGE_H 24.f

/* The frame for the bubble of `g`: words only when it is over. */
UtttBubble uttt_bubble(const UtttGame *g);

/* THE SCALE THE BUBBLE IS BAKED AT: the sender's own display scale, clamped
 * to 2..3 (owner: crispness first; a 3x bake on a 3x phone is +1.6 MB at the
 * stage's peak, accepted). Under 2 - an unknown or 1x screen, NaN - bakes at
 * 2, because every other phone in the thread is shown the same bitmap and
 * none of them is below 2x; over 3 is pixels no phone shows. */
float uttt_bubble_scale(float display);

/* The nine blocks, named, plus 9 for "anywhere", in the kernel's language
 * (uttt_lang.h). Never NULL. */
const char *uttt_place_name(int block);

/* The app's own face - one hash, an X and an O - drawn with the app's own
 * pen and centred in a `w` by `h` frame in POINTS. Polygons come back in
 * 0..1 like everything else. This feeds a build-time tool, not the app: see
 * tools/icons.sh, which writes the PNGs the asset catalogues carry. */
int uttt_draw_icon(UtttDL *d, float w, float h);

/* ------------------------------------------------------------ the rules */
/* THE RULES SHEET, ILLUSTRATED (docs/RULES.html, owner-approved): eight
 * lines (uttt_say.h, uttt_rules_line), each beside a small drawing of what it
 * says, out of this pen - the same hashes, marks, big marks, win line, wash
 * and promise the board draws.
 *
 * Drawing `i` (0..7) in a unit square; seeded by its index, so every phone
 * draws the same eight. 0, or -1 for an `i` off the list or a list that ran
 * out of room. */
int uttt_draw_rule(UtttDL *d, int i);

/* THE YELLOW OUTLINE ROUND A PHRASE ("yellow outline" in rule 6), a pen box
 * drawn in POINTS round a `w` by `h` frame and handed back in 0..1 of it, as
 * the doors are. The phrase's box is the frame less UTTT_RULES_LOOK's
 * box_pad on every side. */
int uttt_draw_rule_box(UtttDL *d, float w, float h);

/* WHERE EVERYTHING ON THE RULES SHEET GOES, in points, docs/RULES.html's
 * numbers. The text engine measures the lines; every other number is here. */
typedef struct {
    float margin_x;      /* the sheet's side margin                           */
    float top;           /* the title's top under the grabber's area          */
    float bottom;        /* room under the last row                           */
    float title_pt;      /* the title, bold                                   */
    float title_gap;     /* title to the first row                            */
    float art;           /* each drawing's side                               */
    float art_gap;       /* drawing to its text                               */
    float row_gap;       /* between rows; every row is as tall as the tallest */
    float body_pt;       /* the rules' type                                   */
    float body_lead;     /* line height over type size                        */
    float box_pad;       /* the outline's frame past the phrase, each side    */
    float word_room;     /* extra space either side of a marked phrase        */
    float tint_pad_x, tint_pad_y;   /* the tint past the phrase               */
    uint32_t ink;        /* the text                                          */
    uint32_t tint;       /* the "yellow tinted area" behind its phrase        */
} UtttRulesLook;

UtttRulesLook uttt_rules_look(void);

/* The sheet everything is drawn on: w*h pixels of RGBA, opaque. Here rather
 * than in a renderer because two phones have to be looking at the same piece
 * of paper - it had drifted into three copies before it moved. */
void uttt_paper(uint8_t *rgba, int w, int h);

#endif
