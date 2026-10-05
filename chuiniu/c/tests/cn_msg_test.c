/* The wire:
 *   - round trips: a lobby, the start, every bubble of random games, the
 *     finished game; bytes and text; every body re-encodes to itself
 *   - THE SIZE: the longest game there can be, six 48-byte names, measured
 *     against the 5,000 characters of MSMessage.url (and the compile-time
 *     bound), and the sizes real games reach
 *   - THE HOSTILE SWEEP: every prefix and every single-byte corruption of a
 *     pool of real bubbles, each in a buffer of exactly its length on the
 *     heap (so ASan sees one byte past it), never reads out of bounds and
 *     never accepts a game that is not the one written
 *   - tampering with a recomputed check: a move off its menu, a header that
 *     disagrees with its replay, flags, format, magic, roster
 *   - the lobby, the race rule (Rule P), the common moves, the seat resolver
 *
 *     ./build/cn_msg_test [games per size] */
#include "cn_check.h"
#include "../src/cn_msg.h"
#include "../src/cn_code.h"
#include "../../../shared/c/sha256.h"
#include "../../../shared/c/b32.h"
#include "../../../shared/c/mixrad.h"

static CnMsg M, D, E;
static uint8_t buf[CN_MSG_MAX_BYTES], buf2[CN_MSG_MAX_BYTES];
static char text[CN_MSG_MAX_TEXT + 64];

static void tag_of(const uint8_t seed[32], int person, uint8_t tag[CN_TAG_LEN])
{
    uint8_t id[8];
    for (int i = 0; i < 8; i++) id[i] = (uint8_t)(person * 13 + i);
    cn_tag(seed, id, 8, tag);
}

static const char *NM[6] = { "Alex", "Bo", "Cy", "Dee", "Eve", "Fox" };

/* A started table of n seats: the creator, n - 1 joins, the creator starts. */
static int table(CnMsg *m, uint32_t k, int n, int dm, int long_names)
{
    uint8_t seed[32], tag[CN_TAG_LEN];
    seed_wide(seed, k);
    char nm[6][64];
    for (int s = 0; s < 6; s++) {
        if (long_names) {
            /* 16 characters of three bytes each: the 48-byte cap */
            int o = 0;
            for (int c = 0; c < 16; c++) { nm[s][o++] = (char)0xE5; nm[s][o++] = (char)(0x90 + s); nm[s][o++] = (char)(0x80 + c); }
            nm[s][o] = 0;
        } else {
            strcpy(nm[s], NM[s]);
        }
    }
    tag_of(seed, 0, tag);
    if (cn_msg_new(m, seed, dm, tag, (const uint8_t *)nm[0], (int)strlen(nm[0]))) return 0;
    for (int s = 1; s < n; s++) {
        tag_of(seed, s, tag);
        if (cn_msg_join(m, tag, (const uint8_t *)nm[s], (int)strlen(nm[s])) != s) return 0;
    }
    return cn_msg_start(m, 0) == CN_EOK;
}

static int roundtrip(const CnMsg *m, const char *what)
{
    int n = cn_msg_encode(m, buf, sizeof buf);
    CHECK(n > 0, "%s: encodes (%d)", what, n);
    if (n <= 0) return 0;
    int e = cn_msg_decode(buf, n, &D);
    CHECK(e == CN_EOK, "%s: decodes (%d)", what, e);
    int n2 = cn_msg_encode(&D, buf2, sizeof buf2);
    CHECK(n2 == n && !memcmp(buf, buf2, (size_t)n), "%s: re-encodes to its own bytes", what);
    if (m->phase != CN_PHASE_WAITING)
        CHECK(cn_hash(&D.game) == cn_hash(&m->game) && !memcmp(D.game.dice, m->game.dice, sizeof D.game.dice),
              "%s: the same game, the same dice", what);
    int t = cn_msg_text_encode(m, text, sizeof text);
    CHECK(t > 0 && t < 5000 && t <= CN_MSG_MAX_TEXT - 1, "%s: a link of %d characters", what, t);
    char url[sizeof text + 64];     /* the whole text plus the URL around it */
    snprintf(url, sizeof url, "https://example.invalid/x%s&v=1#frag", text);
    CHECK(cn_msg_text_decode(url, &E) == CN_EOK && cn_msg_encode(&E, buf2, sizeof buf2) == n
          && !memcmp(buf, buf2, (size_t)n), "%s: through a whole URL", what);
    return t;
}

/* ---- round trips and sizes -------------------------------------------------- */

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_roundtrip(int per)
{
    TEST("round trips and sizes");
    static int sizes[5][20000];
    int ns[5] = { 0 };
    for (int n = 2; n <= 6; n++)
        for (int k = 0; k < per; k++) {
            CHECK(table(&M, (uint32_t)(n * 1000 + k), n, n == 2 && k % 2, 0), "n %d game %d: a table", n, k);
            roundtrip(&M, "the start");
            while (M.game.phase == CN_PH_BIDDING || M.game.phase == CN_PH_REVEALED) {
                if (!cn_apply(&M.game, M.game.turn, bot_move(&M.game))) break;
                int t = roundtrip(&M, "a bubble");
                if (ns[n - 2] < 20000) sizes[n - 2][ns[n - 2]++] = t;
            }
            CHECK(M.game.phase == CN_PH_OVER, "finished");
            cn_msg_encode(&M, buf, sizeof buf);
            CHECK(buf[2] == CN_PHASE_FINISHED, "the phase byte says FINISHED");
        }
    for (int i = 0; i < 5; i++) {
        qsort(sizes[i], (size_t)ns[i], sizeof(int), cmp_int);
        if (ns[i])
            printf("  %d seats: %d bubbles, link median %d, p99 %d, max %d\n", i + 2, ns[i], sizes[i][ns[i] / 2],
                   sizes[i][ns[i] * 99 / 100], sizes[i][ns[i] - 1]);
    }
}

static void test_worst_case(void)
{
    TEST("the worst case fits MSMessage.url");
    for (int n = 2; n <= 6; n++) {
        CHECK(table(&M, 77 + (uint32_t)n, n, 0, 1), "n %d: a table of 48-byte names", n);
        for (int s = 0; s < n; s++) CHECK(M.seat[s].name_len == 48, "48 bytes");
        while (M.game.phase == CN_PH_BIDDING || M.game.phase == CN_PH_REVEALED) if (!cn_apply(&M.game, M.game.turn, long_move(&M.game))) break;
        CHECK(M.game.hist_n == CN_MAX_MOVES_OF(n), "the longest game");
        int t = roundtrip(&M, "the longest game");
        int body = cn_code_encode(&M.game, buf, sizeof buf);
        printf("  the longest %d-seat game: %d moves, a %d-byte body, a %d-character link (bound %d)\n",
               n, M.game.hist_n, body, t, CN_MSG_MAX_TEXT - 1);
        CHECK(t < 5000, "n %d: %d characters", n, t);
    }
    CHECK(CN_MSG_MAX_TEXT - 1 < 5000, "the compile-time bound %d", CN_MSG_MAX_TEXT - 1);
}

/* ---- the hostile sweep ------------------------------------------------------------ */

#define POOL 8
static uint8_t pool[POOL][CN_MSG_MAX_BYTES];
static int pool_n[POOL];

static int decode_exact(const uint8_t *src, int n, CnMsg *out)
{
    uint8_t *p = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (n > 0) memcpy(p, src, (size_t)n);
    int e = cn_msg_decode(p, n, out);
    free(p);
    return e;
}

static void build_pool(void)
{
    uint8_t seed[32], tag[CN_TAG_LEN];
    int k = 0;
    seed_wide(seed, 4242);
    tag_of(seed, 0, tag);
    cn_msg_new(&M, seed, 0, tag, (const uint8_t *)"Alex", 4);
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* a lobby of one */
    tag_of(seed, 1, tag);
    cn_msg_join(&M, tag, (const uint8_t *)"Bo", 2);
    tag_of(seed, 2, tag);
    cn_msg_join(&M, tag, (const uint8_t *)"Cy", 2);
    cn_msg_leave(&M, 1);
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* a leave */
    table(&M, 99, 3, 0, 0);
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* the start */
    for (int i = 0; i < 6; i++) cn_apply(&M.game, M.game.turn, bot_move(&M.game));
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* mid round */
    while (M.game.phase == CN_PH_BIDDING || M.game.phase == CN_PH_REVEALED) if (!cn_apply(&M.game, M.game.turn, bot_move(&M.game))) break;
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* finished */
    table(&M, 5, 2, 1, 0);
    cn_apply(&M.game, 0, bid(3, 4));
    cn_apply(&M.game, 1, call_move());
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* a DM, revealed */
    table(&M, 6, 6, 0, 1);
    for (int i = 0; i < 40; i++) cn_apply(&M.game, M.game.turn, bot_move(&M.game));
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* six long names */
    table(&M, 7, 2, 0, 0);
    while (M.game.phase == CN_PH_BIDDING || M.game.phase == CN_PH_REVEALED) if (!cn_apply(&M.game, M.game.turn, long_move(&M.game))) break;
    pool_n[k] = cn_msg_encode(&M, pool[k], CN_MSG_MAX_BYTES); k++;          /* the longest 2-seat */
    for (int i = 0; i < POOL; i++) CHECK(pool_n[i] > 0, "pool %d encodes", i);
}

static void test_hostile(void)
{
    TEST("the hostile sweep");
    long tried = 0, accepted = 0;
    for (int p = 0; p < POOL; p++) {
        const int n = pool_n[p];
        CHECK(decode_exact(pool[p], n, &D) == CN_EOK, "pool %d reads", p);
        if (n <= 0) continue;                  /* red above; nothing to corrupt */
        for (int k = 0; k < n; k++)
            CHECK(decode_exact(pool[p], k, &D) != CN_EOK, "pool %d: the prefix of %d refused", p, k);
        /* EVERY BYTE TO EVERY OTHER VALUE */
        for (int i = 0; i < n; i++)
            for (int f = 1; f < 256; f++) {
                uint8_t c[CN_MSG_MAX_BYTES];
                memcpy(c, pool[p], (size_t)n);
                c[i] ^= (uint8_t)f;
                tried++;
                if (decode_exact(c, n, &D) != CN_EOK) continue;
                /* THE ONLY CORRUPTION THAT MAY READ is one that is itself a
                 * whole message: it writes back to exactly its bytes */
                accepted++;
                int w = cn_msg_encode(&D, buf, sizeof buf);
                CHECK(w == n && !memcmp(buf, c, (size_t)n), "pool %d byte %d ^%02x: read as another message", p, i, f);
            }
        /* longer: trailing bytes are part of the body, and the check fails */
        uint8_t c[CN_MSG_MAX_BYTES + 1];
        memcpy(c, pool[p], (size_t)n);
        c[n] = 1;
        CHECK(decode_exact(c, n + 1, &D) != CN_EOK, "pool %d: a trailing byte refused", p);
    }
    printf("  hostile: %ld corruptions, %ld read (each a whole message)\n", tried, accepted);
    CHECK(accepted * 1000 < tried, "almost every corruption is refused (%ld of %ld read)", accepted, tried);
}

/* ---- tampering with the check recomputed ----------------------------------------------- */

/* Re-seal `b` (n bytes) with a correct check: the attacker who knows the
 * format. The roster ends where the check begins. */
static void reseal(uint8_t *b, int n)
{
    int at = CN_HEAD_LEN;
    for (int s = 0; s < b[40] && at + CN_TAG_LEN < n; s++) at += CN_TAG_LEN + 1 + b[at + CN_TAG_LEN];
    if (at + 2 > n) return;                 /* a roster past the end: nothing to seal */
    uint8_t d[32];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, b, (size_t)at);
    sha256_update(&c, b + at + 2, (size_t)(n - at - 2));
    sha256_final(&c, d);
    b[at] = d[0];
    b[at + 1] = d[1];
}

static void test_tamper(void)
{
    TEST("tampering");
    table(&M, 31, 3, 0, 0);
    cn_apply(&M.game, 0, bid(2, 3));
    cn_apply(&M.game, 1, bid(3, 3));
    const int n = cn_msg_encode(&M, buf, sizeof buf);
    CHECK(n > 0, "the bubble to tamper with encodes (%d)", n);
    if (n <= 0) return;
    uint8_t c[CN_MSG_MAX_BYTES];
#define TAMPER(expr, want, what) do { memcpy(c, buf, (size_t)n); expr; reseal(c, n); \
        int e_ = decode_exact(c, n, &D); CHECK(e_ == (want), "%s: %d, want %d", what, e_, want); } while (0)
    TAMPER((void)0, CN_EOK, "untouched");
    TAMPER(c[38]++, CN_EGAME, "one move more than the body says");
    TAMPER(c[38]--, CN_EGAME, "one move fewer");
    TAMPER(c[2] = CN_PHASE_FINISHED, CN_EGAME, "a live game claimed finished");
    TAMPER(c[2] = 7, CN_EGAME, "an unknown phase");
    TAMPER(c[3] |= 0x04, CN_EFLAGS, "a reserved flag");
    TAMPER(c[3] |= CN_FLAG_LEFT, CN_EFLAGS, "LEFT in a started game");
    TAMPER(c[1] = 2, CN_EFORMAT, "a newer format");
    TAMPER(c[0] = 0xB9, CN_EMAGIC, "another game's magic");
    TAMPER(c[41] = 3, CN_EROSTER, "a starter off the table");
    TAMPER(c[40] = 7, CN_EROSTER, "seven seats");
    /* THE SEED IS ONLY GUARDED BY THE CHECK: a bid's menu does not depend on
     * the dice, so a resealed seed reads, as another game with other dice */
    TAMPER(c[4] ^= 1, CN_EOK, "another seed, resealed");
    CHECK(memcmp(D.game.dice, M.game.dice, sizeof D.game.dice) != 0, "another seed rolls other dice");
    memcpy(c, buf, (size_t)n);
    c[4] ^= 1;
    CHECK(decode_exact(c, n, &D) == CN_ECHECK, "unsealed, the check refuses it");
    (void)0;
    /* A MOVE OFF ITS MENU: the body's number with a digit past its base */
    {
        CnGame g;
        uint8_t seed[32];
        memcpy(seed, M.seed, 32);
        cn_new(&g, seed, 3);
        int nm = cn_legal(&g, 0, 0);
        uint8_t v[8] = { 1 };
        int len = 1;
        /* one digit: index nm (off the menu) folded as base nm + 1 */
        mixrad_mul_add(v, &len, 8, (uint32_t)nm + 1, (uint32_t)nm);
        CnGame out;
        CHECK(!cn_code_decode(&out, seed, 3, 1, v, len), "a digit past its menu is refused");
        CHECK(cn_code_decode(&out, seed, 3, 0, (const uint8_t[]){ 1 }, 1), "the sentinel alone is the start");
        CHECK(!cn_code_decode(&out, seed, 3, 0, (const uint8_t[]){ 1, 0 }, 2), "a zero top byte is not minimal");
        CHECK(!cn_code_decode(&out, seed, 3, 0, (const uint8_t[]){ 2 }, 1), "bytes left over");
    }
    /* A MOVE AFTER THE END: a finished body with one more digit claimed */
    table(&M, 32, 2, 0, 0);
    while (M.game.phase == CN_PH_BIDDING || M.game.phase == CN_PH_REVEALED) if (!cn_apply(&M.game, M.game.turn, bot_move(&M.game))) break;
    int len = cn_code_encode(&M.game, buf, sizeof buf);
    CnGame out;
    CHECK(!cn_code_decode(&out, M.seed, 2, M.game.hist_n + 1, buf, len), "nothing after the end");
    /* the roster */
    table(&M, 33, 3, 0, 0);
    M.seat[2] = M.seat[1];
    CHECK(cn_msg_encode(&M, buf, sizeof buf) == CN_EROSTER, "a duplicate seat refused at encode");
    table(&M, 33, 3, 0, 0);
    M.seat[1].name_len = 0;
    CHECK(cn_msg_encode(&M, buf, sizeof buf) == CN_EROSTER, "an empty name refused");
}

/* ---- the lobby, Rule P, common moves, the resolver --------------------------------------- */

static void test_lobby(void)
{
    TEST("the lobby");
    uint8_t seed[32], tag[CN_TAG_LEN];
    seed_wide(seed, 808);
    tag_of(seed, 0, tag);
    CHECK(cn_msg_new(&M, seed, 0, tag, (const uint8_t *)"Alex", 4) == CN_EOK, "a lobby");
    CHECK(cn_msg_offered(&M, 0) == CN_LOBBY_WAITING && cn_msg_offered(&M, -1) == CN_LOBBY_JOIN, "alone: waiting; others may join");
    CHECK(cn_msg_start(&M, 0) == CN_EREFUSED, "no start alone");
    for (int s = 1; s < 6; s++) {
        tag_of(seed, s, tag);
        if (s == 5) {
            E = M;
            CHECK(cn_msg_join_start(&E, tag, (const uint8_t *)NM[s], (int)strlen(NM[s])) == 5, "the sixth joins and starts");
            CHECK(E.phase == CN_PHASE_LIVE && E.game.n == 6 && E.starter == 5, "six seats, started by the joiner");
        }
        CHECK(cn_msg_join(&M, tag, (const uint8_t *)NM[s], (int)strlen(NM[s])) == s, "seat %d", s);
        CHECK(cn_msg_offered(&M, s) == (s == 5 ? CN_LOBBY_START : CN_LOBBY_WAITING), "the newest joiner waits unless full");
        CHECK(cn_msg_offered(&M, 0) == CN_LOBBY_START, "the others may start");
    }
    CHECK(cn_msg_offered(&M, -1) == CN_LOBBY_FULL, "six is full");
    tag_of(seed, 9, tag);
    CHECK(cn_msg_join(&M, tag, (const uint8_t *)"Gus", 3) == CN_EREFUSED, "no seventh");
    tag_of(seed, 1, tag);
    E = M;
    CHECK(cn_msg_leave(&E, 1) == CN_EOK && E.n_seats == 5 && !memcmp(E.seat[1].name, "Cy", 2), "a leave moves later rows down");
    CHECK(cn_msg_join(&E, tag, (const uint8_t *)"Cy", 2) == CN_EROSTER, "a taken name refused");
    CnEvent ev[16];
    int k = cn_msg_plan_lobby(&M, &E, ev, 16);
    CHECK(k == 1 && ev[0].kind == CN_EV_LOBBY_LEAVE && ev[0].seat == 1, "the leave's event");
    roundtrip(&E, "after a leave");
    CHECK(cn_msg_start(&M, 2) == CN_EOK && M.game.n == 6, "a full table starts");
    /* a DM holds two */
    tag_of(seed, 0, tag);
    cn_msg_new(&M, seed, 1, tag, (const uint8_t *)"Alex", 4);
    tag_of(seed, 1, tag);
    CHECK(cn_msg_join_start(&M, tag, (const uint8_t *)"Bo", 2) == 1 && M.game.n == 2, "a DM: join and start");
}

static void test_prefer(void)
{
    TEST("Rule P and common moves");
    table(&M, 404, 3, 0, 0);
    CnMsg lobby = M;
    lobby.phase = CN_PHASE_WAITING;
    lobby.starter = CN_SEAT_NONE;
    cn_apply(&M.game, 0, bid(2, 2));
    CnMsg a = M, b = M;
    cn_apply(&a.game, 1, bid(3, 3));
    cn_apply(&b.game, 1, bid(4, 4));
    CHECK(cn_msg_prefer(&M, &a) > 0 && cn_msg_prefer(&a, &M) < 0, "more moves win");
    CHECK(cn_msg_prefer(&lobby, &M) > 0, "started beats waiting");
    int ab = cn_msg_prefer(&a, &b), ba = cn_msg_prefer(&b, &a);
    CHECK(ab != 0 && ab == -ba, "a race of two equal lengths has one winner, both ways (%d %d)", ab, ba);
    CHECK(cn_msg_prefer(&a, &a) == 0, "the same bubble");
    CnMsg other;
    table(&other, 405, 3, 0, 0);
    CHECK(cn_msg_prefer(&a, &other) > 0 && cn_msg_prefer(&other, &a) > 0, "another game: the tapped one");
    CHECK(cn_common_moves(&a, &b) == 1 && cn_common_moves(&a, &M) == 1 && cn_common_moves(&a, &a) == 2, "common moves");
    CHECK(cn_common_moves(&a, &other) == 0, "none across games");
}

static void test_resolve(void)
{
    TEST("the seat resolver");
    table(&M, 505, 3, 0, 0);
    uint8_t tag[CN_TAG_LEN];
    int by;
    tag_of(M.seed, 2, tag);
    CHECK(cn_msg_resolve(&M, -1, cn_msg_seat_of_tag(&M, tag), 0, CN_SENT_UNKNOWN, 0, 0, &by) == 2 && by == CN_BY_TAG, "the tag");
    CHECK(cn_msg_resolve(&M, 1, 2, 0, CN_SENT_UNKNOWN, 0, 0, &by) == 1 && by == CN_BY_RECORD, "the record first");
    CHECK(cn_msg_resolve(&M, -1, -1, 0, 1, 0, 0, &by) == 0 && by == CN_BY_SENDER, "I sent the start: the starter");
    cn_apply(&M.game, 0, bid(2, 5));
    CHECK(cn_msg_sender(&M) == 0, "the newest move's seat sent it");
    cn_apply(&M.game, 1, bid(2, 6));
    CHECK(cn_msg_sender(&M) == 1, "and again");
    CHECK(cn_msg_resolve(&M, -1, -1, 0, CN_SENT_UNKNOWN, (const uint8_t *)"Cy", 2, &by) == 2 && by == CN_BY_NAME, "the name last");
    CHECK(cn_msg_resolve(&M, CN_REC_GONE, -1, 0, 1, (const uint8_t *)"Cy", 2, &by) == -1, "a gone record is not seated by inference");
    static uint8_t recs[CN_REC_BYTES];
    int rn = cn_rec_put(recs, 0, &M, 2);
    CHECK(rn == CN_REC_LEN && cn_rec_find(recs, rn, &M) == 2, "a record");
    CnMsg other;
    table(&other, 506, 3, 0, 0);
    CHECK(cn_rec_find(recs, rn, &other) == -1, "no record of another game");
    rn = cn_rec_forget(recs, rn, &M);
    CHECK(rn == 0 && cn_rec_find(recs, rn, &M) == -1, "forgotten");
}

/* THE SEEN BYTE and the stored form: the newest round whose throw this phone
 * watched rides in the game's record, survives a save and a load, survives
 * the game being recorded again, and the first form (17-byte records, no
 * mark) loads with every seat kept and nothing seen. */
static void test_records(void)
{
    static uint8_t recs[CN_REC_BYTES], back[CN_REC_BYTES], file[CN_REC_FILE_BYTES];
    CnMsg a, b;
    table(&a, 701, 3, 0, 0);
    table(&b, 702, 2, 0, 0);
    int rn = cn_rec_put(recs, 0, &a, 1);
    rn = cn_rec_put(recs, rn, &b, 0);
    CHECK(cn_rec_seen(recs, rn, &a) == 0 && cn_rec_seen(recs, rn, &b) == 0, "a new record has seen nothing");
    CHECK(cn_rec_see(recs, rn, &a, 3) == 1 && cn_rec_seen(recs, rn, &a) == 3, "round 2 watched");
    CHECK(cn_rec_seen(recs, rn, &b) == 0, "the other game's record is its own");
    CHECK(cn_rec_see(recs, rn, &a, 2) == 0 && cn_rec_seen(recs, rn, &a) == 3, "never lowered");
    CHECK(cn_rec_see(recs, rn, &a, 3) == 0, "the same again changes nothing");
    CnMsg c;
    table(&c, 703, 2, 0, 0);
    CHECK(cn_rec_see(recs, rn, &c, 1) == 0 && cn_rec_seen(recs, rn, &c) == 0, "no record: nothing to keep");
    rn = cn_rec_put(recs, rn, &a, 2);
    CHECK(cn_rec_find(recs, rn, &a) == 2 && cn_rec_seen(recs, rn, &a) == 3, "recorded again (a new seat): the seen round stays");

    int fn = cn_rec_save(recs, rn, file, sizeof file);
    CHECK(fn == CN_REC_MAGIC_LEN + rn && !memcmp(file, CN_REC_MAGIC, CN_REC_MAGIC_LEN), "saved behind the mark");
    CHECK(cn_rec_save(recs, rn, file, fn - 1) == -1, "a short buffer");
    memset(back, 0xEE, sizeof back);
    int bn = cn_rec_load(back, file, fn);
    CHECK(bn == rn && !memcmp(back, recs, (size_t)rn), "a round trip");
    CHECK(cn_rec_seen(back, bn, &a) == 3 && cn_rec_find(back, bn, &a) == 2 && cn_rec_find(back, bn, &b) == 0, "loaded: the seats and the seen round");
    CHECK(cn_rec_load(back, file, fn - 5) == rn - CN_REC_LEN, "a cut store drops its partial record");
    CHECK(cn_rec_save(recs, 0, file, sizeof file) == CN_REC_MAGIC_LEN && cn_rec_load(back, file, CN_REC_MAGIC_LEN) == 0, "no records: the mark alone");

    /* the first form: id and tag, 17 bytes a game */
    static uint8_t v1[CN_REC_LEN_V1 * CN_REC_MAX + 3];
    for (int i = 0; i < CN_REC_MAX; i++) {
        CnMsg g;
        table(&g, 800 + i, 2, 0, 0);
        cn_game_id(g.seed, v1 + i * CN_REC_LEN_V1);
        memcpy(v1 + i * CN_REC_LEN_V1 + 8, g.seat[i % 2].tag, CN_TAG_LEN);
    }
    bn = cn_rec_load(back, v1, (int)sizeof v1);
    CHECK(bn == CN_REC_BYTES, "a full first-form store loads every game");
    int kept = 1;
    for (int i = 0; i < CN_REC_MAX; i += 37) {
        CnMsg g;
        table(&g, 800 + i, 2, 0, 0);
        kept &= cn_rec_find(back, bn, &g) == i % 2 && cn_rec_seen(back, bn, &g) == 0;
    }
    CHECK(kept, "first form: every seat kept, every round unwatched");
    CHECK(cn_rec_load(back, v1, CN_REC_LEN_V1 * 2 + 5) == 2 * CN_REC_LEN, "a first-form store's partial record dropped");
    CHECK(cn_rec_load(back, 0, 40) == 0 && cn_rec_load(back, v1, -1) == 0, "nothing to load");
}

int main(int argc, char **argv)
{
    int per = argc > 1 ? atoi(argv[1]) : 40;
    test_roundtrip(per);
    test_worst_case();
    build_pool();
    test_hostile();
    test_tamper();
    test_lobby();
    test_prefer();
    test_resolve();
    test_records();
    return report("cn_msg_test");
}
