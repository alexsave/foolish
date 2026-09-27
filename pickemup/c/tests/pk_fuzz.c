/* THE RANDOM-PLAY FUZZ: thousands of games at 2..8 players, every step a
 * legal action chosen by a fixed-seed PRNG (pk_check.h's bot), and after
 * every step the invariants the rules promise:
 *
 *   - card conservation: all 104 ids accounted for exactly once across the
 *     hands, the deck (buried start cards included) and the stack;
 *   - one representation (7.2.5): `exposed` and `said` only on one-card seats;
 *   - a turn ends: a bubble can never be sealed mid-turn (the long-game stop
 *     excepted), and every turn in a sealed bubble ended in a play or a pass;
 *   - undo by replay: a replay of hist[] reproduces the game's hash, and
 *     undoing a play or pass returns exactly the state before it, while a
 *     draw can never be undone (D8);
 *   - the game terminates, within the long-game stop.
 *
 *     ./build/pk_fuzz [games] [seed]
 */
#include "pk_check.h"

static int kind_of_last_turn(const PkGame *g)
{
    for (int i = g->hist_n - 1; i >= 0 && g->hist[i].kind != PK_A_BUBBLE; i--)
        if (g->hist[i].kind == PK_A_DRAW || g->hist[i].kind == PK_A_PLAY || g->hist[i].kind == PK_A_PASS)
            return g->hist[i].kind;
    return 0;
}

int main(int argc, char **argv)
{
    int games = argc > 1 ? atoi(argv[1]) : 2800;
    RS ^= argc > 2 ? (uint64_t)strtoull(argv[2], 0, 10) * 0x9e3779b97f4a7c15ull : 0;
    int over_kind[4] = { 0 }, per_n[PK_MAX_SEATS + 1] = { 0 };
    long total_actions = 0, total_bubbles = 0, replays = 0, undos = 0, reshuffles = 0;
    int max_actions = 0;

    for (int gi = 0; gi < games; gi++) {
        TEST("fuzz");
        uint8_t seed[32];
        seed_of(seed, 900000u + (uint32_t)gi);
        int n = 2 + gi % 7;
        PkGame g, prev, tmp;
        CHECK(pk_new(&g, seed, n), "game %d deals", gi);
        per_n[n]++;
        int steps = 0;
        for (; steps < 20000; steps++) {
            if (g.over && !g.b_open) break;
            prev = g;
            int before_bubbles = g.bubbles;
            if (!bot_step(&g)) {
                CHECK(0, "game %d step %d: nothing legal in a live game (n %d turn %d over %d "
                      "draft %d by %d, said %02x exposed %02x, deck %d stack %d, seat hands %d %d, "
                      "last turn action %d, ended %d, sealable %d)", gi, steps, g.n, g.turn, g.over,
                      g.b_open, g.b_sender, g.said, g.exposed, g.deck_n, g.stack_n, g.hand_n[0],
                      g.hand_n[1], kind_of_last_turn(&g), pk_turn_ended(&g), pk_can_seal(&g));
                break;
            }

            TEST("fuzz conservation");
            CHECK(conserved(&g), "game %d step %d: 104 cards accounted for", gi, steps);
            TEST("7.2.5 one representation");
            CHECK(one_rep(&g), "game %d step %d: exposed %02x said %02x on a seat not on one card",
                  gi, steps, g.exposed, g.said);
            TEST("fuzz a turn ends");
            if (g.b_open && !g.over && kind_of_last_turn(&g) == PK_A_DRAW)
                CHECK(!pk_can_seal(&g), "game %d step %d: sealable mid-turn", gi, steps);
            if (g.bubbles != before_bubbles) {
                int last = kind_of_last_turn(&prev);
                CHECK(prev.over || last != PK_A_DRAW, "game %d: a bubble sealed on a draw", gi);
            }
            CHECK(g.actions <= PK_MAX_ACTIONS && g.bubbles <= PK_MAX_BUBBLES
                  && g.hist_n <= PK_HIST_CAP, "game %d: within the caps", gi);

            TEST("fuzz undo by replay");
            if (g.bubbles != before_bubbles || rnd(100) < 10) {
                replays++;
                CHECK(pk_replay(&tmp, &g, g.hist_n) && pk_hash(&tmp) == pk_hash(&g),
                      "game %d step %d: the replay reproduces the game", gi, steps);
            }
            int just = g.b_open && g.hist_n > prev.hist_n ? g.hist[g.hist_n - 1].kind : 0;
            if (just && rnd(100) < 25) {
                tmp = g;
                if (just == PK_A_DRAW) {
                    CHECK(!pk_undo(&tmp), "game %d step %d: a draw was undone", gi, steps);
                } else if (just == PK_A_PLAY || just == PK_A_PASS) {
                    undos++;
                    CHECK(pk_undo(&tmp) && pk_hash(&tmp) == pk_hash(&prev),
                          "game %d step %d: undo is the state before the %s", gi, steps,
                          just == PK_A_PLAY ? "play" : "pass");
                }
            }
        }
        TEST("fuzz terminates");
        CHECK(g.over && !g.b_open, "game %d (n=%d) ended within %d steps", gi, n, steps);
        CHECK(g.winner < n, "game %d has a winner", gi);
        over_kind[g.over & 3]++;
        total_actions += g.actions;
        total_bubbles += g.bubbles;
        reshuffles += g.reshuffles;
        if (g.actions > max_actions) max_actions = g.actions;
    }

    printf("fuzz: %d games (", games);
    for (int n = 2; n <= PK_MAX_SEATS; n++) printf("%s%dp %d", n > 2 ? ", " : "", n, per_n[n]);
    printf("); out %d, stuck %d, long %d\n", over_kind[PK_OVER_OUT], over_kind[PK_OVER_STUCK],
           over_kind[PK_OVER_LONG]);
    printf("fuzz: %.1f actions and %.1f bubbles a game, max %d actions, %ld reshuffles, "
           "%ld replays, %ld undos checked\n",
           games ? (double)total_actions / games : 0.0, games ? (double)total_bubbles / games : 0.0,
           max_actions, reshuffles, replays, undos);
    return report("pk_fuzz");
}
