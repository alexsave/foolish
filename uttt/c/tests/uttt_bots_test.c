/* The searching bots, held to three promises the ladder takes on trust.
 *
 *   LEGAL AND DETERMINISTIC. Every move is legal, and a game is a function
 *   of its seed: forget everything, replay it, and every move repeats.
 *
 *   A SIDE ONLY EVER RE-ROOTS ITS OWN TREE. The kept tree used to belong to
 *   whoever searched last, so quill@10 playing quill@4000 re-rooted the big
 *   tree and played its move. A one-playout reply after the other side's
 *   big search must be exactly the reply with no big search before it.
 *
 *   FOUNTAIN IS A PLAYER. It never loses to uniform play.
 *
 *     ./uttt/c/build/uttt_bots_test [games]
 */
#include "../src/uttt_bots.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

/* One game, X against O, into `moves`; returns the result. */
static uint8_t game(UtttBot x, int bx, UtttBot o, int bo, uint64_t seed, uint8_t *moves, int *n)
{
    UtttGame g; uttt_init(&g);
    uint64_t rs = seed | 1;
    uttt_bots_forget();
    *n = 0;
    while (!g.over) {
        uint8_t mv = g.turn == UTTT_X ? uttt_bot_move(x, &g, bx, &rs)
                                      : uttt_bot_move(o, &g, bo, &rs);
        if (!uttt_play(&g, mv)) {
            CHECK(0, "illegal move %d at ply %d", mv, g.n_plies);
            return 0;
        }
        moves[(*n)++] = mv;
    }
    return g.over;
}

static void deterministic(UtttBot a, UtttBot b, int games)
{
    for (int i = 0; i < games; i++) {
        uint8_t m1[81], m2[81]; int n1, n2;
        uint8_t r1 = game(a, 30, b, 20, 1000 + (uint64_t)i, m1, &n1);
        uint8_t r2 = game(a, 30, b, 20, 1000 + (uint64_t)i, m2, &n2);
        CHECK(r1 == r2 && n1 == n2 && !memcmp(m1, m2, (size_t)n1),
              "%s vs %s, seed %d: the replay diverged", UTTT_BOT_NAME[a], UTTT_BOT_NAME[b], i);
    }
}

/* Early positions only: past thirty empty squares the exact endgame's
 * proofs are shared by design, and a proof may settle a move sooner. */
static void own_tree(UtttBot bot, int games)
{
    uint64_t s = 77;
    for (int i = 0; i < games; i++) {
        UtttGame g; uttt_init(&g);
        int plies = 4 + (int)(xs(&s) % 12);
        for (int p = 0; p < plies && !g.over; p++) {
            uint8_t l[81]; int n = uttt_legal(&g, l);
            uttt_play(&g, l[xs(&s) % (uint64_t)n]);
        }
        if (g.over) continue;
        uint64_t r1 = 5 + (uint64_t)i, r2 = r1, rb = 99;

        uttt_bots_forget();
        UtttGame h = g;
        uint8_t big = uttt_bot_move(bot, &h, 400, &rb);      /* the big search */
        uttt_play(&h, big);
        if (h.over) continue;
        uint8_t after = uttt_bot_move(bot, &h, 1, &r1);

        uttt_bots_forget();                                   /* the same move, no tree */
        h = g;
        uttt_play(&h, big);
        uint8_t fresh = uttt_bot_move(bot, &h, 1, &r2);
        CHECK(after == fresh, "%s, position %d: the reply read the other side's tree (%d, alone %d)",
              UTTT_BOT_NAME[bot], i, after, fresh);
    }
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 10;
    deterministic(BOT_FOUNTAIN, BOT_QUILL, games);
    deterministic(BOT_QUILL, BOT_FOUNTAIN, games);
    own_tree(BOT_QUILL, games * 4);
    own_tree(BOT_FOUNTAIN, games * 4);

    int lost = 0;
    for (int i = 0; i < games; i++) {
        uint8_t m[81]; int n;
        int fx = i & 1;
        uint8_t r = fx ? game(BOT_FOUNTAIN, 20, BOT_RANDOM, 1, 500 + (uint64_t)i, m, &n)
                       : game(BOT_RANDOM, 1, BOT_FOUNTAIN, 20, 500 + (uint64_t)i, m, &n);
        if (r != UTTT_DRAW && (r == UTTT_X) != fx) lost++;
    }
    CHECK(lost == 0, "fountain lost %d of %d to uniform play", lost, games);

    printf("bots: %d games each way, %s\n", games, fails ? "FAILED" : "ok");
    return fails != 0;
}
