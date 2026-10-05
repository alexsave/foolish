/* The scene renderer: a software rasterizer with a shadow map, freestanding C.
 * The study (docs/UI.html) runs it as wasm through wasm/cn_scene_web.c; the
 * iOS host links it natively and draws the framebuffer (a CGImage on a layer).
 * One renderer a process: the state is the module's, the memory the caller's.
 *
 * A FRAME, in order:
 *   cn_scene_init(mem, bytes)        once, or again after freeing the memory
 *   cn_scene_tex_new(...)            any time between frames; write its pixels
 *                                    through cn_scene_tex_rgba / _bump
 *   cn_scene_begin(...)              0 when the frame does not fit: draw nothing
 *   cn_scene_occluder(...)           each body's footprint on the table
 *   write cn_scene_verts() / cn_scene_faces()
 *   cn_scene_render(nverts, nfaces)
 *   read cn_scene_fb(): cn_scene_fb_w() by cn_scene_fb_h() RGBA, straight alpha
 *   (the table's pixels are black-ish with alpha; bodies are opaque but at their
 *   smoothed edges), or premultiplied after cn_scene_premultiply(1)
 *
 * MEMORY is the caller's block: textures (and their half-size copies, made the
 * first time a texture is drawn) grow down from its top, a frame's buffers up
 * from its bottom, and a frame that would meet the textures fails cleanly
 * (cn_scene_begin returns 0). Nothing is allocated; the caller may free the
 * block whenever no call is running, after which cn_scene_init must come first.
 * A frame keeps whole only its picture (4 bytes a pixel) and each pixel's
 * surface (2 bytes); the depth and the rest of what the shade reads live in a
 * strip of CN_SCENE_STRIP rows a band, drawn and shaded strip by strip. */
#ifndef CN_SCENE_H
#define CN_SCENE_H

#include <stddef.h>
#include <stdint.h>

/* the default budgets: the browser's static arena, and what the iOS host hands
 * over (the Messages extension is killed past its memory limit, so the frame
 * must fit a fixed block rather than grow) */
#define CN_SCENE_ARENA_WEB  ((size_t)256 << 20)
#define CN_SCENE_ARENA_IOS  ((size_t)48 << 20)

/* face flags */
#define CN_SCENE_F_CULL     1   /* a closed body: faces turned away are skipped       */
#define CN_SCENE_F_CAST     2   /* casts a shadow                                      */
#define CN_SCENE_F_RECEIVE  4   /* takes the shadow (every lit thing does)             */
#define CN_SCENE_F_RECEIVER 8   /* the table: writes only the shadow that falls on it  */
/* THE SURFACE a face belongs to, in the flags' bits from 8 up: a body's number
 * (1 to 65535; 0 is every unnumbered face). Where a pixel's surface (its body
 * and its texture) differs from a neighbour's, the edges pass draws the pixel
 * from four samples, so a silhouette is smooth; the facets of one surface are
 * never an edge. */
#define CN_SCENE_F_ID(n)    ((n) << 8)

/* the largest shadow map: a pixel keeps its place on the map at a 64th of a texel in 16 bits */
#define CN_SCENE_SHADOW_MAX 1024

#define CN_SCENE_VF 6  /* floats a vertex: x y z nx ny nz                                   */
#define CN_SCENE_FF 16  /* floats a face: i0 i1 i2 u0 v0 u1 v1 u2 v2 tex r g b km ka flags */

/* the arena: 16-byte aligned memory of `bytes`; drops every texture and frame.
 * 0 when mem is null or too small to be of use. */
int cn_scene_init(void *mem, size_t bytes);
/* drop every texture and the frame, keep the arena */
void cn_scene_reset(void);

/* a texture: w by h RGBA, and a normal map of (dx, dy) as signed bytes at a 20th
 * each, or none. Its id, or -1 when there is no room. */
int cn_scene_tex_new(int w, int h, int has_bump);
uint8_t *cn_scene_tex_rgba(int id);
int8_t *cn_scene_tex_bump(int id);

/* the bytes a frame of these numbers takes from the arena, and the bytes a frame
 * has (the arena less the textures and their copies) */
size_t cn_scene_frame_bytes(int W, int H, int pad, float dpr, int shadow_res, int vcapacity, int fcapacity);
size_t cn_scene_room(void);

/* a frame: the board W by H points (plus pad above it), dpr device pixels a
 * point, the eye over (ex, ey) at height hc, the light's direction (toward the
 * light), the shadow map's resolution, how dark a shadow is (0..1), and room for
 * vcapacity vertices and fcapacity faces. vcapacity + fcapacity, or 0 when the
 * frame does not fit or the numbers are out of range (no frame is then drawable). */
int cn_scene_begin(int W, int H, int pad, float dpr, float ex, float ey, float hc,
                   float lx, float ly, float lz, int shadow_res, float dark, int vcapacity, int fcapacity);
float *cn_scene_verts(void);
float *cn_scene_faces(void);
/* a body's footprint: its centre and radius (points), its height above the table,
 * and its strength when down */
void cn_scene_occluder(float x, float y, float r, float lift, float strength);

/* the shadow pass, the picture, the shade, on the calling thread. The faces drawn,
 * or -1 when no frame has begun or the counts exceed its capacities. A face naming
 * a vertex past nverts is skipped. Exactly cn_scene_prepare, then every pass as
 * one band. */
int cn_scene_render(int nverts, int nfaces);

/* THE SAME FRAME ON SEVERAL THREADS. cn_scene_prepare does the frame's serial
 * work (the faces' order, which are drawn and each one's setup, the textures'
 * half-size copies, the contact dark) and returns what cn_scene_render would.
 * Then each pass, in order, is cut into nbands horizontal bands (1 to
 * CN_SCENE_MAX_BANDS) which may run at once on any threads, every band of a pass
 * finishing before the next pass starts (on iOS: DispatchQueue.concurrentPerform
 * (iterations: nbands) per pass). A band writes only its own rows, so the picture
 * is the same bits for any nbands. Occluders are given before cn_scene_prepare.
 * Band b's working memory is its own (the strip, its faces, its edge samples):
 * two calls with the same band never run at once. */
#define CN_SCENE_MAX_BANDS 16
#define CN_SCENE_STRIP     16   /* the rows a band draws and shades at a time       */
#define CN_SCENE_PASS_SHADOW  0   /* the shadow map's rows                          */
#define CN_SCENE_PASS_PICTURE 1   /* the picture's rows, a strip at a time: depth,
                                     texel, normal, light, then the shadow and the shade */
#define CN_SCENE_PASS_EDGES   2   /* the picture's rows again: each pixel on a surface's
                                     edge drawn again from four samples           */
#define CN_SCENE_PASS_COMMIT  3   /* each band's first and last rows' edges, held back
                                     while the bands beside it read them          */
#define CN_SCENE_PASSES       4
int cn_scene_prepare(int nverts, int nfaces);
void cn_scene_band(int pass, int band, int nbands);
uint8_t *cn_scene_fb(void);
int cn_scene_fb_w(void);
int cn_scene_fb_h(void);

/* the output's alpha from the next frame on: 0 straight (the default; a
 * browser's putImageData wants it), 1 premultiplied (what Core Animation draws
 * without converting) */
void cn_scene_premultiply(int on);

/* profiling: the last frame's fragments shaded, box pixels walked, map texels,
 * map box pixels, edge pixels, edge samples shaded; and passes to leave out (1
 * shadow map, 2 picture, 4 shading, 16 the edges). 8 is a test's: the open
 * table lit pixel by pixel everywhere, which must draw the same bytes as the
 * blocks it settles whole. */
#define CN_SCENE_PROFS 6
uint32_t cn_scene_prof(int i);
void cn_scene_skip(int mask);

#endif
