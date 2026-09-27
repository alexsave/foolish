/* Pick 'Em Up - the bot's belief: one pass over the public events, and the
 * sampler that deals a consistent world from it. See pk_belief.h for what is
 * deducible and why; the shape is octogen's og_build_belief / og_sample_world. */
#include "pk_belief.h"
#include "pk_plan.h"
#include <string.h>

/* ---- the void predicate ------------------------------------------------------ */

static int forbids(const PkVoid *v, uint8_t c)
{
    if (c < 96 && (v->suits >> pk_suit(c) & 1u)) return 1;
    return (v->ranks >> pk_rank(c) & 1u) != 0;
}

/* "Holds nothing that plays": the live suit, the top's rank and both wilds. */
static PkVoid no_match(int live, uint8_t top, int k)
{
    PkVoid v;
    memset(&v, 0, sizeof v);
    v.suits = (uint8_t)(1u << live);
    v.ranks = (uint16_t)((1u << pk_rank(top)) | (1u << PK_R_WILD) | (1u << PK_R_WILD4));
    v.k = (uint8_t)k;
    return v;
}

static void push_void(PkVoid *list, uint8_t *n, PkVoid v)
{
    if (*n == PK_BELIEF_VOIDS) {                 /* the oldest binds the least */
        memmove(list, list + 1, (PK_BELIEF_VOIDS - 1) * sizeof *list);
        (*n)--;
    }
    list[(*n)++] = v;
}

/* ---- the pass ---------------------------------------------------------------- */

typedef struct {
    PkBelief *b;
    int       started;          /* the start card is down: burying is over */
} Pass;

static void gain(PkBelief *b, int s)
{
    b->count[s]++;
    for (int j = 0; j < b->hard_n[s]; j++) if (b->hard[s][j].gained < 0xFF) b->hard[s][j].gained++;
    for (int j = 0; j < b->soft_n[s]; j++) if (b->soft[s][j].gained < 0xFF) b->soft[s][j].gained++;
}

/* A card left seat s's hand face up. A card a void forbids cannot be one the
 * void binds, so it spends the void's `gained` budget and leaves k alone; any
 * other card may have been a bound one, so k drops. More forbidden plays than
 * gained cards is a contradiction: the void was wrong. */
static void played(PkBelief *b, int s, uint8_t c)
{
    for (int j = 0; j < b->pinned_n[s]; j++)
        if (b->pinned[s][j] == c) {
            b->pinned[s][j] = b->pinned[s][--b->pinned_n[s]];
            break;
        }
    for (int j = 0; j < b->hard_n[s]; j++) {
        PkVoid *v = &b->hard[s][j];
        if (forbids(v, c)) v->hits++;
        else if (v->k) v->k--;
    }
    int broke = 0;
    for (int j = 0; j < b->soft_n[s]; j++) {
        PkVoid *v = &b->soft[s][j];
        if (forbids(v, c)) { v->hits++; if (v->hits > v->gained) broke = 1; }
        else if (v->k) v->k--;
    }
    if (broke) {                                  /* octogen's mc_tell: distrust the seat */
        b->distrust |= (uint8_t)(1u << s);
        b->soft_n[s] = 0;
    }
    if (b->count[s]) b->count[s]--;
    b->plays[s]++;
}

/* A face-down card into seat s's hand. When the position drawn was a known
 * buried card, everyone knows who holds it now. */
static void drew(PkBelief *b, int s, int deck_after)
{
    if (deck_after < b->deck_known_n) {
        uint8_t c = b->deck_known[deck_after];
        b->deck_known_n = (uint8_t)deck_after;
        if (s != b->me && b->pinned_n[s] < PK_HAND_CAP) b->pinned[s][b->pinned_n[s]++] = c;
    }
    gain(b, s);
}

static void on_event(const PkEvent *e, void *ctx)
{
    Pass *p = (Pass *)ctx;
    PkBelief *b = p->b;
    int s = e->seat;
    switch (e->kind) {
    case PK_EV_DEAL:
        if (s < b->n) b->count[s]++;
        break;
    case PK_EV_BURY:
        /* the flip goes face up to the bottom: every known card moves up one */
        if (!p->started && b->deck_known_n < PK_DECK) {
            memmove(b->deck_known + 1, b->deck_known, b->deck_known_n);
            b->deck_known[0] = e->card;
            b->deck_known_n++;
        }
        break;
    case PK_EV_START_CARD:
        p->started = 1;
        b->top = e->card;
        b->live_suit = e->suit;
        break;
    case PK_EV_DRAW:
        if (s >= b->n) break;
        /* D6: a draw proves nothing, but a first draw is usually a stuck hand */
        if (e->i == 1 && s != b->me && !(b->distrust >> s & 1u))
            push_void(b->soft[s], &b->soft_n[s], no_match(e->suit, b->top, b->count[s]));
        b->draws[s]++;
        drew(b, s, e->deck_n);
        break;
    case PK_EV_PENALTY_DRAW:
        if (s < b->n) drew(b, s, e->deck_n);
        break;
    case PK_EV_RESHUFFLE_GATHER:
        b->deck_known_n = 0;
        break;
    case PK_EV_PLAY:
        if (s >= b->n) break;
        played(b, s, e->card);
        b->top = e->card;
        b->live_suit = e->suit;
        break;
    case PK_EV_WILD_SUIT:
        b->live_suit = e->suit;
        break;
    case PK_EV_PASS:
        if (s >= b->n || s == b->me) break;
        if (e->n == 0)          /* D10: a bare pass is legal only with nothing playable */
            push_void(b->hard[s], &b->hard_n[s], no_match(b->live_suit, b->top, b->count[s]));
        else if (!(b->distrust >> s & 1u))   /* drew and still passed: likely nothing */
            push_void(b->soft[s], &b->soft_n[s], no_match(b->live_suit, b->top, b->count[s]));
        break;
    default:
        break;
    }
}

int pk_belief_build(PkBelief *b, const PkGame *g, int me)
{
    memset(b, 0, sizeof *b);
    b->me = (uint8_t)me;
    b->n = g->n;
    if (me < 0 || me >= g->n) return 0;
    Pass p = { b, 0 };
    if (pk_plan_each(g, me, -1, 0xFFFF, on_event, &p) < 0) return 0;

    /* the public table: the deck count, the stack, my own hand */
    b->deck_n = g->deck_n;
    if (b->deck_known_n > b->deck_n) b->deck_known_n = b->deck_n;
    for (int i = 0; i < g->stack_n; i++) b->loc[g->stack[i]] = PK_LOC_STACK;
    for (int i = 0; i < b->deck_known_n; i++) b->loc[b->deck_known[i]] = PK_LOC_DECK;
    for (int s = 0; s < b->n; s++)
        for (int i = 0; i < b->pinned_n[s]; i++) b->loc[b->pinned[s][i]] = (uint8_t)(PK_LOC_SEAT + s);
    for (int i = 0; i < g->hand_n[me]; i++) b->loc[g->hand[me][i]] = PK_LOC_MINE;
    b->pinned_n[me] = 0;
    b->hard_n[me] = b->soft_n[me] = 0;
    for (int c = 0; c < PK_DECK; c++)
        if (b->loc[c] == PK_LOC_UNSEEN) b->pool[b->pool_n++] = (uint8_t)c;
    b->ok = 1;
    return 1;
}

/* ---- the constraint a slot is under --------------------------------------------- */

/* Seat s's unknown slots are its hand beyond the pinned cards. A void binds k
 * cards of the whole hand; the pinned ones may be among them, so it binds at
 * least k - pinned of the unknown slots, and those are slots 0.. first. */
static int binds(const PkBelief *b, int s, const PkVoid *v, int slot)
{
    return (int)v->k - (int)b->pinned_n[s] > slot;
}

int pk_belief_allows(const PkBelief *b, int s, int slot, uint8_t c, int use_soft)
{
    for (int j = 0; j < b->hard_n[s]; j++)
        if (binds(b, s, &b->hard[s][j], slot) && forbids(&b->hard[s][j], c)) return 0;
    if (use_soft && !(b->distrust >> s & 1u))
        for (int j = 0; j < b->soft_n[s]; j++)
            if (binds(b, s, &b->soft[s][j], slot) && forbids(&b->soft[s][j], c)) return 0;
    return 1;
}

/* ---- sampling ------------------------------------------------------------------- */

static uint64_t mix64(uint64_t z)
{
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Everything up to the history, the live history, and the seed: the unused
 * history tail is never read (pk.h), so a world copies ~1 KB, not 16. */
static void lite_copy(PkGame *d, const PkGame *s)
{
    size_t head = (size_t)((const char *)&s->hist[0] - (const char *)s);
    memcpy(d, s, head + (size_t)s->hist_n * sizeof(PkAct));
    memcpy(d->seed, s->seed, sizeof d->seed);
}

int pk_belief_sample(const PkBelief *b, const PkGame *g, uint64_t seed, int use_soft, PkGame *w)
{
    lite_copy(w, g);
    uint64_t r = seed ^ 0x6a09e667f3bcc909ull;
    for (int i = 0; i < 32; i += 8) {
        uint64_t z = mix64(r += 0x9e3779b97f4a7c15ull);
        for (int j = 0; j < 8; j++) w->seed[i + j] = (uint8_t)(z >> (8 * j));
    }

    /* the pool in a random order */
    uint8_t pool[PK_DECK];
    int m = b->pool_n;
    memcpy(pool, b->pool, (size_t)m);
    for (int i = m - 1; i > 0; i--) {
        r = mix64(r);
        int j = (int)(r % (uint64_t)(i + 1));
        uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    uint8_t used[PK_DECK];
    memset(used, 0, sizeof used);

    /* constrained slots first, each the first card in the order it allows;
     * a slot nothing fits takes the first card left (octogen degrades too) */
    int broken = 0;
    for (int s = 0; s < b->n; s++) {
        if (s == b->me) continue;
        int pn = b->pinned_n[s];
        memcpy(w->hand[s], b->pinned[s], (size_t)pn);
        int unknown = b->count[s] - pn;
        w->hand_n[s] = b->count[s];
        for (int slot = 0; slot < unknown; slot++) {
            int bound = 0;
            for (int j = 0; j < b->hard_n[s] && !bound; j++) bound = binds(b, s, &b->hard[s][j], slot);
            if (use_soft && !(b->distrust >> s & 1u))
                for (int j = 0; j < b->soft_n[s] && !bound; j++) bound = binds(b, s, &b->soft[s][j], slot);
            if (!bound) { w->hand[s][pn + slot] = PK_CARD_NONE; continue; }
            int pick = -1, any = -1;
            for (int i = 0; i < m && pick < 0; i++) {
                if (used[i]) continue;
                if (any < 0) any = i;
                if (pk_belief_allows(b, s, slot, pool[i], use_soft)) pick = i;
            }
            if (pick < 0) { pick = any; broken++; }
            if (pick < 0) { w->hand[s][pn + slot] = PK_CARD_NONE; continue; }
            used[pick] = 1;
            w->hand[s][pn + slot] = pool[pick];
        }
    }
    /* then the free slots, then the deck above its known bottom */
    int next = 0;
    for (int s = 0; s < b->n; s++) {
        if (s == b->me) continue;
        for (int i = b->pinned_n[s]; i < b->count[s]; i++) {
            if (w->hand[s][i] != PK_CARD_NONE) continue;
            while (next < m && used[next]) next++;
            if (next < m) { w->hand[s][i] = pool[next]; used[next] = 1; }
        }
    }
    memcpy(w->deck, b->deck_known, b->deck_known_n);
    int d = b->deck_known_n;
    for (int i = 0; i < m; i++)
        if (!used[i] && d < PK_DECK) w->deck[d++] = pool[i];
    w->deck_n = (uint8_t)d;
    return broken;
}
