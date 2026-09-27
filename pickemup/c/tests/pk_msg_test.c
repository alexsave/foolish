/* THE WIRE: the body coder and the envelope (RULES_AND_KERNEL.md 7.4, 7.7.4,
 * 7.8.7), and the two sweeps that prove a hostile buffer cannot make decode
 * read past its end or crash (run under ASan by `make asan`).
 *
 *     ./build/pk_msg_test [games per table size] [size-case games]
 *
 * `make run` plays 30 games at each of 2..8 players and encodes, decodes and
 * re-encodes after EVERY bubble of every one; the rules doc's 10,000 a size
 * is `./build/pk_msg_test 10000` (DECISION D43). */
#include "pk_check.h"
#include "../src/pk_code.h"
#include "../src/pk_msg.h"
#include "../../../shared/c/b32.h"
#include "../../../shared/c/sha256.h"

/* ---- building messages ------------------------------------------------------ */

static const char *NAMES[PK_MAX_SEATS] = { "Alex", "Bo", "Cleo", "Dev", "Esme", "Finn", "Gus", "Hana" };

static void id_tag(const uint8_t seed[32], const char *id, uint8_t tag[PK_TAG_LEN])
{
    pk_tag(seed, (const uint8_t *)id, (int)strlen(id), tag);
}

static const uint8_t *U(const char *s) { return (const uint8_t *)s; }
static int L(const char *s) { return (int)strlen(s); }

/* A lobby of `n` (DM when n == 2 and dm), everybody joined, not started. */
static void lobby_of(PkMsg *m, const uint8_t seed[32], int n, int dm)
{
    uint8_t tag[PK_TAG_LEN];
    id_tag(seed, "id-Alex", tag);
    pk_msg_new(m, seed, dm, tag, U(NAMES[0]), L(NAMES[0]));
    for (int s = 1; s < n; s++) {
        char id[16] = "id-";
        memcpy(id + 3, NAMES[s], strlen(NAMES[s]) + 1);
        id_tag(seed, id, tag);
        pk_msg_join(m, tag, U(NAMES[s]), L(NAMES[s]));
    }
}

/* ...and started by seat 0. */
static int started_of(PkMsg *m, const uint8_t seed[32], int n)
{
    lobby_of(m, seed, n, 0);
    return pk_msg_start(m, 0) == PK_EOK;
}

static int text_len(const PkMsg *m)
{
    static char t[PK_MSG_MAX_TEXT];
    return pk_msg_text_encode(m, t, (int)sizeof t);
}

/* ---- the size table ----------------------------------------------------------- */

static int cmp_i(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

typedef struct { int n, cap; int *v; } Sizes;

static void sizes_add(Sizes *s, int x)
{
    if (s->n < s->cap) s->v[s->n++] = x;
}

static int pct(Sizes *s, double p)
{
    if (!s->n) return 0;
    int i = (int)(p * (s->n - 1) + 0.5);
    return s->v[i];
}

/* ---- 7.4.1 round trip, 7.4.5 sizes ----------------------------------------------- */

static int sizes_mem[PK_MAX_SEATS + 1][400000];
static Sizes by_n[PK_MAX_SEATS + 1];

static void round_trip(int games)
{
    static PkMsg m, back, again;
    static uint8_t b1[PK_MSG_MAX_BYTES], b2[PK_MSG_MAX_BYTES];
    static char t1[PK_MSG_MAX_TEXT];
    long bubbles = 0;
    for (int n = 2; n <= PK_MAX_SEATS; n++) {
        by_n[n].v = sizes_mem[n];
        by_n[n].cap = (int)(sizeof sizes_mem[n] / sizeof sizes_mem[n][0]);
    }
    for (int gi = 0; gi < games * (PK_MAX_SEATS - 1); gi++) {
        int n = 2 + gi % (PK_MAX_SEATS - 1);
        uint8_t seed[32];
        seed_wide(seed, 700000u + (uint32_t)gi);
        TEST("7.4.1 round trip");
        CHECK(started_of(&m, seed, n), "game %d starts", gi);
        int last = -1;
        for (int step = 0; step < 20000 && !(m.game.over && !m.game.b_open); step++) {
            if (m.game.b_open || (int)m.game.bubbles == last) {
                if (!bot_step(&m.game)) break;
                if (m.game.b_open) continue;
            }
            last = m.game.bubbles;
            bubbles++;
            int n1 = pk_msg_encode(&m, b1, (int)sizeof b1);
            CHECK(n1 > 0, "game %d bubble %d encodes (%d)", gi, last, n1);
            if (n1 <= 0) break;
            int e = pk_msg_decode(b1, n1, &back);
            CHECK(e == PK_EOK, "game %d bubble %d decodes (%d)", gi, last, e);
            CHECK(pk_hash(&back.game) == pk_hash(&m.game) && conserved(&back.game),
                  "game %d bubble %d: the decoded game is the game", gi, last);
            CHECK(back.phase == (m.game.over ? PK_PHASE_FINISHED : PK_PHASE_LIVE),
                  "game %d bubble %d: phase", gi, last);
            int n2 = pk_msg_encode(&back, b2, (int)sizeof b2);
            CHECK(n2 == n1 && !memcmp(b1, b2, (size_t)n1), "game %d bubble %d re-encodes byte-identical", gi, last);
            int tl = pk_msg_text_encode(&m, t1, (int)sizeof t1);
            CHECK(tl == 3 + B32_LEN(n1) && pk_msg_text_decode(t1, &again) == PK_EOK
                  && pk_hash(&again.game) == pk_hash(&m.game), "game %d bubble %d: the text round trips", gi, last);
            sizes_add(&by_n[n], tl);
        }
        CHECK(m.game.over, "game %d ended", gi);
    }
    printf("round trip: %d games, %ld bubbles each encoded, decoded and re-encoded\n",
           games * (PK_MAX_SEATS - 1), bubbles);

    TEST("7.4.5 size gate");
    printf("  link characters per bubble, by table size (median / p95 / p99 / p99.9 / max):\n");
    for (int n = 2; n <= PK_MAX_SEATS; n++) {
        Sizes *s = &by_n[n];
        qsort(s->v, (size_t)s->n, sizeof(int), cmp_i);
        printf("    %dp  %5d  %5d  %5d  %5d  %5d   (%d bubbles)\n", n, pct(s, 0.5), pct(s, 0.95),
               pct(s, 0.99), pct(s, 0.999), s->n ? s->v[s->n - 1] : 0, s->n);
    }
    CHECK(pct(&by_n[8], 0.95) < 1000, "p95 at 8 players is %d characters, the guardrail is 1,000",
          pct(&by_n[8], 0.95));
}

/* THE OWNER'S p99 CASE (4.5): 8 players, 40 turns, six draws a turn, ten
 * catches. Measured at the fortieth turn over `games` deals. */
static void many_draws(int games)
{
    static PkMsg m;
    static int len[20000];
    int k = 0;
    TEST("7.4.5 size gate: 8p, 40 turns, 6 draws each, 10 catches");
    for (int gi = 0; gi < games && k < 20000; gi++) {
        uint8_t seed[32];
        seed_wide(seed, 810000u + (uint32_t)gi);
        CHECK(started_of(&m, seed, 8), "deal %d starts", gi);
        PkGame *g = &m.game;
        int catches = 0;
        while (g->turns < 40 && !g->over) {
            /* a catch out of turn every fourth turn, ten in all */
            if (catches < 10 && g->turns % 4 == 1) {
                int o = pk_next(g, g->turn, 3);
                for (int t = 0; t < g->n; t++) {
                    PkAct c = { PK_A_CALL_OUT, (uint8_t)((o + 1 + t) % g->n), 0, 0 };
                    if (pk_apply(g, o, c)) { pk_seal(g); catches++; break; }
                }
            }
            int s = g->turn;
            PkAct d = { PK_A_DRAW, 0, 0, 0 };
            for (int i = 0; i < 6 && !g->over && pk_apply(g, s, d); i++) {}
            if (g->over) { pk_seal(g); break; }
            PkAct menu[PK_HAND_CAP * 4 + 2];
            int nm = pk_legal_turn(g, s, menu, (int)(sizeof menu / sizeof menu[0]));
            int pick = nm - 1;                           /* PASS, after the draws */
            for (int i = 0; i < nm; i++)
                if (menu[i].kind == PK_A_PLAY && rnd(100) < 60) { pick = i; break; }
            pk_apply(g, s, menu[pick]);
            if (pk_can_seal(g) && pk_turn_ended(g)) pk_seal(g);
            else if (pk_can_seal(g)) pk_seal(g);
        }
        int tl = text_len(&m);
        CHECK(tl > 0, "deal %d encodes", gi);
        len[k++] = tl;
    }
    qsort(len, (size_t)k, sizeof(int), cmp_i);
    int p99 = len[(int)(0.99 * (k - 1) + 0.5)], mx = len[k - 1];
    printf("  8p, 40 turns, 6 draws each, 10 catches: median %d, p99 %d, max %d characters (%d deals)\n",
           len[k / 2], p99, mx, k);
    CHECK(mx < 1000, "the owner's p99 case peaks at %d characters, the guardrail is 1,000", mx);
}

/* A game at `n` seats with every name at its 48-byte cap (sixteen three-byte
 * characters), played the long way: each turn draws once when it can and
 * then plays the first card it can, else passes, so a hand never shrinks
 * and nobody goes out; and at two players every turn that comes straight
 * back is taken in the same bubble (D7). Returns the link length. */
static int long_game(PkMsg *m, int n, uint32_t k)
{
    static char t[PK_MSG_MAX_TEXT];
    static PkMsg back;
    uint8_t seed[32], tag[PK_TAG_LEN];
    seed_wide(seed, k);
    char big[PK_MAX_SEATS][64];
    for (int s = 0; s < n; s++) {
        for (int i = 0; i < 16; i++) {
            big[s][3 * i] = (char)0xE3; big[s][3 * i + 1] = (char)0x81;
            big[s][3 * i + 2] = (char)(0x81 + (i + s) % 60);
        }
        big[s][48] = 0;
        char id[8] = { 'i', 'd', (char)('0' + s), 0 };
        id_tag(seed, id, tag);
        if (s == 0) CHECK(pk_msg_new(m, seed, 0, tag, U(big[0]), 48) == PK_EOK, "a 48-byte name creates");
        else CHECK(pk_msg_join(m, tag, U(big[s]), 48) == s, "a 48-byte name joins at %d", s);
    }
    CHECK(pk_msg_start(m, 0) == PK_EOK, "%d start", n);
    PkGame *g = &m->game;
    while (!g->over) {
        const int s = g->turn;
        for (;;) {
            PkAct d = { PK_A_DRAW, 0, 0, 0 };
            pk_apply(g, s, d);
            if (g->over) break;
            PkAct menu[PK_HAND_CAP * 4 + 2];
            int nm = pk_legal_turn(g, s, menu, (int)(sizeof menu / sizeof menu[0]));
            int pick = nm - 1;
            for (int i = 0; i < nm; i++) if (menu[i].kind == PK_A_PLAY) { pick = i; break; }
            if (nm <= 0 || !pk_apply(g, s, menu[pick])) break;
            if (pk_turn_ended(g)) break;             /* else the turn came back */
        }
        CHECK(pk_can_seal(g), "a turn seals (actions %d)", g->actions);
        if (!pk_seal(g)) break;
    }
    int tl = pk_msg_text_encode(m, t, (int)sizeof t);
    CHECK(tl > 0 && pk_msg_text_decode(t, &back) == PK_EOK && pk_hash(&back.game) == pk_hash(g),
          "the long %dp game seals and decodes (%d)", n, tl);
    return tl;
}

/* 7.4.6: the caps are rules - a game driven to the 1,500-action stop ends
 * PK_OVER_LONG with the fewest cards winning, and still seals and decodes.
 * And the longest links: every name at its cap. */
static void caps_are_rules(void)
{
    static PkMsg m;
    TEST("7.4.6 caps are rules");
    int tl = long_game(&m, 2, 4242);
    PkGame *g = &m.game;
    CHECK(g->over == PK_OVER_LONG && g->actions == PK_MAX_ACTIONS,
          "the game stops at the action cap: over %d after %d actions, %d bubbles", g->over, g->actions, g->bubbles);
    int fewest = 0;
    for (int s = 1; s < g->n; s++) if (g->hand_n[s] < g->hand_n[fewest]) fewest = s;
    CHECK(g->hand_n[g->winner] == g->hand_n[fewest], "the fewest cards win");
    printf("  the 1,500-action stop at 2p: %d bubbles, %d characters\n", g->bubbles, tl);
    TEST("7.4.5 size gate: the capped worst case");
    tl = long_game(&m, 8, 4243);
    CHECK(tl < 5000, "the longest 8p game is %d characters, MSMessage.url holds 5,000", tl);
    printf("  the longest 8p game, eight 48-byte names: %d actions, %d bubbles, %d characters; "
           "the analytic bound is %d\n", m.game.actions, m.game.bubbles, tl, PK_MSG_MAX_TEXT - 1);
}

/* ---- 7.4.2 canonicality, 7.4.3 header agreement, 7.4.4 check, the sweeps ---- */

/* A pool of valid envelopes: lobbies of every size and state, and live games
 * at several points, from a few seeds. */
#define POOL 64
static uint8_t pool[POOL][PK_MSG_MAX_BYTES];
static int     pool_n[POOL], npool;

static void pool_add(const PkMsg *m)
{
    if (npool >= POOL) return;
    int n = pk_msg_encode(m, pool[npool], PK_MSG_MAX_BYTES);
    if (n > 0) pool_n[npool++] = n;
}

static void build_pool(void)
{
    static PkMsg m;
    for (int k = 0; k < 8; k++) {
        uint8_t seed[32];
        seed_wide(seed, 5000u + (uint32_t)k);
        int n = 2 + k % 7;
        lobby_of(&m, seed, n, n == 2 && k % 2);
        pool_add(&m);
        if (n > 2) { pk_msg_leave(&m, 1); pool_add(&m); lobby_of(&m, seed, n, 0); }
        pk_msg_start(&m, 0);
        pool_add(&m);
        for (int step = 0, stop = 30 + 60 * k; step < 20000 && !(m.game.over && !m.game.b_open); step++) {
            bot_step(&m.game);
            if (!m.game.b_open && m.game.bubbles >= stop) { pool_add(&m); stop += 90; }
        }
        pool_add(&m);
    }
}

/* Decode `in` and, if it is a message, re-encode it: it must be exactly
 * its own bytes. 1 if refused or canonical, 0 if it read as something that
 * writes differently. */
static int refused_or_canonical(const uint8_t *in, int n, int *valid)
{
    static PkMsg m;
    static uint8_t again[PK_MSG_MAX_BYTES];
    int e = pk_msg_decode(in, n, &m);
    *valid = e == PK_EOK;
    if (e != PK_EOK) return e < 0 && e >= PK_ETEXT;
    int n2 = pk_msg_encode(&m, again, (int)sizeof again);
    return n2 == n && !memcmp(again, in, (size_t)n);
}

/* Where the check sits in an envelope (after the roster). */
static int check_at(const uint8_t *in)
{
    int at = PK_HEAD_LEN;
    for (int s = 0; s < in[42]; s++) at += PK_TAG_LEN + 1 + in[at + PK_TAG_LEN];
    return at;
}

/* Recompute the check where the PRISTINE envelope keeps it (`at`), so a
 * corruption reaches the semantics behind the check. The corrupted bytes
 * may say the check is somewhere else; the decoder finds that out. */
static void recheck(uint8_t *in, int n, int at)
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, in, (size_t)at);
    sha256_update(&c, in + at + PK_CHECK_LEN, (size_t)(n - at - PK_CHECK_LEN));
    sha256_final(&c, d);
    memcpy(in + at, d, PK_CHECK_LEN);
}

/* THE BYTES UNDER TEST SIT AT THE VERY END OF A STATIC ARRAY, so the first
 * byte past them is the array's end and ASan's redzone: a decoder that reads
 * one byte too far is a report, not a quiet read of the next byte. */
static uint8_t *at_end(uint8_t *arr, int arr_len, const uint8_t *src, int n)
{
    uint8_t *p = arr + (arr_len - n);
    memcpy(p, src, (size_t)n);
    return p;
}

static void tamper(void)
{
    static uint8_t area[PK_MSG_MAX_BYTES];
    uint8_t *buf;
    long flips = 0, valid_after = 0, body_flips = 0;
    TEST("7.4.2 canonicality: every bit flipped");
    for (int p = 0; p < npool; p++) {
        int n = pool_n[p];
        for (int bit = 0; bit < n * 8; bit++) {
            buf = at_end(area, (int)sizeof area, pool[p], n);
            buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            int v;
            flips++;
            CHECK(refused_or_canonical(buf, n, &v), "envelope %d bit %d reads as a different writing", p, bit);
            valid_after += v;
            /* ...and again with the check recomputed, so the flip reaches the
             * header's semantics and the body decoder instead of stopping at
             * the check */
            if (bit / 8 >= check_at(pool[p]) && bit / 8 < check_at(pool[p]) + PK_CHECK_LEN) continue;
            recheck(buf, n, check_at(pool[p]));
            body_flips++;
            CHECK(refused_or_canonical(buf, n, &v), "envelope %d bit %d (re-checked) reads as a different writing", p, bit);
            valid_after += v;
        }
    }
    printf("tamper: %d envelopes, %ld raw flips and %ld re-checked flips, %ld read as a game "
           "(each re-encoding to itself)\n", npool, flips, body_flips, valid_after);

    TEST("the corruption sweep: every byte, every value");
    long bytes = 0;
    for (int p = 0; p < npool; p += 7) {
        int n = pool_n[p];
        for (int i = 0; i < n; i++)
            for (int x = 1; x < 256; x += 17) {
                buf = at_end(area, (int)sizeof area, pool[p], n);
                buf[i] ^= (uint8_t)x;
                recheck(buf, n, check_at(pool[p]));
                int v;
                bytes++;
                CHECK(refused_or_canonical(buf, n, &v), "envelope %d byte %d ^ %02x", p, i, x);
            }
    }
    printf("corruption: %ld corrupted envelopes (check recomputed), none crashed or misread\n", bytes);

    TEST("7.4.4 check: every truncation");
    long cuts = 0;
    for (int p = 0; p < npool; p++) {
        int n = pool_n[p], body = check_at(pool[p]) + PK_CHECK_LEN;
        for (int k = 0; k < n; k++) {
            uint8_t *cut = at_end(area, (int)sizeof area, pool[p], k);
            static PkMsg m;
            int e = pk_msg_decode(cut, k, &m);
            cuts++;
            CHECK(e < 0, "envelope %d cut to %d of %d bytes still decodes", p, k, n);
            if (k >= body) CHECK(e == PK_ECHECK || e == PK_EGAME, "envelope %d body cut to %d: %d", p, k, e);
        }
    }
    printf("truncation: %ld prefixes of %d envelopes, every one refused\n", cuts, npool);
}

static void header_agreement(void)
{
    static PkMsg m, back;
    static uint8_t b[PK_MSG_MAX_BYTES];
    TEST("7.4.3 header agreement");
    uint8_t seed[32];
    seed_wide(seed, 31337);
    started_of(&m, seed, 3);
    /* play until a bubble that said "Last card!" is the tip, so every
     * derived field has a value to lie about */
    for (int step = 0; step < 20000 && !m.game.over; step++) {
        bot_step(&m.game);
        if (!m.game.b_open && m.game.bubbles > 20 && m.game.hist_n) break;
    }
    int n = pk_msg_encode(&m, b, (int)sizeof b);
    CHECK(n > 0 && pk_msg_decode(b, n, &back) == PK_EOK, "the honest one decodes");
    static uint8_t c[PK_MSG_MAX_BYTES];
    struct { int at; int delta; const char *what; } lie[] = {
        { 40, 1, "turns + 1" }, { 40, -1, "turns - 1" },
        { 38, 1, "bubbles + 1" }, { 38, -1, "bubbles - 1" },
        { 2, 1, "phase FINISHED on a live game" },
        { 3, PK_FLAG_TIP_SAID, "TIP_SAID flipped" },
    };
    for (int i = 0; i < (int)(sizeof lie / sizeof lie[0]); i++) {
        memcpy(c, b, (size_t)n);
        if (lie[i].at == 3) c[3] ^= (uint8_t)lie[i].delta;
        else c[lie[i].at] = (uint8_t)(c[lie[i].at] + lie[i].delta);
        recheck(c, n, check_at(b));
        CHECK(pk_msg_decode(c, n, &back) == PK_EGAME, "a header that says %s is refused", lie[i].what);
    }
    /* a reserved flag, a format from the future, a lobby with a body */
    memcpy(c, b, (size_t)n); c[3] |= 0x80; recheck(c, n, check_at(b));
    CHECK(pk_msg_decode(c, n, &back) == PK_EFLAGS, "a reserved flag is refused");
    memcpy(c, b, (size_t)n); c[1] = 2;
    CHECK(pk_msg_decode(c, n, &back) == PK_EFORMAT, "a newer format is refused, never misread");
    memcpy(c, b, (size_t)n); c[0] = 0xB7;
    CHECK(pk_msg_decode(c, n, &back) == PK_EMAGIC, "somebody else's magic");
}

/* One plain turn by the turn seat, sealed: the first play on the menu, else
 * one draw and then the first play or a pass. 1, or 0 if nothing moved. */
static int plain_turn(PkGame *g)
{
    if (g->over || g->b_open) return 0;
    const int s = g->turn;
    for (int guard = 0; guard < 8; guard++) {
        PkAct menu[PK_HAND_CAP * 4 + 2];
        int nm = pk_legal_turn(g, s, menu, (int)(sizeof menu / sizeof menu[0]));
        if (nm <= 0) return 0;
        int pick = -1;
        for (int i = 0; i < nm && pick < 0; i++) if (menu[i].kind == PK_A_PLAY) pick = i;
        if (pick < 0) pick = menu[0].kind == PK_A_DRAW && !g->t_drew ? 0 : nm - 1;
        if (!pk_apply(g, s, menu[pick])) return 0;
        if (menu[pick].kind != PK_A_DRAW || g->over) return pk_seal(g);
    }
    return 0;
}

/* ---- 7.7.4 Rule P ------------------------------------------------------------------ */

static int sgn(int x) { return (x > 0) - (x < 0); }

static void rule_p(void)
{
    static PkMsg parent, a, b, c;
    TEST("7.7.4 Rule P races");
    /* Races are built from real play: deals where somebody plays down to
     * one card, and then two answers to that same bubble. */
    int races = 0;
    for (uint32_t k = 0; races < 16 && k < 4000; k++) {
        uint8_t seed[32];
        seed_wide(seed, 61000u + k);
        started_of(&parent, seed, 3);
        PkGame *g = &parent.game;
        /* play plain turns until somebody is exposed after their seal */
        for (int guard = 0; !g->over && !g->exposed && guard < 400; guard++)
            if (!plain_turn(g)) break;
        if (g->over || !g->exposed) continue;
        int x = 0;
        while (!(g->exposed >> x & 1)) x++;
        int y = -1;                        /* somebody else, not the turn seat */
        for (int s = 0; s < g->n; s++) if (s != x && s != g->turn) y = s;
        if (y < 0 || x == g->turn) continue;
        races++;

        /* A: the exposed seat says it, out of turn. B: y catches them. */
        a = parent; b = parent;
        PkAct say = { PK_A_SAY_IT, 0, 0, 0 }, call = { PK_A_CALL_OUT, (uint8_t)x, 0, 0 };
        CHECK(pk_apply(&a.game, x, say) && pk_seal(&a.game), "race %d: the say", races);
        CHECK(pk_apply(&b.game, y, call) && pk_seal(&b.game), "race %d: the catch", races);
        int ab = pk_msg_prefer(&a, &b), ba = pk_msg_prefer(&b, &a);
        CHECK(ab < 0 && ba > 0, "race %d: say-it beats a catch of the same parent (%d, %d)", races, ab, ba);

        /* C: the turn seat takes its turn; B2: the catch, then a second
         * out-of-turn catch - more bubbles, fewer turns */
        c = parent;
        CHECK(plain_turn(&c.game), "race %d: the turn", races);
        PkAct call2 = { PK_A_CALL_OUT, (uint8_t)x, 0, 0 };
        int again = pk_apply(&b.game, y, call2) && pk_seal(&b.game);
        CHECK(again && b.game.bubbles == c.game.bubbles + 1 && b.game.turns + 1 == c.game.turns,
              "race %d: the catcher's chain has more bubbles and fewer turns", races);
        CHECK(pk_msg_prefer(&c, &b) < 0 && pk_msg_prefer(&b, &c) > 0,
              "race %d: a turn beats catches that answered the same parent", races);

        /* child against parent */
        CHECK(pk_msg_prefer(&parent, &a) > 0 && pk_msg_prefer(&a, &parent) < 0, "race %d: the child wins", races);
        CHECK(pk_msg_prefer(&a, &a) == 0, "race %d: a bubble against itself", races);
        CHECK(pk_common_bubbles(&a, &b) == parent.game.bubbles && pk_common_bubbles(&a, &parent) == parent.game.bubbles,
              "race %d: siblings share the parent's bubbles", races);
    }
    CHECK(races == 16, "found 16 races (%d)", races);

    /* racing Starts: the fuller, later roster */
    uint8_t seed[32];
    seed_wide(seed, 777);
    lobby_of(&a, seed, 2, 0);
    b = a;
    uint8_t tag[PK_TAG_LEN];
    id_tag(seed, "id-Cleo", tag);
    pk_msg_join(&b, tag, U("Cleo"), 4);
    CHECK(pk_msg_start(&a, 0) == PK_EOK && pk_msg_start(&b, 0) == PK_EOK, "two Starts");
    CHECK(pk_msg_prefer(&a, &b) > 0 && pk_msg_prefer(&b, &a) < 0, "racing Starts go to the fuller roster");
    lobby_of(&c, seed, 2, 0);
    CHECK(pk_msg_prefer(&c, &a) > 0 && pk_msg_prefer(&a, &c) < 0, "a started chain beats its lobby");
    seed_wide(seed, 778);
    lobby_of(&c, seed, 2, 0);
    CHECK(pk_msg_prefer(&a, &c) > 0 && pk_msg_prefer(&c, &a) > 0, "a different game: the tapped one");

    /* antisymmetric and transitive, over a pool of one game's branches */
    static PkMsg br[48];
    int nb = 0;
    seed_wide(seed, 999);
    lobby_of(&br[nb++], seed, 4, 0);
    br[nb] = br[nb - 1]; pk_msg_leave(&br[nb], 2); nb++;
    br[nb] = br[0]; pk_msg_start(&br[nb], 0); nb++;
    while (nb < 48) {
        PkMsg *from = &br[2 + rnd((uint32_t)(nb - 2))];
        if (from->game.over) { if (rnd(4) == 0) break; continue; }
        br[nb] = *from;
        int before = br[nb].game.bubbles;
        for (int s = 0; s < 400 && (br[nb].game.b_open || (int)br[nb].game.bubbles == before); s++)
            if (!bot_step(&br[nb].game)) break;
        if (!br[nb].game.b_open) nb++;
    }
    long pairs = 0;
    for (int i = 0; i < nb; i++)
        for (int j = 0; j < nb; j++) {
            int pij = pk_msg_prefer(&br[i], &br[j]), pji = pk_msg_prefer(&br[j], &br[i]);
            pairs++;
            CHECK(sgn(pij) == -sgn(pji), "branches %d and %d: antisymmetric (%d, %d)", i, j, pij, pji);
            static uint8_t ei[PK_MSG_MAX_BYTES], ej[PK_MSG_MAX_BYTES];
            int ni = pk_msg_encode(&br[i], ei, PK_MSG_MAX_BYTES), nj = pk_msg_encode(&br[j], ej, PK_MSG_MAX_BYTES);
            CHECK((pij == 0) == (ni == nj && !memcmp(ei, ej, (size_t)ni)),
                  "branches %d and %d tie exactly when they are the same bytes", i, j);
        }
    for (int k = 0; k < 10000; k++) {
        int i = (int)rnd((uint32_t)nb), j = (int)rnd((uint32_t)nb), l = (int)rnd((uint32_t)nb);
        if (pk_msg_prefer(&br[i], &br[j]) < 0 && pk_msg_prefer(&br[j], &br[l]) < 0)
            CHECK(pk_msg_prefer(&br[i], &br[l]) < 0, "transitive over %d, %d, %d", i, j, l);
    }
    printf("rule p: %d races, %d branches (%ld ordered pairs, 10,000 triples)\n", races, nb, pairs);
}

/* ---- 7.8.7 seat resolve ----------------------------------------------------------- */

static void seat_resolve(void)
{
    static PkMsg m, lob, gone;
    TEST("7.8.7 seat resolve");
    uint8_t seed[32], t[PK_TAG_LEN];
    seed_wide(seed, 4444);
    started_of(&m, seed, 4);
    int by;
    /* (a) the record, and it outranks everything after it */
    CHECK(pk_msg_resolve(&m, 2, -1, 0, PK_SENT_UNKNOWN, 0, 0, &by) == 2 && by == PK_BY_RECORD,
          "a record seats a rotated id in a group");
    CHECK(pk_msg_resolve(&m, 2, 1, 1, 1, U("Bo"), 2, &by) == 2 && by == PK_BY_RECORD,
          "the record outranks the tag, the sender and the name");
    /* (b) the tag */
    id_tag(seed, "id-Cleo", t);
    CHECK(pk_msg_resolve(&m, -1, pk_msg_seat_of_tag(&m, t), 0, PK_SENT_UNKNOWN, 0, 0, &by) == 2 && by == PK_BY_TAG,
          "the tag seats Cleo");
    id_tag(seed, "id-Cleo-rotated", t);
    CHECK(pk_msg_seat_of_tag(&m, t) == -1, "a rotated id's tag matches nobody");
    CHECK(pk_msg_resolve(&m, -1, 1, 1, 0, U("Dev"), 3, &by) == 1 && by == PK_BY_TAG,
          "the tag outranks the sender and the name");
    /* (c) the sender: the start bubble is the starter's */
    CHECK(pk_msg_sender(&m) == 0, "a start bubble's sender is the starter");
    CHECK(pk_msg_resolve(&m, -1, -1, 0, 1, 0, 0, &by) == 0 && by == PK_BY_SENDER, "I sent it: I am the starter");
    CHECK(pk_msg_resolve(&m, -1, -1, 1, 0, 0, 0, &by) == -1 && by == PK_BY_NONE,
          "four seats: 'not mine' says nothing, even in a two-person chat");
    CHECK(plain_turn(&m.game), "seat 1 takes a turn");
    CHECK(pk_msg_sender(&m) == 1, "the newest bubble's sender");
    CHECK(pk_msg_resolve(&m, -1, -1, 0, 1, 0, 0, &by) == 1 && by == PK_BY_SENDER, "I sent it: seat 1");
    /* (d) the name */
    CHECK(pk_msg_resolve(&m, -1, -1, 0, PK_SENT_UNKNOWN, U("Dev"), 3, &by) == 3 && by == PK_BY_NAME,
          "the nickname is the last witness");
    CHECK(pk_msg_resolve(&m, -1, -1, 0, PK_SENT_UNKNOWN, U("Zed"), 3, &by) == -1 && by == PK_BY_NONE,
          "nobody: a spectator");
    /* two seats in a DM: 'not mine' is the other seat */
    static PkMsg dm;
    seed_wide(seed, 4445);
    lobby_of(&dm, seed, 2, 1);
    CHECK(pk_msg_resolve(&dm, -1, -1, 1, 0, U("Alex"), 4, &by) == 0 && by == PK_BY_SENDER,
          "a DM lobby, Bo's join from the other phone: I am Alex");
    pk_msg_start(&dm, 0);
    CHECK(pk_msg_resolve(&dm, -1, -1, 1, 0, 0, 0, &by) == 1 && by == PK_BY_SENDER,
          "a DM, Alex's start from the other phone: I am Bo");

    /* THE LOBBY GATE: an inference never seats a named device on a row with
     * somebody else's name */
    seed_wide(seed, 4446);
    lobby_of(&lob, seed, 3, 0);
    CHECK(pk_msg_resolve(&lob, -1, -1, 0, 1, U("Cleo"), 4, &by) == 2 && by == PK_BY_SENDER,
          "the newest joiner who sent it is seated");
    CHECK(pk_msg_resolve(&lob, -1, -1, 0, 1, U("Zoe"), 3, &by) == -1,
          "a lobby row under another name is not mine, whoever sent the bubble");
    gone = lob;
    CHECK(pk_msg_leave(&gone, 1) == PK_EOK && gone.left, "Bo leaves");
    CHECK(pk_msg_sender(&gone) == -1, "a leave's sender has no seat");
    CHECK(pk_msg_resolve(&gone, -1, -1, 0, 1, U("Bo"), 2, &by) == -1 && by == PK_BY_NONE,
          "the leaver, holding their own leave, is not seated");
    static PkMsg dml;
    lobby_of(&dml, seed, 2, 1);
    CHECK(pk_msg_leave(&dml, 1) == PK_EOK, "a DM joiner leaves");
    CHECK(pk_msg_resolve(&dml, -1, -1, 1, 1, U("Bo"), 2, &by) == -1,
          "the DM leaver is never handed the stayer's seat");

    /* THE RECORDS: a tag, so a leave that moves rows down still finds mine */
    static uint8_t recs[PK_REC_BYTES];
    int rn = 0;
    rn = pk_rec_put(recs, rn, &lob, 2);
    CHECK(rn == PK_REC_LEN && pk_rec_find(recs, rn, &lob) == 2, "Cleo's record");
    CHECK(pk_rec_find(recs, rn, &gone) == 1, "after Bo leaves, Cleo's record finds row 1");
    rn = pk_rec_put(recs, rn, &gone, 1);
    CHECK(rn == PK_REC_LEN, "re-recording a game replaces its record");
    rn = pk_rec_forget(recs, rn, &gone);
    CHECK(rn == 0 && pk_rec_find(recs, rn, &lob) == -1, "forget drops it");
    for (int k = 0; k < PK_REC_MAX + 5; k++) {
        static PkMsg o;
        uint8_t s2[32];
        seed_wide(s2, 90000u + (uint32_t)k);
        lobby_of(&o, s2, 2, 0);
        rn = pk_rec_put(recs, rn, &o, 1);
    }
    CHECK(rn == PK_REC_BYTES, "the records stop at %d", PK_REC_MAX);
    {
        static PkMsg o;
        uint8_t s2[32];
        seed_wide(s2, 90000u);
        lobby_of(&o, s2, 2, 0);
        CHECK(pk_rec_find(recs, rn, &o) == -1, "the oldest fell off");
        seed_wide(s2, 90000u + PK_REC_MAX + 4);
        lobby_of(&o, s2, 2, 0);
        CHECK(pk_rec_find(recs, rn, &o) == 1, "the newest is there");
    }
}

/* ---- the lobby through the envelope --------------------------------------------- */

static void lobby_wire(void)
{
    static PkMsg m, back, dm;
    TEST("lobby: rows, verdicts and the wire");
    uint8_t seed[32], t[PK_TAG_LEN];
    seed_wide(seed, 1234);
    lobby_of(&m, seed, 1, 0);
    CHECK(pk_msg_offered(&m, 0) == PK_LOBBY_WAITING && pk_msg_offered(&m, -1) == PK_LOBBY_JOIN,
          "a fresh invitation: the creator waits, anybody joins");
    id_tag(seed, "id-Bo", t);
    CHECK(pk_msg_join(&m, t, U("Bo"), 2) == 1, "Bo joins at 1");
    CHECK(pk_msg_join(&m, t, U("Bo2"), 3) == PK_EROSTER, "the same tag cannot join twice");
    id_tag(seed, "id-Other", t);
    CHECK(pk_msg_join(&m, t, U("Bo"), 2) == PK_EROSTER, "a taken name cannot join");
    CHECK(pk_msg_join(&m, t, U(""), 0) == PK_EROSTER, "an empty name cannot join");
    CHECK(pk_msg_join(&m, t, U("\x01x"), 2) == PK_EROSTER, "a control character cannot join");
    CHECK(pk_msg_join(&m, t, U("\xC0\x80"), 2) == PK_EROSTER, "an overlong encoding cannot join");
    CHECK(pk_msg_join(&m, t, U("seventeen chars!!"), 17) == PK_EROSTER, "seventeen characters cannot join");
    CHECK(pk_name_verdict(U("sixteen chars!!!"), 16) == PK_NAME_OK, "sixteen characters are fine");
    CHECK(pk_msg_offered(&m, 0) == PK_LOBBY_START && pk_msg_offered(&m, 1) == PK_LOBBY_WAITING,
          "two in a group: the creator may start, the newest joiner may not");
    int n = 0;
    static uint8_t b[PK_MSG_MAX_BYTES];
    n = pk_msg_encode(&m, b, (int)sizeof b);
    CHECK(n > 0 && pk_msg_decode(b, n, &back) == PK_EOK && back.n_seats == 2 && back.lobby_rev == 1
          && !back.left && pk_msg_offered(&back, 1) == PK_LOBBY_WAITING, "the lobby round trips with its verdicts");
    CHECK(pk_msg_leave(&m, 0) == PK_EOK && m.n_seats == 1 && !memcmp(m.seat[0].name, "Bo", 2), "the creator leaves: Bo is seat 0");
    n = pk_msg_encode(&m, b, (int)sizeof b);
    CHECK(n > 0 && pk_msg_decode(b, n, &back) == PK_EOK && back.left && back.lobby_rev == 2
          && pk_msg_offered(&back, 0) == PK_LOBBY_INVITE, "after a leave Bo, alone, is offered the invite");
    CHECK(pk_msg_leave(&m, 0) == PK_EREFUSED, "the last one cannot leave");
    CHECK(pk_msg_start(&m, 0) == PK_EREFUSED, "one cannot start");

    /* a DM: the second person joins and starts in one bubble */
    seed_wide(seed, 1235);
    lobby_of(&dm, seed, 1, 1);
    id_tag(seed, "id-Bo", t);
    int s = pk_msg_join_start(&dm, t, U("Bo"), 2);
    CHECK(s == 1 && dm.phase == PK_PHASE_LIVE && dm.starter == 1 && dm.game.n == 2, "join and start in a DM");
    n = pk_msg_encode(&dm, b, (int)sizeof b);
    CHECK(n > 0 && pk_msg_decode(b, n, &back) == PK_EOK && back.starter == 1 && back.game.starter == 1
          && pk_msg_sender(&back) == 1, "the start bubble names its starter");
    id_tag(seed, "id-Cleo", t);
    CHECK(pk_msg_join(&dm, t, U("Cleo"), 4) == PK_EREFUSED, "nobody joins a started game");

    /* the lobby's events, known by tag across a leave */
    static PkMsg a1, a2;
    seed_wide(seed, 1236);
    lobby_of(&a1, seed, 3, 0);
    a2 = a1;
    pk_msg_leave(&a2, 1);
    id_tag(seed, "id-Dev", t);
    pk_msg_join(&a2, t, U("Dev"), 3);
    PkEvent ev[8];
    int ne = pk_msg_plan_lobby(&a1, &a2, ev, 8);
    CHECK(ne == 2 && ev[0].kind == PK_EV_LOBBY_LEAVE && ev[0].seat == 1
          && ev[1].kind == PK_EV_LOBBY_JOIN && ev[1].seat == 2,
          "Bo leaves from 1 and Dev joins at 2; Cleo moved down and is nobody's event (%d)", ne);

    /* a staged draft is sealed before it is written */
    started_of(&m, seed, 2);
    PkAct d = { PK_A_DRAW, 0, 0, 0 };
    pk_apply(&m.game, 1, d);
    CHECK(pk_msg_encode(&m, b, (int)sizeof b) == PK_EGAME, "an open draft is not written");
    CHECK(pk_msg_encode(&m, b, 10) == PK_EGAME, "...whatever the buffer");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    int games = argc > 1 ? atoi(argv[1]) : 30;
    int deals = argc > 2 ? atoi(argv[2]) : 1000;
    lobby_wire();
    header_agreement();
    seat_resolve();
    rule_p();
    build_pool();
    tamper();
    caps_are_rules();
    many_draws(deals);
    round_trip(games);
    return report("pk_msg_test");
}
