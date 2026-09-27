/* Pick 'Em Up - this phone's own arrangement of its own hand. See pk_arrange.h. */
#include "pk_arrange.h"
#include "pk_plan.h"
#include <string.h>

void pk_arr_reset(PkArr *a, const uint8_t id[8], int seat)
{
    memset(a, 0, sizeof *a);
    if (id) memcpy(a->id, id, 8);
    a->seat = seat >= 0 && seat < PK_MAX_SEATS ? (uint8_t)seat : PK_SEAT_NONE;
}

int pk_arr_valid(const PkArr *a)
{
    if (a->seat != PK_SEAT_NONE && a->seat >= PK_MAX_SEATS) return 0;
    if (a->n > PK_DECK) return 0;
    uint8_t seen[PK_DECK];
    memset(seen, 0, sizeof seen);
    for (int e = 0; e < a->n; e++) {
        if (a->card[e] >= PK_DECK || seen[a->card[e]] || a->receipt[e] == 0) return 0;
        seen[a->card[e]] = 1;
    }
    return 1;
}

/* ---- the receipts: one replay, the hand followed through its events ---------- */

typedef struct {
    int      seat, bad;
    int      n;
    uint16_t k;                      /* receipts so far */
    uint8_t  card[PK_HAND_CAP];
    uint16_t receipt[PK_HAND_CAP];
} Follow;

static void follow(const PkEvent *e, void *ctx)
{
    Follow *f = ctx;
    if (e->seat != f->seat) return;
    switch (e->kind) {
    case PK_EV_DEAL: case PK_EV_DRAW: case PK_EV_PENALTY_DRAW:
        if (f->n >= PK_HAND_CAP) { f->bad = 1; return; }
        f->card[f->n] = e->card;
        f->receipt[f->n] = ++f->k;
        f->n++;
        break;
    case PK_EV_PLAY:
        if (e->i >= f->n) { f->bad = 1; return; }
        for (int i = e->i; i + 1 < f->n; i++) {
            f->card[i] = f->card[i + 1];
            f->receipt[i] = f->receipt[i + 1];
        }
        f->n--;
        break;
    default: break;
    }
}

int pk_arr_receipts(const PkGame *g, int seat, uint16_t out[PK_HAND_CAP])
{
    if (seat < 0 || seat >= g->n) return 0;
    static Follow f;                 /* 300 bytes, kept off a host thread's stack like pk_beats_pre's */
    memset(&f, 0, sizeof f);
    f.seat = seat;
    /* every bubble, the deal (0) and the open draft (bubbles + 1) included */
    if (pk_plan_each(g, seat, -1, g->bubbles + 1, follow, &f) < 0 || f.bad) return 0;
    if (f.n != g->hand_n[seat] || memcmp(f.card, g->hand[seat], (size_t)f.n) != 0) return 0;
    memcpy(out, f.receipt, (size_t)f.n * sizeof out[0]);
    return 1;
}

/* ---- the list ---------------------------------------------------------------------- */

static int find(const PkArr *a, uint8_t card)
{
    for (int e = 0; e < a->n; e++)
        if (a->card[e] == card) return e;
    return -1;
}

static void remove_at(PkArr *a, int e)
{
    for (int i = e; i + 1 < a->n; i++) {
        a->card[i] = a->card[i + 1];
        a->receipt[i] = a->receipt[i + 1];
    }
    a->n--;
    a->card[a->n] = 0;
    a->receipt[a->n] = 0;
}

static void insert_at(PkArr *a, int e, uint8_t card, uint16_t receipt)
{
    for (int i = a->n; i > e; i--) {
        a->card[i] = a->card[i - 1];
        a->receipt[i] = a->receipt[i - 1];
    }
    a->card[e] = card;
    a->receipt[e] = receipt;
    a->n++;
}

int pk_arr_sync(PkArr *a, const PkGame *g, int seat)
{
    static uint16_t rc[PK_HAND_CAP];
    if (!pk_arr_receipts(g, seat, rc)) return -1;
    int changed = 0;
    if (!pk_arr_valid(a)) {
        uint8_t id[8];
        memcpy(id, a->id, 8);
        pk_arr_reset(a, id, seat);
        changed = 1;
    }
    for (int p = 0; p < g->hand_n[seat]; p++) {
        uint8_t c = g->hand[seat][p];
        int e = find(a, c);
        if (e >= 0 && a->receipt[e] == rc[p]) continue;      /* the same acquisition */
        if (e >= 0) remove_at(a, e);                         /* an older one of this card */
        insert_at(a, a->n, c, rc[p]);                        /* new: on the right */
        changed = 1;
    }
    return changed;
}

void pk_arr_slots(const PkArr *a, const uint8_t *hand, int n, uint8_t *slot)
{
    /* a card the arrangement holds sorts by its entry, any other after them
     * all by its position: every key distinct, so the ranks are a permutation */
    int key[PK_HAND_CAP];
    if (n > PK_HAND_CAP) n = PK_HAND_CAP;
    for (int i = 0; i < n; i++) {
        int e = hand[i] < PK_DECK ? find(a, hand[i]) : -1;
        key[i] = e >= 0 ? e : PK_DECK + i;
    }
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int j = 0; j < n; j++) r += key[j] < key[i];
        slot[i] = (uint8_t)r;
    }
}

int pk_arr_pos(const PkArr *a, const uint8_t *hand, int n, int slot)
{
    uint8_t s[PK_HAND_CAP];
    if (slot < 0 || slot >= n || n > PK_HAND_CAP) return -1;
    pk_arr_slots(a, hand, n, s);
    for (int i = 0; i < n; i++)
        if (s[i] == slot) return i;
    return -1;
}

int pk_arr_move(PkArr *a, const uint8_t *hand, int n, int from, int to)
{
    if (n > PK_HAND_CAP || from < 0 || from >= n || to < 0 || to >= n || from == to) return 0;
    uint8_t s[PK_HAND_CAP];
    int order[PK_HAND_CAP];                   /* slot -> hand position */
    pk_arr_slots(a, hand, n, s);
    for (int i = 0; i < n; i++) {
        if (find(a, hand[i]) < 0) return 0;   /* not synced: nothing to move against */
        order[s[i]] = i;
    }
    const uint8_t card = hand[order[from]];
    const int e = find(a, card);
    const uint16_t receipt = a->receipt[e];
    /* the anchor, in the order the others keep once the card is lifted out:
     * the card that will sit just right of it, or for the last slot the card
     * that will sit just left of it. Entries of cards not in the hand stay
     * where they are, so an undo still finds its place. */
    int rest[PK_HAND_CAP], k = 0;
    for (int t = 0; t < n; t++)
        if (t != from) rest[k++] = order[t];
    remove_at(a, e);
    if (to < n - 1) insert_at(a, find(a, hand[rest[to]]), card, receipt);
    else            insert_at(a, find(a, hand[rest[n - 2]]) + 1, card, receipt);
    return 1;
}

/* ---- the bytes ------------------------------------------------------------------------ */

void pk_arr_put(const PkArr *a, uint8_t out[PK_ARR_LEN])
{
    memset(out, 0, PK_ARR_LEN);
    memcpy(out, a->id, 8);
    out[8] = a->seat;
    out[9] = a->n;
    for (int e = 0; e < a->n && e < PK_DECK; e++) {
        out[10 + 3 * e] = a->card[e];
        out[11 + 3 * e] = (uint8_t)(a->receipt[e] & 0xFF);
        out[12 + 3 * e] = (uint8_t)(a->receipt[e] >> 8);
    }
}

int pk_arr_get(PkArr *a, const uint8_t in[PK_ARR_LEN])
{
    memset(a, 0, sizeof *a);
    memcpy(a->id, in, 8);
    a->seat = in[8];
    a->n = in[9];
    if (a->n <= PK_DECK)
        for (int e = 0; e < a->n; e++) {
            a->card[e] = in[10 + 3 * e];
            a->receipt[e] = (uint16_t)(in[11 + 3 * e] | in[12 + 3 * e] << 8);
        }
    if (pk_arr_valid(a)) return 1;
    uint8_t id[8];
    memcpy(id, in, 8);
    pk_arr_reset(a, id, PK_SEAT_NONE);
    return 0;
}
