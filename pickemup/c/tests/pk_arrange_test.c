/* This phone's own arrangement of its hand (ORCHESTRATION O9, RULES_AND_KERNEL
 * D54 to D57): the kernel module (src/pk_arrange.c) and the bridge that keeps
 * it in the phone's record (ios/pk_api.c, compiled in here as the two-phone
 * test compiles it).
 *
 * THE ORACLE IS THIS FILE'S OWN. The order a hand should read in is kept here
 * as a plain list of card ids, from O9's words alone: a drag moves one card
 * (lift it out, put it in at the target slot); a card that arrives goes on
 * the right; a card that leaves closes the gap; an undo straight after a play
 * gives the order back exactly as it was. Which card arrived or left is read
 * off the acquisition-order hand before and after (a hand only ever loses at
 * most one card and gains at the end), never off the arrangement.
 *
 *   ./build/pk_arrange_test [games]   the random games of the property test */
#include "../ios/pk_api.c"
/* pk_check.h's action builder and the bridge's own act() share a name */
#define act chk_act
#include "pk_check.h"

/* ---- the oracle -------------------------------------------------------------- */

typedef struct { int n; uint8_t c[PK_HAND_CAP]; } List;

static int list_has(const List *l, uint8_t c)
{
    for (int i = 0; i < l->n; i++) if (l->c[i] == c) return 1;
    return 0;
}

static void list_move(List *l, int from, int to)
{
    uint8_t c = l->c[from];
    for (int i = from; i + 1 < l->n; i++) l->c[i] = l->c[i + 1];
    for (int i = l->n - 1; i > to; i--) l->c[i] = l->c[i - 1];
    l->c[to] = c;
}

/* The hand went from `before` to `after` (acquisition order): keep the cards
 * that stayed, in the list's order, then the new ones on the right in the
 * order they came. 0 if the change is not "lose at most one, gain at the end". */
static int list_follow(List *l, const uint8_t *before, int nb, const uint8_t *after, int na)
{
    int kept = 0, lost = 0;
    for (int i = 0; i < nb; i++) {
        int still = 0;
        for (int j = 0; j < na; j++) if (after[j] == before[i]) still = 1;
        if (!still) { lost++; continue; }
        if (kept >= na || after[kept] != before[i]) return 0;
        kept++;
    }
    if (lost > 1) return 0;
    List out = { 0, { 0 } };
    for (int i = 0; i < l->n; i++) {
        int still = 0;
        for (int j = 0; j < kept; j++) if (after[j] == l->c[i]) still = 1;
        if (still) out.c[out.n++] = l->c[i];
    }
    for (int j = kept; j < na; j++) out.c[out.n++] = after[j];
    *l = out;
    return 1;
}

static void list_of_hand(List *l, const uint8_t *hand, int n)
{
    l->n = n;
    memcpy(l->c, hand, (size_t)n);
}

/* The cards of `hand` in the order `slot` draws them. */
static void drawn(const uint8_t *hand, const uint8_t *slot, int n, List *out)
{
    out->n = n;
    for (int i = 0; i < n; i++) out->c[i] = 0xEE;
    for (int i = 0; i < n; i++) if (slot[i] < n) out->c[slot[i]] = hand[i];
}

static int list_eq(const List *a, const List *b)
{
    return a->n == b->n && memcmp(a->c, b->c, (size_t)a->n) == 0;
}

static void list_print(const char *what, const List *l)
{
    fprintf(stderr, "    %s:", what);
    for (int i = 0; i < l->n; i++) fprintf(stderr, " %d", l->c[i]);
    fputc('\n', stderr);
}

/* ---- the module: receipts, slots, moves, bytes ----------------------------------- */

static void test_module(void)
{
    static PkGame g;
    uint8_t seed[32];
    seed_wide(seed, 7);
    pk_new(&g, seed, 3);
    uint8_t id[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    PkArr a;
    uint16_t rc[PK_HAND_CAP];
    uint8_t slot[PK_HAND_CAP];

    TEST("receipts");
    CHECK(pk_arr_receipts(&g, 0, rc), "the deal replays");
    int in_order = 1;
    for (int i = 0; i < 7; i++) in_order &= rc[i] == i + 1;
    CHECK(in_order, "the deal is receipts 1..7 in hand order");
    CHECK(!pk_arr_receipts(&g, 3, rc) && !pk_arr_receipts(&g, -1, rc), "a seat off the table has none");

    TEST("empty reads in acquisition order");
    static PkView pv;
    pk_view(&g, 0, &pv);
    int kident = pv.my_n == 7;
    for (int i = 0; i < pv.my_n; i++) kident &= pv.my_slot[i] == i;
    CHECK(kident, "the kernel's own view: slot i is position i (D54)");
    pk_arr_reset(&a, id, 0);
    CHECK(a.n == 0 && a.seat == 0 && !memcmp(a.id, id, 8), "reset keeps the game and the seat");
    pk_arr_slots(&a, g.hand[0], g.hand_n[0], slot);
    int ident = 1;
    for (int i = 0; i < g.hand_n[0]; i++) ident &= slot[i] == i;
    CHECK(ident, "no arrangement: slot i is position i");
    CHECK(pk_arr_sync(&a, &g, 0) == 1 && a.n == 7, "the first fold takes the hand (%d entries)", a.n);
    CHECK(pk_arr_sync(&a, &g, 0) == 0, "a second fold changes nothing");
    pk_arr_slots(&a, g.hand[0], g.hand_n[0], slot);
    ident = 1;
    for (int i = 0; i < g.hand_n[0]; i++) ident &= slot[i] == i;
    CHECK(ident, "folded in: still acquisition order");

    TEST("move");
    List want;
    list_of_hand(&want, g.hand[0], 7);
    static const int mv[][2] = { { 0, 6 }, { 6, 0 }, { 2, 4 }, { 5, 1 }, { 3, 3 }, { 1, 2 }, { 6, 5 } };
    for (unsigned k = 0; k < sizeof mv / sizeof mv[0]; k++) {
        int r = pk_arr_move(&a, g.hand[0], 7, mv[k][0], mv[k][1]);
        CHECK(r == (mv[k][0] != mv[k][1]), "move %d -> %d answers %d", mv[k][0], mv[k][1], r);
        if (mv[k][0] != mv[k][1]) list_move(&want, mv[k][0], mv[k][1]);
        List got;
        pk_arr_slots(&a, g.hand[0], 7, slot);
        drawn(g.hand[0], slot, 7, &got);
        CHECK(list_eq(&got, &want), "after move %d -> %d the hand reads as the drag put it", mv[k][0], mv[k][1]);
        int pos_ok = 1;
        for (int s = 0; s < 7; s++) pos_ok &= g.hand[0][pk_arr_pos(&a, g.hand[0], 7, s)] == want.c[s];
        CHECK(pos_ok, "pk_arr_pos is the inverse of the slots");
    }
    CHECK(!pk_arr_move(&a, g.hand[0], 7, -1, 2) && !pk_arr_move(&a, g.hand[0], 7, 0, 7)
          && !pk_arr_move(&a, g.hand[0], 7, 7, 0), "a slot off the hand is refused");
    CHECK(pk_arr_pos(&a, g.hand[0], 7, 7) == -1 && pk_arr_pos(&a, g.hand[0], 7, -1) == -1, "no position off the hand");
    PkArr fresh;
    pk_arr_reset(&fresh, id, 0);
    CHECK(!pk_arr_move(&fresh, g.hand[0], 7, 0, 3), "a move against cards the arrangement does not hold is refused");

    TEST("a card the arrangement does not hold");
    uint8_t other[3] = { g.hand[0][0], 0xFE, g.hand[0][1] };      /* a hidden card between two held ones */
    pk_arr_slots(&a, other, 3, slot);
    CHECK(slot[1] == 2 && slot[0] != slot[2] && slot[0] < 2 && slot[2] < 2, "held cards first, then the rest in order");

    TEST("bytes");
    uint8_t b[PK_ARR_LEN];
    PkArr back;
    pk_arr_put(&a, b);
    CHECK(pk_arr_get(&back, b) && back.n == a.n && back.seat == a.seat && !memcmp(back.id, a.id, 8)
          && !memcmp(back.card, a.card, a.n) && !memcmp(back.receipt, a.receipt, a.n * sizeof a.receipt[0]),
          "an arrangement survives its bytes");
    PkArr late = a;
    late.receipt[0] = 300;                                        /* a long game: past one byte */
    pk_arr_put(&late, b);
    CHECK(pk_arr_get(&back, b) && back.receipt[0] == 300, "a receipt past 255 survives its bytes (%d)", back.receipt[0]);
    pk_arr_put(&a, b);
    uint8_t bad[PK_ARR_LEN];
    memcpy(bad, b, sizeof bad);
    bad[10 + 3] = bad[10];                                        /* entry 1 is entry 0's card */
    CHECK(!pk_arr_get(&back, bad) && back.n == 0 && back.seat == PK_SEAT_NONE, "a card twice is not an arrangement");
    memcpy(bad, b, sizeof bad);
    bad[9] = PK_DECK + 1;
    CHECK(!pk_arr_get(&back, bad) && back.n == 0, "more entries than the deck is not an arrangement");
    memcpy(bad, b, sizeof bad);
    bad[11] = bad[12] = 0;
    CHECK(!pk_arr_get(&back, bad), "receipt 0 is not an arrangement");
    memcpy(bad, b, sizeof bad);
    bad[10] = PK_DECK;
    CHECK(!pk_arr_get(&back, bad), "a card off the deck is not an arrangement");

    TEST("an arrangement for another hand is not trusted");
    PkArr wrong = a;
    for (int e = 0; e < wrong.n; e++) wrong.receipt[e] = (uint16_t)(wrong.receipt[e] + 50);
    CHECK(pk_arr_valid(&wrong), "well formed");
    CHECK(pk_arr_sync(&wrong, &g, 0) == 1, "the fold replaces every entry");
    pk_arr_slots(&wrong, g.hand[0], 7, slot);
    ident = 1;
    for (int i = 0; i < 7; i++) ident &= slot[i] == i;
    CHECK(ident, "receipts that match nothing: the hand reads in acquisition order");
    PkArr dup = a;
    dup.card[1] = dup.card[0];
    CHECK(pk_arr_sync(&dup, &g, 0) == 1 && pk_arr_valid(&dup), "a corrupt arrangement is emptied and refolded");
    pk_arr_slots(&dup, g.hand[0], 7, slot);
    ident = 1;
    for (int i = 0; i < 7; i++) ident &= slot[i] == i;
    CHECK(ident, "a corrupt arrangement: the hand reads in acquisition order");
}

/* ---- the property: random games, random drags, every step checked -------------- */

static void test_games(int games)
{
    static PkGame g, tmp;
    static PkArr a, a2;
    uint8_t slot[PK_HAND_CAP], before[PK_HAND_CAP];
    int reshuffled = 0, came_back = 0, undone = 0, moves = 0, penalties = 0;
    TEST("random games");
    for (int gi = 0; gi < games; gi++) {
        uint8_t seed[32];
        seed_wide(seed, 9000u + (uint32_t)gi);
        int n = 2 + gi % 3;
        pk_new(&g, seed, n);
        uint8_t id[8] = { 0 };
        id[0] = (uint8_t)gi;
        pk_arr_reset(&a, id, 0);
        List want;
        list_of_hand(&want, g.hand[0], g.hand_n[0]);
        uint8_t ever[PK_DECK] = { 0 };               /* seat 0 has held it */
        for (int i = 0; i < g.hand_n[0]; i++) ever[g.hand[0][i]] = 1;
        int bad = 0;
        for (int step = 0; step < 4000 && !bad; step++) {
            int nb = g.hand_n[0];
            memcpy(before, g.hand[0], (size_t)nb);
            List prev = want;
            const uint16_t resh = g.reshuffles;
            if (!bot_step(&g)) break;
            if (g.reshuffles != resh) reshuffled++;
            if (!list_follow(&want, before, nb, g.hand[0], g.hand_n[0])) {
                CHECK(0, "game %d step %d: a hand changed by more than one card out", gi, step);
                break;
            }
            for (int j = 0; j < g.hand_n[0]; j++) {
                if (j >= nb || !list_has(&prev, g.hand[0][j])) {
                    if (ever[g.hand[0][j]]) came_back++;
                    ever[g.hand[0][j]] = 1;
                }
            }
            if (g.hand_n[0] > nb + 1) penalties++;
            CHECK(pk_arr_sync(&a, &g, 0) >= 0, "game %d step %d: the fold replays", gi, step);
            List got;
            pk_arr_slots(&a, g.hand[0], g.hand_n[0], slot);
            drawn(g.hand[0], slot, g.hand_n[0], &got);
            if (!list_eq(&got, &want)) {
                CHECK(0, "game %d step %d: the hand reads as O9 says (arrivals right, gaps closed)", gi, step);
                list_print("got ", &got);
                list_print("want", &want);
                bad = 1;
                break;
            }
            /* an undo straight after my play gives the order back */
            if (g.hand_n[0] < nb && g.b_open && g.b_sender == 0 && rnd(100) < 40) {
                tmp = g;
                a2 = a;
                if (pk_undo(&tmp) && tmp.hand_n[0] == nb) {
                    undone++;
                    CHECK(pk_arr_sync(&a2, &tmp, 0) >= 0, "the undone game replays");
                    pk_arr_slots(&a2, tmp.hand[0], tmp.hand_n[0], slot);
                    drawn(tmp.hand[0], slot, tmp.hand_n[0], &got);
                    if (!list_eq(&got, &prev)) {
                        CHECK(0, "game %d step %d: an undo puts the played card back in its slot", gi, step);
                        list_print("got ", &got);
                        list_print("want", &prev);
                        bad = 1;
                    }
                }
            }
            /* and the player drags now and then */
            if (g.hand_n[0] >= 2 && rnd(100) < 50) {
                int from = (int)rnd(g.hand_n[0]), to = (int)rnd(g.hand_n[0]);
                int r = pk_arr_move(&a, g.hand[0], g.hand_n[0], from, to);
                CHECK(r == (from != to), "game %d step %d: move %d -> %d answers %d", gi, step, from, to, r);
                if (r) { list_move(&want, from, to); moves++; }
            }
        }
    }
    printf("  %d games: %d drags, %d reshuffles, %d penalty arrivals, %d cards back after leaving, %d undos\n",
           games, moves, reshuffled, penalties, came_back, undone);
    CHECK(reshuffled > 0 && came_back > 0 && undone > 0 && penalties > 0,
          "the games reached a reshuffle, a card drawn again, an undo and a penalty");
}

/* ---- the bridge: two phones, the phone's record, the wire -------------------------- */

typedef struct {
    uint8_t     id[16];
    const char *nick;
    uint8_t     rec[PK_API_REC_BYTES];
    int         rec_n;
} Phone;

static Phone ph[2];
static int   holding = -1;
static char  link_[PK_API_TEXT_MAX];

static void hold(int p)
{
    if (holding >= 0) ph[holding].rec_n = pk_api_seats_save(ph[holding].rec, PK_API_REC_BYTES);
    pk_api_seats_load(ph[p].rec, ph[p].rec_n);
    pk_api_me(ph[p].id, 16);
    pk_api_nickname((const uint8_t *)ph[p].nick, (int)strlen(ph[p].nick));
    pk_api_sender(0, 0, 0);
    holding = p;
    if (link_[0]) pk_api_read(link_);
}

static void send_(void)
{
    CHECK(pk_api_text(link_, sizeof link_) > 0, "the link writes");
    pk_api_commit();
}

static const PkView *me_view(void) { return (const PkView *)pk_api_view(PK_API_ME); }

static void shown(List *out)
{
    const PkView *v = me_view();
    drawn(v->my_hand, v->my_slot, v->my_n, out);
}

/* One plain turn for the phone in hand: the first card that plays (a wild on
 * circles), else a draw and then that card if it plays, else a pass; then
 * Send once the bubble may go. */
static void plain_turn(void)
{
    for (int guard = 0; guard < 8; guard++) {
        const PkView *v = me_view();
        if (v->over || v->turn != v->me) break;
        int n = v->my_n, done = 0;
        for (int p = 0; p < n && !done; p++)
            if (pk_api_can_play(p)) done = pk_api_play(p, pk_api_is_wild(p) ? 0 : PK_NO_SUIT);
        if (!done && pk_api_draw()) {
            int p = me_view()->my_n - 1;
            if (pk_api_can_play(p)) done = pk_api_play(p, pk_api_is_wild(p) ? 0 : PK_NO_SUIT);
            if (!done) done = pk_api_pass();
        }
        const PkApiTable *t = (const PkApiTable *)pk_api_table();
        if (t->can_send) break;
    }
    const PkApiTable *t = (const PkApiTable *)pk_api_table();
    if (t->can_send) send_();
}

/* A card of mine that plays now, at a slot that is neither end, else any. */
static int playable_slot(int n)
{
    int any = -1;
    for (int s = 0; s < n; s++) {
        int p = pk_api_arranged_pos(s);
        if (p < 0 || !pk_api_can_play(p)) continue;
        if (s > 0 && s < n - 1) return s;
        if (any < 0) any = s;
    }
    return any;
}

static void test_bridge(void)
{
    uint8_t seed[32];
    List want, got;
    for (int p = 0; p < 2; p++) {
        memset(&ph[p], 0, sizeof ph[p]);
        for (int i = 0; i < 16; i++) ph[p].id[i] = (uint8_t)(p * 101 + i * 7 + 3);
    }
    ph[0].nick = "Alex";
    ph[1].nick = "Bo";

    /* THE SEED IS FOUND: one where Alex (seat 0) has a card to play in the
     * middle of the arranged hand on the first turn, after a drag. */
    int found = 0;
    for (uint32_t k = 1; k < 400 && !found; k++) {
        seed_wide(seed, k);
        memset(ph[0].rec, 0, sizeof ph[0].rec); ph[0].rec_n = 0;
        memset(ph[1].rec, 0, sizeof ph[1].rec); ph[1].rec_n = 0;
        holding = -1;
        link_[0] = 0;
        hold(0);
        if (pk_api_new(seed, 1) != 0) continue;
        send_();
        hold(1);
        if (pk_api_join_start() < 0) continue;
        send_();
        hold(0);
        const PkView *v = me_view();
        if (v->my_n != 7 || v->turn != 1) continue;
        /* Bo's plain first turn, then mine must have a middle play */
        hold(1);
        plain_turn();
        hold(0);
        v = me_view();
        if (v->over || v->turn != 0 || v->my_n < 5) continue;
        found = 1;
    }
    TEST("bridge: a seed");
    CHECK(found, "a seed where Alex moves second");
    if (!found) return;

    /* replay that seed from the start with every assertion */
    memset(ph[0].rec, 0, sizeof ph[0].rec); ph[0].rec_n = 0;
    memset(ph[1].rec, 0, sizeof ph[1].rec); ph[1].rec_n = 0;
    holding = -1;
    link_[0] = 0;
    hold(0);
    pk_api_new(seed, 1);
    send_();
    hold(1);
    pk_api_join_start();
    /* Bo's hand came from the deal Bo made, never from a read: the fold that
     * gives a played card its entry is the one just before the play (D56) */
    TEST("bridge: a play straight after my own deal, the hand never read");
    int bo_pos = -1;
    for (int p = 1; p < 7 && bo_pos < 0; p++) if (pk_api_can_play(p)) bo_pos = p;
    CHECK(bo_pos >= 0, "Bo has a card to play past position 0");
    if (bo_pos >= 0) {
        CHECK(pk_api_play(bo_pos, pk_api_is_wild(bo_pos) ? 0 : PK_NO_SUIT), "Bo plays position %d", bo_pos);
        CHECK(me_view()->my_n == 6 && pk_api_undo(), "the view after the play, then undo");
        const PkView *bv = me_view();
        int back_ident = 1;
        for (int i = 0; i < bv->my_n; i++) back_ident &= bv->my_slot[i] == i;
        CHECK(bv->my_n == 7 && back_ident, "the card is back at position %d, the hand in acquisition order", bo_pos);
    }
    send_();
    hold(0);

    TEST("bridge: the deal reads in acquisition order");
    const PkView *v = me_view();
    int ident = 1;
    for (int i = 0; i < v->my_n; i++) ident &= v->my_slot[i] == i;
    CHECK(v->my_n == 7 && ident, "no drag yet: slot i is position i");
    list_of_hand(&want, v->my_hand, v->my_n);
    List acq_before;
    list_of_hand(&acq_before, v->my_hand, v->my_n);

    TEST("bridge: a reorder moves the card and the next view reads the new order");
    CHECK(pk_api_arrange_move(0, 6), "slot 0 to slot 6, out of turn");
    list_move(&want, 0, 6);
    shown(&got);
    CHECK(list_eq(&got, &want), "the view reads the drag");
    CHECK(pk_api_arrange_move(5, 1), "slot 5 to slot 1");
    list_move(&want, 5, 1);
    shown(&got);
    CHECK(list_eq(&got, &want), "the view reads both drags");
    CHECK(!pk_api_arrange_move(3, 3) && !pk_api_arrange_move(-1, 0) && !pk_api_arrange_move(0, 7),
          "no move to itself or off the hand");
    CHECK(pk_api_arranged_pos(1) >= 0 && me_view()->my_hand[pk_api_arranged_pos(1)] == want.c[1],
          "the position at a slot is the card drawn there");
    char before_turn[PK_API_TEXT_MAX];
    int rt = pk_api_text(before_turn, sizeof before_turn);
    CHECK(rt > 0 && strcmp(before_turn, link_) == 0, "a drag writes nothing on the wire");

    TEST("bridge: the arrangement survives the switch of phone (the record's bytes)");
    hold(1);
    uint8_t alex_rec[PK_API_REC_BYTES];
    int alex_n = ph[0].rec_n;
    memcpy(alex_rec, ph[0].rec, (size_t)alex_n);
    CHECK(alex_n > PK_API_ARR_BYTES && (alex_n - PK_API_ARR_BYTES) % 17 == 0, "Alex's record carries the block (%d bytes)", alex_n);
    List bo_before;
    shown(&bo_before);
    int bo_ident = 1;
    for (int i = 0; i < me_view()->my_n; i++) bo_ident &= me_view()->my_slot[i] == i;
    CHECK(bo_ident, "Bo's hand is Bo's: Alex's drags are not on it");

    TEST("bridge: a received bubble never scrambles it");
    int alex_before_n = want.n;
    plain_turn();
    hold(0);
    v = me_view();
    /* acquisition order before: the deal; after: the deal plus whatever Bo's
     * bubble gave me, on the right */
    CHECK(list_follow(&want, acq_before.c, acq_before.n, v->my_hand, v->my_n), "Bo's bubble only added cards");
    shown(&got);
    CHECK(list_eq(&got, &want) && got.n >= alex_before_n, "Alex's order is as dragged, anything new on the right");
    if (!list_eq(&got, &want)) { list_print("got ", &got); list_print("want", &want); }
    CHECK(v->turn == 0, "Alex's turn");

    TEST("bridge: a draw after a reorder lands on the right");
    int n0 = me_view()->my_n;
    CHECK(pk_api_arrange_move(n0 - 1, 0), "the rightmost card to the left end");
    list_move(&want, n0 - 1, 0);
    uint8_t acq[PK_HAND_CAP];
    memcpy(acq, me_view()->my_hand, (size_t)n0);
    CHECK(pk_api_draw(), "Alex draws");
    v = me_view();
    CHECK(v->my_n == n0 + 1, "one more card");
    CHECK(list_follow(&want, acq, n0, v->my_hand, v->my_n), "a draw only adds");
    shown(&got);
    CHECK(list_eq(&got, &want), "the drawn card is on the right, the rest as dragged");
    CHECK(got.c[got.n - 1] == v->my_hand[v->my_n - 1], "the right end is the newest card");

    TEST("bridge: a play after a reorder plays the right card, and the gap closes");
    int n1 = me_view()->my_n;
    int s = playable_slot(n1);
    CHECK(s >= 0, "a card of Alex's plays");
    if (s < 0) return;
    const uint8_t card = want.c[s];
    List pre_play = want;
    memcpy(acq, me_view()->my_hand, (size_t)n1);
    int pos = pk_api_arranged_pos(s);
    const int wild = pk_api_is_wild(pos);
    CHECK(pk_api_play_slot(s, wild ? 1 : PK_NO_SUIT), "the card at slot %d plays", s);
    v = me_view();
    CHECK(v->top == card, "the pile's top is the card that was drawn at slot %d (%d, want %d)", s, v->top, card);
    CHECK(list_follow(&want, acq, n1, v->my_hand, v->my_n), "a play takes one card");
    CHECK(!list_has(&want, card) && want.n == n1 - 1, "the played card left");
    shown(&got);
    CHECK(list_eq(&got, &want), "the gap closed, the rest as dragged");

    TEST("bridge: the wire is acquisition order, whatever the arrangement");
    char arranged_text[PK_API_TEXT_MAX], plain_text[PK_API_TEXT_MAX];
    CHECK(pk_api_text(arranged_text, sizeof arranged_text) > 0, "the draft writes");

    TEST("bridge: undo restores the arrangement");
    CHECK(pk_api_undo(), "the play comes back");
    shown(&got);
    CHECK(list_eq(&got, &pre_play), "the card is back in slot %d, every other card where it was", s);
    if (!list_eq(&got, &pre_play)) { list_print("got ", &got); list_print("want", &pre_play); }
    /* a drag between the play and its undo: the card comes back and the drag stays */
    CHECK(pk_api_play_slot(s, wild ? 1 : PK_NO_SUIT), "played again");
    List after_play;
    shown(&after_play);
    CHECK(pk_api_arrange_move(0, after_play.n - 1), "a drag while it is on the pile");
    list_move(&after_play, 0, after_play.n - 1);
    CHECK(pk_api_undo(), "undone again");
    shown(&got);
    List without = { 0, { 0 } };
    for (int i = 0; i < got.n; i++) if (got.c[i] != card) without.c[without.n++] = got.c[i];
    CHECK(list_has(&got, card) && list_eq(&without, &after_play), "the card is back and the drag made meanwhile stays");
    /* back to the arrangement the wire comparison was taken with */
    int back_s = -1;
    for (int i = 0; i < got.n; i++) if (got.c[i] == card) back_s = i;
    CHECK(pk_api_play_slot(back_s, wild ? 1 : PK_NO_SUIT), "and played for good");
    CHECK(me_view()->top == card, "the same card");
    shown(&got);
    CHECK(list_eq(&got, &after_play), "the gap closes on the order the drag left");
    want = after_play;

    TEST("bridge: the wire is acquisition order, whatever the arrangement");
    CHECK(pk_api_text(plain_text, sizeof plain_text) > 0 && strcmp(plain_text, arranged_text) == 0,
          "the same play after a different drag writes the same link");
    /* the same draft on a phone with no arrangement at all */
    int with_n = pk_api_seats_save(alex_rec, PK_API_REC_BYTES);
    pk_api_seats_load(alex_rec, with_n - PK_API_ARR_BYTES);        /* the seat records alone */
    CHECK(pk_api_read(link_) == 0, "Bo's bubble again, on a phone that never dragged");
    /* no view before the play: the hand was never read on this phone */
    CHECK(pk_api_draw() && pk_api_play(pos, wild ? 1 : PK_NO_SUIT), "the same draw and the same position");
    CHECK(pk_api_text(plain_text, sizeof plain_text) > 0 && strcmp(plain_text, arranged_text) == 0,
          "byte for byte the link the arranged phone wrote");
    TEST("bridge: an undo on a phone that never read its hand before the play");
    v = me_view();
    CHECK(v->my_n == n1 - 1, "the play is staged");
    CHECK(pk_api_undo(), "and undone");
    ident = 1;
    v = me_view();
    for (int i = 0; i < v->my_n; i++) ident &= v->my_slot[i] == i;
    CHECK(ident, "no drags: acquisition order, the card back at position %d", pos);
    pk_api_seats_load(alex_rec, with_n);
    CHECK(pk_api_read(link_) == 0 && pk_api_draw(), "back on the arranged phone");
    CHECK(pk_api_play_slot(back_s, wild ? 1 : PK_NO_SUIT) && me_view()->top == card, "the same play by slot");
    shown(&got);
    CHECK(list_eq(&got, &want), "the record brought the arrangement back");

    TEST("bridge: the arrangement survives the record's save and restore");
    uint8_t saved[PK_API_REC_BYTES];
    int saved_n = pk_api_seats_save(saved, PK_API_REC_BYTES);
    CHECK(saved_n == with_n, "the same bytes' length (%d, %d)", saved_n, with_n);
    CHECK(pk_api_seats_save(saved, saved_n - 1) == -1, "a buffer too small is refused");
    pk_api_seats_load(0, 0);
    CHECK(pk_api_read(arranged_text) == 0, "a phone with no record");
    ident = 1;
    v = me_view();
    for (int i = 0; i < v->my_n; i++) ident &= v->my_slot[i] == i;
    CHECK(ident, "without the record: acquisition order");
    pk_api_seats_load(saved, saved_n);
    CHECK(pk_api_read(arranged_text) == 0, "the record back");
    shown(&got);
    CHECK(list_eq(&got, &want), "with the record: the arrangement as it was");
    CHECK(((const PkApiTable *)pk_api_table())->by == PK_BY_RECORD, "and the seat records in front of it still seat me");

    TEST("bridge: a corrupted record is reset, not trusted");
    uint8_t bad[PK_API_REC_BYTES];
    memcpy(bad, saved, (size_t)saved_n);
    bad[saved_n - PK_API_ARR_BYTES + 8 + 10 + 1] ^= 0x40;       /* a receipt, checksum left stale */
    pk_api_seats_load(bad, saved_n);
    CHECK(pk_api_read(arranged_text) == 0, "read with the damaged record");
    ident = 1;
    v = me_view();
    for (int i = 0; i < v->my_n; i++) ident &= v->my_slot[i] == i;
    CHECK(ident, "a checksum that fails: acquisition order");
    CHECK(((const PkApiTable *)pk_api_table())->by == PK_BY_RECORD, "the seat records still load");
    /* a well-formed block whose game holds one card twice */
    memcpy(bad, saved, (size_t)saved_n);
    uint8_t *blk = bad + saved_n - PK_API_ARR_BYTES;
    blk[8 + 10 + 3] = blk[8 + 10];
    uint32_t sum = fnv32(blk, PK_API_ARR_BYTES - 4);
    for (int i = 0; i < 4; i++) blk[PK_API_ARR_BYTES - 4 + i] = (uint8_t)(sum >> (8 * i));
    pk_api_seats_load(bad, saved_n);
    CHECK(pk_api_read(arranged_text) == 0, "read with a card twice");
    ident = 1;
    v = me_view();
    for (int i = 0; i < v->my_n; i++) ident &= v->my_slot[i] == i;
    CHECK(ident, "an arrangement that is not one: acquisition order");
    /* bytes from before O9: seat records only */
    pk_api_seats_load(saved, saved_n - PK_API_ARR_BYTES);
    CHECK(pk_api_read(arranged_text) == 0 && ((const PkApiTable *)pk_api_table())->by == PK_BY_RECORD, "a record with no block still seats me");

    TEST("bridge: a slot's position folds the hand in first");
    /* the card at slot 0 recorded as some other acquisition of it (a card
     * that left and came back after a reshuffle): the fold must put it on
     * the right before a play by slot reads a position, view read or not */
    memcpy(bad, saved, (size_t)saved_n);
    blk = bad + saved_n - PK_API_ARR_BYTES;
    int stale = -1;
    for (int e = 0; e < blk[8 + 9] && stale < 0; e++) if (blk[8 + 10 + 3 * e] == want.c[0]) stale = e;
    CHECK(stale >= 0 && want.n >= 2, "slot 0's card has its entry in the record");
    if (stale >= 0) blk[8 + 12 + 3 * stale] ^= 0x01;                 /* receipt + 256: no acquisition of this hand */
    sum = fnv32(blk, PK_API_ARR_BYTES - 4);
    for (int i = 0; i < 4; i++) blk[PK_API_ARR_BYTES - 4 + i] = (uint8_t)(sum >> (8 * i));
    pk_api_seats_load(bad, saved_n);
    CHECK(pk_api_read(arranged_text) == 0, "read with the stale entry");
    int last_pos = pk_api_arranged_pos(want.n - 1);                  /* before any view */
    v = me_view();
    CHECK(last_pos >= 0 && v->my_hand[last_pos] == want.c[0] && v->my_slot[last_pos] == want.n - 1,
          "a card back as a new acquisition is at the right end, for a play by slot too (at %d)", last_pos);

    TEST("bridge: frames of a plan are laid out by the arrangement");
    pk_api_seats_load(saved, saved_n);
    CHECK(pk_api_read(arranged_text) == 0, "the arranged phone");
    shown(&got);
    const void *pl = pk_api_beats_host(PK_HM_REFUSED, 0, me_view()->my_n - 1);
    CHECK(pl != 0, "a host motion from the settled view");
    const PkBeatFrame *f = (const PkBeatFrame *)pk_api_beats_frame(0);
    List fr;
    drawn(f->my_hand, f->my_slot, f->my_n, &fr);
    CHECK(list_eq(&fr, &got), "the frame's hand is drawn as the view's");
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 120;
    test_module();
    test_games(games);
    test_bridge();
    return report("pk_arrange_test");
}
