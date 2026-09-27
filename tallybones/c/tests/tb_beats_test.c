/* The plan and the timeline (T9): a turn's first bubble settles all five
 * dice, a KEEP bubble settles the rerolled dice only, a SCORE bubble stamps
 * and passes the turn, and every plan ends on the settled board.
 *
 *   ./build/tb_beats_test [games]  */
#include "tb_check.h"
#include "../src/tb_plan.h"
#include "../src/tb_beats.h"

static int build(const TbGame *g, int from, int to, int mode, TbBeats *b)
{
    static TbEvent ev[TB_BEATS_EVENTS];
    int n = tb_plan(g, from, to, ev, TB_BEATS_EVENTS);
    TbBeatFrame start;
    if (n < 0 || !tb_beats_pre(g, from, &start)) return -1;
    return tb_beats_build(ev, n, &start, g->n, mode, b);
}

static int kinds(const TbBeats *b, int *out)
{
    for (int i = 0; i < b->n; i++) out[i] = b->beat[i].kind;
    return b->n;
}

static TbBeats B;

static void b_start(void)
{
    TEST("beats start");
    uint8_t seed[32];
    seed_wide(seed, 1);
    TbGame g;
    tb_new(&g, seed, 2, 0);
    CHECK(build(&g, -1, 0, TB_BEATS_OPEN, &B) == 2, "the start: turn, settle (%d)", B.n);
    int k[8];
    kinds(&B, k);
    CHECK(k[0] == TB_BK_TURN && k[1] == TB_BK_SETTLE, "in that order");
    CHECK(B.beat[0].start_ms == TB_T_LEAD_OPEN, "an opened bubble leads by 100: %u", B.beat[0].start_ms);
    const TbBeat *s = &B.beat[1];
    CHECK(s->mask == 31 && s->parts == 5 && s->sub == 1, "roll 1 settles all five");
    CHECK(B.total_ms == TB_T_LEAD_OPEN + TB_T_TURN + TB_T_GAP + 4 * TB_T_SETTLE_STEP + TB_T_SETTLE,
          "the start plays in %u", B.total_ms);
    TbBeatFrame f;
    tb_beats_frame(&B, 0, &f);
    CHECK(f.turn == TB_SEAT_NONE && f.dice[0] == 0, "before: no turn, no dice");
    tb_beats_frame(&B, s->start_ms + 10, &f);
    CHECK(f.turn == 0 && (f.rolling & 1) && f.dice[0] >= 1 && f.dice[4] == 0, "die 0 tumbling, die 4 still in the cup");
    tb_beats_frame(&B, B.total_ms, &f);
    CHECK(f.done && !f.rolling && !memcmp(f.dice, g.dice, 5) && f.next_ms == TB_BEAT_NEVER, "settled on the dice");
}

static void b_keep(void)
{
    TEST("beats keep");
    uint8_t seed[32];
    seed_wide(seed, 2);
    TbGame g, before;
    tb_new(&g, seed, 2, 0);
    before = g;
    TbMove h[1] = { mv(TB_M_KEEP, 0, 0x05) };
    tb_replay(&g, seed, 2, 0, h, 1);
    CHECK(build(&g, 0, 1, TB_BEATS_ARRIVAL, &B) == 1, "a keep: one settle (%d)", B.n);
    const TbBeat *s = &B.beat[0];
    CHECK(s->kind == TB_BK_SETTLE && s->mask == 0x1A && s->parts == 3 && s->sub == 2, "the rerolled three only");
    CHECK(s->start_ms == TB_T_LEAD_LIVE, "an arrival leads by 16");
    for (uint32_t t = 0; t <= B.total_ms; t += 7) {
        TbBeatFrame f;
        tb_beats_frame(&B, t, &f);
        CHECK(!(f.rolling & 0x05) && f.dice[0] == before.dice[0] && f.dice[2] == before.dice[2],
              "a kept die never moves (t %u)", t);
        for (int i = 0; i < 5; i++) CHECK(!(f.rolling >> i & 1) || (f.dice[i] >= 1 && f.dice[i] <= 6), "a tumbling face");
        if (t >= s->start_ms) CHECK(f.kept == 0x05 && f.roll == 2, "the keep marks show");
    }
    TbBeatSample x;
    tb_beat_sample(s, s->start_ms + 50, 0, &x);
    CHECK(x.state == TB_BS_DONE && !x.apply, "a kept die's sample never applies");
    tb_beat_sample(s, s->start_ms + 50, 1, &x);
    CHECK(x.state == TB_BS_ACTIVE && x.apply && x.rot > 0, "die 1 is in the air");
    tb_beat_sample(s, s->start_ms + 50, 3, &x);
    CHECK(x.state == TB_BS_PENDING, "die 3 waits two steps");
}

static void b_score(void)
{
    TEST("beats score");
    uint8_t seed[32];
    seed_wide(seed, 3);
    TbGame g, before;
    tb_new(&g, seed, 3, 0);
    before = g;
    TbMove h[1] = { mv(TB_M_SCORE, 0, TB_C_ANY) };
    tb_replay(&g, seed, 3, 0, h, 1);
    CHECK(build(&g, 0, 1, TB_BEATS_OPEN, &B) == 3, "a score: stamp, turn, settle (%d)", B.n);
    int k[8];
    kinds(&B, k);
    CHECK(k[0] == TB_BK_STAMP && k[1] == TB_BK_TURN && k[2] == TB_BK_SETTLE && B.beat[2].mask == 31,
          "the stamp, the turn passing, then the next seat's five dice");
    const TbBeat *st = &B.beat[0];
    CHECK(st->cat == TB_C_ANY && st->value == g.score[0][TB_C_ANY] && st->total == tb_total(&g, 0), "what it stamps");
    TbBeatFrame f;
    tb_beats_frame(&B, st->start_ms + st->dur_ms - 1, &f);
    CHECK(!(f.filled[0] >> TB_C_ANY & 1) && f.total[0] == 0 && f.turn == 0, "in the air: not on the card yet");
    tb_beats_frame(&B, st->start_ms + st->dur_ms, &f);
    CHECK((f.filled[0] >> TB_C_ANY & 1) && f.total[0] == tb_total(&g, 0), "landed");
    CHECK(!memcmp(f.dice, before.dice, 5), "Alex's dice stay until the next roll");
    tb_beats_frame(&B, B.beat[1].start_ms, &f);
    CHECK(f.turn == 1, "the turn bar moves to Bo");
    TbBeatSample x;
    tb_beat_sample(st, st->start_ms - 1, 0, &x);
    CHECK(x.state == TB_BS_PENDING && x.apply && x.opacity == 0.0f, "a stamp is hidden before it starts");
}

static void b_end(void)
{
    TEST("beats end");
    uint8_t seed[32];
    seed_wide(seed, 4);
    TbGame g;
    TbMove h[1] = { mv(TB_M_LEAVE, 1, 0) };
    tb_new(&g, seed, 2, 0);
    tb_replay(&g, seed, 2, 0, h, 1);
    CHECK(build(&g, 0, 1, TB_BEATS_OPEN, &B) == 3, "a leave that ends it: fade, results, hold (%d)", B.n);
    CHECK(B.beat[0].kind == TB_BK_FADE && B.beat[0].sub == TB_FADE_SEAT_OUT && B.beat[1].sub == TB_FADE_RESULTS_IN
          && B.beat[2].kind == TB_BK_HOLD && B.beat[2].dur_ms == TB_T_OVER, "in that order");
    TbBeatFrame f;
    tb_beats_frame(&B, B.total_ms, &f);
    CHECK(f.results && f.winners == 1 && !(f.still_in & 2) && f.turn == TB_SEAT_NONE, "Alex wins, Bo gone");
}

/* Every bubble of random games: the plan ends on the board the game settles
 * on, and the clock only runs forward. */
static void b_games(int games)
{
    TEST("beats games");
    uint32_t longest = 0;
    for (int k = 0; k < games; k++) {
        uint8_t seed[32];
        seed_wide(seed, 60 + (uint32_t)k);
        TbGame g;
        int n = 2 + k % 7;
        tb_new(&g, seed, n, 0);
        while (!g.over) {
            if (!bot_step(&g, 80)) break;
            int nb = build(&g, g.hist_n - 1, g.hist_n, TB_BEATS_ARRIVAL, &B);
            CHECK(nb >= 0, "a bubble lays out");
            if (B.total_ms > longest) longest = B.total_ms;
            TbBeatFrame f;
            uint32_t t = 0, guard = 0;
            while (t != TB_BEAT_NEVER && guard++ < 400) {
                tb_beats_frame(&B, t, &f);
                CHECK(f.next_ms > t, "the next moment is later (%u, %u)", t, f.next_ms);
                t = f.next_ms;
            }
            CHECK(guard < 400, "the clock ends");
            tb_beats_frame(&B, B.total_ms, &f);
            CHECK(f.done && !memcmp(f.dice, g.dice, 5) && f.turn == g.turn, "the end frame is the settled board");
            for (int s = 0; s < n; s++)
                CHECK(f.filled[s] == g.filled[s] && f.total[s] == tb_total(&g, s) && ((f.still_in >> s) & 1) == tb_is_in(&g, s),
                      "seat %d settled", s);
            CHECK(B.total_ms <= 3000, "a bubble plays within 3 s (%u)", B.total_ms);
        }
        /* the whole game at once fits, or is refused whole */
        int all = build(&g, -1, g.hist_n, TB_BEATS_OPEN, &B);
        CHECK(all == -1 || all <= TB_BEATS_MAX, "a long range is refused, never cut");
    }
    printf("  the longest bubble: %u ms\n", longest);
}

int main(int argc, char **argv)
{
    b_start();
    b_keep();
    b_score();
    b_end();
    b_games(argc > 1 ? atoi(argv[1]) : 70);
    return report("tb_beats_test");
}
