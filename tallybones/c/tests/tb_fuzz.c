/* Random play at 2..8 seats against every invariant, and through the body
 * coder after every bubble: decode(encode) is the game, and the body the
 * resident replay rolls from is the body the encoder writes.
 *
 *   ./build/tb_fuzz [games] [stream]  */
#include "tb_check.h"
#include "../src/tb_plan.h"

static int popcount(unsigned x) { int k = 0; while (x) { k += x & 1; x >>= 1; } return k; }

static void invariants(const TbGame *g, int *scores)
{
    TEST("fuzz invariants");
    CHECK(g->turns == *scores, "turns %d is the SCOREs %d", g->turns, *scores);
    int lo = 99, hi = -1, in = 0;
    for (int s = 0; s < g->n; s++) {
        for (int c = 0; c < TB_CATS; c++)
            CHECK((g->filled[s] >> c & 1) || g->score[s][c] == 0, "an open category holds nothing");
        if (!tb_is_in(g, s)) continue;
        in++;
        int f = popcount(g->filled[s]);
        if (f < lo) lo = f;
        if (f > hi) hi = f;
    }
    if (!g->over) {
        CHECK(hi - lo <= 1, "seats in play keep within one turn of each other (%d..%d)", lo, hi);
        CHECK(g->turn < g->n && tb_is_in(g, g->turn) && g->filled[g->turn] != TB_FULL_CARD, "a live turn seat");
        CHECK(g->roll >= 1 && g->roll <= 3, "roll %d", g->roll);
        for (int i = 0; i < 5; i++) CHECK(g->dice[i] >= 1 && g->dice[i] <= 6, "a resident die has a value");
        CHECK(in >= 2, "two seats in");
    } else {
        CHECK(g->turn == TB_SEAT_NONE && tb_winners(g) != 0, "over: nobody on turn, somebody won");
        int full = 1;
        for (int s = 0; s < g->n; s++) if (tb_is_in(g, s) && g->filled[s] != TB_FULL_CARD) full = 0;
        CHECK(full || in < 2, "over only when every card is full or one seat is left");
    }
}

static void wire(const TbGame *g)
{
    TEST("fuzz wire");
    static uint8_t a[TB_CODE_MAX], b[TB_CODE_MAX];
    static TbGame back;
    int n = tb_code_encode(g, a, sizeof a);
    CHECK(n > 0, "encode %d", n);
    CHECK(tb_code_decode(&back, g->seed, g->n, g->starter, g->hist_n, a, n, 1) && tb_hash(&back) == tb_hash(g),
          "decode(encode) is the game at bubble %d", g->hist_n);
    int m = tb_code_body(g, g->hist_n, b, sizeof b);
    CHECK(m == n && !memcmp(a, b, (size_t)n), "the body the roll reads is the body the wire carries");
}

static void draft_of(const TbGame *g)
{
    TEST("fuzz draft");
    if (g->over) return;
    static TbGame d, back;
    TbMove m = bot_move(g, 20);
    CHECK(tb_draft(&d, g, m), "a legal move drafts");
    if (m.kind == TB_M_KEEP)
        for (int i = 0; i < 5; i++)
            CHECK((m.arg >> i & 1) ? d.dice[i] == g->dice[i] : d.dice[i] == 0, "a draft keep derives nothing");
    static uint8_t a[TB_CODE_MAX];
    int n = tb_code_encode(&d, a, sizeof a);
    CHECK(n > 0 && tb_code_decode(&back, g->seed, g->n, g->starter, g->hist_n + 1, a, n, 1)
          && back.hist_n == g->hist_n + 1 && !memcmp(&back.hist[g->hist_n], &m, sizeof m), "the draft's body says the move");
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 700;
    RS ^= (uint64_t)(argc > 2 ? atoi(argv[2]) : 0) * 0x2545F4914F6CDD1Dull;
    long bubbles = 0, longest = 0, shouts = 0, bonuses = 0, leaves = 0, zeros = 0;
    for (int k = 0; k < games; k++) {
        uint8_t seed[32];
        seed_wide(seed, (uint32_t)k + 70000u * (uint32_t)(argc > 2 ? atoi(argv[2]) : 0));
        int n = 2 + k % 7;
        TbGame g;
        tb_new(&g, seed, n, (int)rnd((uint32_t)n));
        int scores = 0;
        invariants(&g, &scores);
        while (!g.over) {
            draft_of(&g);
            TbGame before = g;
            if (!bot_step(&g, 150)) { TEST("fuzz"); CHECK(0, "a bot move replays"); break; }
            TbMove m = g.hist[g.hist_n - 1];
            if (m.kind == TB_M_SCORE) {
                scores++;
                if (m.arg == TB_C_TALLYBONES && g.score[m.seat][m.arg]) shouts++;
                if (!g.score[m.seat][m.arg]) zeros++;
                if (!tb_bonus(&before, m.seat) && tb_bonus(&g, m.seat)) bonuses++;
            }
            leaves += m.kind == TB_M_LEAVE;
            if (m.kind == TB_M_KEEP) {
                TEST("fuzz keep");
                for (int i = 0; i < 5; i++)
                    if (m.arg >> i & 1) CHECK(g.dice[i] == before.dice[i], "a kept die holds");
            }
            invariants(&g, &scores);
            if (g.hist_n % 7 == 0 || g.over) wire(&g);
        }
        bubbles += g.hist_n;
        if (g.hist_n > longest) longest = g.hist_n;
    }
    printf("  %d games, %ld bubbles (longest %ld), %ld five-alike scored, %ld bonuses, %ld zeros, %ld leaves\n",
           games, bubbles, longest, shouts, bonuses, zeros, leaves);
    return report("tb_fuzz");
}
