/* Chui Niu - the camera: the seated eye the bodies are drawn from, the turn of
 * the whole screen that the leaning head sees, and the peek's tip of my cup.
 *
 * THE EYE (DECISIONS K14). The study draws every body from a seat's eye, not
 * a lamp's: CN_CAM_HC above the table and CN_CAM_DOWN of the board's height
 * past its centre toward me, with a shifted lens, so the table plane (and the
 * layout on it) is drawn 1:1 and everything upright leans away from that
 * eye's foot. A point (x, y, z) of the board lands at
 *     e + (p - e) * hc / (hc - z)        (cn_cam_project)
 * The camera is FIXED at the leaning head's place on every screen (round
 * twenty-one): the eye CN_CAM_VIEW_HC up and CN_CAM_VIEW_DOWN boards past the
 * centre. Turning the camera toward my cup is only a change of image plane,
 * a homography of the flat picture, so the host applies it to the whole
 * screen's flat layers at once (the planks, the names, the scene's canvas),
 * never to the HUD (the bid plate and the picker stay flat and untilted).
 *
 * THE TURN, FOR A SWIFT HOST. The study's CSS is
 *     transform-origin: origin;  transform: perspective(D) rotateX(theta) scale(zoom)
 * and CATransform3D is the same algebra in row-vector order. Either
 *   (a) set the layer's anchorPoint so its anchor is at `origin` (screen
 *       points) and its transform to cam.ca (m11 .. m44 in order): this is
 *       scale(zoom) * rotateX(theta) * (m34 = -1/D), so equivalently
 *           var t = CATransform3DIdentity; t.m34 = -1 / D
 *           t = CATransform3DConcat(CATransform3DMakeRotation(theta, 1, 0, 0), t)
 *           t = CATransform3DConcat(CATransform3DMakeScale(zoom, zoom, 1), t)
 *   or (b) leave the layer's anchor at the screen's (0, 0) and use cam.ca_screen,
 *       which has the origin's translation folded in.
 * cam.h is the same map as a 3x3 homography on the screen plane (row-major,
 * [x' y' w'] = h * [x y 1], divide by w'), for a host that warps a bitmap.
 * cn_cam_map is that map, point by point.
 *
 * TWO MAPS, AND WHY. cn_cam_to_screen / cn_cam_from_screen are the study's
 * tiltOf.toScreen / fromScreen, which the layout's fit (cn_lay.c) is pinned
 * to: they apply the zoom AFTER the perspective divide. The transform the
 * study paints (and cam.ca) applies it BEFORE (CSS multiplies scale first),
 * so the two disagree a little far from my cup: 3.5 points at 400 points
 * above it on a 390 by 718 screen. The layout keeps the study's map so its
 * numbers are the study's; docs_pkgB.md records the difference.
 *
 * Points throughout; the board's coordinates are cn_lay.h's (its top-left at
 * the board's corner), the screen's are the drawer's. */
#ifndef CN_CAM_H
#define CN_CAM_H

#include <stdint.h>

#define CN_CAM_HC         720.0   /* the seat's eye, up                          */
#define CN_CAM_DOWN       .5      /* and past the board's centre, of its height  */
#define CN_CAM_VIEW_HC    560.0   /* the leaning head's eye (the study's VIEW_B) */
#define CN_CAM_VIEW_DOWN  .85
#define CN_CAM_ZOOM       1.06    /* ZOOM_B                                      */
#define CN_CAM_ORIGIN_UP  8.0     /* the turn's line is this far above the origin */
#define CN_PEEK_EXTRA_DEG 7       /* a little past the least tip that shows the dice */
#define CN_PEEK_MS        420     /* the peek's tween                             */
#define CN_PEEK_BACK      .2      /* the hand draws the cup back, of its radius   */
#define CN_PEEK_LIFT      .12     /* and up                                       */

typedef struct {
    float board_w, board_h;   /* the board the eye is placed over                 */
    float eye_x, eye_y, eye_z;/* the eye the bodies are drawn from, board points  */
    float origin_x, origin_y; /* the turn's centre, screen points                 */
    float theta;              /* the turn about x, radians                        */
    float D;                  /* the perspective's distance                       */
    float zoom;
    float t;                  /* how far toward the leaning head (always 1 now)   */
    float ca[16];             /* CATransform3D about the origin, m11 .. m44       */
    float ca_screen[16];      /* the same about the screen's (0, 0)               */
    float h[9];               /* the homography, row-major, column vectors        */
} CnCam;

/* The camera over a board of board_w by board_h, the turn about origin (screen
 * points; the study's my cup's centre on the screen), t of the way from the
 * seat's eye to the leaning head's (the study's tiltOf; t is 1 everywhere). */
void  cn_cam_make(CnCam *c, float board_w, float board_h, float origin_x, float origin_y, float t);
/* The study's toScreen: the screen y a point dy below the origin (untilted)
 * lands at, and the scale there. */
float cn_cam_to_screen(const CnCam *c, float dy, float *scale);
/* The study's fromScreen: the untilted dy below the origin that lands at y. */
float cn_cam_from_screen(const CnCam *c, float y);
/* The painted map: a screen point through cam.ca_screen. */
void  cn_cam_map(const CnCam *c, float x, float y, float *sx, float *sy);
/* A board point (x, y, z) as the eye sees it, on the table plane. */
void  cn_cam_project(const CnCam *c, float x, float y, float z, float *px, float *py);

/* ---- the peek ------------------------------------------------------------------- */
/* The cup turns about the far edge of its mouth (hinge_y = -R in the cup's
 * frame), and as it turns the hand draws it back and up, in proportion to how
 * far it has turned of the full tip. */
typedef struct { float angle, hinge_y, back, lift; } CnPeek;
CnPeek cn_cam_peek_tilt(float R, float angle, float full);
/* The least tip (whole degrees from 10, plus CN_PEEK_EXTRA_DEG, at most a
 * right angle) at which the near rim's shadow from the eye falls past the far
 * edge of the farthest of my dice, with 4 points to spare. my_cy is my cup's
 * centre's board y; dice_y and dice_d my dice's centres and sides. */
float cn_cam_peek_angle(const CnCam *c, float R, float my_cy, const float *dice_y, const float *dice_d, int n);
/* The peek's tween: 1 - (1 - t)^3 (1 - .12 sin(pi t)), t the fraction of
 * CN_PEEK_MS gone. The tip at a moment is from + (to - from) * ease(t). */
float cn_cam_peek_ease(float t);

#endif
