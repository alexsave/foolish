/* The wire (T7): the envelope round trip, the lobby rows, the race rule with
 * consecutive bubbles from one seat, the seat resolver, the fit in
 * MSMessage.url, and the tamper, corruption and truncation sweeps.
 *
 *   ./build/tb_msg_test [games per table size]  */
#include "tb_check.h"
#include "../src/tb_msg.h"

static void tag_of(int who, const uint8_t seed[32], uint8_t tag[TB_TAG_LEN])
{
    uint8_t id[4] = { (uint8_t)who, 7, 7, (uint8_t)(who * 3) };
    tb_tag(seed, id, 4, tag);
}

static const char *NAMES[8] = { "Alex", "Bo", "Cy", "Dee", "Eve", "Fin", "Gus", "Hal" };

/* A lobby of `n` from seat 0's invitation, started by seat 0. */
static void started(TbMsg *m, uint32_t k, int n, int dm)
{
    uint8_t seed[32], tag[TB_TAG_LEN];
    seed_wide(seed, k);
    tag_of(0, seed, tag);
    tb_msg_new(m, seed, dm, tag, (const uint8_t *)NAMES[0], (int)strlen(NAMES[0]));
    for (int s = 1; s < n; s++) {
        tag_of(s, seed, tag);
        tb_msg_join(m, tag, (const uint8_t *)NAMES[s], (int)strlen(NAMES[s]));
    }
    tb_msg_start(m, 0);
}

static int round_trip(const TbMsg *m, TbMsg *back)
{
    static uint8_t b[TB_MSG_MAX_BYTES];
    int n = tb_msg_encode(m, b, sizeof b);
    if (n < 0) return n;
    return tb_msg_decode(b, n, back);
}

static void w_lobby(void)
{
    TEST("wire lobby");
    uint8_t seed[32], tag[TB_TAG_LEN];
    seed_wide(seed, 1);
    TbMsg m, back;
    tag_of(0, seed, tag);
    CHECK(tb_msg_new(&m, seed, 0, tag, (const uint8_t *)"Alex", 4) == TB_EOK, "a new lobby");
    CHECK(round_trip(&m, &back) == TB_EOK && back.phase == TB_PHASE_WAITING && back.n_seats == 1, "it reads back");
    CHECK(tb_msg_offered(&m, 0) == TB_LOBBY_WAITING && tb_msg_offered(&m, -1) == TB_LOBBY_JOIN, "the verdicts");
    tag_of(1, seed, tag);
    CHECK(tb_msg_join(&m, tag, (const uint8_t *)"Alex", 4) == TB_EROSTER, "a name taken");
    CHECK(tb_msg_join(&m, tag, (const uint8_t *)"Bo", 2) == 1, "Bo joins seat 1");
    CHECK(tb_msg_start(&m, 1) == TB_EREFUSED, "the newest joiner stands aside while there is room");
    tag_of(2, seed, tag);
    CHECK(tb_msg_join(&m, tag, (const uint8_t *)"Cy", 2) == 2, "Cy joins");
    CHECK(tb_msg_leave(&m, 1) == TB_EOK && m.n_seats == 2 && m.seat[1].name_len == 2 && !memcmp(m.seat[1].name, "Cy", 2),
          "Bo leaves; Cy moves down");
    CHECK(round_trip(&m, &back) == TB_EOK && back.left && back.lobby_rev == 3, "a leave reads back (rev %d)", back.lobby_rev);
    CHECK(tb_msg_start(&m, 0) == TB_EOK && m.phase == TB_PHASE_LIVE && m.game.n == 2, "Alex starts");
    CHECK(round_trip(&m, &back) == TB_EOK && back.phase == TB_PHASE_LIVE && back.game.hist_n == 0
          && !memcmp(back.game.dice, m.game.dice, 5), "the start bubble: roll 1 is the same on both sides");
    /* join and start: only when the join fills the table */
    TbMsg dm;
    tag_of(0, seed, tag);
    tb_msg_new(&dm, seed, 1, tag, (const uint8_t *)"Alex", 4);
    tag_of(1, seed, tag);
    CHECK(tb_msg_join_start(&dm, tag, (const uint8_t *)"Bo", 2) == 1 && dm.phase == TB_PHASE_LIVE && dm.starter == 1,
          "a DM: Bo joins and starts");
}

static void w_draft_encode(void)
{
    TEST("wire draft");
    TbMsg m, back;
    started(&m, 2, 2, 1);
    TbGame d;
    CHECK(tb_draft(&d, &m.game, mv(TB_M_KEEP, 0, 0x05)), "stage a keep");
    TbMsg md = m;
    md.game = d;
    CHECK(round_trip(&md, &back) == TB_EOK, "a draft writes, and reads back as resident");
    CHECK(!back.game.draft && back.game.hist_n == 1 && back.game.hist[0].kind == TB_M_KEEP && back.game.hist[0].arg == 5,
          "the pending move is the newest bubble");
    for (int i = 0; i < 5; i++) CHECK(back.game.dice[i] >= 1, "the receiver derives die %d", i);
    CHECK(tb_msg_sender(&md) == 0 && tb_msg_sender(&back) == 0, "the sender is the mover");
}

static void w_race(void)
{
    TEST("wire race");
    TbMsg base, a, b, c;
    started(&base, 3, 2, 1);
    /* Alex: KEEP, KEEP, SCORE - one chain, growing */
    a = base;
    TbMove h[3] = { mv(TB_M_KEEP, 0, 1), mv(TB_M_KEEP, 0, 3), mv(TB_M_SCORE, 0, TB_C_ANY) };
    CHECK(tb_replay(&a.game, a.seed, 2, 0, h, 1), "keep");
    b = base;
    CHECK(tb_replay(&b.game, b.seed, 2, 0, h, 2), "keep keep");
    c = base;
    CHECK(tb_replay(&c.game, c.seed, 2, 0, h, 3), "keep keep score");
    CHECK(tb_msg_prefer(&a, &b) > 0 && tb_msg_prefer(&b, &a) < 0, "the second keep beats the first");
    CHECK(tb_msg_prefer(&b, &c) > 0 && tb_msg_prefer(&c, &b) < 0, "the score beats the keeps");
    CHECK(tb_msg_prefer(&base, &a) > 0, "a keep beats the start");
    CHECK(tb_msg_prefer(&c, &c) == 0, "the same bubble");
    CHECK(tb_common_bubbles(&a, &c) == 1 && tb_common_bubbles(&b, &c) == 2, "one chain: the prefix is shared");
    CHECK(tb_msg_sender(&a) == 0 && tb_msg_sender(&b) == 0 && tb_msg_sender(&c) == 0, "the same seat three times");
    TbMsg d = base;
    TbMove hd[4] = { h[0], h[1], h[2], mv(TB_M_KEEP, 1, 0) };
    CHECK(tb_replay(&d.game, d.seed, 2, 0, hd, 4) && tb_msg_sender(&d) == 1, "then Bo's keep: Bo sent it");
    /* two different bubbles on one parent: decided by the digest, the same way both ways round */
    TbMsg x = base, y = base;
    TbMove hx[1] = { mv(TB_M_KEEP, 0, 1) }, hy[1] = { mv(TB_M_LEAVE, 1, 0) };
    tb_replay(&x.game, x.seed, 2, 0, hx, 1);
    tb_replay(&y.game, y.seed, 2, 0, hy, 1);
    int p = tb_msg_prefer(&x, &y), q = tb_msg_prefer(&y, &x);
    CHECK(p != 0 && p == -q, "a race on one parent: %d %d", p, q);
    CHECK(tb_common_bubbles(&x, &y) == 0, "they share nothing past the start");
    /* the resolver in consecutive bubbles from one seat */
    uint8_t tag[TB_TAG_LEN];
    tag_of(1, b.seed, tag);
    int by;
    CHECK(tb_msg_resolve(&b, -1, -1, 1, 1, 0, 0, &by) == 0 && by == TB_BY_SENDER, "I sent the keep: Alex");
    CHECK(tb_msg_resolve(&b, -1, -1, 1, 0, 0, 0, &by) == 1 && by == TB_BY_SENDER, "I did not, in a DM: Bo");
    CHECK(tb_msg_resolve(&b, -1, tb_msg_seat_of_tag(&b, tag), 1, 1, 0, 0, &by) == 1 && by == TB_BY_TAG,
          "the tag outranks the sender");
    uint8_t recs[TB_REC_BYTES];
    int rn = tb_rec_put(recs, 0, &b, 1);
    CHECK(tb_rec_find(recs, rn, &c) == 1, "the record follows the game from bubble to bubble");
}

static void w_fit(void)
{
    TEST("wire fit");
    CHECK(TB_MSG_MAX_TEXT - 1 < 5000, "the bound: %d", TB_MSG_MAX_TEXT - 1);
    /* the longest real one: eight 16-character names of three bytes each (48),
     * a whole game of three bubbles a turn */
    uint8_t seed[32], tag[TB_TAG_LEN], name[48];
    seed_wide(seed, 4);
    TbMsg m;
    for (int s = 0; s < 8; s++) {
        for (int i = 0; i < 16; i++) { name[3 * i] = 0xE2; name[3 * i + 1] = 0x82; name[3 * i + 2] = (uint8_t)(0x80 + (s + i) % 16); }
        tag_of(s, seed, tag);
        if (s == 0) tb_msg_new(&m, seed, 0, tag, name, 48);
        else CHECK(tb_msg_join(&m, tag, name, 48) == s, "join %d", s);
    }
    CHECK(tb_msg_start(&m, 0) == TB_EOK, "start");
    while (!m.game.over) {
        TbMove me[TB_MENU_MAX];
        int n = tb_menu(&m.game, me, TB_MENU_MAX);
        /* the largest index that is not a leave: the biggest number a menu allows */
        int last = n - 1;
        while (me[last].kind == TB_M_LEAVE) last--;
        TbMove pick = me[0].kind == TB_M_KEEP ? me[30] : me[last];
        static TbMove h[TB_HIST_CAP];
        memcpy(h, m.game.hist, sizeof(TbMove) * m.game.hist_n);
        h[m.game.hist_n] = pick;
        if (!tb_replay(&m.game, m.seed, 8, 0, h, m.game.hist_n + 1)) break;
    }
    static char text[TB_MSG_MAX_TEXT];
    int n = tb_msg_text_encode(&m, text, sizeof text);
    CHECK(m.game.hist_n == 8 * 13 * 3 && n > 0 && n < 5000, "8 players, 312 bubbles, 48-byte names: %d chars", n);
    printf("  the longest game, 8 x 48-byte names, 312 bubbles: %d link characters (bound %d)\n", n, TB_MSG_MAX_TEXT - 1);
    TbMsg back;
    CHECK(tb_msg_text_decode(text, &back) == TB_EOK && back.game.over && tb_hash(&back.game) == tb_hash(&m.game),
          "and it reads back");
}

/* ---- the sweeps -------------------------------------------------------------------- */

static uint8_t tail[TB_MSG_MAX_BYTES + 64];

static void w_sweeps(void)
{
    TEST("wire sweeps");
    TbMsg m, back;
    started(&m, 5, 3, 0);
    for (int i = 0; i < 12; i++) CHECK(bot_step(&m.game, 0), "a bot move");
    static uint8_t b[TB_MSG_MAX_BYTES];
    int n = tb_msg_encode(&m, b, sizeof b);
    CHECK(n > 0, "encode");
    /* every prefix is refused, placed at the very end of the array (ASan) */
    for (int k = 0; k < n; k++) {
        uint8_t *at = tail + sizeof tail - k;
        memcpy(at, b, (size_t)k);
        CHECK(tb_msg_decode(at, k, &back) != TB_EOK, "prefix %d of %d refused", k, n);
    }
    /* every byte, corrupted: refused, or it reads as itself */
    int passed = 0;
    for (int i = 0; i < n; i++)
        for (int x = 1; x < 256; x += 37) {
            memcpy(tail, b, (size_t)n);
            tail[i] ^= (uint8_t)x;
            if (tb_msg_decode(tail, n, &back) != TB_EOK) continue;
            passed++;
            static uint8_t again[TB_MSG_MAX_BYTES];
            int k = tb_msg_encode(&back, again, sizeof again);
            CHECK(k == n && !memcmp(again, tail, (size_t)n), "a corruption that reads is its own game");
        }
    CHECK(passed < 4, "the check catches corruptions (%d passed)", passed);
    /* the header must agree with the replay */
    memcpy(tail, b, (size_t)n);
    tail[40]++;                                  /* turns */
    CHECK(tb_msg_decode(tail, n, &back) != TB_EOK, "a lying header");
    CHECK(tb_msg_decode(b, n, &back) == TB_EOK && tb_hash(&back.game) == tb_hash(&m.game), "the real one reads");
    b[1] = 2;
    CHECK(tb_msg_decode(b, n, &back) == TB_EFORMAT, "a newer format is refused as such");
    CHECK(tb_msg_text_decode("https://x/?q=1&m=%%%", &back) == TB_ETEXT, "not base32");
}

/* ---- every bubble of random games through the wire ------------------------------------- */

static int max_chars[9];

static void w_games(int games)
{
    TEST("wire games");
    for (int n = 2; n <= 8; n++)
        for (int k = 0; k < games; k++) {
            TbMsg m, back;
            started(&m, (uint32_t)(n * 1000 + k), n, n == 2);
            static char text[TB_MSG_MAX_TEXT];
            while (!m.game.over) {
                if (!bot_step(&m.game, 60)) { CHECK(0, "a bot move replays"); break; }
                int len = tb_msg_text_encode(&m, text, sizeof text);
                CHECK(len > 0, "encode %d", len);
                if (len > max_chars[n]) max_chars[n] = len;
                CHECK(tb_msg_text_decode(text, &back) == TB_EOK && tb_hash(&back.game) == tb_hash(&m.game),
                      "%d seats game %d bubble %d reads back", n, k, m.game.hist_n);
            }
        }
    for (int n = 2; n <= 8; n++) printf("  %d seats: longest link %d characters\n", n, max_chars[n]);
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 10;
    w_lobby();
    w_draft_encode();
    w_race();
    w_fit();
    w_sweeps();
    w_games(games);
    return report("tb_msg_test");
}
