/* Chui Niu - the stage. See cn_stage.h; the decisions are DECISIONS I19 on. */
#include "cn_stage.h"
#include "cn_scene.h"
#include "cn_beats.h"
#include <string.h>

/* the renderer's format is cn_geom's, written there as numbers */
_Static_assert(CN_GEOM_VF == CN_SCENE_VF && CN_GEOM_FF == CN_SCENE_FF, "one vertex and face format");
_Static_assert(CN_STAGE_ARENA == CN_SCENE_ARENA_IOS, "the host's arena is the renderer's budget");
_Static_assert(CN_STAGE_PASSES == CN_SCENE_PASSES && CN_STAGE_BANDS <= CN_SCENE_MAX_BANDS, "the bands");

#define BIG_T 1.0e6        /* seconds: past the end of every bake (a still table) */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* every upload into the renderer is numbered, so a stage notices another one used it */
static uint32_t scene_gen_now;

uint32_t cn_stage_round_seed(const uint8_t seed[32], int round)
{
    static const char tag[] = "chuiniu.throw.1|";
    uint32_t h = 2166136261u;
    for (int i = 0; tag[i]; i++) { h ^= (uint8_t)tag[i]; h *= 16777619u; }
    for (int i = 0; i < 32; i++) { h ^= seed ? seed[i] : 0; h *= 16777619u; }
    h ^= (uint32_t)(round & 0xFF); h *= 16777619u;
    h ^= (uint32_t)((round >> 8) & 0xFF); h *= 16777619u;
    return h;
}

/* ---- the arena and the textures ----------------------------------------------------- */

int cn_stage_attach(CnStage *st, void *arena, size_t bytes)
{
    st->arena = 0; st->arena_bytes = 0; st->uploaded = 0;
    if (!arena || !cn_scene_init(arena, bytes)) return CN_STAGE_E_ARENA;
    st->arena = arena; st->arena_bytes = bytes;
    return 0;
}

/* the pack only: a begin needs nothing else (the layout, the meshes and the bakes are the handle's), and
 * the arena is a frame's, attached when the host is about to draw */
int cn_stage_init(CnStage *st, const uint8_t *pack, size_t pack_len)
{
    memset(st, 0, sizeof *st);
    int e = pack && pack_len <= 0xFFFFFFFFu ? cn_tex_pack_open(&st->pack, pack, (uint32_t)pack_len) : CN_TEX_E_SHORT;
    if (e != CN_TEX_OK) return e;
    st->pack_ok = 1;
    return 0;
}

void cn_stage_purge(CnStage *st)
{
    if (st->arena) cn_scene_init(0, 0);
    st->arena = 0; st->arena_bytes = 0;   /* the textures went with it: cn_stage_attach uploads again */
    st->shot.ok = 0;
}

static int sink_alloc(void *ctx, int w, int h, int has_bump, CnTexImage *img)
{
    (void)ctx;
    int id = cn_scene_tex_new(w, h, has_bump);
    if (id < 0) return -1;
    img->w = w; img->h = h; img->rgba = cn_scene_tex_rgba(id); img->bump = has_bump ? cn_scene_tex_bump(id) : 0;
    return id;
}

static int upload(CnStage *st, CnTexWhat what, uint32_t seed, int count, int out, int px)
{
    const CnTexSink sink = { st, sink_alloc };
    CnTexSpec s;
    memset(&s, 0, sizeof s);
    s.what = what; s.seed = seed; s.count = count; s.out = out; s.numeral_px = px; s.bump = 1;
    return cn_tex_upload(&st->pack, &sink, &s);
}

/* the crowns the begun table wears: one a cup, by its seed, count, lie and numeral size */
static int crown_of(const CnStage *st, const CnObj *o)
{
    for (int i = 0; i < st->ncrown; i++) {
        const CnStageCrown *c = &st->crown[i];
        if (c->seed == o->tex_seed && c->count == (o->out ? -1 : o->value) && c->out == o->out && c->big == o->crown_big) return i;
    }
    return -1;
}

/* THE TEXTURE SET IS A PURE FUNCTION OF THE BEGUN TABLE (I20): the shared side,
 * inside and floor (my cup's seed, 3), the shared die atlas (my first die's,
 * 60), and the crowns in the order begin listed them, uploaded into an emptied
 * renderer; then one throwaway frame draws a face of each so the renderer makes
 * every half-size copy now, before any real frame's buffers are taken (made on
 * first use inside a frame, a copy that found no room would leave the chain
 * short, and the picture would depend on what was drawn before). */
static int textures(CnStage *st)
{
    if (st->uploaded && st->scene_gen == scene_gen_now) return 1;
    st->uploaded = 0;
    if (!st->arena || !st->pack_ok) return 0;
    cn_scene_reset();
    st->tex_side = upload(st, CN_TEX_SIDE, 3, 0, 0, 0);
    st->tex_inner = upload(st, CN_TEX_INNER, 3, 0, 0, 0);
    st->tex_floor = upload(st, CN_TEX_FLOOR, 3, 0, 0, 0);
    st->tex_die = upload(st, CN_TEX_DIE, 60, 0, 0, 0);
    int ok = st->tex_side >= 0 && st->tex_inner >= 0 && st->tex_floor >= 0 && st->tex_die >= 0;
    for (int i = 0; i < st->ncrown && ok; i++) {
        CnStageCrown *c = &st->crown[i];
        c->id = upload(st, CN_TEX_CROWN_T, c->seed, c->count, c->out, c->big ? CN_TEX_NUMERAL_LARGE : CN_TEX_NUMERAL_SMALL);
        ok = c->id >= 0;
    }
    if (!ok) { cn_scene_reset(); return 0; }
    /* the warm frame: a triangle a texture, facing the eye, never culled */
    const int nt = 4 + st->ncrown;
    if (!cn_scene_begin(8, 8, 0, 1, 4, 4, 100, 0, 0, 1, 4, 0, 3 * nt, nt)) { cn_scene_reset(); return 0; }
    float *V = cn_scene_verts(), *F = cn_scene_faces();
    for (int i = 0; i < nt; i++) {
        const float tri[3][2] = { { 1, 1 }, { 7, 1 }, { 1, 7 } };
        for (int k = 0; k < 3; k++) {
            float *v = V + (size_t)(i * 3 + k) * CN_SCENE_VF;
            v[0] = tri[k][0]; v[1] = tri[k][1]; v[2] = 1; v[3] = 0; v[4] = 0; v[5] = 1;
        }
        float *f = F + (size_t)i * CN_SCENE_FF;
        memset(f, 0, CN_SCENE_FF * sizeof *f);
        f[0] = (float)(i * 3); f[1] = (float)(i * 3 + 1); f[2] = (float)(i * 3 + 2);
        f[5] = 1; f[8] = 1;
        f[9] = (float)(i < 4 ? (i == 0 ? st->tex_side : i == 1 ? st->tex_inner : i == 2 ? st->tex_floor : st->tex_die) : st->crown[i - 4].id);
        f[13] = 1; f[15] = CN_SCENE_F_RECEIVE;
    }
    if (cn_scene_render(3 * nt, nt) < 0) { cn_scene_reset(); return 0; }
    st->uploaded = 1;
    st->scene_gen = ++scene_gen_now;
    return 1;
}

/* ---- begin -------------------------------------------------------------------------- */

static int valid(const CnStageIn *in)
{
    if (in->kind < CN_STAGE_TABLE || in->kind > CN_STAGE_BUBBLE) return 0;
    if (in->seats < 2 || in->seats > CN_STAGE_SEATS || in->me >= in->seats || in->turn >= in->seats) return 0;
    for (int s = 0; s < in->seats; s++) {
        if (in->dice[s] > CN_STAGE_DICE) return 0;
        if (in->known_mask >> s & 1)
            for (int k = 0; k < in->dice[s]; k++) if (in->faces[s * CN_STAGE_DICE + k] < 1 || in->faces[s * CN_STAGE_DICE + k] > 6) return 0;
    }
    if (in->kind == CN_STAGE_TABLE && !(in->known_mask >> in->me & 1) && in->dice[in->me]) return 0;   /* my own dice */
    if (!(in->scale > 0) || !(in->scale < 1000)) return 0;
    return 1;
}

/* every body before the clock, and the mesh each wears */
static int bodies(CnStage *st)
{
    const CnStageIn *in = &st->in;
    if (in->kind == CN_STAGE_BUBBLE) {
        /* the study's bubble(): the cups in a row at 46, mouth radius 16 past four seats, else 19 */
        const int n = in->seats;
        const float cs = n > 4 ? 16 : 19, step = n > 4 ? 46 : 60, x0 = 150 - (n - 1) * step / 2;
        for (int i = 0; i < n; i++) {
            const int out = in->out_mask >> i & 1;
            cn_geom_cup_obj(&st->base[i], cs, in->dice[i], (uint32_t)(i * 7), x0 + i * step, 46, out, 150);
            st->base[i].seat = (uint8_t)i;
            /* the study gives a lying cup an inside (no dome) and a standing one none */
            st->obj_mesh[i] = out ? CN_STAGE_MESH_CUP_MINE : CN_STAGE_MESH_CUP_THEIRS;
        }
        const double h = cs * CN_CUP_TALL, rc = cs * CN_CUP_RC;
        if (!cn_geom_cup_mesh(&st->mesh[CN_STAGE_MESH_CUP_THEIRS], cs, rc, h, CN_CUP_SEGS, 0, 0)) return 0;
        if (!cn_geom_cup_mesh(&st->mesh[CN_STAGE_MESH_CUP_MINE], cs, rc, h, CN_CUP_SEGS, cs * CN_CUP_WALL, 0)) return 0;
        st->nobj = (uint8_t)n;
        return 1;
    }
    const CnLay *L = &st->lay;
    int n = cn_lay_objects(&st->lin, L, st->base, CN_LAY_MAX_OBJS);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        const CnObj *o = &st->base[i];
        st->obj_mesh[i] = (uint8_t)(o->kind == CN_OBJ_DIE ? (o->mine ? CN_STAGE_MESH_DIE_MINE : CN_STAGE_MESH_DIE_THEIRS)
                                                            : (o->mine ? CN_STAGE_MESH_CUP_MINE : CN_STAGE_MESH_CUP_THEIRS));
    }
    /* every cup has an inside, its floor domed to its dice (the study's ROLL_DOME) */
    const double R = L->my_r, r = L->cup_r;
    if (!cn_geom_cup_mesh(&st->mesh[CN_STAGE_MESH_CUP_MINE], R, R * CN_CUP_RC, R * CN_CUP_TALL, CN_CUP_SEGS, R * CN_CUP_WALL, CN_DOME * L->d)) return 0;
    if (!cn_geom_cup_mesh(&st->mesh[CN_STAGE_MESH_CUP_THEIRS], r, r * CN_CUP_RC, r * CN_CUP_TALL, CN_CUP_SEGS, r * CN_CUP_WALL, CN_DOME * L->sd)) return 0;
    if (!cn_geom_die_mesh(&st->mesh[CN_STAGE_MESH_DIE_MINE], L->d)) return 0;
    if (!cn_geom_die_mesh(&st->mesh[CN_STAGE_MESH_DIE_THEIRS], L->sd)) return 0;
    st->nobj = (uint8_t)n;
    return 1;
}

/* every seat's throw, baked; each body told which throw and which pose it rides */
static int throws(CnStage *st)
{
    memset(st->obj_throw, -1, sizeof st->obj_throw);
    st->nthrow = 0;
    if (st->in.kind == CN_STAGE_BUBBLE) return 1;
    CnLayThrow lt[CN_STAGE_SEATS];
    int nt = cn_lay_throws(&st->lin, &st->lay, CN_THROW_CUP, lt, CN_STAGE_SEATS);
    if (nt < 0) return 0;
    for (int j = 0; j < nt; j++) {
        CnStageThrow *T = &st->thr[j];
        T->t = lt[j];
        int n = cn_roll_bake(&T->t.t, T->t.seed, T->frames, T->phase, CN_ROLL_MAX_FRAMES, &T->info);
        if (n < 1) return 0;
        T->n = (uint16_t)n;
        T->idle_at = (uint16_t)(n - 1);
        for (int f = 0; f < n; f++) if (T->phase[f] == CN_RP_IDLE) { T->idle_at = (uint16_t)f; break; }
        T->delay_ms = (uint32_t)(T->t.delay * 1000.0f + .5f);
        int k = 0;
        for (int i = 0; i < st->nobj; i++) {
            if (st->base[i].seat != T->t.seat) continue;
            if (st->base[i].kind == CN_OBJ_CUP) { st->obj_throw[i] = (int8_t)j; st->obj_pose[i] = 0; }
            else if (k < T->t.t.dice) { st->obj_throw[i] = (int8_t)j; st->obj_pose[i] = (int8_t)(1 + k++); }
        }
    }
    st->nthrow = (uint8_t)nt;
    return 1;
}

/* THE HAND (I19): each known seat's dice painted on the faces its throw left up */
static void paint(CnStage *st)
{
    const CnStageIn *in = &st->in;
    int k[CN_STAGE_SEATS] = { 0 };
    for (int i = 0; i < st->nobj; i++) {
        CnObj *o = &st->base[i];
        if (o->kind != CN_OBJ_DIE) continue;
        const int s = o->seat, idx = k[s]++;
        if (!(in->known_mask >> s & 1) || idx >= in->dice[s]) continue;
        const int value = in->faces[s * CN_STAGE_DICE + idx];
        int up = 4;                                  /* +z: a die nobody threw lies as placed */
        if (st->obj_throw[i] >= 0) up = st->thr[(int)st->obj_throw[i]].info.up[st->obj_pose[i] - 1];
        cn_die_cells(up >> 1, up & 1 ? -1 : 1, value, o->cells);
        o->value = (uint8_t)value;
    }
}

/* the seconds into throw j at t (BIG_T for a still table) */
static double throw_t(const CnStage *st, int j, uint32_t t_ms)
{
    if (st->in.roll_at_ms == CN_STAGE_NO_ROLL) return BIG_T;
    return ((double)t_ms - (double)st->in.roll_at_ms - (double)st->thr[j].delay_ms) / 1000.0;
}

/* the bodies at t, before the peek and the lift */
static void posed(CnStage *st, uint32_t t_ms, CnObj *out)
{
    memcpy(out, st->base, sizeof(CnObj) * st->nobj);
    for (int i = 0; i < st->nobj; i++) {
        const int j = st->obj_throw[i];
        if (j < 0) continue;
        const CnStageThrow *T = &st->thr[j];
        CnPose p;
        cn_geom_pose_at(T->frames, T->phase, T->n, throw_t(st, j, t_ms), st->obj_pose[i], &p);
        cn_geom_place(&out[i], &p, -1);
    }
}

/* a board point, up z, on the glass: the eye's projection, the board's place, the turn */
static void glass(const CnStage *st, const CnCam *c, float x, float y, float z, float *gx, float *gy)
{
    float px, py;
    cn_cam_project(c, x, y, z, &px, &py);
    cn_cam_map(c, st->hud.board[0] + px, st->hud.board[1] + py, gx, gy);
}

static void hud_cam(CnStageHud *h, const CnCam *c)
{
    h->origin_x = c->origin_x; h->origin_y = c->origin_y;
    h->theta = c->theta; h->cam_d = c->D; h->zoom = c->zoom;
    memcpy(h->ca, c->ca, sizeof h->ca); memcpy(h->ca_screen, c->ca_screen, sizeof h->ca_screen); memcpy(h->hom, c->h, sizeof h->hom);
}

/* the timeline (I21): when my dice and everything are at rest */
static void timeline(CnStage *st)
{
    CnStageHud *h = &st->hud;
    h->rest_ms = h->total_ms = 0;
    h->rolls = 0; h->roll_at_ms = 0;
    if (st->in.roll_at_ms == CN_STAGE_NO_ROLL || !st->nthrow) { st->in.roll_at_ms = CN_STAGE_NO_ROLL; return; }
    h->rolls = 1; h->roll_at_ms = st->in.roll_at_ms;
    /* THE SHAKE BEAT IS NEVER CUT OFF (I21): everything is at rest no sooner than the
     * kernel's SHAKE beat ends. Every throw outlasts it today (the shortest shake is
     * 1.5 s, the beat .76), so this floor cannot go red alone; it states the rule. */
    uint32_t total = st->in.roll_at_ms + CN_T_SHAKE;
    for (int j = 0; j < st->nthrow; j++) {
        const CnStageThrow *T = &st->thr[j];
        const uint32_t idle = st->in.roll_at_ms + T->delay_ms + ((uint32_t)T->idle_at * 1000u + CN_ROLL_HZ - 1) / CN_ROLL_HZ;
        if (T->t.seat == st->in.me) h->rest_ms = idle;
        if (idle > total) total = idle;
    }
    if (!h->rest_ms) h->rest_ms = st->in.roll_at_ms;
    h->total_ms = total;
}

/* THE CANVAS'S TOP (I20): the highest any body's picture reaches, as the eye
 * sees it, and 8 points more, never more than the study's pad (the glass's top,
 * untilted). Each body is bounded by a sphere round its centre (a cup's mouth
 * and crown are within sqrt(R^2 + h^2) of its mouth's centre, and a cup tipped
 * about its rim within one radius more; a die within .87 of its side of its
 * centre; a cup standing untipped is its mouth's and its crown's far points
 * exactly), and a sphere's picture reaches no higher than its top point pushed
 * up by its nearest edge. The light falls from the upper left, so no shadow
 * falls above the bodies that cast it. */
static int reach_pad(const CnStage *st, const CnObj *o, int n)
{
    if (!st->pad_max) return 0;
    const CnCam *c = &st->lay.cam;
    double top = 0;
    for (int i = 0; i < n; i++) {
        double e, cz;
        if (o[i].kind == CN_OBJ_CUP && !o[i].has_rot && o[i].tilt_angle == 0) {
            /* a cup standing untipped: its mouth's far point on the table, its crown's far point at h */
            const double rc = o[i].R * CN_CUP_RC, z = o[i].lift + o[i].h;
            const double ym = o[i].y - o[i].R, yc = c->eye_y + (o[i].y - rc - c->eye_y) * c->eye_z / (c->eye_z - z);
            if (ym < top) top = ym;
            if (yc < top) top = yc;
            continue;
        }
        if (o[i].kind == CN_OBJ_DIE) { e = o[i].d * .87; cz = o[i].lift; }
        else {
            e = cn_m_sqrt((double)o[i].R * o[i].R + (double)o[i].h * o[i].h) + (o[i].tilt_angle != 0 ? o[i].R : 0);
            cz = o[i].lift + o[i].tilt_lift;
        }
        const double y = o[i].y - o[i].tilt_back - e, z = cz + e;
        if (!(z < c->eye_z - 1)) return st->pad_max;
        const double yp = c->eye_y + (y - c->eye_y) * c->eye_z / (c->eye_z - z);
        if (yp < top) top = yp;
    }
    const int pad = (int)cn_m_ceil(-top + 8);
    return pad < st->pad_max ? pad : st->pad_max;
}

static void canvas_of(const CnStage *st, int pad, float out[4])
{
    out[0] = st->hud.board[0] - st->pad_x; out[1] = st->hud.board[1] - (float)pad;
    out[2] = (float)st->W; out[3] = (float)(st->H + pad);
}

const CnStageHud *cn_stage_begin(CnStage *st, const CnStageIn *in_)
{
    st->begun = 0; st->nobj = 0; st->nthrow = 0;
    memset(&st->hud, 0, sizeof st->hud);
    memset(&st->shot, 0, sizeof st->shot);
    if (!in_ || !valid(in_)) return 0;
    st->in = *in_;
    CnStageIn *in = &st->in;
    CnStageHud *h = &st->hud;
    CnLay *L = &st->lay;
    memset(L, 0, sizeof *L);
    memset(&st->lin, 0, sizeof st->lin);
    h->kind = in->kind; h->seats = in->seats; h->me = in->me; h->out_mask = in->out_mask;
    h->scale_still = clampf(in->scale, CN_STAGE_SCALE_MIN, CN_STAGE_SCALE_STILL);
    h->scale_roll = clampf(in->scale, CN_STAGE_SCALE_MIN, CN_STAGE_SCALE_ROLL);

    if (in->kind == CN_STAGE_BUBBLE) {
        in->roll_at_ms = CN_STAGE_NO_ROLL;
        in->w = CN_STAGE_BUBBLE_W; in->h = CN_STAGE_BUBBLE_H;
        cn_cam_make(&L->cam, CN_STAGE_BUBBLE_W, CN_STAGE_BUBBLE_H, CN_STAGE_BUBBLE_W / 2, 150, 1);
        st->W = CN_STAGE_BUBBLE_W; st->H = CN_STAGE_BUBBLE_H; st->pad = st->pad_max = 0; st->pad_x = 0;
        h->w = in->w; h->h = in->h;
        h->board[2] = h->canvas[2] = CN_STAGE_BUBBLE_W; h->board[3] = h->canvas[3] = CN_STAGE_BUBBLE_H;
        /* the plate under the row, drawn by the host, and each name under its cup */
        h->has_plate = 1; h->plate[0] = 70; h->plate[1] = 116; h->plate[2] = CN_LAY_PLATE_W; h->plate[3] = CN_LAY_PLATE_H;
    } else {
        CnLayIn *li = &st->lin;
        li->seats = in->seats; li->me = in->me; li->turn = in->turn; li->out_mask = in->out_mask;
        memcpy(li->dice, in->dice, sizeof li->dice);
        for (int k = 0; k < CN_STAGE_DICE; k++) { uint8_t f = in->faces[in->me * CN_STAGE_DICE + k]; li->my_faces[k] = f ? f : 1; }
        /* a reveal's shelf is one row (Next round), the roll's shelf's height */
        li->rolling = in->kind == CN_STAGE_REVEAL;
        if (in->kind == CN_STAGE_REVEAL) in->roll_at_ms = CN_STAGE_NO_ROLL;
        li->w = in->w; li->h = in->h; li->peek = 0; li->seed = in->seed;
        if (!cn_lay_make(li, L)) return 0;
        st->W = (int)(L->board_w + 2 * L->pad_x + .5f); st->H = (int)(L->board_h + L->pad_below + .5f);
        st->pad = st->pad_max = (int)(L->pad + .5f); st->pad_x = L->pad_x;
        h->w = L->w; h->h = L->h;
        h->board[0] = L->board_x; h->board[1] = L->board_y; h->board[2] = L->board_w; h->board[3] = L->board_h;
        h->short_board = L->short_board; h->has_plate = L->has_plate; h->has_shelf = L->has_shelf;
        memcpy(h->plate, L->plate, sizeof h->plate); memcpy(h->shelf, L->shelf, sizeof h->shelf);
        h->my_band[0] = L->board_x + L->my_band[0]; h->my_band[1] = L->board_y + L->my_band[1];
        h->my_band[2] = L->my_band[2]; h->my_band[3] = L->my_band[3];
        h->cup_r = L->cup_r; h->my_r = L->my_r;
        h->pad = L->pad; h->pad_x = L->pad_x; h->pad_below = L->pad_below;
        for (int s = 0; s < in->seats; s++) {
            h->cup_x[s] = L->board_x + L->cup_x[s]; h->cup_y[s] = L->board_y + L->cup_y[s];
            h->name_x[s] = L->board_x + L->name_x[s]; h->name_y[s] = L->board_y + L->name_y[s];
            h->name_how[s] = L->name_how[s];
            h->die_d[s] = L->die_g[s]; h->brass_r[s] = L->brass_r[s];
        }
    }
    hud_cam(h, &L->cam);
    st->eye[0] = L->cam.eye_x + st->pad_x; st->eye[1] = L->cam.eye_y; st->eye[2] = L->cam.eye_z;

    if (!bodies(st) || !throws(st)) return 0;
    paint(st);
    if (in->kind == CN_STAGE_BUBBLE)
        for (int i = 0; i < in->seats; i++) {
            const CnObj *o = &st->base[i];
            h->cup_x[i] = o->home_x; h->cup_y[i] = o->home_y; h->cup_r = o->R;
            h->name_x[i] = o->home_x; h->name_y[i] = 70 + CN_LAY_NAME_UP; h->name_how[i] = CN_NAME_BOX;
        }

    /* the crowns this table wears; the set changed, so the textures go up again */
    CnStageCrown old[CN_STAGE_CROWNS];
    const int nold = st->ncrown;
    memcpy(old, st->crown, sizeof old);
    st->ncrown = 0;
    for (int i = 0; i < st->nobj; i++) {
        const CnObj *o = &st->base[i];
        if (o->kind != CN_OBJ_CUP || crown_of(st, o) >= 0 || st->ncrown >= CN_STAGE_CROWNS) continue;
        CnStageCrown *c = &st->crown[st->ncrown++];
        c->seed = o->tex_seed; c->count = (int16_t)(o->out ? -1 : o->value); c->out = o->out; c->big = o->crown_big; c->id = -1;
    }
    int same = nold == st->ncrown;
    for (int i = 0; i < st->ncrown && same; i++)
        same = old[i].seed == st->crown[i].seed && old[i].count == st->crown[i].count && old[i].out == st->crown[i].out && old[i].big == st->crown[i].big;
    if (same) for (int i = 0; i < st->ncrown; i++) st->crown[i].id = old[i].id;
    else st->uploaded = 0;

    /* at rest: where my dice lie decides the peek, and each seat's dice its cup's lift */
    CnObj *rest = st->obj;
    posed(st, CN_STAGE_NO_ROLL - 1, rest);
    float dy[CN_STAGE_SEATS][CN_STAGE_DICE], dd[CN_STAGE_SEATS][CN_STAGE_DICE];
    int nd[CN_STAGE_SEATS] = { 0 };
    for (int i = 0; i < st->nobj; i++) {
        const CnObj *o = &rest[i];
        if (o->kind != CN_OBJ_DIE) continue;
        const int s = o->seat, k = nd[s]++;
        dy[s][k] = o->y; dd[s][k] = o->d;
        float gx, gy;
        glass(st, &L->cam, o->x, o->y, o->lift, &gx, &gy);
        h->die_x[s * CN_STAGE_DICE + k] = gx; h->die_y[s * CN_STAGE_DICE + k] = gy;
    }
    memset(st->lift_angle, 0, sizeof st->lift_angle);
    for (int i = 0; i < st->nobj && in->kind != CN_STAGE_BUBBLE; i++) {
        const CnObj *o = &rest[i];
        if (o->kind != CN_OBJ_CUP || o->out || !nd[o->seat]) continue;
        st->lift_angle[o->seat] = cn_cam_peek_angle(&L->cam, o->R, o->home_y, dy[o->seat], dd[o->seat], nd[o->seat]);
    }
    h->peek_target = st->lift_angle[in->me];
    /* my cup's picture on the glass, standing: the box round its mouth and crown */
    if (in->kind != CN_STAGE_BUBBLE) {
        const float R = L->my_r, rc = R * (float)CN_CUP_RC, hh = R * CN_CUP_TALL, cx = L->cup_x[in->me], cy = L->cup_y[in->me];
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        for (int k = 0; k < 2 * CN_CUP_SEGS; k++) {
            const int top = k >= CN_CUP_SEGS;
            const double a = (k % CN_CUP_SEGS) * 2 * CN_PI / CN_CUP_SEGS;
            const float r = top ? rc : R, gxp = cx + r * (float)cn_m_cos(a), gyp = cy + r * (float)cn_m_sin(a);
            float gx, gy;
            glass(st, &L->cam, gxp, gyp, top ? hh : 0, &gx, &gy);
            if (gx < x0) x0 = gx;
            if (gx > x1) x1 = gx;
            if (gy < y0) y0 = gy;
            if (gy > y1) y1 = gy;
        }
        h->hit[0] = (x0 + x1) / 2; h->hit[1] = (y0 + y1) / 2; h->hit[2] = (x1 - x0) / 2; h->hit[3] = (y1 - y0) / 2;
    }
    timeline(st);
    /* the still picture's place: the bodies at rest, the cups down */
    st->begun = 1;
    { int n; const CnObj *o = cn_stage_objects(st, CN_STAGE_NO_ROLL - 1, 0, 0, &n); canvas_of(st, reach_pad(st, o, n), h->canvas); }
    h->ok = 1;
    return h;
}

/* ---- the clock -------------------------------------------------------------------------- */

const CnObj *cn_stage_objects(CnStage *st, uint32_t t_ms, float peek, float lift, int *n)
{
    if (n) *n = 0;
    if (!st->begun) return 0;
    posed(st, t_ms, st->obj);
    peek = clampf(peek, 0, 1);
    lift = st->in.kind == CN_STAGE_REVEAL ? clampf(lift, 0, 1) : 0;
    for (int i = 0; i < st->nobj; i++) {
        CnObj *o = &st->obj[i];
        if (o->kind != CN_OBJ_CUP || o->out || st->in.kind == CN_STAGE_BUBBLE) continue;
        /* a cup in its throw is the throw's; standing, mine tips for the peek and every one for the lift */
        if (o->has_rot) continue;
        const float full = st->lift_angle[o->seat];
        float a = full * lift;
        if (o->mine && full * peek > a) a = full * peek;
        if (!(a > 0)) continue;
        const CnPeek p = cn_cam_peek_tilt(o->R, a, full);
        o->tilt_angle = p.angle; o->tilt_hinge_y = p.hinge_y; o->tilt_back = p.back; o->tilt_lift = p.lift;
    }
    if (n) *n = st->nobj;
    return st->obj;
}

static int rolling_at(const CnStage *st, uint32_t t)
{
    return st->hud.rolls && t >= st->hud.roll_at_ms && t < st->hud.total_ms;
}

int cn_stage_done(const CnStage *st, uint32_t t_ms)
{
    return st->begun && (!st->hud.rolls || t_ms >= st->hud.total_ms);
}

uint32_t cn_stage_total_ms(const CnStage *st) { return st->begun && st->hud.rolls ? st->hud.total_ms : 0; }

/* ---- the frame ----------------------------------------------------------------------------- */

int cn_stage_prepare(CnStage *st, uint32_t t_ms, float peek, float lift)
{
    memset(&st->shot, 0, sizeof st->shot);
    st->shot.t_ms = t_ms;
    if (!st->begun || !textures(st)) return 0;
    int n;
    const CnObj *obj = cn_stage_objects(st, t_ms, peek, lift, &n);
    int nv = 0, nf = 0;
    for (int i = 0; i < n; i++) { nv += st->mesh[st->obj_mesh[i]].ncorner; nf += st->mesh[st->obj_mesh[i]].ntri; }
    const int moving = rolling_at(st, t_ms);
    st->pad = reach_pad(st, obj, n);
    /* the scale asked for, then each smaller one, until the frame fits */
    static const float steps[] = { CN_STAGE_SCALE_STILL, CN_STAGE_SCALE_ROLL, CN_STAGE_SCALE_MIN };
    const float want = moving ? st->hud.scale_roll : st->hud.scale_still;
    float used = 0;
    const double ln = cn_m_sqrt(.45 * .45 + .55 * .55 + 1);
    const float lx = (float)(-.45 / ln), ly = (float)(-.55 / ln), lz = (float)(1 / ln);
    for (int k = -1; k < 3 && !used; k++) {
        const float s = k < 0 ? want : steps[k];
        if (k >= 0 && !(s < want)) continue;
        if (cn_scene_begin(st->W, st->H, st->pad, s, st->eye[0], st->eye[1], st->eye[2], lx, ly, lz,
                           CN_STAGE_SHADOW_RES, CN_STAGE_SHADOW_DARK, nv, nf)) used = s;
    }
    if (!used) return 0;
    float *V = cn_scene_verts(), *F = cn_scene_faces();
    int vb = 0, fb = 0;
    for (int i = 0; i < n; i++) {
        const CnObj *o = &obj[i];
        float occ[5];
        cn_geom_occluder(o, st->pad_x, occ);
        cn_scene_occluder(occ[0], occ[1], occ[2], occ[3], occ[4]);
        int ids[CN_TEX_SLOTS] = { st->tex_side, -1, st->tex_inner, st->tex_floor, st->tex_die };
        if (o->kind == CN_OBJ_CUP) { int c = crown_of(st, o); ids[CN_TEX_CUP_CROWN] = c >= 0 ? st->crown[c].id : -1; }
        const CnMesh *m = &st->mesh[st->obj_mesh[i]];
        cn_geom_emit(m, o, st->pad_x, ids, V, vb, F, fb);
        vb += m->ncorner; fb += m->ntri;
    }
    if (cn_scene_prepare(vb, fb) < 0) return 0;
    st->shot.ok = 1;
    st->shot.w = (uint16_t)cn_scene_fb_w(); st->shot.h = (uint16_t)cn_scene_fb_h();
    st->shot.scale = used;
    canvas_of(st, st->pad, st->shot.canvas);
    st->shot.rolling = (uint8_t)moving;
    st->shot.done = (uint8_t)cn_stage_done(st, t_ms);
    return 1;
}

void cn_stage_band(CnStage *st, int pass, int band, int nbands)
{
    if (st->shot.ok) cn_scene_band(pass, band, nbands);
}

const uint8_t *cn_stage_finish(CnStage *st) { return st->shot.ok ? cn_scene_fb() : 0; }

const uint8_t *cn_stage_frame(CnStage *st, uint32_t t_ms, float peek, float lift, int *w, int *h)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (!cn_stage_prepare(st, t_ms, peek, lift)) return 0;
    for (int pass = 0; pass < CN_STAGE_PASSES; pass++) cn_stage_band(st, pass, 0, 1);
    if (w) *w = st->shot.w;
    if (h) *h = st->shot.h;
    return cn_stage_finish(st);
}

const CnStageShot *cn_stage_shot(const CnStage *st) { return &st->shot; }
