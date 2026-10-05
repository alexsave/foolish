/* Chui Niu - the bodies on the table: the cup's and the die's meshes, where
 * each body stands, and a baked throw's pose at any instant.
 *
 * WHAT MOVED HERE. chuiniu/docs/UI.html built all of this in JavaScript
 * (R3.cup, R3.die, setDieUp, dieValue, cupObj, dieObj, the render loop's
 * world transform and fan, ROLL.poseAt). It is the same on a watch with a
 * different screen, so it is the kernel's (docs/ARCHITECTURE_AS_A_PATTERN.md,
 * "the kernel owns the SHAPE"). The host keeps the textures, the rasterizer
 * and the paint; it hands this file's arrays to the renderer as they are.
 *
 * UNITS. Points; z is up; the table is z = 0; x right, y down the screen, as
 * the board's own coordinates (cn_lay.h).
 *
 * THE RENDERER'S FORMAT, written here as numbers so this file does not include
 * the renderer (chuiniu/c/wasm/cn_scene.c, VF and FF):
 *   a vertex is CN_GEOM_VF = 6 floats   x y z nx ny nz          (world)
 *   a face   is CN_GEOM_FF = 16 floats  i0 i1 i2  u0 v0 u1 v1 u2 v2
 *                                       tex  r g b  km ka  flags
 * One vertex a polygon corner (normals are per corner), one triangle a fan
 * step, as the study wrote them. flags are CN_GEOM_F_*, the renderer's F_*
 * bits; every body face carries CN_GEOM_F_BODY. tex is the host's texture id,
 * looked up from the face's CN_TEX_* slot through the table the host passes.
 *
 * Fixed size, no allocation, no libc beyond memset; the trigonometry is the
 * kernel's own series (cn_m_* below), never a platform libm's. */
#ifndef CN_GEOM_H
#define CN_GEOM_H

#include <stdint.h>
#include "cn_roll.h"

/* ---- the kernel's own maths (doubles), shared by cn_cam.c and cn_lay.c --------- */
#define CN_PI 3.14159265358979323846
double cn_m_sin(double x);
double cn_m_cos(double x);
double cn_m_sqrt(double x);
double cn_m_atan2(double y, double x);
double cn_m_floor(double x);
double cn_m_ceil(double x);
/* The study's integer hash (TEX.hash): three 32-bit lattice coordinates to
 * [0, 1). Every jitter below (a die off its station, an out cup's lie, a
 * throw's shake length and start) is drawn from it. */
double cn_geom_hash(int32_t ix, int32_t iy, int32_t seed);

/* ---- the renderer's format ----------------------------------------------------- */
#define CN_GEOM_VF 6
#define CN_GEOM_FF 16
#define CN_GEOM_F_CULL     1   /* a closed body: faces turned away are skipped */
#define CN_GEOM_F_CAST     2   /* casts a shadow                               */
#define CN_GEOM_F_RECEIVE  4   /* takes the shadow                             */
#define CN_GEOM_F_RECEIVER 8   /* the table (no mesh here carries it)          */
#define CN_GEOM_F_BODY     (CN_GEOM_F_CULL | CN_GEOM_F_CAST | CN_GEOM_F_RECEIVE)

/* The texture a face wears, as a slot; the host maps each slot of a body to
 * its own texture (the study's TEX3.cupSide(seed) and so on). */
enum { CN_TEX_CUP_SIDE, CN_TEX_CUP_CROWN, CN_TEX_CUP_INNER, CN_TEX_CUP_FLOOR, CN_TEX_DIE, CN_TEX_SLOTS };

/* ---- meshes --------------------------------------------------------------------- */
#define CN_CUP_SEGS        36     /* facets round a cup                              */
#define CN_CUP_RC          .72    /* the crown's radius, of the mouth's              */
#define CN_CUP_WALL        .06    /* the wall's thickness, of the mouth's radius     */
#define CN_CUP_FILLET      .09    /* the crown's rounded edge, of the mouth's radius */
#define CN_CUP_FILLET_ROWS 3
#define CN_CUP_WALL_ROWS   2
#define CN_DIE_ROUND       .18    /* a die's edge radius, of its side                */
#define CN_DOME            .35    /* the cup floor's dome, of a die (cn_roll.c's DOME) */

#define CN_MESH_MAX_POS    520
#define CN_MESH_MAX_POLY   340
#define CN_MESH_MAX_CORNER 1300

/* A body in its own frame: positions, and polygons as runs of corners. A
 * corner names a position and carries its uv and its normal; a polygon whose
 * `smooth` is 0 takes the flat normal of its first three corners, in the
 * world, as the study did. A die's polygon knows its face slot (`face`, the
 * study's AX order: +z -z -y +x +y -x) and its uv is the face-local one; the
 * atlas cell is chosen when it is emitted, from the die's cells. */
typedef struct {
    uint16_t npos, npoly, ncorner, ntri;
    float    pos[CN_MESH_MAX_POS][3];
    uint16_t first[CN_MESH_MAX_POLY];
    uint8_t  count[CN_MESH_MAX_POLY];
    uint8_t  tex[CN_MESH_MAX_POLY];
    uint8_t  smooth[CN_MESH_MAX_POLY];
    int8_t   face[CN_MESH_MAX_POLY];
    uint16_t at[CN_MESH_MAX_CORNER];
    float    uv[CN_MESH_MAX_CORNER][2];
    float    nrm[CN_MESH_MAX_CORNER][3];
} CnMesh;

/* The cup: mouth radius R at z = 0, crown radius rc at z = h, a cone wall in
 * CN_CUP_WALL_ROWS rows, a fillet of CN_CUP_FILLET * R in CN_CUP_FILLET_ROWS
 * rows with its own normals (the light rolls off it), a flat crown. With
 * t > 0 it has an inside: the inner wall (R - t to rc - t over h - t), the
 * floor at h - t raised toward the mouth by `dome` at its centre, and the rim.
 * Returns 0 when the mesh would not fit. */
int cn_geom_cup_mesh(CnMesh *m, double R, double rc, double h, int segs, double t, double dome);
/* The die: a cube of side d about its centre, its edges rounded at
 * CN_DIE_ROUND * d, each face a 5 by 5 grid that bends round the radius. */
int cn_geom_die_mesh(CnMesh *m, double d);

/* ---- the die's values ------------------------------------------------------------- */
/* Face slots in the study's order, and cn_roll.c's up index (0 +x, 1 -x, 2 +y,
 * 3 -y, 4 +z, 5 -z) to a slot. */
int cn_die_slot(int axis, int sign);
int cn_die_slot_of_up(int up);
/* The values on the six slots with `value` on the face (axis, sign): a
 * standard right-handed die, every pair of opposite faces summing to 7 and
 * 1, 2, 3 running counterclockwise round their corner. (The study put the
 * opposite of the up face right and filled the four sides in order, so its
 * 2 sat opposite its 4.) The side faces are the lowest pair first on the
 * next axis round, positive side first. */
void cn_die_cells(int axis, int sign, int value, uint8_t cells[6]);
int  cn_die_value(const uint8_t cells[6], int axis, int sign);

/* ---- bodies placed on the board ------------------------------------------------------ */
enum { CN_OBJ_CUP = 1, CN_OBJ_DIE = 2 };
typedef struct {
    uint8_t  kind;          /* CN_OBJ_*                                              */
    uint8_t  seat;
    uint8_t  mine;          /* my cup (the peekable one) or one of my dice           */
    uint8_t  out;           /* a cup lying on its side: the seat has no dice        */
    uint8_t  value;         /* a die: the value up; a cup: the count on its crown    */
    uint8_t  cells[6];      /* a die: the value on each face slot                    */
    uint8_t  has_rot;       /* 1: rot is the pose; 0: a turn of yaw about z          */
    uint8_t  crown_big;     /* the crown's count is set at 184 of 256, not 112       */
    uint8_t  pad0[3];
    uint32_t tex_seed;      /* the seed the host's textures for this body are cut by */
    float    x, y, lift, yaw;   /* board points; lift is z of the body's origin      */
    float    rot[9];        /* columns: the body's x, y, z axes in the world (c*3+r) */
    float    tilt_angle, tilt_hinge_y, tilt_back, tilt_lift;   /* the peek (cn_cam.h) */
    float    R, h, d;       /* a cup's mouth radius and height; a die's side         */
    float    ring;          /* a cup: the radius of the ring of five under it         */
    float    home_x, home_y;/* where it stands when nothing moves it                 */
    float    tint[3], kmul[2];  /* the faces' r g b and km ka                         */
    float    shadow_r, shadow_dx, shadow_dy;   /* a lying cup's contact footprint      */
} CnObj;

/* A seat's cup standing mouth down at (x, y), the study's cupObj: rc and h
 * from R (CN_CUP_RC, CN_CUP_TALL). `out` lays it on its side across the seat,
 * both rims on the table, its mouth toward toward_x (left or right), turned a
 * little by its seed; it is then dim (its tint). */
void cn_geom_cup_obj(CnObj *o, float R, int count, uint32_t seed, float x, float y, int out, float toward_x);
/* A die of side d resting at (x, y) turned yaw, `value` up. */
void cn_geom_die_obj(CnObj *o, float d, int value, uint32_t seed, float x, float y, float yaw);
/* The contact footprint the renderer darkens the table under (scene_occluder's
 * five arguments: x y r h k), pad_x added as the renderer's canvas has it. */
void cn_geom_occluder(const CnObj *o, float pad_x, float out[5]);

/* Write a placed body: m->ncorner vertices at verts + vbase * VF and m->ntri
 * faces at faces + fbase * FF, the faces' indices counting from vbase. The
 * world is the board shifted right by pad_x (the canvas's side room).
 * tex_ids maps CN_TEX_* to the host's texture ids. */
void cn_geom_emit(const CnMesh *m, const CnObj *o, float pad_x, const int tex_ids[CN_TEX_SLOTS],
                  float *verts, int vbase, float *faces, int fbase);

/* ---- a baked throw at any instant ------------------------------------------------------ */
typedef struct {
    float    p[3];
    float    rot[9];        /* columns, as CnObj.rot */
    uint8_t  phase;         /* CN_RP_* of the frame at or before the instant */
    uint8_t  pad0;
    uint16_t frame;
} CnPose;
/* Pose k (0 the cup, 1.. the dice) of a bake of n frames at T seconds: the two
 * 60 Hz frames round T, the position lerped and the quaternion nlerped the
 * short way round; T is clamped to the bake. */
void cn_geom_pose_at(const float *frames, const uint8_t *phase, int n, double T, int k, CnPose *out);
/* Put a body where a pose says: a cup in an IDLE frame stands at its home,
 * otherwise it takes the pose; a die always does. A die in an IDLE frame of
 * the cup roll reads its value off the face cn_roll.c says is up (`up`, or -1
 * to leave it). */
void cn_geom_place(CnObj *o, const CnPose *p, int up);

#endif
