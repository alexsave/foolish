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

/* --------------------------------------------------------- the message */
/* What a bubble carries and who may do what with it - uttt_msg.h has the
 * layout and the rules. The host hands over an opaque string and gets one
 * back; it never sees a byte, a tag or a hash. The resident message IS the
 * resident game: every uti_* game accessor above reads its board.
 *
 * THIS DEVICE'S IDENTITY is whatever bytes the host says it is - Messages'
 * localParticipantIdentifier, 16 bytes. It is hashed with each message's
 * seed into a seat tag here and never leaves the kernel. */
#define UTI_SEAT_SPECTATOR 0
#define UTI_SEAT_X         1
#define UTI_SEAT_O         2
#define UTI_SEAT_WAITING   3
#define UTI_SEAT_OPEN      4

void uti_me(const uint8_t *id, int n);

/* A new invitation from me, now: the empty board, me in the O seat, the seed
 * the send time. The joiner will be X and moves first. */
int  uti_msg_open(int64_t unix_seconds);

/* Adopt the message in `text` (a whole URL string is fine) - roster and
 * game. 0 (UTM_EOK), or a negative UTM_E* and nothing changes. */
int  uti_msg_read(const char *text);

/* Whether `text` is a message this build reads, WITHOUT adopting it: 0, or
 * the negative UTM_E* that uti_msg_read would return. */
int  uti_msg_check(const char *text);

/* The resident message as the text for MSMessage.url. Length, or negative.
 * A buffer of UTI_MSG_TEXT_MAX always fits. */
#define UTI_MSG_TEXT_MAX 128
int  uti_msg_text(char *out, int cap);

/* UTM_SEAT_*: 0 spectator, 1 X, 2 O, 3 waiting (my invitation), 4 open (X is
 * mine to take). */
int  uti_msg_seat(void);
int  uti_msg_mark(void);        /* the mark I play, or 0 */
int  uti_msg_sealed(void);
int32_t uti_msg_seed(void);
int  uti_msg_can_move(void);

/* Play as me - on an open invitation, TAKE THE SEAT with this move. 1 if
 * played. Undo takes back my own last move only; undoing the joining move
 * gives the seat back. */
int  uti_msg_play(int mv);
int  uti_msg_undo(void);

/* The one door a screen may offer for the resident message (utm_door). */
#define UTI_DOOR_NONE      0
#define UTI_DOOR_AGAIN     1
int  uti_msg_door(void);

/* GETTING A STAGED BUBBLE INTO THE FIELD (uttt_msg.h has why). How long a
 * staged bubble waits before the send hint shows, how long an insert may go
 * unanswered, and what that silence means on try `attempt` (1-based) in the
 * compact drawer or not. */
#define UTI_INSERT_LISTEN  0
#define UTI_INSERT_RETRY   1
#define UTI_INSERT_DOOR    2
int  uti_send_hint_ms(void);
int  uti_insert_silence_ms(void);
int  uti_insert_silence(int attempt, int compact);

/* Which of two messages to show: <0 mine (the device's staged draft), >0 the
 * tapped one, 0 the same. An unreadable one always loses. */
int  uti_msg_prefer(const char *mine, const char *tapped);
int  uti_msg_same_game(const char *a, const char *b);

/* Seal the resident game with these two identities in the O and X seats.
 * A game one device could never reach by itself - the DEBUG harness's
 * door, and the preview's. 0 if the two are the same person. */
int  uti_msg_seat_ids(const uint8_t *o_id, int o_n, const uint8_t *x_id, int x_n);

/* A touch at (u, v) in the board's 0..1 square, to block*9+cell, or -1. */
int  uti_hit(float u, float v);

/* ----------------------------------------------------------- the words */
/* Every sentence on a screen or a bubble, for the resident message as this
 * device sees it (uttt_say.h has the keys: UTTT_SAY_*). The pointer is
 * valid until the next call - copy it out at once. Never NULL. */
#define UTI_SAY_BUBBLE_HEADLINE      0
#define UTI_SAY_BUBBLE_PLACE         1
#define UTI_SAY_CAPTION              2
#define UTI_SAY_HEADLINE_PRE         3
#define UTI_SAY_HEADLINE_POST        4
#define UTI_SAY_SUBLINE              5
#define UTI_SAY_WATCH_LABEL          6
#define UTI_SAY_WATCH_LINE           7
#define UTI_SAY_WAITING_HEADLINE     8
#define UTI_SAY_WAITING_SUBLINE      9
#define UTI_SAY_UNREADABLE_HEADLINE  10
#define UTI_SAY_UNREADABLE_SUBLINE   11
#define UTI_SAY_YOU_ARE_1            12
#define UTI_SAY_YOU_ARE_2            13
#define UTI_SAY_DOOR_AGAIN           14
#define UTI_SAY_HEADLINE_SPOKEN      15
#define UTI_SAY_YOU_ARE_SPOKEN       16
#define UTI_SAY_DOOR_RULES           17
#define UTI_SAY_SEND_HINT            18
#define UTI_SAY_DOOR_SEND            19

const char *uti_say(int key);

/* The mark drawn in the play-surface headline, or 0: "Waiting on <O>". */
int  uti_say_mark(void);
/* The same two, of the position one ply back: what a screen says until the
 * last move's ink has landed. */
const char *uti_say_before(int key);
int  uti_say_mark_before(void);

/* uti_say, with `who` standing for the SENDER of the bubble being written:
 * "$" and the local participant's UUID, which Messages shows as a name.
 * See uttt_say_by for the two captions that use it. */
const char *uti_say_by(int key, const char *who);

/* The mark the bubble's headline draws before its words, or 0. */
int  uti_say_bubble_mark(void);

/* VoiceOver's words for square `mv` of the resident game ("Top left board,
 * centre square, empty"), and the square's rectangle in the board's 0..1
 * space as x, y, w, h (0 for an `mv` off the board). */
const char *uti_say_cell(int mv);
int  uti_cell_rect(int mv, float r[4]);

/* ---------------------------------------------------------- the drawing */
/* Rebuild the display list for the resident game. Returns polygon count.
 * active: block 0..8, 9 for anywhere, -1 for none.
 * last:   block*9+cell of the move being drawn, -1 for none. */
int  uti_draw(int active, int last, float mark_t, float meta_t);

/* Non-zero if the LAST uti_draw() ran out of buffer. A display list that
 * fills up does not fail - it stops appending and the board comes back with
 * marks missing - so this is the only way anyone finds out. */
int  uti_draw_overflow(void);

/* ---- motion: what the board looks like t milliseconds into a move ----
 * The kernel's plan (src/uttt_anim.h) as plain values. The host runs one
 * display-link loop: it asks uti_motion_at for the frame at the clock and
 * draws it, and schedules nothing. docs/UI.html, "How it moves". */
#define UTI_CH_STILL    0   /* nothing moves                                */
#define UTI_CH_STAGE    1   /* A: I tapped a square                         */
#define UTI_CH_REPLAY   2   /* C: my own bubble, reopened - no pulse        */
#define UTI_CH_THEIRS   3   /* D: a bubble of theirs, opened                */
#define UTI_CH_ARRIVAL  4   /* E: their move landed while I was looking     */
#define UTI_CH_OPEN     5   /* a bubble opened: C or D, the kernel decides  */

typedef struct {
    int32_t ch, mv, mark, from, to;
    int32_t ink_ms, wash_at, wash_ms, pulse_at, end_ms;
} UtiMotion;

typedef struct {
    float    mark_t;        /* how far the last mark is drawn, 0..1          */
    float    wash[4];       /* x, y, w, h on the board's 0..1; w 0 = none    */
    uint32_t wash_rgba;
    float    pulse[4];      /* the ring's inner rect; w 0 = no ring          */
    float    pulse_spread;  /* how far it stands out, board units            */
    uint32_t pulse_rgba;
    int32_t  landed;        /* the ink is down: the drawer may move now      */
    int32_t  settled;       /* the wash has arrived: insert the bubble now   */
    int32_t  running;       /* 0: nothing changes again, stop the loop       */
} UtiFrame;

/* The plan for the resident game's last move arriving through `ch`. */
UtiMotion uti_motion(int ch);
void      uti_motion_at(const UtiMotion *m, int32_t now_ms, UtiFrame *f);

/* ---- the drawer: the height the sheet is laid out at ----
 * Not the height Messages last handed, which arrives in steps: a critically
 * damped spring on the host's response from the layout toward it, a
 * finger's small steps followed at once (src/uttt_anim.h UtttDrawer). The
 * host reports every height it is handed and lays out at uti_drawer_at on
 * each display frame while *moving is 1. */
typedef struct {
    float   target, from, vel;
    int32_t t0, moving, seen;
} UtiDrawer;
void  uti_drawer_report(UtiDrawer *d, float h, int32_t now_ms);
float uti_drawer_at(const UtiDrawer *d, int32_t now_ms, int32_t *moving);
/* What uti_drawer_at would say had `h` just been reported, without
 * reporting it: a layout pass sees a new height before the host's change
 * callback does, and must not draw that one frame at the raw height. */
float uti_drawer_peek(const UtiDrawer *d, float h, int32_t now_ms, int32_t *moving);
/* The layout at rest at `h` at once (the auto-collapse's slide), and the
 * slide's push `t_ms` into a travel (src/uttt_anim.h UTTT_COLLAPSE_*). */
void  uti_drawer_rest(UtiDrawer *d, float h);
float uti_collapse_push(float travel, int32_t t_ms);
int32_t uti_collapse_ms(void);
int32_t uti_collapse_steps(void);
float uti_collapse_flip(void);

/* ---- one layout for every screen (src/uttt_anim.h UtttSheet) ----
 * The board's square centred on the sheet at every height and every screen,
 * scaled continuously with the drawer and as large as the sheet allows; the
 * words, columns and doors fitted around it. The host sets its words inside
 * the box it is given (`words`, wrapped to its width). */
#define UTI_SHEET_PLAY   0
#define UTI_SHEET_WATCH  1
#define UTI_SHEET_WAIT   2
typedef struct {
    float   w, h;
    int32_t kind, words, hint;
} UtiSheetIn;
typedef struct {
    float t, board[3], hpad, vpad, col, bar, foot, door, icon, icon_lead,
          icon_top, words_alpha, door_alpha, words[4];
    int32_t words_side;
    float band[4], band_alpha;
} UtiSheet;
UtiSheet uti_sheet(UtiSheetIn in);

/* The board with the last move's mark LEFT OUT and no wash - what a host
 * caches while the motion draws the rest over it. */
int  uti_draw_under(void);
/* How far the main lines run past the board on each side, as a fraction of
 * the board (UI.html's 5%), so a layout can keep them on the sheet. */
float uti_board_reach(void);

/* The last move's heavy mark alone, drawn to `t`. */
int  uti_draw_last(float t);


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

/* The Again door, a hachured bar in the rulebook's pen. In points like the
 * rulebook, and the polygons come back in 0..1 of the bar - x over w and y
 * over h, so the caller fills them into a w-by-h rectangle, not a square. */
int  uti_draw_door(float w, float h);

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

/* uti_draw for the bubble's board: the main lines run short enough to stop on
 * the 195-point frame (UtttBubble.reach in uttt_draw.h). */
int   uti_draw_bubble(int active, int last);

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
