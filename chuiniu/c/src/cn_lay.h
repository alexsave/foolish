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
 *     top_m   = h > 400 ? 30 : 8
 *     board_h = h - top_m - 12 - (shelf ? shelf_h + 10 : 0)
 * A SHORT board (board_h < CN_LAY_SHORT_H = badge 72 + plate 56 + band 120 + 4
 * margins of 8 = 280, the study's isShort and DECISIONS I17) puts the other
 * seats in one row along its top, the plate beside the row; a tall one puts
 * them on the ring fitted to the camera (DECISIONS K14, round twenty-two).
 * The rule reads board_h, never a named drawer size, so whatever height the
 * host measures for the collapsed drawer lands on the right side of it.
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

typedef struct {
    uint8_t  seats;                 /* 2..6                                        */
    uint8_t  me;                    /* my seat                                     */
    uint8_t  turn;                  /* whose turn: mine puts the picker up         */
    uint8_t  out_mask;              /* bit s: seat s has no dice (its cup lies)    */
    uint8_t  dice[CN_LAY_SEATS];    /* each seat's dice                            */
    uint8_t  my_faces[CN_LAY_DICE]; /* my dice, 1..6                               */
    uint8_t  rolling;               /* the roll's screen: its shelf, not the picker */
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
    uint8_t  pad1[2];
    float    ring_cy, ring_rx, ring_ry;   /* the ring's centre y and radii (0 on a short board) */
    float    pad, pad_x, pad_below;       /* the canvas's room past the board        */
    float    peek_target;           /* my cup's full tip                            */
    CnPeek   peek;                  /* its tip at in->peek                          */
    CnCam    cam;
} CnLay;

/* Lay out the table. 0 for an input out of range (seats, me, a count over 5,
 * a face out of 1..6 among my dice, a drawer too small to hold a board). */
int cn_lay_make(const CnLayIn *in, CnLay *L);

/* The bodies on the board, as the study placed them: each other seat's dice
 * in a rough ring under its cup (hidden faces drawn 1 up until a reveal sets
 * them with cn_die_cells), then its cup (lying if out); my dice; my cup last,
 * tipped by the peek. Returns the count written, or -1 when cap is short
 * (CN_LAY_MAX_OBJS always suffices). */
#define CN_LAY_MAX_OBJS (CN_LAY_SEATS * (CN_LAY_DICE + 1))
int cn_lay_objects(const CnLayIn *in, const CnLay *L, CnObj *objs, int cap);

/* The throws at a roll. The cup roll (CN_THROW_CUP): mine, and every other
 * seat's that still has dice, each with its own seed (in->seed + 1000 per
 * seat round from me), shake length (1.5 + .8 hash s) and start (.1 + .5 hash
 * s), so no two shake alike or land together; mine starts at once with the
 * default shake. The table roll (CN_THROW_TABLE): mine only. A throw's walls
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

#endif
