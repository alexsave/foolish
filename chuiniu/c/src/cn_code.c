/* Chui Niu - the body coder. See cn_code.h. */
#include "cn_code.h"
#include "../../../shared/c/mixrad.h"
#include <string.h>

#define MENU_MAX (CN_MAX_DICE * CN_BID_FACES + 1)

/* The walker's two faces: encoding, each index comes from the history and
 * is written down; decoding, it comes out of the number. */
typedef struct {
    int decode;
    const CnGame *src;
    int nd;
    uint8_t base[CN_MAX_MOVES];
    uint8_t idx[CN_MAX_MOVES];
    uint8_t v[CN_CODE_MAX];
    int len;
} Coder;

/* THE ONE MOVE, in both directions. 1, or 0 when the body or the history
 * does not say a move. */
static int move(CnGame *g, Coder *c, int i)
{
    CnMove menu[MENU_MAX];
    int nm = cn_legal(g, menu, MENU_MAX);
    if (nm <= 0 || nm > MENU_MAX) return 0;
    int k = 0;
    if (c->decode) {
        if (nm > 1) k = (int)mixrad_div_mod(c->v, &c->len, (uint32_t)nm);
    } else {
        CnMove want = c->src->hist[i];
        k = -1;
        for (int j = 0; j < nm; j++)
            if (menu[j].q == want.q && menu[j].f == want.f) { k = j; break; }
        if (k < 0) return 0;
        if (nm > 1) {
            c->base[c->nd] = (uint8_t)nm;
            c->idx[c->nd] = (uint8_t)k;
            c->nd++;
        }
    }
    return cn_apply(g, g->turn, menu[k]);
}

int cn_code_encode(const CnGame *g, uint8_t *buf, int cap)
{
    static Coder c;
    static CnGame r;
    if (!g) return -1;
    memset(&c, 0, sizeof c);
    c.src = g;
    if (!cn_new(&r, g->seed, g->n)) return -1;
    for (int i = 0; i < g->hist_n; i++)
        if (!move(&r, &c, i)) return -1;
    int len = 1;
    c.v[0] = 1;
    for (int d = c.nd - 1; d >= 0; d--)
        if (!mixrad_mul_add(c.v, &len, CN_CODE_MAX, c.base[d], c.idx[d])) return -1;
    if (len > cap) return -2;
    memcpy(buf, c.v, (size_t)len);
    return len;
}

int cn_code_decode(CnGame *out, const uint8_t seed[32], int n, int moves, const uint8_t *buf, int len)
{
    static Coder c;
    if (!out || !buf || len < 1 || len > CN_CODE_MAX || buf[len - 1] == 0) return 0;
    if (moves < 0 || moves > CN_MAX_MOVES) return 0;
    memset(&c, 0, sizeof c);
    c.decode = 1;
    memcpy(c.v, buf, (size_t)len);
    c.len = len;
    if (!cn_new(out, seed, n)) return 0;
    for (int i = 0; i < moves; i++)
        if (!move(out, &c, i)) return 0;
    return c.len == 1 && c.v[0] == 1;     /* the sentinel, and nothing else */
}
