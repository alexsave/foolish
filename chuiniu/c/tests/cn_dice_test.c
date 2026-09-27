/* The dice (DECISIONS K2, K3): derived from already-sent state, never rolled.
 *
 *   - the recipe, recomputed here from sha256 and deal_rng by the test's own
 *     code, is the kernel's, and a pinned golden holds it to the format
 *   - the same already-sent state gives identical dice; a different parent
 *     (seed, or any earlier move) gives different ones
 *   - dice differ per seat and per round, and a seat's dice do not depend on
 *     how many dice any other seat holds
 *   - THE EXPLOIT, THROUGH THE BRIDGE: stage a raise, cancel, stage again, and
 *     the next round's dice are byte-identical; the same for a staged call;
 *     and a staged call shows nothing it would reveal
 *
 * The bridge is compiled into this file, so the calls are the shipped
 * cn_api.c's. */
#include "../ios/cn_api.c"
#include "cn_check.h"
#include "../../../shared/c/deal_rng.h"
#include "../../../shared/c/sha256.h"

static CnGame A, B;

/* THE RECIPE AS DECISIONS K3 STATES IT, written again from the primitives. */
static void oracle_round(const uint8_t seed[32], int round, const CnMove *hist, int k,
                         const uint8_t dice_n[CN_MAX_SEATS], int n, uint8_t out[CN_MAX_SEATS][CN_START_DICE])
{
    uint8_t log[32], key[32];
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, "chuiniu.log.1|", 14);
    for (int i = 0; i < k; i++) sha256_update(&c, &hist[i], 2);
    sha256_final(&c, log);
    uint8_t r[2] = { (uint8_t)round, (uint8_t)(round >> 8) };
    sha256_init(&c);
    sha256_update(&c, "chuiniu.dice.1|", 15);
    sha256_update(&c, seed, 32);
    sha256_update(&c, r, 2);
    sha256_update(&c, log, 32);
    sha256_final(&c, key);
    memset(out, 0, CN_MAX_SEATS * CN_START_DICE);
    for (int s = 0; s < n; s++) {
        DealRng g;
        deal_rng_seed_at(&g, key, (uint64_t)s << 32);
        for (int i = 0; i < dice_n[s]; i++) out[s][i] = (uint8_t)(1 + deal_rng_bounded(&g, 6));
    }
}

static void test_recipe(void)
{
    TEST("K3 the recipe");
    for (uint32_t k = 0; k < 300; k++) {
        uint8_t seed[32], want[CN_MAX_SEATS][CN_START_DICE];
        seed_wide(seed, 900 + k);
        int n = 2 + (int)(k % 5);
        cn_new(&A, seed, n);
        int rounds = 0;
        while (A.phase != CN_PH_OVER && rounds < 6) {
            oracle_round(seed, A.round, A.hist, A.round_at, A.dice_n, n, want);
            CHECK(!memcmp(want, A.dice, sizeof want), "game %u round %d: the kernel's dice are the recipe's", k, A.round);
            int r = A.round;
            while (A.phase != CN_PH_OVER && A.round == r) cn_apply(&A, A.turn, bot_move(&A));
            rounds++;
        }
    }
}

static void test_golden(void)
{
    TEST("K3 golden");
    static const uint8_t r0[3][5] = { { 2, 4, 4, 4, 2 }, { 5, 2, 2, 1, 3 }, { 5, 4, 3, 1, 6 } };
    static const uint8_t r1[3][5] = { { 4, 1, 5, 3, 5 }, { 1, 2, 4, 4, 0 }, { 4, 3, 5, 1, 2 } };
    uint8_t seed[32];
    seed_wide(seed, 2026);
    cn_new(&A, seed, 3);
    CHECK(!memcmp(A.dice, r0, sizeof r0), "round 0 of seed 2026 at three seats");
    cn_apply(&A, 0, bid(2, 3));
    cn_apply(&A, 1, call_move());
    CHECK(A.round == 1 && A.call_loser == 1, "seat 1 called a true bid");
    CHECK(!memcmp(A.dice, r1, sizeof r1), "round 1, seat 1 on four");
}

static void test_same_state(void)
{
    TEST("K2 same state, same dice");
    for (uint32_t k = 0; k < 200; k++) {
        uint8_t seed[32];
        seed_wide(seed, 3000 + k);
        int n = 2 + (int)(k % 5);
        cn_new(&A, seed, n);
        while (A.phase != CN_PH_OVER) cn_apply(&A, A.turn, bot_move(&A));
        /* a second phone replays the same bubbles and meets the same dice
         * at every round's open */
        cn_new(&B, seed, n);
        for (int i = 0; i < A.hist_n; i++) {
            if (B.round_at == B.hist_n) {
                CnGame at;
                cn_replay(&at, &A, i);
                CHECK(!memcmp(at.dice, B.dice, sizeof at.dice), "game %u move %d: identical", k, i);
            }
            cn_apply(&B, B.turn, A.hist[i]);
        }
        CHECK(cn_hash(&A) == cn_hash(&B), "game %u: the same game", k);
    }
}

/* Two games from one seed whose round 0 differs by one bid, then the same
 * call: round 1's dice differ. */
static void test_different_parent(void)
{
    TEST("K2 a different parent, different dice");
    int same = 0, same_seed = 0;
    for (uint32_t k = 0; k < 400; k++) {
        uint8_t seed[32], other[32];
        seed_wide(seed, 7000 + k);
        seed_wide(other, 17000 + k);
        cn_new(&A, seed, 2);
        cn_new(&B, seed, 2);
        cn_apply(&A, 0, bid(1, 2));
        cn_apply(&B, 0, bid(1, 3));
        cn_apply(&A, 1, bid(9, 6));
        cn_apply(&B, 1, bid(9, 6));
        cn_apply(&A, 0, call_move());
        cn_apply(&B, 0, call_move());
        CHECK(A.round == 1 && B.round == 1 && A.call_loser == B.call_loser, "game %u: both in round 1", k);
        same += !memcmp(A.dice, B.dice, sizeof A.dice);
        CnGame C;
        cn_new(&C, other, 2);
        cn_new(&B, seed, 2);
        same_seed += !memcmp(C.dice, B.dice, sizeof B.dice);
    }
    CHECK(same == 0, "an earlier bid re-rolls the next round (%d of 400 alike)", same);
    CHECK(same_seed == 0, "another seed, other dice (%d of 400 alike)", same_seed);
}

static void test_per_seat_round(void)
{
    TEST("K3 per seat, per round");
    int seat_same = 0, round_same = 0, prefix_bad = 0;
    long faces[7] = { 0 };
    long rolled = 0;
    for (uint32_t k = 0; k < 2000; k++) {
        uint8_t seed[32], key[32], log[32], five[5], three[3];
        seed_wide(seed, 40000 + k);
        cn_log_digest(0, 0, log);
        cn_round_key(seed, 0, log, key);
        uint8_t d[CN_MAX_SEATS][5];
        for (int s = 0; s < CN_MAX_SEATS; s++) cn_roll(key, s, 5, d[s]);
        for (int s = 1; s < CN_MAX_SEATS; s++) seat_same += !memcmp(d[0], d[s], 5);
        cn_round_key(seed, 1, log, key);
        cn_roll(key, 0, 5, five);
        round_same += !memcmp(five, d[0], 5);
        /* a seat on three dice holds the first three of its five */
        cn_roll(key, 2, 5, five);
        cn_roll(key, 2, 3, three);
        prefix_bad += memcmp(five, three, 3) != 0;
        for (int s = 0; s < CN_MAX_SEATS; s++)
            for (int i = 0; i < 5; i++) { faces[d[s][i]]++; rolled++; }
    }
    CHECK(seat_same < 5, "two seats alike %d times in 10,000 (chance 1.3 expected)", seat_same);
    CHECK(round_same < 3, "a seat alike across rounds %d times in 2,000", round_same);
    CHECK(prefix_bad == 0, "a count is a prefix of the stream (%d)", prefix_bad);
    for (int f = 1; f <= 6; f++) {
        double share = (double)faces[f] / (double)rolled;
        CHECK(share > 0.16 && share < 0.1733, "face %d share %.4f of %ld", f, share, rolled);
    }
    CHECK(faces[0] == 0, "no face 0");
}

/* ---- through the bridge -------------------------------------------------------- */

static uint8_t recs[3][CN_API_REC_BYTES];
static int     recn[3];
static int     who = -1;
static const char *NICK[3] = { "Alex", "Bo", "Cy" };
static char link[CN_API_TEXT_MAX], link2[CN_API_TEXT_MAX];

static void be(int i)
{
    if (who >= 0) recn[who] = cn_api_seats_save(recs[who], CN_API_REC_BYTES);
    cn_api_seats_load(recs[i], recn[i]);
    cn_api_sender(NULL, 0, -1);
    uint8_t id[16];
    for (int k = 0; k < 16; k++) id[k] = (uint8_t)(i * 41 + k);
    cn_api_me(id, 16);
    cn_api_nickname((const uint8_t *)NICK[i], (int)strlen(NICK[i]));
    who = i;
}

static int send_as(int i)
{
    int n = cn_api_text(link, sizeof link);
    if (n <= 0) return 0;
    cn_api_commit();
    be((i + 1) % 3);
    return cn_api_read(link) == CN_EOK;
}

/* Alex, Bo and Cy at one table, Alex to bid. */
static void table3(uint32_t k)
{
    memset(recn, 0, sizeof recn);
    who = -1;
    be(0);
    uint8_t seed[32];
    seed_wide(seed, 50000 + k);
    cn_api_new(seed, 0);
    cn_api_text(link, sizeof link);
    be(1); cn_api_read(link); cn_api_join(); cn_api_text(link, sizeof link);
    be(2); cn_api_read(link); cn_api_join(); cn_api_text(link, sizeof link);
    be(0); cn_api_read(link); cn_api_start(); cn_api_text(link, sizeof link);
    be(1); cn_api_read(link);
    be(0); cn_api_read(link);
}

static const CnView *all_view(void) { return (const CnView *)cn_api_view(CN_API_ALL); }

static void test_bridge_exploit(void)
{
    TEST("K2 the cancel exploit, through the bridge");
    for (uint32_t k = 0; k < 40; k++) {
        /* THE CONTROL: Alex bids two 4s, Bo calls. */
        table3(k);
        CHECK(cn_api_raise(2, 4) == 1, "game %u: Alex stages two 4s", k);
        CHECK(send_as(0), "game %u: sent", k);
        CHECK(cn_api_call() == 1, "game %u: Bo stages the call", k);
        CHECK(send_as(1), "game %u: the call is sent", k);
        uint8_t want[CN_MAX_DICE];
        memcpy(want, all_view()->all, sizeof want);

        /* THE TRY: Alex stages, cancels and stages again, several times, with
         * a look at everything in between; Bo does the same with the call. */
        table3(k);
        CHECK(cn_api_raise(5, 6) == 1 && cn_api_cancel() == 1, "game %u: staged and cancelled", k);
        CHECK(cn_api_raise(3, 2) == 1, "game %u: staged something else", k);
        cn_api_text(link2, sizeof link2);
        CHECK(cn_api_cancel() == 1 && cn_api_cancel() == 0, "game %u: one staged move to cancel", k);
        CHECK(cn_api_raise(2, 4) == 1, "game %u: two 4s again", k);
        CHECK(send_as(0), "game %u: sent", k);
        CHECK(cn_api_call() == 1 && cn_api_cancel() == 1, "game %u: Bo stages the call and cancels", k);
        CHECK(cn_api_raise(3, 4) == 1, "game %u: Bo stages a raise instead", k);
        cn_api_text(link2, sizeof link2);
        CHECK(cn_api_call() == 1, "game %u: staging replaces: the call again", k);
        CHECK(send_as(1), "game %u: the call is sent", k);
        CHECK(!memcmp(want, all_view()->all, sizeof want), "game %u: round 2's dice are byte-identical", k);
    }
}

static void test_staged_call_reveals_nothing(void)
{
    TEST("K8 a staged call reveals nothing");
    for (uint32_t k = 0; k < 40; k++) {
        table3(100 + k);
        cn_api_raise(3, 5);
        send_as(0);
        const CnView *before = (const CnView *)cn_api_view(CN_API_ME);
        CnView v0 = *before;
        CHECK(cn_api_call() == 1, "game %u: Bo stages the call", k);
        const CnView *v = (const CnView *)cn_api_view(CN_API_ME);
        CHECK(!memcmp(v, &v0, sizeof v0), "game %u: the view is the committed one", k);
        CHECK(v->revealed == 0 && v->call_seat == CN_SEAT_NONE, "game %u: no reveal while staged", k);
        const CnApiEvents *e = (const CnApiEvents *)cn_api_plan_staged();
        CHECK(e && e->n == 1 && e->ev[0].kind == CN_EV_CALL, "game %u: the staged plan is the call alone", k);
        const CnBeats *b = (const CnBeats *)cn_api_beats_staged();
        CHECK(b && b->n == 1 && b->beat[0].kind == CN_BK_CALL, "game %u: the staged beats are the stamp alone", k);
        char line[256];
        cn_api_words(CN_API_W_STAGED_CAPTION, 0, line, sizeof line);
        CHECK(!strcmp(line, "Bo calls three 5s"), "game %u: the staged caption names the call only: %s", k, line);
        cn_api_words(CN_API_W_OUTCOME, 0, line, sizeof line);
        CHECK(line[0] == 0, "game %u: no outcome while staged: %s", k, line);
        CHECK(cn_api_commit() == 1, "game %u: sent", k);
        v = (const CnView *)cn_api_view(CN_API_ME);
        CHECK(v->revealed == 1 && v->call_seat == 1, "game %u: once sent, the cups lift", k);
    }
}

int main(void)
{
    test_recipe();
    test_golden();
    test_same_state();
    test_different_parent();
    test_per_seat_round();
    test_bridge_exploit();
    test_staged_call_reveals_nothing();
    return report("cn_dice_test");
}
