/* Chui Niu - the table screen's layout: from the game's state and the drawer's
 * size to where everything goes, in points.
 *
 * WHAT MOVED HERE. chuiniu/docs/UI.html's screen(), ringSeats, rowSeats,
 * plateFrame, isShort and startRoll's throwOf, the study's L constants
 * (DiceTableLayout's, ported): everything between the table's state and the
 * pixels that would be the same on a watch with a different screen. The host
 * reads a CnLay and paints; it never decides a seat's place.
 *
 * THE SCREEN. The drawer is w by h points. The board (the planks' part the
 * bodies stand on) is a rectangle inset 16 from each side, top_m from the top,
 * ending 12 above the shelf when the shelf is up (the picker on my turn, or
 * the roll's Roll again and Send), at the drawer's foot less 12 otherwise:
 *     top_m   = h <= 400 ? 8 : min(30, h - 392)     (the study's stepped at 400)
 *     board_h = h - top_m - 12 - (shelf ? shelf_h + 10 : 0)
 * A SHORT board puts the other seats in one row along its top, the plate
 * beside the row; a tall one puts them on the ring fitted to the camera
 * (DECISIONS K14, round twenty-two). The board is short when the board this
 * drawer has WITH THE PICKER UP is under CN_LAY_SHORT_H = badge 72 + plate 56
 * + band 120 + 4 margins of 8 = 280 (the study's isShort; DECISIONS I17, "the
 * compact drawer's table is a short board"): the rule reads the drawer, so the
 * table stays a row on their turn too rather than turning into a ring each
 * time the picker goes down (the study read the board left after the shelf,
 * and a 340 drawer flipped between the two). Never a named drawer size, so
 * whatever height the host measures lands on the right side of it; the top
 * margin grows a point a point, so the threshold is one (a drawer under 400 is
 * short, and every one from 400 up tall) and the board never shrinks as the
 * drawer grows (package V2).
 *
 * NOTHING LEAVES THE DRAWER (package U). Everything the eye sees stands up
 * off the table and leans up the screen from the eye's foot, and a held cup
 * is held high: on a short board the study's row put its crowns past the
 * drawer's top below a 323 drawer, and every seat's held cup far past it. So:
 *   the row's cups are as big as the row allows (the study's) and then made
 *   smaller, a point at a time, until every crown, standing or lying, is
 *   CN_LAY_EDGE inside the drawer;
 *   on a short board my dice are the study's 16 and then half a point
 *   smaller at a time until my cup, tipped as far as a peek tips it, stays
 *   CN_LAY_EDGE inside (its crown swings up the screen; under a 310 drawer
 *   the study's cup went past the top);
 *   on a short board the other seats' cups do not throw (a far cup held as a
 *   throw holds it rises four of its radii up the screen, and the row has
 *   one); their dice are hidden under them anyway;
 *   my throw is held at the largest reach (of the study's, never under
 *   CN_LAY_REACH_MIN) at which my held cup stays CN_LAY_EDGE inside the
 *   drawer on every screen of it (the least of each screen's fit), so the
 *   reach is one on my turn, on theirs and at the reveal.
 * AND ON A TALL BOARD AND AT THE REVEAL (package V2):
 *   my dice fit my peek on a tall board too (24, then half a point smaller:
 *   from 400 to about 450 the study's cup tipped past the top), and on a short
 *   board my cup standing stays off the row's names (CN_LAY_NAME_TEXT_*);
 *   the ring is fitted to the painted glass and kept off the plate (the
 *   study's fit uses its own map, up to 36 points off far up a tall drawer);
 *   a far seat throws (throw_mask) only when its held cup, at the study's
 *   reach, stays CN_LAY_EDGE inside the whole throw; else its cup stays down;
 *   at the reveal (in->reveal) every standing cup tips (DECISIONS I23): the
 *   ring is fitted with each far cup tipped the whole way up, and a short
 *   board lays every seat in one row (one_row, DECISIONS I29) since its row
 *   has no room above it, and a tall board lays them as the study's list, in
 *   rows (list_rows, package S), since on the ring a tipped cup covers the
 *   dice of the seat behind it; cn_lay_lift_fit is the stage's last word.
 * The fit reads the throw's own path (cn_roll_cup_pose), never a copy of it.
 *
 *
 * COORDINATES. Board points (origin the board's top-left, x right, y down,
 * z up off the table) for every body, name and the band; screen points for
 * the HUD (the plate, the shelf) and the board rectangle itself. The scene's
 * canvas reaches past the board: pad above it (a far cup leans up past the
 * board's top), pad_x either side, pad_below under it. */
#ifndef CN_LAY_H
#define CN_LAY_H

#include <stdint.h>
#include "cn_cam.h"
#include "cn_geom.h"
#include "cn_roll.h"

#define CN_LAY_SEATS      6
#define CN_LAY_DICE       5
#define CN_LAY_BADGE      72.0
#define CN_LAY_MY_BAND    120.0
#define CN_LAY_SHORT_BAND 64.0
#define CN_LAY_PLATE_W    160.0
#define CN_LAY_PLATE_H    56.0
#define CN_LAY_PLATE_MIN  100.0
#define CN_LAY_PLATE_SHORT_W 140.0 /* the short board's plate at the least: a bid at four seats on one line (package S) */
#define CN_LAY_ROW_STEP_MIN  60.0  /* the row's step is never narrowed under this to give the plate room */
#define CN_LAY_MARGIN     8.0
#define CN_LAY_SIDE       16.0    /* the board's inset from the drawer's sides */
#define CN_LAY_SHORT_H    (CN_LAY_BADGE + CN_LAY_PLATE_H + CN_LAY_MY_BAND + 4 * CN_LAY_MARGIN)
#define CN_LAY_PICKER_H   90.0    /* the picker's shelf (two rows)              */
#define CN_LAY_ROLL_H     50.0    /* the roll's shelf (Roll again, Send)        */
#define CN_LAY_HUD_TOP    8.0     /* the plate's top on a tall board            */
#define CN_LAY_PAD_BELOW  70.0    /* the canvas's room under the board          */
#define CN_LAY_NAME_W     80.0    /* a far seat's name box, centred on its anchor */
#define CN_LAY_NAME_H     30.0
#define CN_LAY_NAME_UP    8.0     /* the box's top is this far above the anchor */
#define CN_LAY_NAME_TEXT_W 44.0   /* the letters of a short name in the box (four small capitals at 12 to 14), */
#define CN_LAY_NAME_TEXT_H 18.0   /* one line: what my standing cup must not cover on a short board           */
#define CN_LAY_EDGE       4.0     /* no body nearer the drawer's edge than this, on the glass */
#define CN_LAY_REACH_MIN  .8    /* the least reach a throw is held at: lower, the shaken cup's crown dips into the table */
#define CN_LAY_ROW_MIN_R  12.0    /* the row's cups are never made smaller than this */
#define CN_LAY_MY_D_MIN   10.0    /* nor my dice on a short board                   */
#define CN_LAY_PLATE_REVEAL_W 240.0 /* the plate at the reveal: the tally ("There were twelve") is longer than a bid */
#define CN_LAY_STAMP_FOOT 52.0    /* the loser's stamp under a name at the reveal: its foot this far under the anchor
                                     (the host sets it on the glass 4 under the name's block, 22 tall, turned 5 degrees,
                                     6 more at an end; 40 left it over the next row's crown and the outcome line, package S) */
#define CN_LAY_OUTCOME_H  48.0    /* the reveal's outcome line (two lines of the roman on a band) over the shelf */

typedef struct {
    uint8_t  seats;                 /* 2..6                                        */
    uint8_t  me;                    /* my seat                                     */
    uint8_t  turn;                  /* whose turn: mine puts the picker up         */
    uint8_t  out_mask;              /* bit s: seat s has no dice (its cup lies)    */
    uint8_t  dice[CN_LAY_SEATS];    /* each seat's dice                            */
    uint8_t  my_faces[CN_LAY_DICE]; /* my dice, 1..6                               */
    uint8_t  rolling;               /* the roll's screen: its shelf, not the picker */
    uint8_t  reveal;                /* the reveal's screen: the roll's shelf, every cup lifts (cn_stage) */
    float    w, h;                  /* the drawer, points                          */
    float    peek;                  /* the peek's tip, 0 shut .. 1 open, already eased */
    uint32_t seed;                  /* the throw's (cn_lay_throws)                 */
} CnLayIn;

/* How a name sits on its anchor. */
enum {
    CN_NAME_BOX = 1,   /* a far seat: an 80 by 30 box, centred on x, its top 8 above y */
    CN_NAME_FOOT,      /* mine, tall board: centred on x, its foot on y                */
    CN_NAME_LEFT,      /* mine, short board: its left edge on x, centred on y          */
};

typedef struct {
    float    w, h;                  /* the drawer                                   */
    float    board_x, board_y, board_w, board_h;   /* screen points                 */
    float    top_m;
    uint8_t  seats, me, short_board, has_plate, has_shelf, pad0[3];
    float    plate[4];              /* the bid plate: x y w h, screen points (flat)  */
    float    shelf[4];              /* the picker's or the roll's shelf, screen       */
    float    my_band[4];            /* my band, board points                          */
    float    d, ring;               /* my dice's side and the ring my five sit on     */
    float    my_r;                  /* my cup's mouth radius                          */
    float    cup_r;                 /* every other seat's (one size for all, K14)     */
    float    sd, sring;             /* their dice's side and ring                     */
    float    cup_x[CN_LAY_SEATS], cup_y[CN_LAY_SEATS];     /* every cup, mine too    */
    float    name_x[CN_LAY_SEATS], name_y[CN_LAY_SEATS];   /* every name's anchor    */
    uint8_t  name_how[CN_LAY_SEATS];                       /* CN_NAME_*              */
    uint8_t  throw_mask;            /* bit s: seat s's cup throws (cn_lay_throws)     */
    uint8_t  one_row;               /* the reveal on a short board: every seat in one row */
    uint8_t  list_rows;             /* the reveal on a tall board: the study's list in this many rows (0: none) */
    uint8_t  plate_throw;           /* my throw's held cup passes over the plate: the host stands the plate down
                                       until my dice rest (package S: two seats, compact, the plate hid half my cup) */
    float    ring_cy, ring_rx, ring_ry;   /* the ring's centre y and radii (0 on a short board) */
    float    pad, pad_x, pad_below;       /* the canvas's room past the board        */
    float    peek_target;           /* my cup's full tip                            */
    CnPeek   peek;                  /* its tip at in->peek                          */
    float    my_reach;              /* my throw's reach, of the study's (1 when it fits the drawer) */
    float    die_g[CN_LAY_SEATS];   /* each seat's die side on the glass, at its cup */
    float    outcome[4];            /* the reveal's outcome line, x y w h, screen (0 0 0 0 off the reveal) */
    CnCam    cam;
    double   seg_c[CN_CUP_SEGS], seg_s[CN_CUP_SEGS];   /* a cup's rim corners round the circle: the fits' table */
} CnLay;

/* Lay out the table. 0 for an input out of range (seats, me, a count over 5,
 * a face out of 1..6 among my dice, a drawer too small to hold a board). */
int cn_lay_make(const CnLayIn *in, CnLay *L);
/* The camera cn_lay_make makes for `in` (its L->cam), without the layout's fits:
 * 0 (and c zeroed) when cn_lay_make would refuse for the board alone. */
int cn_lay_cam(const CnLayIn *in, CnCam *c);

/* The bodies on the board, as the study placed them: each other seat's dice
 * in a rough ring under its cup (hidden faces drawn 1 up until a reveal sets
 * them with cn_die_cells), then its cup (lying if out); my dice; my cup last,
 * tipped by the peek. Returns the count written, or -1 when cap is short
 * (CN_LAY_MAX_OBJS always suffices). */
#define CN_LAY_MAX_OBJS (CN_LAY_SEATS * (CN_LAY_DICE + 1))
int cn_lay_objects(const CnLayIn *in, const CnLay *L, CnObj *objs, int cap);

/* The throws at a roll. The cup roll (CN_THROW_CUP): mine, held at
 * L->my_reach, and on a tall board every other seat's that still has dice and
 * whose held cup stays inside (L->throw_mask), each with its own seed (in->seed + 1000 per seat round from me), shake
 * length (1.5 + .8 hash s) and start (.1 + .5 hash s), so no two shake alike
 * or land together; mine starts at once with the default shake. On a short
 * board mine only, at the reveal on a short board none. The table roll
 * (CN_THROW_TABLE): mine only. A throw's walls
 * are a box round its cup, kept 20 off the board's sides and top and 10 off
 * its foot. Returns the count. A throw's frame at the host's clock T is
 * cn_geom_pose_at(..., T - delay, ...). */
typedef struct {
    CnThrow  t;
    uint64_t seed;
    float    delay;
    uint8_t  seat;
    uint8_t  pad0[3];
} CnLayThrow;
int cn_lay_throws(const CnLayIn *in, const CnLay *L, int kind, CnLayThrow *out, int cap);

/* THE REVEAL'S LIFT, FITTED. A standing cup (as it rests on this screen) tipped
 * about the far edge of its mouth by `full` (cn_cam_peek_tilt's tip, the least
 * that shows its dice) when it stays CN_LAY_EDGE inside the drawer and off the
 * plate; else the largest tip under it that does. The layout makes room for the
 * full tip (the ring and the reveal's row are fitted to the dice die_spot puts
 * down); this is the stage's guarantee for dice a throw put elsewhere. */
float cn_lay_lift_fit(const CnLay *L, const CnObj *cup, float full);

#endif
