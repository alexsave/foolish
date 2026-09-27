/* Pick 'Em Up - the body coder. See pk_code.h. */
#include "pk_code.h"
#include "pk_internal.h"
#include "../../../shared/c/mixrad.h"
#include <string.h>

/* The walker's two faces. Encoding, each decision's index comes from the
 * history (`want`) and is written down; decoding, it comes out of the number.
 * Everything else - which decisions there are and what each one's menu is - is
 * the same code in both directions. */
typedef struct {
    int decode;
    int bad;
    /* encode: the history being said, and the digits so far */
    const PkGame *src;
    int pos;
    int nd;
    uint8_t base[PK_CODE_MAX_DIGITS];
    uint8_t idx[PK_CODE_MAX_DIGITS];
    /* decode: the number being read */
    uint8_t v[PK_CODE_MAX];
    int len;
} Coder;

/* One decision with `base` options. A single option is not a digit. Encoding,
 * `want` must be on the menu or the history is not one the menus can say. */
static int pick(Coder *c, int base, int want)
{
    if (c->decode) {
        if (base <= 1) return 0;
        return (int)mixrad_div_mod(c->v, &c->len, (uint32_t)base);
    }
    if (want < 0 || want >= base) { c->bad = 1; return 0; }
    if (base > 1) {
        if (c->nd >= PK_CODE_MAX_DIGITS || base > 255) { c->bad = 1; return 0; }
        c->base[c->nd] = (uint8_t)base;
        c->idx[c->nd] = (uint8_t)want;
        c->nd++;
    }
    return want;
}

/* A forced decision: encoding, the history must agree with it. */
static int forced(Coder *c, int value, int want)
{
    if (!c->decode && value != want) c->bad = 1;
    return value;
}

static int may_say(const PkGame *g, int s)
{
    PkAct a = { PK_A_SAY_IT, 0, 0, 0 };
    return pk_is_legal(g, s, a);
}

/* The seats `s` may catch, ascending. */
static int targets(const PkGame *g, int s, uint8_t *out)
{
    int k = 0;
    for (int t = 0; t < g->n; t++) {
        PkAct a = { PK_A_CALL_OUT, (uint8_t)t, 0, 0 };
        if (pk_is_legal(g, s, a)) out[k++] = (uint8_t)t;
    }
    return k;
}

static int index_of(const uint8_t *list, int n, int x)
{
    for (int i = 0; i < n; i++) if (list[i] == x) return i;
    return -1;
}

static int is_turn_action(int kind)
{
    return kind == PK_A_DRAW || kind == PK_A_PLAY || kind == PK_A_PASS;
}

/* THE ONE BUBBLE, in both directions. 1, or 0 when the body or the history
 * does not say a bubble. */
static int bubble(PkGame *g, Coder *c)
{
    if (g->over || g->b_open) return 0;
    PkAct rec = { 0, 0, 0, 0 };
    if (!c->decode) {
        if (c->pos >= c->src->hist_n) return 0;
        rec = c->src->hist[c->pos++];
        if (rec.kind != PK_A_BUBBLE || !(rec.b & PK_BR_SEALED)) return 0;
    }
    const int turn = g->turn;

    /* S: the turn seat, or one of the seats with something to send */
    uint8_t others[PK_MAX_SEATS], tg[PK_MAX_SEATS];
    int no = 0;
    for (int s = 0; s < g->n; s++)
        if (s != turn && (may_say(g, s) || targets(g, s, tg) > 0)) others[no++] = (uint8_t)s;
    int want_out = rec.a != turn;
    int out = no ? pick(c, 2, want_out) : forced(c, 0, want_out);
    int sender = turn;
    if (out) sender = others[pick(c, no, index_of(others, no, rec.a))];
    if (c->bad) return 0;
    const int in_turn = sender == turn;

    /* Y and C */
    const int nt = targets(g, sender, tg);
    const int want_y = (rec.b & PK_BR_SAID) != 0, want_c = (rec.b & PK_BR_CALL) != 0;
    int y;
    if (!may_say(g, sender))  y = forced(c, 0, want_y);
    else if (in_turn || nt)   y = pick(c, 2, want_y);
    else                      y = forced(c, 1, want_y);
    int call;
    if (!in_turn && !y)       call = forced(c, 1, want_c);
    else if (!nt)             call = forced(c, 0, want_c);
    else                      call = pick(c, 2, want_c);
    int target = PK_SEAT_NONE;
    if (call) target = tg[pick(c, nt, index_of(tg, nt, rec.c))];
    if (c->bad) return 0;

    /* T */
    int want_t = !c->decode && c->pos < c->src->hist_n && is_turn_action(c->src->hist[c->pos].kind);
    int t;
    if (!in_turn)             t = forced(c, 0, want_t);
    else if (y || call)       t = pick(c, 2, want_t);
    else                      t = forced(c, 1, want_t);
    if (c->bad) return 0;

    if (y) {
        PkAct a = { PK_A_SAY_IT, 0, 0, 0 };
        if (!pk_apply(g, sender, a)) return 0;
    }
    if (call) {
        PkAct a = { PK_A_CALL_OUT, (uint8_t)target, 0, 0 };
        if (!pk_apply(g, sender, a)) return 0;
    }

    while (t) {
        PkAct m[PK_HAND_CAP * 4 + 2];
        int nm = pk_legal_turn(g, sender, m, (int)(sizeof m / sizeof m[0]));
        if (nm <= 0) return 0;
        int want = -1;
        if (!c->decode) {
            if (c->pos >= c->src->hist_n) return 0;
            PkAct r = c->src->hist[c->pos++];
            for (int i = 0; i < nm; i++)
                if (m[i].kind == r.kind && m[i].a == r.a && m[i].b == r.b) { want = i; break; }
        }
        int i = pick(c, nm, want);
        if (c->bad || i >= nm || !pk_apply(g, sender, m[i])) return 0;
        if (m[i].kind == PK_A_DRAW) {
            if (g->over) break;          /* the long-game stop, mid-turn */
            continue;
        }
        if (pk_turn_ended(g)) break;
        /* K: the turn came straight back (D7) */
        int want_k = !c->decode && c->pos < c->src->hist_n && c->src->hist[c->pos].kind == PK_A_CONTINUE;
        if (!pick(c, 2, want_k)) break;
        if (c->bad) return 0;
        if (!c->decode) c->pos++;
    }
    return pk_seal(g);
}

int pk_code_encode(const PkGame *g, uint8_t *buf, int cap)
{
    if (!g || g->b_open) return -1;
    static Coder c;              /* ~20 KB; the kernel is single-threaded */
    static PkGame r;
    memset(&c, 0, sizeof c);
    c.src = g;
    if (!pk__new(&r, g->seed, g->n, g->starter == PK_SEAT_NONE ? -1 : g->starter, 0)) return -1;
    for (int b = 0; b < g->bubbles; b++)
        if (!bubble(&r, &c) || c.bad) return -1;
    /* the walk said every record, and nothing else */
    if (c.pos != g->hist_n || r.hist_n != g->hist_n
        || memcmp(r.hist, g->hist, sizeof(PkAct) * g->hist_n) != 0)
        return -1;

    int len = 1;
    c.v[0] = 1;
    for (int d = c.nd - 1; d >= 0; d--)
        if (!mixrad_mul_add(c.v, &len, PK_CODE_MAX, c.base[d], c.idx[d])) return -1;
    if (len > cap) return -2;
    memcpy(buf, c.v, (size_t)len);
    return len;
}

int pk_code_decode(PkGame *out, const uint8_t seed[32], int n, int starter, int bubbles,
                   const uint8_t *buf, int len)
{
    /* minimal: at least the sentinel, and never a zero top byte */
    if (!out || !buf || len < 1 || len > PK_CODE_MAX || buf[len - 1] == 0) return 0;
    if (bubbles < 0 || bubbles > PK_MAX_BUBBLES) return 0;
    static Coder c;
    memset(&c, 0, sizeof c);
    c.decode = 1;
    memcpy(c.v, buf, (size_t)len);
    c.len = len;
    if (!pk__new(out, seed, n, starter, 0)) return 0;
    for (int b = 0; b < bubbles; b++)
        if (!bubble(out, &c)) return 0;
    return c.len == 1 && c.v[0] == 1;     /* the sentinel, and nothing else */
}
