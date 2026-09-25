/* The post-game analyser's smoke test.
 *
 *     make -C uttt/c analyse-test
 *
 * 1. A GAME WITH AN OBVIOUS BLUNDER. Random play until the side to move has
 *    a mate in one; they play a move after which the OPPONENT has a forced
 *    win instead, and sniper finishes the game from there. The analyser must
 *    call that ply a blunder, say the forced win was thrown, and say it is
 *    proved - and must not call the mate the opponent then plays anything
 *    but best.
 *    TWO RULES LABEL IT, on purpose: the cost (1.0, far past 20 points) and
 *    the thrown-win rule. The second is the one that holds when quill's
 *    estimate of the played move is kind, so each is mutation-checked
 *    alone; the opponent's mate is likewise proved twice over (mate search,
 *    then the exact solver this near the end), and only the pair of them
 *    turns P(played) into an estimate.
 * 2. THE SAME BYTES TWICE. The whole report, from two runs in one process,
 *    hashed; in-process is the harder case, since the solver table and the
 *    kept tree would carry over if the analyser did not forget them. */
#define _POSIX_C_SOURCE 200809L
#include "../src/uttt_analyse.h"
#include "../src/uttt_bots.h"
#include "../src/uttt_code.h"
#include "../../../shared/c/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYOUTS 200

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Returns the ply index of the blunder, -1 when this seed found none. */
static int blunder_game(uint64_t seed, UtttGame *g)
{
    uint64_t rs = seed;
    uttt_init(g);
    int at = -1;
    while (!g->over) {
        uint8_t list[81], x;
        int n = uttt_legal(g, list);
        if (at < 0 && uttt_mate_in(g, 2000, &x) == 1) {
            for (int i = 0; i < n; i++) {
                UtttGame c = *g;
                uttt_play(&c, list[i]);
                if (!c.over && uttt_mate_in(&c, 2000, &x) > 0) {
                    at = g->n_plies;
                    uttt_play(g, list[i]);
                    break;
                }
            }
            if (at < 0) return -1;
            continue;
        }
        uttt_play(g, at < 0 ? uttt_bot_move(BOT_BIRO, g, 0, &rs)
                            : uttt_bot_move(BOT_SNIPER, g, 20, &rs));
    }
    return at;
}

static void report(const UtttGame *g, UtttNote *notes, uint8_t hash[32],
                   char **text)
{
    size_t len = 0;
    FILE *f = open_memstream(text, &len);
    uttt_analyse(g, PLAYOUTS, notes, NULL);
    uttt_analyse_print(f, g, notes, PLAYOUTS);
    fclose(f);
    sha256(*text, len, hash);
}

int main(void)
{
    UtttGame g;
    int at = -1;
    uint64_t seed = 1;
    for (; seed < 500 && at < 0; seed++) at = blunder_game(seed * 0x9E3779B97F4A7C15ull, &g);
    CHECK(at >= 0, "no blunder game in 500 seeds");
    if (at < 0) return 1;
    char url[128];
    uttt_replay_url(&g, 0, url, sizeof url);
    printf("game: %s (%d plies, blunder at ply %d)\n", url, g.n_plies, at + 1);

    static UtttNote a[UTTT_MAX_PLIES], b[UTTT_MAX_PLIES];
    uint8_t ha[32], hb[32];
    char *ta = NULL, *tb = NULL;
    report(&g, a, ha, &ta);
    report(&g, b, hb, &tb);

    const UtttNote *n = &a[at];
    CHECK(n->label == UA_BLUNDER, "ply %d labelled %s, not blunder",
          at + 1, UTTT_LABEL_NAME[n->label]);
    CHECK(n->tags & UA_TAG_THROWN, "ply %d: forced win not called thrown", at + 1);
    CHECK(n->tags & UA_TAG_PROOF, "ply %d: not marked proved", at + 1);
    CHECK(n->p_best == 1.0 && n->p_played == 0.0,
          "ply %d: P(best) %.2f P(played) %.2f, want 1 and 0",
          at + 1, n->p_best, n->p_played);
    CHECK(a[at + 1].label == UA_BEST || a[at + 1].label == UA_ONLY,
          "the reply to the blunder is %s", UTTT_LABEL_NAME[a[at + 1].label]);
    CHECK(strstr(ta, "blunder!") != NULL, "the report does not say blunder!");

    CHECK(memcmp(ha, hb, 32) == 0 && strcmp(ta, tb) == 0,
          "two runs printed different reports");
    printf("report sha256: ");
    for (int i = 0; i < 32; i++) printf("%02x", ha[i]);
    printf("\n");
    free(ta); free(tb);

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
