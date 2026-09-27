/* Random legal games to the end at 2..6 seats, every invariant after every
 * move:
 *   - the dice on the table are the seats' counts, and a call takes exactly
 *     one die away, a bid none
 *   - the menu is what cn_is_legal says, in rank order, the call first
 *   - a round's dice change only when a round opens
 *   - the game ends inside CN_MAX_MOVES with one winner holding dice and
 *     every other seat on none
 *   - replay equals apply, and the body round-trips
 *
 *     ./build/cn_fuzz [games] [stream] */
#include "cn_check.h"
#include "../src/cn_code.h"

static CnGame G, H, R;
static uint8_t body[CN_CODE_MAX];

static int sum_dice(const CnGame *g)
{
    int t = 0;
    for (int s = 0; s < g->n; s++) t += g->dice_n[s];
    return t;
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 3000;
    if (argc > 2) RS ^= (uint64_t)atoll(argv[2]) * 0x9e3779b97f4a7c15ull;
    long moves = 0, calls = 0, longest = 0;
    TEST("fuzz");
    for (int k = 0; k < games; k++) {
        uint8_t seed[32];
        seed_wide(seed, (uint32_t)k * 2654435761u);
        const int n = 2 + k % 5;
        cn_new(&G, seed, n);
        int calls_here = 0;
        while (G.phase != CN_PH_OVER) {
            CHECK(G.total == sum_dice(&G), "game %d: total %d is the counts' %d", k, G.total, sum_dice(&G));
            CHECK(G.dice_n[G.turn] > 0, "game %d: the seat on turn holds dice", k);
            CnMove menu[CN_MAX_DICE * CN_BID_FACES + 1];
            int nm = cn_legal(&G, menu, (int)(sizeof menu / sizeof menu[0]));
            CHECK(nm >= 1, "game %d: something is legal", k);
            CHECK(cn_is_call(menu[0]) == cn_can_call(&G), "game %d: the call first exactly when legal", k);
            int legal = 0;
            for (int q = 0; q <= G.total + 1; q++)
                for (int f = 0; f <= 7; f++) legal += cn_is_legal(&G, G.turn, bid(q, f));
            CHECK(legal == nm, "game %d: the menu is every legal move (%d vs %d)", k, nm, legal);
            for (int i = 1 + cn_can_call(&G); i < nm; i++)
                CHECK(cn_rank(menu[i].q, menu[i].f) > cn_rank(menu[i - 1].q, menu[i - 1].f), "ranked");
            CHECK(G.bid_q == 0 ? !cn_can_call(&G) : 1, "game %d: no call on an opening", k);
            /* the picker's table: for each face, the least legal quantity */
            for (int f = 2; f <= 6; f++) {
                int least = 0;
                for (int q = G.total; q >= 1; q--) if (cn_is_legal(&G, G.turn, bid(q, f))) least = q;
                CHECK(cn_min_quantity(&G, f) == least, "game %d face %d: least %d, the table %d", k, f, least, cn_min_quantity(&G, f));
            }

            H = G;
            CnMove m = bot_move(&G);
            CHECK(cn_apply(&G, G.turn, m), "game %d: the bot's move is legal", k);
            if (cn_is_call(m)) {
                calls_here++;
                CHECK(G.total == H.total - 1, "game %d: a call takes exactly one die (%d -> %d)", k, H.total, G.total);
                int c = 0;
                for (int s = 0; s < n; s++)
                    for (int i = 0; i < H.dice_n[s]; i++) c += H.dice[s][i] == H.bid_f || H.dice[s][i] == 1;
                int loser = c >= H.bid_q ? H.turn : H.bidder;
                CHECK(G.call_loser == loser && G.dice_n[loser] == H.dice_n[loser] - 1, "game %d: the right loser", k);
                if (G.phase != CN_PH_OVER)
                    CHECK(G.round == H.round + 1 && G.round_at == G.hist_n, "game %d: a new round opens", k);
            } else {
                CHECK(G.total == H.total && G.round == H.round && !memcmp(G.dice, H.dice, sizeof G.dice),
                      "game %d: a bid changes no die", k);
            }
            CHECK(G.hist_n <= CN_MAX_MOVES_OF(n), "game %d: within the bound", k);
            if (rnd(50) == 0) {
                CHECK(cn_replay(&R, &G, G.hist_n) && cn_hash(&R) == cn_hash(&G), "game %d: replay equals apply", k);
            }
        }
        int live = 0;
        for (int s = 0; s < n; s++) live += G.dice_n[s] > 0;
        CHECK(live == 1 && G.winner < n && G.dice_n[G.winner] > 0, "game %d: one winner", k);
        CHECK(G.total == G.dice_n[G.winner] && calls_here == 5 * n - G.total, "game %d: one die per call", k);
        CHECK(cn_replay(&R, &G, G.hist_n) && cn_hash(&R) == cn_hash(&G), "game %d: the end replays", k);
        int len = cn_code_encode(&G, body, sizeof body);
        CHECK(len > 0 && cn_code_decode(&R, seed, n, G.hist_n, body, len) && cn_hash(&R) == cn_hash(&G),
              "game %d: the body round-trips", k);
        moves += G.hist_n;
        calls += calls_here;
        if (G.hist_n > longest) longest = G.hist_n;
    }
    printf("cn_fuzz: %d games, %ld moves, %ld calls, longest %ld\n", games, moves, calls, longest);
    return report("cn_fuzz");
}
