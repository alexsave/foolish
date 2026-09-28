/* The bots (src/pk_bot.h) and their belief (src/pk_belief.h).
 *
 *   - every move a bot chooses is on the kernel's menu and the kernel applies
 *     it, over thousands of positions at 2..8 players;
 *   - the belief against the truth on real histories: counts, pinned cards,
 *     the pool, the deck's known bottom, hard voids never contradicted by the
 *     hand they bind; and it reads no hidden card (scrambling every other
 *     hand and the deck moves nothing);
 *   - hand-built histories on real deals: a draw-out that pins a buried card
 *     and ends in a bare pass (a hard void, and a pool that is exactly the
 *     passer's hand), and a two-player Skip chain that breaks a soft void;
 *   - determinism given a seed;
 *   - the wild's suit: the suit held most, and the suit the next seat is
 *     void in where greedy's tie-break would name another;
 *   - MC beats random at positions with a real choice, judged by how the
 *     true game ends.
 *
 *     make -C pickemup/c run           (./build/pk_bot_test [scale], default 4)
 *
 * tests/MUTATIONS.md records the mutation each test was seen red on. */
#include "pk_check.h"
#include "../src/pk_bot.h"
#include "../src/pk_belief.h"

static int SCALE = 4;

static PkBotKnobs fast_knobs(void)
{
    PkBotKnobs k;
    pk_bot_knobs_default(&k);
    k.w1 = 2; k.w2 = 2; k.w3 = 2;
    k.depth = 16;
    return k;
}

static PkBotKnobs strong_knobs(void)
{
    PkBotKnobs k;
    pk_bot_knobs_default(&k);
    k.w1 = 96; k.w2 = 160; k.w3 = 160;
    return k;
}

static int pos_of(const PkGame *g, int s, uint8_t c)
{
    for (int i = 0; i < g->hand_n[s]; i++) if (g->hand[s][i] == c) return i;
    return -1;
}

static int in_menu(const PkGame *g, int seat, PkAct a)
{
    PkAct m[PK_BOT_MENU_CAP];
    int n = pk_legal(g, seat, m, PK_BOT_MENU_CAP);
    for (int i = 0; i < n; i++)
        if (m[i].kind == a.kind && m[i].a == a.a && m[i].b == a.b) return 1;
    return 0;
}

/* ---- the checked driver: pk_bot_round's order, every move checked ---------------- */

static long g_moves;

static int checked_bubble(PkGame *g, int seat, int strat, const PkBotKnobs *k, uint64_t *rs)
{
    int done = 0;
    for (int guard = 0; guard < 4 * PK_MAX_ACTIONS; guard++) {
        PkBotMove mv;
        uint64_t h = pk_hash(g);
        int got = pk_bot_choose(g, seat, strat, k, rs, &mv);
        CHECK(pk_hash(g) == h, "choosing leaves the game alone");
        if (!got) return done;
        g_moves++;
        if (mv.what == PK_BOT_ACT) {
            const char *was = g_test;
            TEST("D60: say it at once, call only the proven");
            CHECK(!pk_is_legal(g, seat, SAY) || mv.act.kind == PK_A_SAY_IT, "a legal say comes first");
            if (mv.act.kind == PK_A_CALL_OUT) {
                uint8_t ref = g->b_open ? (uint8_t)(g->exposed & g->b_exposed_at_open) : g->exposed;
                CHECK(ref >> mv.act.a & 1u, "a call names an exposed seat: %d", mv.act.a);
            }
            g_test = was;
            CHECK(mv.seat == seat, "the move is the asked seat's");
            CHECK(pk_is_legal(g, seat, mv.act), "legal: kind %d a %d b %d, n %d", mv.act.kind, mv.act.a, mv.act.b, g->n);
            CHECK(in_menu(g, seat, mv.act), "on the menu: kind %d a %d b %d", mv.act.kind, mv.act.a, mv.act.b);
        } else {
            CHECK(mv.what == PK_BOT_SEAL && g->b_open && g->b_sender == seat && pk_can_seal(g),
                  "a seal only of the seat's own sealable bubble");
        }
        if (!pk_bot_apply(g, &mv)) { CHECK(0, "the kernel applied it"); return -1; }
        done++;
        if (mv.what == PK_BOT_SEAL) return done;
    }
    CHECK(0, "a bubble ends");
    return -1;
}

static int last_sender(const PkGame *g)
{
    for (int i = g->hist_n - 1; i >= 0; i--)
        if (g->hist[i].kind == PK_A_BUBBLE) return g->hist[i].a;
    return g->turn;
}

static int checked_round(PkGame *g, const uint8_t *strat, const PkBotKnobs *k, uint64_t *rs)
{
    int from = last_sender(g);
    for (int j = 0; j < g->n && !g->over; j++) {
        int s = pk_next(g, from, j);
        if (s == g->turn) continue;
        if (checked_bubble(g, s, strat[s], &k[s], rs) < 0) return -1;
    }
    if (g->over) return 0;
    int t = g->turn;
    int r = checked_bubble(g, t, strat[t], &k[t], rs);
    CHECK(r > 0, "the turn seat always has something to do");
    return r > 0 && !g->over;
}

/* ---- the belief against the truth ------------------------------------------------ */

static int cards_forbidden_free(const PkGame *g, int s, const PkBelief *b, const PkVoid *v)
{
    (void)b;
    int n = 0;
    for (int i = 0; i < g->hand_n[s]; i++) {
        uint8_t c = g->hand[s][i];
        int bad = (c < 96 && (v->suits >> pk_suit(c) & 1u)) || (v->ranks >> pk_rank(c) & 1u);
        if (!bad) n++;
    }
    return n;
}

static void belief_vs_truth(const PkGame *g, int me)
{
    TEST("belief: counts, pins, pool and hard voids against the truth");
    PkBelief b;
    CHECK(pk_belief_build(&b, g, me) && b.ok, "the history replays");
    int unknown = 0;
    for (int s = 0; s < g->n; s++) {
        CHECK(b.count[s] == g->hand_n[s], "seat %d count %d, truth %d", s, b.count[s], g->hand_n[s]);
        if (s == me) continue;
        for (int i = 0; i < b.pinned_n[s]; i++)
            CHECK(pos_of(g, s, b.pinned[s][i]) >= 0, "a pinned card is in the hand");
        unknown += b.count[s] - b.pinned_n[s];
        /* a hard void binds k cards of the hand, so at least k cards avoid it */
        for (int j = 0; j < b.hard_n[s]; j++)
            CHECK(cards_forbidden_free(g, s, &b, &b.hard[s][j]) >= b.hard[s][j].k,
                  "hard void %d of seat %d: k %d", j, s, b.hard[s][j].k);
    }
    CHECK(b.deck_n == g->deck_n, "deck count");
    for (int i = 0; i < b.deck_known_n; i++)
        CHECK(b.deck_known[i] == g->deck[i], "the known bottom card %d", i);
    CHECK(b.pool_n == unknown + b.deck_n - b.deck_known_n, "pool %d = unknown %d + deck %d - known %d",
          b.pool_n, unknown, b.deck_n, b.deck_known_n);
    /* the pool is exactly the cards no one can see */
    uint8_t truth[PK_DECK];
    memset(truth, 0, sizeof truth);
    for (int s = 0; s < g->n; s++) {
        if (s == me) continue;
        for (int i = 0; i < g->hand_n[s]; i++) truth[g->hand[s][i]] = 1;
        for (int i = 0; i < b.pinned_n[s]; i++) truth[b.pinned[s][i]] = 0;
    }
    for (int i = b.deck_known_n; i < g->deck_n; i++) truth[g->deck[i]] = 1;
    int agree = 1;
    for (int i = 0; i < b.pool_n; i++) if (!truth[b.pool[i]]) agree = 0;
    CHECK(agree, "every pool card is unseen");
    /* a sampled world keeps every public fact */
    PkGame w;
    int broken = pk_belief_sample(&b, g, 0x77 + (uint64_t)g->hist_n, 1, &w);
    CHECK(conserved(&w), "a world holds every card once");
    for (int s = 0; s < g->n; s++) {
        if (s == me) continue;
        int ok = 1;
        for (int i = 0; i < b.count[s] - b.pinned_n[s]; i++)
            ok &= pk_belief_allows(&b, s, i, w.hand[s][b.pinned_n[s] + i], 1);
        CHECK(ok || broken, "seat %d: every constrained slot holds a card its voids allow", s);
    }
    int same = w.deck_n == g->deck_n && w.stack_n == g->stack_n && w.live_suit == g->live_suit;
    for (int s = 0; s < g->n; s++) same &= w.hand_n[s] == g->hand_n[s];
    for (int i = 0; i < g->hand_n[me]; i++) same &= w.hand[me][i] == g->hand[me][i];
    CHECK(same, "a world keeps the counts, the stack and my hand");
}

/* THE BELIEF READS NO HIDDEN CARD: every other hand and the deck scrambled
 * (counts kept) leave it byte for byte the same. */
static void belief_blind(const PkGame *g, int me, uint64_t *rs)
{
    TEST("belief: reads no hidden card");
    static PkGame x;
    x = *g;
    uint8_t pool[PK_DECK];
    int m = 0;
    for (int s = 0; s < x.n; s++) if (s != me) for (int i = 0; i < x.hand_n[s]; i++) pool[m++] = x.hand[s][i];
    for (int i = 0; i < x.deck_n; i++) pool[m++] = x.deck[i];
    for (int i = m - 1; i > 0; i--) {
        int j = (int)(pk_bot_rand(rs) % (uint64_t)(i + 1));
        uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    int k = 0;
    for (int s = 0; s < x.n; s++) if (s != me) for (int i = 0; i < x.hand_n[s]; i++) x.hand[s][i] = pool[k++];
    for (int i = 0; i < x.deck_n; i++) x.deck[i] = pool[k++];
    static PkBelief a, b;
    pk_belief_build(&a, g, me);
    pk_belief_build(&b, &x, me);
    CHECK(memcmp(&a, &b, sizeof a) == 0, "the belief moved when hidden cards did (n %d, hist %d)", g->n, g->hist_n);
}

/* ---- 1. legality everywhere, and the belief on the way ----------------------------- */

static void t_legal_everywhere(void)
{
    long positions = 0, rounds = 0;
    int games = 0, ended = 0;
    uint64_t rs = 0x1234567;
    for (int n = 2; n <= 8; n++)
        for (int gi = 0; gi < 3 * SCALE; gi++) {
            TEST("every chosen move is legal, 2..8 players");
            PkGame g;
            uint8_t seed[32];
            seed_wide(seed, (uint32_t)(n * 1000 + gi));
            pk_new(&g, seed, n);
            uint8_t strat[PK_MAX_SEATS];
            PkBotKnobs k[PK_MAX_SEATS];
            for (int s = 0; s < n; s++) {
                strat[s] = (uint8_t)((s + gi) % PK_BOT_COUNT);
                k[s] = fast_knobs();
            }
            long before = g_moves;
            int r = 1;
            while (r > 0 && rounds < 1000000) {
                if (rounds % 5 == 0) belief_vs_truth(&g, (int)(rounds % n));
                if (rounds % 23 == 0) belief_blind(&g, (int)((rounds / 23) % n), &rs);
                TEST("every chosen move is legal, 2..8 players");
                r = checked_round(&g, strat, k, &rs);
                rounds++;
            }
            positions += g_moves - before;
            games++;
            ended += g.over != 0;
            if (g.over) belief_vs_truth(&g, gi % n);
        }
    TEST("every chosen move is legal, 2..8 players");
    CHECK(ended == games, "every game ended: %d of %d", ended, games);
    CHECK(positions >= 2000L * SCALE / 4, "positions: %ld", positions);
    printf("  legality: %ld moves over %d games at 2..8 players, every one on the menu and applied\n",
           positions, games);

    /* the arena's own driver, unchecked inside: it must never report a refusal */
    TEST("pk_bot_round never refuses a move");
    int refused = 0, over = 0;
    for (int n = 2; n <= 8; n++)
        for (int gi = 0; gi < 2 * SCALE; gi++) {
            PkGame g;
            uint8_t seed[32];
            seed_wide(seed, (uint32_t)(n * 7000 + gi));
            pk_new(&g, seed, n);
            uint8_t strat[PK_MAX_SEATS];
            PkBotKnobs k[PK_MAX_SEATS];
            for (int s = 0; s < n; s++) { strat[s] = (uint8_t)((s * 2 + gi) % PK_BOT_COUNT); k[s] = fast_knobs(); }
            int r;
            while ((r = pk_bot_round(&g, strat, k, &rs)) > 0) { }
            refused += r < 0;
            over += g.over != 0;
        }
    CHECK(refused == 0, "refused games: %d", refused);
    CHECK(over == 7 * 2 * SCALE, "every game ended: %d", over);
}

/* ---- 2. a draw-out: a buried card pinned, then a bare pass -------------------------- */

typedef struct { uint8_t buried[PK_DECK]; int n; } Buried;
static void count_bury(const PkEvent *e, void *ctx)
{
    Buried *b = (Buried *)ctx;
    if (e->kind == PK_EV_BURY) b->buried[b->n++] = e->card;
}

static void t_drawout_bare_pass(void)
{
    TEST("belief: a draw-out pins the buried card, a bare pass is a hard void");
    int found = 0;
    for (uint32_t k = 0; k < 20000 && !found; k++) {
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, k);
        pk_new(&g, seed, 2);
        Buried bur = { { 0 }, 0 };
        pk_plan_each(&g, PK_VIEW_ALL, -1, 0, count_bury, &bur);
        if (bur.n != 1) continue;
        /* seat 1 draws the whole deck, then passes */
        PkBelief b0;
        pk_belief_build(&b0, &g, 0);
        CHECK(b0.deck_known_n == 1 && b0.deck_known[0] == bur.buried[0] && g.deck[0] == bur.buried[0],
              "the buried card is known at the bottom");
        CHECK(b0.loc[bur.buried[0]] == PK_LOC_DECK, "its place is the deck");
        while (pk_is_legal(&g, 1, DRAW)) pk_apply(&g, 1, DRAW);
        CHECK(g.hand[1][g.hand_n[1] - 1] == bur.buried[0], "the last card drawn was the buried one");
        pk_belief_build(&b0, &g, 0);
        int pinned = 0;
        for (int i = 0; i < b0.pinned_n[1]; i++) pinned |= b0.pinned[1][i] == bur.buried[0];
        CHECK(pinned && b0.loc[bur.buried[0]] == PK_LOC_SEAT + 1, "seat 0 knows seat 1 holds it");
        CHECK(pk_apply(&g, 1, PASS) && pk_seal(&g), "seat 1 passes after drawing");
        /* seat 0 must hold nothing that plays: only then is its pass bare */
        PkAct m[PK_BOT_MENU_CAP];
        int n = pk_legal_turn(&g, 0, m, PK_BOT_MENU_CAP);
        if (n != 1 || m[0].kind != PK_A_PASS) continue;
        found = 1;
        uint8_t top = g.stack[g.stack_n - 1];
        int live = g.live_suit;
        CHECK(pk_apply(&g, 0, PASS) && pk_seal(&g), "the bare pass");
        PkBelief b1;
        pk_belief_build(&b1, &g, 1);
        CHECK(b1.hard_n[0] == 1, "one hard void on seat 0, got %d", b1.hard_n[0]);
        PkVoid v = b1.hard[0][0];
        CHECK(v.k == 7 && v.suits == (1u << live)
              && v.ranks == ((1u << pk_rank(top)) | (1u << PK_R_WILD) | (1u << PK_R_WILD4)),
              "the void: the live suit, the top's rank, both wilds, all 7 cards (k %d suits %x ranks %x)",
              v.k, v.suits, v.ranks);
        CHECK(!pk_belief_allows(&b1, 0, 0, num(live, pk_rank(top) == 9 ? 1 : 9, 0), 1),
              "no card of the live suit");
        CHECK(!pk_belief_allows(&b1, 0, 6, wild_(0), 1), "no wild in any of the 7 slots");
        CHECK(pk_belief_allows(&b1, 0, 0, num((live + 1) % 4, pk_rank(top) == 9 ? 1 : 9, 0), 1),
              "another suit's other rank is allowed");
        CHECK(b1.soft_n[0] == 0, "a bare pass is no soft void");
        /* seat 1 holds every other card: the pool is exactly seat 0's hand */
        CHECK(b1.pool_n == 7, "pool of 7, got %d", b1.pool_n);
        int exact = 1;
        for (int i = 0; i < b1.pool_n; i++) exact &= pos_of(&g, 0, b1.pool[i]) >= 0;
        CHECK(exact, "the pool is seat 0's hand");
        PkGame w;
        CHECK(pk_belief_sample(&b1, &g, 99, 0, &w) == 0, "a world satisfies every hard void");
        int same = w.hand_n[0] == 7;
        for (int i = 0; i < 7; i++) same &= pos_of(&g, 0, w.hand[0][i]) >= 0;
        CHECK(same, "and deals seat 0 its true hand");

        /* play on (greedy): each card seat 0 plays that the void does not
         * forbid may have been a bound one, so k falls by one; a card it
         * forbids must be a later draw, so it spends the draw instead */
        int plays = 0, forbidden = 0;
        for (int guard = 0; guard < 200 && !g.over && plays < 3; guard++) {
            int t = g.turn;
            int nt = pk_legal_turn(&g, t, m, PK_BOT_MENU_CAP);
            if (nt <= 0) break;
            PkAct a = m[pk_bot_greedy(&g, t, m, nt)];
            if (t == 0 && a.kind == PK_A_PLAY) {
                uint8_t c = g.hand[0][a.a];
                plays++;
                forbidden += (c < 96 && (v.suits >> pk_suit(c) & 1)) || (v.ranks >> pk_rank(c) & 1);
            }
            pk_apply(&g, t, a);
            if (a.kind != PK_A_DRAW) pk_seal(&g);
        }
        pk_belief_build(&b1, &g, 1);
        int want_k = 7 - (plays - forbidden);
        CHECK(plays > 0, "seat 0 played on");
        CHECK(b1.hard_n[0] >= 1 && b1.hard[0][0].k == (want_k < 0 ? 0 : want_k) && b1.hard[0][0].hits == forbidden,
              "after %d plays (%d forbidden) the void binds %d, got k %d hits %d", plays, forbidden, want_k,
              b1.hard[0][0].k, b1.hard[0][0].hits);
    }
    CHECK(found, "a deal with one buried card and a stuck seat 0");
}

/* ---- 3. a soft void, broken by a Skip chain ----------------------------------------- */

static void t_soft_void_distrust(void)
{
    TEST("belief: a first draw is a soft void; two plays past it break it");
    int found = 0;
    for (uint32_t k = 0; k < 20000 && !found; k++) {
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, k);
        pk_new(&g, seed, 2);
        int live = g.live_suit;
        uint8_t skip = PK_CARD_NONE, other = PK_CARD_NONE;
        for (int i = 0; i < g.hand_n[1]; i++) {
            uint8_t c = g.hand[1][i];
            if (pk_suit(c) != live) continue;
            if (pk_rank(c) == PK_R_SKIP && skip == PK_CARD_NONE) skip = c;
            else if (pk_is_number(c) && other == PK_CARD_NONE) other = c;
        }
        if (skip == PK_CARD_NONE || other == PK_CARD_NONE) continue;
        found = 1;
        CHECK(pk_apply(&g, 1, DRAW), "seat 1 draws, holding two plays (D6)");
        PkBelief b;
        pk_belief_build(&b, &g, 0);
        CHECK(b.soft_n[1] == 1 && b.soft[1][0].k == 7 && (b.soft[1][0].suits >> live & 1),
              "a soft void on the live suit over the 7 cards held");
        CHECK(!(b.distrust >> 1 & 1u), "trusted so far");
        CHECK(!pk_belief_allows(&b, 1, 0, other, 1) && pk_belief_allows(&b, 1, 0, other, 0),
              "the soft void forbids only when soft voids are asked for");
        int in_l = 0, free_l = 0;
        for (uint64_t ws = 1; ws <= 40; ws++) {
            PkGame w;
            if (pk_belief_sample(&b, &g, ws, 1, &w)) continue;
            for (int i = 0; i < 7; i++) in_l += pk_suit(w.hand[1][i]) == live;
            pk_belief_sample(&b, &g, ws, 0, &w);
            for (int i = 0; i < 7; i++) free_l += pk_suit(w.hand[1][i]) == live;
        }
        CHECK(in_l == 0 && free_l > 0, "worlds keep the live suit out of the 7 bound slots (%d), "
              "and only when asked (%d without)", in_l, free_l);
        CHECK(pk_apply(&g, 1, PLAYW(pos_of(&g, 1, skip), PK_NO_SUIT)) && g.turn == 1,
              "the Skip: the turn comes straight back (2 players)");
        pk_belief_build(&b, &g, 0);
        CHECK(!(b.distrust >> 1 & 1u), "one forbidden play may be the drawn card");
        CHECK(pk_apply(&g, 1, PLAYW(pos_of(&g, 1, other), PK_NO_SUIT)), "the second card of the suit");
        pk_belief_build(&b, &g, 0);
        CHECK(b.distrust >> 1 & 1u, "two forbidden plays past one draw: the void was wrong");
        CHECK(b.soft_n[1] == 0, "and seat 1's soft voids are dropped");
        CHECK(pk_belief_allows(&b, 1, 0, other, 1), "a distrusted seat is never constrained softly");
    }
    CHECK(found, "a deal with a Skip and a number of the live suit for seat 1");
}

/* ---- 3b. "Last card!" and "Caught you!" (D60) ---------------------------------------
 *
 * Real games (greedy, 3 and 4 players) stopped right after a seal leaves a
 * seat exposed. Before that seat speaks, every other seat's bot calls it
 * out, and nobody else; the exposed seat's own bot says it. */
static void t_say_and_call(void)
{
    TEST("D60: an exposed seat is called by the others and says it itself");
    int found = 0;
    uint64_t rs = 11;
    for (uint32_t gi = 0; gi < 2000 && found < 10 * SCALE; gi++) {
        int n = 3 + (int)(gi % 2);
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, 60000 + gi);
        pk_new(&g, seed, n);
        PkBotKnobs k;
        pk_bot_knobs_default(&k);
        for (int guard = 0; guard < 400 && !g.over; guard++) {
            int t = g.turn;
            PkAct m[PK_BOT_MENU_CAP];
            int nt = pk_legal_turn(&g, t, m, PK_BOT_MENU_CAP);
            if (nt <= 0) break;
            PkAct a = m[pk_bot_greedy(&g, t, m, nt)];
            pk_apply(&g, t, a);
            if (a.kind == PK_A_DRAW) continue;
            pk_seal(&g);
            if (!g.exposed || g.over) continue;
            int x = 0;
            while (!(g.exposed >> x & 1)) x++;
            found++;
            for (int s = 0; s < n; s++) {
                PkBotMove mv;
                int got = pk_bot_choose(&g, s, PK_BOT_GREEDY, &k, &rs, &mv);
                if (s == x) CHECK(got && mv.what == PK_BOT_ACT && mv.act.kind == PK_A_SAY_IT, "seat %d says it", s);
                else CHECK(got && mv.what == PK_BOT_ACT && mv.act.kind == PK_A_CALL_OUT && mv.act.a == x,
                           "seat %d calls seat %d", s, x);
            }
            /* a seat that is not exposed is never called */
            PkGame c = g;
            PkBotMove mv;
            pk_apply(&c, x, SAY);
            pk_seal(&c);
            for (int s = 0; s < n; s++) {
                int got = pk_bot_choose(&c, s, PK_BOT_GREEDY, &k, &rs, &mv);
                if (s != c.turn) CHECK(!got, "after the say, seat %d has nothing to send out of turn", s);
            }
            break;
        }
    }
    CHECK(found == 10 * SCALE, "exposures found: %d", found);
}

/* ---- 4. determinism ---------------------------------------------------------------- */

static void t_deterministic(void)
{
    TEST("determinism: the same seed plays the same game");
    for (int n = 2; n <= 5; n += 3) {
        uint64_t h[2];
        int moves[2];
        for (int rep = 0; rep < 2; rep++) {
            PkGame g;
            uint8_t seed[32];
            seed_wide(seed, (uint32_t)(40 + n));
            pk_new(&g, seed, n);
            uint8_t strat[PK_MAX_SEATS];
            PkBotKnobs k[PK_MAX_SEATS];
            for (int s = 0; s < n; s++) { strat[s] = (uint8_t)(s % PK_BOT_COUNT); k[s] = fast_knobs(); }
            strat[0] = PK_BOT_MC;
            uint64_t rs = 42;
            int r;
            while ((r = pk_bot_round(&g, strat, k, &rs)) > 0) { }
            h[rep] = pk_hash(&g);
            moves[rep] = g.hist_n;
        }
        CHECK(h[0] == h[1] && moves[0] == moves[1], "n %d: two runs, one game", n);
    }
    /* one position, asked twice with the same rng state */
    PkGame g;
    uint8_t seed[32];
    seed_wide(seed, 7);
    pk_new(&g, seed, 3);
    PkBotKnobs k = fast_knobs();
    PkBotMove a, b;
    uint64_t r1 = 5, r2 = 5;
    pk_bot_choose(&g, g.turn, PK_BOT_MC, &k, &r1, &a);
    pk_bot_choose(&g, g.turn, PK_BOT_MC, &k, &r2, &b);
    CHECK(memcmp(&a, &b, sizeof a) == 0 && r1 == r2, "one position, one move");
}

/* ---- 5. the wild's suit: the suit held most ---------------------------------------- */

/* Real openings: seat 1 moves first holding a Wild (no Wild +4, whose turn
 * comes straight back at two players and changes the question), nothing
 * suited plays, and five or more of the other cards are one suit. Name any
 * other suit and the next turn is a draw; that suit is obviously right.
 * (Found by search over real deals: the belief is built from the history,
 * so a position must have one.) */
static void t_wild_held_most(void)
{
    TEST("wild suit: the suit held most");
    int found = 0, mc_right = 0, greedy_right = 0;
    int want = 6 + 2 * SCALE;
    for (uint32_t k = 0; k < 200000 && found < want; k++) {
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, k);
        pk_new(&g, seed, 2);
        PkAct m[PK_BOT_MENU_CAP];
        int n = pk_legal_turn(&g, 1, m, PK_BOT_MENU_CAP), plays = 0, suited = 0;
        for (int i = 0; i < n; i++)
            if (m[i].kind == PK_A_PLAY) { plays++; suited += !pk_is_wild(g.hand[1][m[i].a]); }
        if (!plays || suited) continue;
        int cnt[PK_SUITS] = { 0 }, best = 0;
        for (int i = 0; i < g.hand_n[1]; i++) if (!pk_is_wild(g.hand[1][i])) cnt[pk_suit(g.hand[1][i])]++;
        for (int s = 1; s < PK_SUITS; s++) if (cnt[s] > cnt[best]) best = s;
        /* no Wild +4, and no action card off the suit: a Skip of another
         * suit can chain through a rank match, which is a different plan */
        int plain = 1;
        for (int i = 0; i < g.hand_n[1]; i++) {
            uint8_t c = g.hand[1][i];
            if (pk_rank(c) == PK_R_WILD4) plain = 0;
            if (!pk_is_wild(c) && !pk_is_number(c) && pk_suit(c) != best) plain = 0;
        }
        if (!plain || cnt[best] < 5) continue;
        found++;
        PkBotKnobs kk = strong_knobs();
        PkBotMove mv;
        uint64_t rs = k + 1;
        pk_bot_choose(&g, 1, PK_BOT_MC, &kk, &rs, &mv);
        mc_right += mv.act.kind == PK_A_PLAY && mv.act.b == best;
        int gp = pk_bot_greedy(&g, 1, m, n);
        greedy_right += m[gp].kind == PK_A_PLAY && m[gp].b == best;
    }
    CHECK(found == want, "openings found: %d", found);
    CHECK(greedy_right == found, "greedy names the suit held most: %d of %d", greedy_right, found);
    CHECK(mc_right == found, "MC names the suit held most: %d of %d", mc_right, found);
}

/* ---- 6. the wild's suit: toward the next seat's void --------------------------------
 *
 * Real games (greedy at every seat, 2 and 3 players) stopped wherever the
 * turn seat can play only wilds while the next seat, on two cards or fewer,
 * has a void the belief trusts on some suit. MC with the belief must name a
 * suit that seat is void in clearly more often than the same MC blind to
 * every void (PK_KNOB_NO_BELIEF). Deterministic: fixed deals, fixed seeds. */
static void t_wild_void(void)
{
    TEST("wild suit: the belief moves it toward the next seat's void");
    int found = 0, with = 0, blind = 0, greedy = 0;
    const int want = 400;
    uint64_t rs = 3;
    for (uint32_t gi = 0; gi < 200000 && found < want; gi++) {
        int n = 2 + (int)(gi % 2);
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, 900000 + gi);
        pk_new(&g, seed, n);
        uint8_t strat[PK_MAX_SEATS];
        PkBotKnobs k[PK_MAX_SEATS];
        for (int s = 0; s < n; s++) { strat[s] = PK_BOT_GREEDY; pk_bot_knobs_default(&k[s]); }
        for (int round = 0; round < 300 && !g.over && found < want; round++) {
            int t = g.turn, nx = pk_next(&g, t, 1);
            PkAct m[PK_BOT_MENU_CAP];
            int nt = pk_legal_turn(&g, t, m, PK_BOT_MENU_CAP), wild = 0, suited = 0;
            for (int i = 0; i < nt; i++)
                if (m[i].kind == PK_A_PLAY) { if (pk_is_wild(g.hand[t][m[i].a])) wild++; else suited++; }
            if (!g.b_open && wild && !suited && g.hand_n[nx] <= 2 && g.hand_n[t] > 1) {
                PkBelief b;
                pk_belief_build(&b, &g, t);
                int vs = 0;
                for (int j = 0; j < b.soft_n[nx]; j++)
                    if ((int)b.soft[nx][j].k - b.pinned_n[nx] > 0) vs |= b.soft[nx][j].suits;
                for (int j = 0; j < b.hard_n[nx]; j++)
                    if ((int)b.hard[nx][j].k - b.pinned_n[nx] > 0) vs |= b.hard[nx][j].suits;
                if (vs && vs != 15 && !(b.distrust >> nx & 1)) {
                    found++;
                    PkBotKnobs kk;
                    pk_bot_knobs_default(&kk);
                    PkBotMove mv;
                    uint64_t r = rs;
                    pk_bot_choose(&g, t, PK_BOT_MC, &kk, &r, &mv);
                    with += mv.act.kind == PK_A_PLAY && mv.act.b < PK_SUITS && (vs >> mv.act.b & 1);
                    kk.flags = PK_KNOB_NO_BELIEF;
                    r = rs;
                    pk_bot_choose(&g, t, PK_BOT_MC, &kk, &r, &mv);
                    blind += mv.act.kind == PK_A_PLAY && mv.act.b < PK_SUITS && (vs >> mv.act.b & 1);
                    int gp = pk_bot_greedy(&g, t, m, nt);
                    greedy += m[gp].b < PK_SUITS && (vs >> m[gp].b & 1);
                }
            }
            if (pk_bot_round(&g, strat, k, &rs) <= 0) break;
        }
    }
    CHECK(found == want, "positions found: %d", found);
    printf("  wild void: at %d positions MC named a void suit %d times, blind MC %d, greedy %d\n",
           found, with, blind, greedy);
    CHECK(with - blind >= want / 12, "with the belief %d, blind %d: want %d more", with, blind, want / 12);
}

/* ---- 7. MC beats random at a forced choice ---------------------------------------
 *
 * Real games (greedy at every seat) stopped where the choice matters: the
 * next seat is down to one card, and the turn seat holds both a card that
 * hits it (a +2, a Wild +4, a Skip, or a Reverse at two players) and one that
 * does not. From each such position the TRUE game goes on twice, once after
 * MC's choice and once after a uniformly random legal one, greedy for every
 * seat after that. MC's choices must win clearly more often. */
static int finish_greedy(PkGame *g, int me)
{
    PkAct m[PK_BOT_MENU_CAP];
    for (int guard = 0; guard < 4 * PK_MAX_ACTIONS && !g->over; guard++) {
        int seat = g->b_open ? g->b_sender : g->turn;
        int n = pk_legal_turn(g, seat, m, PK_BOT_MENU_CAP);
        if (n == 0) { if (!pk_seal(g)) break; continue; }
        PkAct a = m[pk_bot_greedy(g, seat, m, n)];
        if (!pk_apply(g, seat, a)) break;
        if (a.kind != PK_A_DRAW && pk_can_seal(g)) pk_seal(g);
    }
    return g->over && g->winner == me;
}

static int hits(const PkGame *g, int seat, PkAct a)
{
    if (a.kind != PK_A_PLAY) return 0;
    int r = pk_rank(g->hand[seat][a.a]);
    return r == PK_R_PLUS2 || r == PK_R_WILD4 || r == PK_R_SKIP || (r == PK_R_REVERSE && g->n == 2);
}

static void t_mc_beats_random(void)
{
    TEST("MC beats random at a forced choice");
    int positions = 0, mc_wins = 0, rnd_wins = 0, mc_hit = 0, greedy_hit = 0;
    double rnd_hit = 0;
    int want = 25 * SCALE;
    uint64_t rs = 99;
    for (uint32_t gi = 0; gi < 200000 && positions < want; gi++) {
        int n = gi % 2 ? 4 : 2;
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, 500000 + gi);
        pk_new(&g, seed, n);
        uint8_t strat[PK_MAX_SEATS];
        PkBotKnobs k[PK_MAX_SEATS];
        for (int s = 0; s < n; s++) { strat[s] = PK_BOT_GREEDY; k[s] = fast_knobs(); }
        for (int round = 0; !g.over && round < 400 && positions < want; round++) {
            int t = g.turn;
            PkAct m[PK_BOT_MENU_CAP];
            int nt = pk_legal_turn(&g, t, m, PK_BOT_MENU_CAP), hit = 0, soft = 0;
            for (int i = 0; i < nt; i++) {
                if (hits(&g, t, m[i])) hit++;
                else if (m[i].kind == PK_A_PLAY) soft++;
            }
            if (!g.b_open && g.hand_n[pk_next(&g, t, 1)] == 1 && g.hand_n[t] > 1 && hit && soft) {
                PkBotKnobs kk;
                pk_bot_knobs_default(&kk);
                PkBotMove mv;
                pk_bot_choose(&g, t, PK_BOT_MC, &kk, &rs, &mv);
                if (mv.what == PK_BOT_ACT && mv.act.kind <= PK_A_PASS) {
                    static PkGame a, b;
                    a = g;
                    b = g;
                    pk_apply(&a, t, mv.act);
                    if (mv.act.kind != PK_A_DRAW && pk_can_seal(&a)) pk_seal(&a);
                    PkAct x = m[pk_bot_rand(&rs) % (uint64_t)nt];
                    pk_apply(&b, t, x);
                    if (x.kind != PK_A_DRAW && pk_can_seal(&b)) pk_seal(&b);
                    mc_hit += hits(&g, t, mv.act);
                    greedy_hit += hits(&g, t, m[pk_bot_greedy(&g, t, m, nt)]);
                    rnd_hit += (double)hit / nt;
                    mc_wins += finish_greedy(&a, t);
                    rnd_wins += finish_greedy(&b, t);
                    positions++;
                }
            }
            if (pk_bot_round(&g, strat, k, &rs) <= 0) break;
        }
    }
    CHECK(positions == want, "positions: %d", positions);
    printf("  MC vs random at %d forced choices: MC hit the one-card seat %d times, greedy %d, random would %.1f; "
           "the moves went on to win %d and %d\n", positions, mc_hit, greedy_hit, rnd_hit, mc_wins, rnd_wins);
    CHECK(mc_hit >= positions * 3 / 4, "MC hits the one-card seat: %d of %d", mc_hit, positions);
    CHECK(greedy_hit >= positions * 3 / 4, "greedy hits the one-card seat: %d of %d", greedy_hit, positions);
    CHECK(mc_hit >= 2 * rnd_hit, "MC hits %d, random %.1f: want twice", mc_hit, rnd_hit);

    /* and whole games, two players, each seed both ways round */
    TEST("MC beats random over whole games");
    int games = 10 * SCALE, won = 0;
    for (int i = 0; i < games; i++) {
        PkGame g;
        uint8_t seed[32];
        seed_wide(seed, 800000 + (uint32_t)(i / 2));
        pk_new(&g, seed, 2);
        int mc = i & 1;
        uint8_t strat[PK_MAX_SEATS] = { 0 };
        PkBotKnobs k[PK_MAX_SEATS];
        for (int s = 0; s < 2; s++) pk_bot_knobs_default(&k[s]);
        strat[mc] = PK_BOT_MC;
        strat[1 - mc] = PK_BOT_RANDOM;
        int r;
        while ((r = pk_bot_round(&g, strat, k, &rs)) > 0) { }
        won += r == 0 && g.winner == mc;
    }
    printf("  MC vs random over %d whole games: MC won %d\n", games, won);
    CHECK(won * 10 >= games * 8, "MC won %d of %d, want 80%%", won, games);
}

int main(int argc, char **argv)
{
    if (argc > 1) SCALE = atoi(argv[1]) > 0 ? atoi(argv[1]) : 1;
    t_legal_everywhere();
    t_drawout_bare_pass();
    t_soft_void_distrust();
    t_say_and_call();
    t_deterministic();
    t_wild_held_most();
    t_wild_void();
    t_mc_beats_random();
    return report("pk_bot_test");
}
