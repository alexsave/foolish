#include "uttt_api.h"
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_bots.h"
#include "../src/uttt_draw.h"
#include <string.h>

/* The resident game, and the buffers the display list is built into. Sized
 * for a full board with every mark drawn, measured rather than guessed:
 * a finished 50-ply game builds 14,064 polygons from 91,416 points. */
#define MAX_PT   140000
#define MAX_POLY  24000

static struct {
    UtttGame  g;
    int32_t   seed;
    uint64_t  rs;
    UtttDL    dl;
    UtttPt    pt[MAX_PT];
    UtttPoly  poly[MAX_POLY];
    int32_t   first[MAX_POLY];
    int32_t   n[MAX_POLY];
    uint32_t  rgba[MAX_POLY];
} S;

void uti_new(int32_t seed)
{
    uttt_init(&S.g);
    S.seed = seed ? seed : 1;
    S.rs = (uint64_t)S.seed * 2654435761u | 1u;
    uttt_dl_init(&S.dl, S.pt, MAX_PT, S.poly, MAX_POLY);
}

int uti_play(int mv)
{
    if (mv < 0 || mv > 80) return 0;
    return uttt_play(&S.g, (uint8_t)mv);
}
int uti_legal(uint8_t *out)   { return uttt_legal(&S.g, out); }
int uti_over(void)            { return S.g.over; }
int uti_turn(void)            { return S.g.turn; }
int uti_forced(void)          { return S.g.forced; }
int uti_n_plies(void)         { return S.g.n_plies; }
int uti_move_at(int i)        { return (i >= 0 && i < S.g.n_plies) ? S.g.move[i] : -1; }
int uti_block(int b)          { return (b >= 0 && b < 9) ? S.g.block[b] : -1; }
int uti_cell(int i)           { return (i >= 0 && i < 81) ? S.g.cell[i] : -1; }

int uti_encode(uint8_t *out, int cap) { return uttt_encode(&S.g, out, (size_t)cap); }

int uti_decode(const uint8_t *buf, int n, int32_t seed)
{
    UtttGame tmp;
    if (!uttt_decode(&tmp, buf, (size_t)n)) return 0;
    S.g = tmp;
    S.seed = seed ? seed : 1;
    S.rs = (uint64_t)S.seed * 2654435761u | 1u;
    return 1;
}

int uti_bot_move(int budget)
{
    return uttt_bot_move(BOT_NIB, &S.g, budget > 0 ? budget : 60, &S.rs);
}

static int publish(void)
{
    for (int i = 0; i < S.dl.n_poly; i++) {
        S.first[i] = S.dl.poly[i].first;
        S.n[i]     = S.dl.poly[i].n;
        S.rgba[i]  = S.dl.poly[i].rgba;
    }
    return S.dl.n_poly;
}

int uti_draw(int active, int last, float mark_t, float meta_t)
{
    uttt_dl_reset(&S.dl);
    UtttDrawOpts o = uttt_draw_opts(S.seed);
    o.active = active; o.last = last;
    o.mark_t = mark_t; o.meta_t = meta_t;
    uttt_draw_board(&S.dl, &S.g, &o);
    return publish();
}

int uti_draw_mark(int mark, int32_t seed)
{
    uttt_dl_reset(&S.dl);
    uttt_draw_mark(&S.dl, mark, seed ? seed : 1, 0.f);
    return publish();
}

const float    *uti_points(void)     { return (const float *)S.dl.pt; }
int             uti_point_count(void){ return S.dl.n_pt; }
const int32_t  *uti_poly_first(void) { return S.first; }
const int32_t  *uti_poly_n(void)     { return S.n; }
const uint32_t *uti_poly_rgba(void)  { return S.rgba; }
