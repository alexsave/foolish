/* Chui Niu - the stage: the table's state and a clock in, pixels and the HUD's
 * frames out. The one module that joins the layout (cn_lay), the bodies
 * (cn_geom), the camera (cn_cam), the throw (cn_roll), the textures (cn_tex)
 * and the renderer (cn_scene); a host hands it the table and asks for frames,
 * and decides nothing about where a cup goes or which face a die shows.
 *
 * THE FLOW.
 *   cn_stage_init(st, arena, bytes, pack, len)   once; the arena is the renderer's
 *   cn_stage_begin(st, &in)                      a table, a reveal or a bubble: the
 *                                                layout, the meshes and every seat's
 *                                                throw, baked; returns the HUD
 *   cn_stage_frame(st, t_ms, peek, lift, &w, &h) the picture at t, one thread; or
 *   cn_stage_prepare(st, t_ms, peek, lift)       the serial part, then
 *   cn_stage_band(st, pass, band, nbands)        each of CN_STAGE_PASSES passes in
 *                                                order, a pass's bands on any threads
 *                                                at once (GCD concurrentPerform), then
 *   cn_stage_finish(st)                          the picture (RGBA, cn_stage_shot's size)
 *   cn_stage_purge(st)                           a memory warning: drop the arena;
 *   cn_stage_attach(st, arena, bytes)            ...and take one again: the next frame
 *                                                rebuilds the textures, the same bytes
 *
 * THE HAND (DECISIONS I19). The dice are the kernel's fair deal (K2); the bake
 * only decides which FACE of each die ends up, and the dealt value is painted
 * on that face (cn_die_cells about the bake's up face), the other faces a
 * standard die round it. The labelling is the body's own from the first frame
 * of the throw, so nothing changes when the dice come to rest.
 *
 * THE CLOCK (DECISIONS I21). t_ms is the host's display clock from the moment
 * it began the stage: the same clock it samples cn_api_beats_frame on when it
 * plays the adopt's plan. The roll starts at in->roll_at_ms (the round's SHAKE
 * beat's start, so every beat before it plays uncut) and every seat's throw
 * runs its own length from there; the HUD says when my dice are at rest
 * (rest_ms: no move is staged before it) and when everything is
 * (total_ms, never before the SHAKE beat's own end).
 *
 * MEMORY (DECISIONS I20). The handle (this struct: the bakes and the meshes,
 * about 700 KB) is the caller's; the arena is the renderer's: one shared
 * texture set (one cup side, inside and floor, one die atlas) plus a crown a
 * seat, about 12 MB with the half-size copies, and the frame's buffers. A
 * frame that does not fit at the scale asked is drawn at the next scale down
 * (2, 1.5, 1) and cn_stage_shot says which. Fixed size, no allocation, no
 * libc beyond memcpy / memset; the arithmetic is the kernel's (cn_m_*). */
#ifndef CN_STAGE_H
#define CN_STAGE_H

#include <stddef.h>
#include <stdint.h>
#include "cn_lay.h"
#include "cn_tex.h"

#define CN_STAGE_SEATS   CN_LAY_SEATS
#define CN_STAGE_DICE    CN_LAY_DICE
#define CN_STAGE_ALL_DICE (CN_STAGE_SEATS * CN_STAGE_DICE)

/* what is on the stage */
enum {
    CN_STAGE_TABLE = 1,     /* the bidding table: my turn (the picker's shelf) or theirs  */
    CN_STAGE_REVEAL = 2,    /* the called round: every cup lifts by `lift`, every die shown */
    CN_STAGE_BUBBLE = 3,    /* the transcript picture, CN_STAGE_BUBBLE_W by _H, cups in a row */
};

#define CN_STAGE_NO_ROLL      0xFFFFFFFFu   /* roll_at_ms: no roll, every die at rest from the start */
#define CN_STAGE_SCALE_ROLL   1.5f          /* a frame while a throw moves: at most this a point     */
#define CN_STAGE_SCALE_STILL  2.0f          /* a still frame: at most this (3x is 68 MB of frame)    */
#define CN_STAGE_SCALE_MIN    1.0f
#define CN_STAGE_BUBBLE_W     300
#define CN_STAGE_BUBBLE_H     195
#define CN_STAGE_SHADOW_RES   1024          /* the study's SHADOW_RES and SHADOW_DARK               */
#define CN_STAGE_SHADOW_DARK  .55f
#define CN_STAGE_PASSES       3             /* cn_scene's CN_SCENE_PASSES                           */
#define CN_STAGE_BANDS        16            /* the bands a pass is cut into (package A: 6.8 ms at 2x) */
#define CN_STAGE_CROWNS       CN_STAGE_SEATS
#define CN_STAGE_ARENA        50331648      /* the arena a host hands over: CN_SCENE_ARENA_IOS, 48 MB */

/* THE INPUT: the table as the kernel holds it (the bridge fills it from the
 * resident game; a test fills it by hand). */
typedef struct {
    uint8_t  kind;                      /* CN_STAGE_*                                         */
    uint8_t  seats;                     /* 2..6                                               */
    uint8_t  me;                        /* my seat                                            */
    uint8_t  turn;                      /* whose turn: mine puts the picker's shelf up (table) */
    uint8_t  out_mask;                  /* bit s: seat s is out, its cup lies dim              */
    uint8_t  known_mask;                /* bit s: faces[s*5 ..] are seat s's dice, painted on  */
    uint8_t  dice[CN_STAGE_SEATS];      /* every seat's count                                 */
    uint8_t  faces[CN_STAGE_ALL_DICE];  /* seat s at s*5: its dice, 1..6, as the view sorts them */
    uint8_t  pad0[2];
    float    w, h;                      /* the drawer, points (a bubble is always 300 by 195)  */
    float    scale;                     /* the device's pixels a point; clamped (shot.scale)   */
    uint32_t seed;                      /* the round's throw (cn_stage_round_seed)             */
    uint32_t roll_at_ms;                /* when the roll starts, or CN_STAGE_NO_ROLL           */
} CnStageIn;

/* THE HUD: everything a host lays over or under the picture, in points. FLAT
 * means the drawer's own points before the camera's turn: the planks, the
 * names, the board and the picture's canvas are drawn flat and the host turns
 * that whole layer by `ca` about (origin_x, origin_y) (or by `ca_screen`
 * about the drawer's (0, 0); `hom` is the same map as a homography). The
 * plate and the shelf are never turned. GLASS means after the turn, where a
 * finger lands. Generated to Swift by structgen (one-dimensional arrays). */
typedef struct {
    uint8_t  ok;                        /* 1 once begun                                       */
    uint8_t  kind;                      /* CN_STAGE_*                                         */
    uint8_t  seats, me;
    uint8_t  short_board;               /* the other seats in one row along the top           */
    uint8_t  has_plate, has_shelf;
    uint8_t  out_mask;
    uint8_t  name_how[CN_STAGE_SEATS];  /* CN_NAME_* (cn_lay.h)                               */
    uint8_t  rolls;                     /* 1: a roll plays (roll_at_ms .. total_ms); 0: still,
                                           and the three times are 0                         */
    uint8_t  pad0;
    float    w, h;                      /* the drawer (300 by 195 for a bubble)               */
    float    board[4];                  /* x y w h, flat                                      */
    float    canvas[4];                 /* where the still picture goes, x y w h, flat (a frame
                                           says its own: cn_stage_shot's canvas)             */
    float    plate[4];                  /* the bid plate, x y w h, never turned               */
    float    shelf[4];                  /* the picker's or the reveal's shelf, never turned    */
    float    my_band[4];                /* my band, flat                                      */
    float    cup_x[CN_STAGE_SEATS], cup_y[CN_STAGE_SEATS];     /* every cup's mouth, flat    */
    float    name_x[CN_STAGE_SEATS], name_y[CN_STAGE_SEATS];   /* every name's anchor, flat  */
    float    cup_r, my_r;               /* the far cups' mouth radius and mine                */
    float    pad, pad_x, pad_below;     /* the canvas's room past the board: pad is the most
                                           any frame takes above it (the study's)            */
    float    hit[4];                    /* my cup on the glass: centre x y, radii x y (the peek's tap) */
    float    die_x[CN_STAGE_ALL_DICE], die_y[CN_STAGE_ALL_DICE];   /* each shown die at rest, glass,
                                           seat s at s*5 in faces[] order (CnView's shown /
                                           my_dice order); 0 0 for none                      */
    float    origin_x, origin_y;        /* the turn's centre, flat                            */
    float    theta, cam_d, zoom;        /* rotateX(theta) with perspective cam_d, scale(zoom)  */
    float    ca[16];                    /* CATransform3D about the origin, m11 .. m44          */
    float    ca_screen[16];             /* the same about the drawer's (0, 0)                  */
    float    hom[9];                    /* the homography, row-major                          */
    float    peek_target;               /* my cup's full tip, radians                         */
    float    scale_still, scale_roll;   /* the scales asked for, clamped                      */
    uint32_t roll_at_ms;                /* when the roll starts                               */
    uint32_t rest_ms;                   /* my dice at rest: no move is staged before it        */
    uint32_t total_ms;                  /* every throw at rest and the SHAKE beat over         */
} CnStageHud;

/* What the last frame was. */
typedef struct {
    uint16_t w, h;                      /* the picture, pixels                                */
    float    scale;                     /* pixels a point it was drawn at                     */
    float    canvas[4];                 /* where it goes, x y w h, flat: its top is cut to the
                                           highest point any body reaches in it (I20)         */
    uint32_t t_ms;
    uint8_t  ok;                        /* 0: nothing was drawn (no arena, no room, not begun) */
    uint8_t  rolling;                   /* a throw moved at t: the next frame differs          */
    uint8_t  done;                      /* t >= total_ms                                      */
    uint8_t  pad0;
} CnStageShot;

/* ---- the handle ------------------------------------------------------------------ */
enum { CN_STAGE_MESH_CUP_MINE, CN_STAGE_MESH_CUP_THEIRS, CN_STAGE_MESH_DIE_MINE, CN_STAGE_MESH_DIE_THEIRS, CN_STAGE_MESHES };

typedef struct {
    CnLayThrow t;
    CnRollInfo info;
    uint16_t   n;                       /* frames                                             */
    uint16_t   idle_at;                 /* the first IDLE frame                               */
    uint32_t   delay_ms;
    float      frames[CN_ROLL_MAX_FRAMES * CN_ROLL_FRAME_FLOATS];
    uint8_t    phase[CN_ROLL_MAX_FRAMES];
} CnStageThrow;

typedef struct {
    uint32_t seed;
    int16_t  count;
    uint8_t  out, big;
    int      id;
} CnStageCrown;

typedef struct {
    /* the textures and the arena */
    CnTexPack     pack;
    uint8_t       pack_ok;
    uint8_t       uploaded;             /* the texture set matches `crown`                     */
    uint8_t       pad0[2];
    void         *arena;
    size_t        arena_bytes;
    uint32_t      scene_gen;            /* which upload into the renderer this is              */
    int           tex_side, tex_inner, tex_floor, tex_die;
    int           ncrown;
    CnStageCrown  crown[CN_STAGE_CROWNS];
    /* the begun table */
    uint8_t       begun;
    uint8_t       nobj, nthrow;
    uint8_t       pad1;
    CnStageIn     in;
    CnLayIn       lin;
    CnLay         lay;
    CnStageHud    hud;
    CnStageShot   shot;
    float         eye[3];               /* the renderer's eye, canvas points                  */
    int           W, H, pad, below;     /* the canvas in points (pad: this frame's)            */
    int           pad_max;              /* the study's pad, the most a frame takes            */
    float         pad_x;
    float         lift_angle[CN_STAGE_SEATS];  /* the reveal's full tip of each cup             */
    int8_t        obj_throw[CN_LAY_MAX_OBJS];  /* the throw an object rides, -1 none           */
    int8_t        obj_pose[CN_LAY_MAX_OBJS];   /* and its pose in it                           */
    uint8_t       obj_mesh[CN_LAY_MAX_OBJS];
    CnObj         base[CN_LAY_MAX_OBJS];       /* where everything stands before the clock     */
    CnObj         obj[CN_LAY_MAX_OBJS];        /* the last frame's bodies                      */
    CnStageThrow  thr[CN_STAGE_SEATS];
    CnMesh        mesh[CN_STAGE_MESHES];
} CnStage;

/* The handle, the renderer's arena and the texture pack (cn_tex.h; the bytes
 * must outlive the stage). 0, or a negative CN_TEX_E* for a pack that does
 * not open, or -100 for an arena too small to be of use. */
#define CN_STAGE_E_ARENA (-100)
int  cn_stage_init(CnStage *st, void *arena, size_t bytes, const uint8_t *pack, size_t pack_len);
/* A memory warning: the renderer forgets the arena, which the host may then
 * free. The begun table stays; the next frame draws nothing until an arena is
 * attached, and then rebuilds the textures. */
void cn_stage_purge(CnStage *st);
/* An arena again (after cn_stage_purge). 0, or CN_STAGE_E_ARENA. */
int  cn_stage_attach(CnStage *st, void *arena, size_t bytes);

/* Begin a table: the layout, the meshes, every throw baked. The HUD, or 0 for
 * an input out of range (the HUD's `ok` is then 0 and frames draw nothing). */
const CnStageHud *cn_stage_begin(CnStage *st, const CnStageIn *in);

/* The bodies at t (the renderer's input): my cup tipped by `peek` (0 shut ..
 * 1 my cup's full tip, already eased: cn_cam_peek_ease), in a reveal every
 * standing cup tipped by `lift` (the LIFT beat's progress). The count. */
const CnObj *cn_stage_objects(CnStage *st, uint32_t t_ms, float peek, float lift, int *n);

/* The frame at t on one thread: the picture's RGBA, its size in *w and *h;
 * 0 when nothing could be drawn. */
const uint8_t *cn_stage_frame(CnStage *st, uint32_t t_ms, float peek, float lift, int *w, int *h);
/* The same frame on several threads: prepare (the serial part; 1, or 0 when
 * nothing can be drawn), every pass's bands, then finish (the picture, or 0). */
int  cn_stage_prepare(CnStage *st, uint32_t t_ms, float peek, float lift);
void cn_stage_band(CnStage *st, int pass, int band, int nbands);
const uint8_t *cn_stage_finish(CnStage *st);
/* The last frame's size, scale and state. */
const CnStageShot *cn_stage_shot(const CnStage *st);

/* Is everything at rest at t (every throw, the SHAKE beat)? */
int      cn_stage_done(const CnStage *st, uint32_t t_ms);
uint32_t cn_stage_total_ms(const CnStage *st);

/* The round's throw seed: the game's seed and the round, so every phone throws
 * the same throw for a round, and a reveal finds the dice where they landed. */
uint32_t cn_stage_round_seed(const uint8_t seed[32], int round);

#endif
