/* The scene renderer for a browser (src/cn_scene.c, in the same module as the
 * throw): the arena is this file's static block, and the exports keep the names
 * docs/UI.html calls. scene_reset hands the block over, so a page that resets
 * first (as the study does) never sees an uninitialised renderer. */
#include "cn_scene.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static uint8_t arena[CN_SCENE_ARENA_WEB] __attribute__((aligned(16)));

EXPORT(scene_reset)    void      scene_reset(void)                          { cn_scene_init(arena, sizeof arena); }
EXPORT(scene_tex_new)  int       scene_tex_new(int w, int h, int has_bump)  { return cn_scene_tex_new(w, h, has_bump); }
EXPORT(scene_tex_rgba) uint8_t  *scene_tex_rgba(int id)                     { return cn_scene_tex_rgba(id); }
EXPORT(scene_tex_bump) int8_t   *scene_tex_bump(int id)                     { return cn_scene_tex_bump(id); }
EXPORT(scene_begin)    int       scene_begin(int W, int H, int pad, float dpr, float ex, float ey, float hc,
                                             float lx, float ly, float lz, int shadow_res, float dark, int vcapacity, int fcapacity)
{
    return cn_scene_begin(W, H, pad, dpr, ex, ey, hc, lx, ly, lz, shadow_res, dark, vcapacity, fcapacity);
}
EXPORT(scene_occluder) void      scene_occluder(float x, float y, float r, float lift, float strength) { cn_scene_occluder(x, y, r, lift, strength); }
EXPORT(scene_verts)    float    *scene_verts(void)                          { return cn_scene_verts(); }
EXPORT(scene_faces)    float    *scene_faces(void)                          { return cn_scene_faces(); }
EXPORT(scene_render)   int       scene_render(int nverts, int nfaces)       { return cn_scene_render(nverts, nfaces); }
EXPORT(scene_fb)       uint8_t  *scene_fb(void)                             { return cn_scene_fb(); }
EXPORT(scene_fb_w)     int       scene_fb_w(void)                           { return cn_scene_fb_w(); }
EXPORT(scene_fb_h)     int       scene_fb_h(void)                           { return cn_scene_fb_h(); }
EXPORT(scene_prof)     uint32_t  scene_prof(int i)                          { return cn_scene_prof(i); }
EXPORT(scene_skip)     void      scene_skip(int mask)                       { cn_scene_skip(mask); }
