/* Tallybones - the body coder and the resident replay. See tb_code.h. */
#include "tb_code.h"
#include "tb_internal.h"
#include "../../../shared/c/mixrad.h"
#include <string.h>

enum { W_ENCODE = 0, W_REPLAY, W_DECODE };

typedef struct {
    int mode, derive;
    const TbMove *src;
    int src_n, pos;
    /* encode: the digits so far */
    int nd;
    uint8_t base[TB_MAX_BUBBLES], idx[TB_MAX_BUBBLES];
    /* decode: the number being read */
    uint8_t v[TB_CODE_MAX];
    int len;
    /* the running body: S and P (tb_code.h), and S + P */
    uint8_t s[TB_CODE_MAX + 1], p[TB_CODE_MAX + 1], body[TB_CODE_MAX + 1];
    int sl, pl, bl;
} Walk;

/* ---- the running body: little-endian byte bignums ------------------------------ */

/* a += b * k (k < 256). 1, or 0 past `cap`. */
static int add_scaled(uint8_t *a, int *al, const uint8_t *b, int bl, uint32_t k, int cap)
{
    const int an = *al;
    uint32_t carry = 0;
    int i = 0;
    for (; i < an || i < bl || carry; i++) {
        if (i >= cap) return 0;
        uint32_t x = (i < an ? a[i] : 0u) + (i < bl ? b[i] * k : 0u) + carry;
        a[i] = (uint8_t)x;
        carry = x >> 8;
    }
    while (i > 1 && a[i - 1] == 0) i--;
    *al = i;
    return 1;
}

static void body_reset(Walk *w)
{
    w->s[0] = 0; w->sl = 1;
    w->p[0] = 1; w->pl = 1;
    w->body[0] = 1; w->bl = 1;
}

/* One digit into the body: S += P * i; P *= b; body = S + P. */
static int body_push(Walk *w, int base, int i)
{
    if (base <= 1) return 1;
    if (!add_scaled(w->s, &w->sl, w->p, w->pl, (uint32_t)i, TB_CODE_MAX)) return 0;
    if (!mixrad_mul_add(w->p, &w->pl, TB_CODE_MAX, (uint32_t)base, 0)) return 0;
    memcpy(w->body, w->s, (size_t)w->sl);
    w->bl = w->sl;
    return add_scaled(w->body, &w->bl, w->p, w->pl, 1, TB_CODE_MAX);
}

/* ---- the one bubble, in every direction ------------------------------------------ */

static int bubble(TbGame *g, Walk *w, TbSink *k)
{
    TbMove menu[TB_MENU_MAX];
    const int nm = tb_menu(g, menu, TB_MENU_MAX);
    if (nm <= 0 || nm > TB_MENU_MAX || g->hist_n >= TB_HIST_CAP) return 0;
    int i = -1;
    if (w->mode == W_DECODE) {
        i = nm > 1 ? (int)mixrad_div_mod(w->v, &w->len, (uint32_t)nm) : 0;
    } else {
        if (w->pos >= w->src_n) return 0;
        TbMove m = w->src[w->pos++];
        for (int j = 0; j < nm; j++)
            if (!memcmp(&menu[j], &m, sizeof m)) { i = j; break; }
        if (i < 0) return 0;
        if (w->mode == W_ENCODE && nm > 1) {
            w->base[w->nd] = (uint8_t)nm;
            w->idx[w->nd] = (uint8_t)i;
            w->nd++;
        }
    }
    if (w->derive && !body_push(w, nm, i)) return 0;
    if (k) k->bubble = (uint16_t)(g->hist_n + 1);
    if (!tb__step(g, menu[i], w->derive ? w->body : 0, w->bl, k)) return 0;
    g->hist[g->hist_n++] = menu[i];
    return 1;
}

static Walk W;              /* ~2 KB; the kernel is single-threaded */

static int start(TbGame *g, const uint8_t seed[32], int n, int starter, int mode, int derive, TbSink *k)
{
    memset(&W, 0, sizeof W);
    W.mode = mode;
    W.derive = derive;
    body_reset(&W);
    return tb__new(g, seed, n, starter, derive ? W.body : 0, W.bl, k);
}

static int fold(uint8_t *buf, int cap)
{
    uint8_t v[TB_CODE_MAX];
    int len = 1;
    v[0] = 1;
    for (int d = W.nd - 1; d >= 0; d--)
        if (!mixrad_mul_add(v, &len, TB_CODE_MAX, W.base[d], W.idx[d])) return -1;
    if (len > cap) return -2;
    memcpy(buf, v, (size_t)len);
    return len;
}

static int encode_moves(const TbGame *g, const TbMove *moves, int k, uint8_t *buf, int cap)
{
    static TbGame r;
    if (!start(&r, g->seed, g->n, g->starter, W_ENCODE, 0, 0)) return -1;
    W.src = moves;
    W.src_n = k;
    for (int b = 0; b < k; b++)
        if (!bubble(&r, &W, 0)) return -1;
    return fold(buf, cap);
}

int tb_code_encode(const TbGame *g, uint8_t *buf, int cap)
{
    static TbMove moves[TB_HIST_CAP + 1];
    if (!g || g->hist_n > TB_HIST_CAP) return -1;
    memcpy(moves, g->hist, sizeof(TbMove) * g->hist_n);
    int k = g->hist_n;
    if (g->draft) moves[k++] = g->pending;
    return encode_moves(g, moves, k, buf, cap);
}

int tb_code_body(const TbGame *g, int k, uint8_t *buf, int cap)
{
    if (!g || k < 0 || k > g->hist_n) return -1;
    return encode_moves(g, g->hist, k, buf, cap);
}

int tb_code_decode(TbGame *out, const uint8_t seed[32], int n, int starter, int bubbles,
                   const uint8_t *buf, int len, int derive)
{
    if (!out || !buf || len < 1 || len > TB_CODE_MAX || buf[len - 1] == 0) return 0;
    if (bubbles < 0 || bubbles > TB_MAX_BUBBLES) return 0;
    if (!start(out, seed, n, starter, W_DECODE, derive != 0, 0)) return 0;
    memcpy(W.v, buf, (size_t)len);
    W.len = len;
    for (int b = 0; b < bubbles; b++)
        if (!bubble(out, &W, 0)) return 0;
    return W.len == 1 && W.v[0] == 1;
}

int tb__replay(TbGame *out, const uint8_t seed[32], int n, int starter, const TbMove *moves, int k,
               TbSink *sink)
{
    if (!out || k < 0 || k > TB_HIST_CAP) return 0;
    if (!start(out, seed, n, starter, W_REPLAY, 1, sink)) return 0;
    W.src = moves;
    W.src_n = k;
    for (int b = 0; b < k; b++)
        if (!bubble(out, &W, sink)) return 0;
    return 1;
}

int tb_replay(TbGame *out, const uint8_t seed[32], int n, int starter, const TbMove *moves, int k)
{
    static TbGame r;
    if (!tb__replay(&r, seed, n, starter, moves, k, 0)) return 0;
    *out = r;
    return 1;
}
