#include "uttt_api.h"
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_bots.h"
#include "../src/uttt_draw.h"
#include <string.h>

/* The resident game, and the buffers the display list is built into. Sized
 * for a full board with every mark drawn, MEASURED rather than guessed:
 * `./build/uttt_render 81` prints the worst case, which today is 25,624
 * polygons from 166,556 points. Measure it again after touching the pen -
 * raising the flattening from 6 segments a curve to the document's 14 grew
 * this by three quarters, and a display list that runs out does not fail, it
 * quietly stops drawing. Everything below is the measurement plus half. */
#define MAX_PT   260000
#define MAX_POLY  40000

static struct {
    UtttGame  g;
    int32_t   seed;
    uint64_t  rs;
    int       overflow;
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

int uti_active(void)
{
    if (S.g.over) return -1;
    if (S.g.forced != UTTT_ANY && S.g.block[S.g.forced] == UTTT_OPEN)
        return S.g.forced;
    return 9;
}

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
    S.overflow = uttt_draw_board(&S.dl, &S.g, &o) != 0;
    return publish();
}

/* A display list that runs out of room does not fail - it stops appending,
 * and the board comes back with a few marks missing. Nothing on screen says
 * so, which is why it gets its own question. */
int uti_draw_overflow(void) { return S.overflow; }

int uti_draw_one(int mv, float t)
{
    if (mv < 0 || mv > 80) return 0;
    uttt_dl_reset(&S.dl);
    uttt_draw_cell(&S.dl, S.g.cell[mv] ? S.g.cell[mv] : S.g.turn,
                   mv, S.seed, t);
    return publish();
}

int uti_draw_mark(int mark, int32_t seed)
{
    uttt_dl_reset(&S.dl);
    uttt_draw_mark(&S.dl, mark, seed ? seed : 1, 0.f);
    return publish();
}

int uti_draw_rulebook(float w, float h)
{
    uttt_dl_reset(&S.dl);
    uttt_draw_rulebook(&S.dl, w, h);
    return publish();
}

void uti_paper(uint8_t *rgba, int w, int h) { uttt_paper(rgba, w, h); }

/* ----------------------------------------------------------- the bubble */
void uti_bubble_size(float *w, float *h)
{
    UtttBubble b = uttt_bubble();
    if (w) *w = b.w;
    if (h) *h = b.h;
}

void uti_bubble_board(float *x, float *y, float *side)
{
    UtttBubble b = uttt_bubble();
    if (x)    *x    = b.board.x;
    if (y)    *y    = b.board.y;
    if (side) *side = b.board.w;
}

void uti_bubble_text(float *x, float *y, float *w, float *h)
{
    UtttBubble b = uttt_bubble();
    if (x) *x = b.text.x;
    if (y) *y = b.text.y;
    if (w) *w = b.text.w;
    if (h) *h = b.text.h;
}

float uti_bubble_type(int line)
{
    UtttBubble b = uttt_bubble();
    return line ? b.place_pt : b.headline_pt;
}

uint32_t uti_bubble_ink(int line)
{
    UtttBubble b = uttt_bubble();
    return line ? b.place_rgba : b.headline_rgba;
}

float uti_bubble_lead(void) { return uttt_bubble().lead; }

const char *uti_place_name(int block, int spoken)
{
    return uttt_place_name(block, spoken);
}

const float    *uti_points(void)     { return (const float *)S.dl.pt; }
int             uti_point_count(void){ return S.dl.n_pt; }
const int32_t  *uti_poly_first(void) { return S.first; }
const int32_t  *uti_poly_n(void)     { return S.n; }
const uint32_t *uti_poly_rgba(void)  { return S.rgba; }
