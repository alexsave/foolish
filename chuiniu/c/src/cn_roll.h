/* Chui Niu - the throw, baked (DECISIONS K14).
 *
 * THE FIRST THREE-DIMENSIONAL THING IN A KERNEL HERE. A seat's roll is a
 * rigid-body simulation: five rounded cubes inside a cup that is held mouth
 * up, shaken, flipped about the grip and slammed on the table, or (the table
 * roll, for other dice games) poured out of a tipped cup onto the planks.
 * The kernel runs the whole thing ONCE, in advance, on a fixed 240 Hz clock,
 * and hands the host every 60 Hz frame of it: one pose (a position and a
 * unit quaternion) for the cup and one for each die. A host plays the
 * frames back on its own display clock, interpolating between them, and
 * never integrates anything itself. So the throw costs the host nothing but
 * a few kilobytes, and it is the same throw, to the bit, on every phone
 * that runs this file with the same compiler flags (-ffp-contract=off; the
 * trig is the kernel's own series, never a platform libm's).
 *
 * WHAT IT DECIDES AND WHAT IT DOES NOT. The bake reports which face of each
 * die came up (CnRollInfo.up[]: a local axis, not a pip count). A die's
 * faces carry no values here; which value sits on which face is the host's
 * drawing, so the host may paint the kernel's own dice (K2) onto the up
 * faces, or read the hand off the faces it drew. Both are one line.
 *
 * THE DICE STAY IN BY THE THROW, NOT BY A RULE: there is no barrier at the
 * mouth. The flip turns about a point a sixth of the way in from the mouth,
 * so the floor swings on a long arm and presses on the dice the whole way
 * over; the turn gathers speed to the end of its travel and stops dead, as a
 * wrist does, and from halfway round the cup is driven down faster than the
 * dice can fall. tests/cn_roll_test.c measures it over hundreds of seeds.
 *
 * Fixed size, no allocation, no libc beyond memset. Lengths are points, z
 * is up, the table is z = 0. */
#ifndef CN_ROLL_H
#define CN_ROLL_H

#include <stdint.h>

#define CN_ROLL_DICE        5
#define CN_ROLL_HZ          60               /* frames a second written          */
#define CN_ROLL_SIM_HZ      240              /* steps a second simulated          */
#define CN_ROLL_MAX_FRAMES  512              /* 8.5 s: the longest throw there is */
#define CN_ROLL_POSES       (1 + CN_ROLL_DICE)
#define CN_ROLL_POSE_FLOATS 7                /* x y z, qx qy qz qw                */
#define CN_ROLL_FRAME_FLOATS (CN_ROLL_POSES * CN_ROLL_POSE_FLOATS)

enum { CN_THROW_CUP = 1, CN_THROW_TABLE = 2 };

/* The phases a frame can be in. The cup roll: HOLD, SHAKE, FLIP, SETTLE,
 * IDLE. The table roll: FALL, SIT, SLIDE, COVER, IDLE. */
enum {
    CN_RP_HOLD = 1, CN_RP_SHAKE, CN_RP_FLIP, CN_RP_SETTLE,
    CN_RP_FALL, CN_RP_SIT, CN_RP_SLIDE, CN_RP_COVER,
    CN_RP_IDLE,
};

/* The throw's geometry. The cup stands at (cup_x, cup_y), mouth down, when
 * the throw is over; the mesh's origin is the mouth's centre. */
typedef struct {
    float   cup_x, cup_y;    /* where my cup stands: the mouth's centre         */
    float   cup_r;           /* the mouth's outer radius                        */
    float   cup_rc;          /* the crown's radius (the cup narrows toward it)  */
    float   cup_h;           /* the cup's height                                */
    float   cup_t;           /* the wall's thickness                            */
    float   die;             /* a die's side                                    */
    float   ring;            /* the ring of five's radius (the table roll's     *
                              * stations; the cup roll's last resort)           */
    float   band_x0, band_x1;/* the table roll's soft walls, x                  */
    float   band_y0, band_y1;/* and y                                           */
    uint8_t kind;            /* CN_THROW_*                                      */
    uint8_t dice;            /* 1..CN_ROLL_DICE                                 */
    uint8_t pad0[2];
} CnThrow;

/* What the bake found. */
typedef struct {
    uint16_t frames;         /* frames written                                  */
    uint16_t slam;           /* the frame the cup meets the table (cup roll), or
                              * the frame the dice start to slide (table roll)  */
    uint8_t  complete;       /* 1 when the throw reached IDLE within the cap    */
    uint8_t  forced;         /* dice placed by the last resort (never seen)     */
    uint8_t  up[CN_ROLL_DICE];   /* the face up per die: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z */
    uint8_t  pad0[3];
    float    bake_s;         /* simulated seconds                               */
} CnRollInfo;

/* Bake the throw. `frames` receives up to `cap` frames of CN_ROLL_FRAME_FLOATS
 * floats each: pose 0 the cup, poses 1.. the dice; `phase` (may be NULL)
 * one CN_RP_* byte a frame. Returns the frames written (0 for a bad throw:
 * a kind that is not one of the two, or dice out of 1..5). A cap below the
 * throw's length leaves info->complete 0. */
int cn_roll_bake(const CnThrow *t, uint64_t seed, float *frames, uint8_t *phase, int cap, CnRollInfo *info);

/* The cup's shape, as the study draws it: its height as a multiple of the mouth's
 * radius (made taller so a harder shake keeps the dice in), the crown .72 of the
 * radius, the wall .06. */
#ifndef CN_CUP_TALL
#define CN_CUP_TALL 2.1f
#endif

/* The cup roll's throw at a given table, filled with the study's numbers. */
void cn_throw_default(CnThrow *t, int kind, float cup_x, float cup_y, float cup_r, float die, float ring);

#endif
