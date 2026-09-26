/* uttt_store_game.c - the one real game behind every App Store frame.
 *
 * The first 18 plies are fixed: the rig's `devgame` opening. The rest is
 * fountain for both seats, X on a small budget and O on a large one, because
 * the listing's win is O's, on a big-board DIAGONAL (owner, 2026-09-26). Every
 * ply goes through uttt_play, the whole list is replayed from scratch through
 * the rules again, and the replay link (drawing seed 77, the DEBUG seeded
 * game's) is read back and compared. See docs/STORE_SHOTS.md.
 *
 *   make store-game          (seed 44, X 5 rollouts, O 2000 - the recorded game)
 *
 * Exits 1 unless O wins on a diagonal, so a changed bot cannot quietly hand
 * the frames a different ending. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_bots.h"
static const uint8_t FIX[] = {34,67,44,80,76,43,69,62,79,63,4,40,39,31,37,16,70,71};
int main(int argc, char **argv) {
    uint64_t seed = argc > 1 ? strtoull(argv[1], 0, 10) : 1;
    int xbudget = argc > 2 ? atoi(argv[2]) : 5;
    int obudget = argc > 3 ? atoi(argv[3]) : 2000;
    UtttGame g; uttt_init(&g);
    for (unsigned i = 0; i < sizeof FIX; i++)
        if (!uttt_play(&g, FIX[i])) { printf("ILLEGAL fixed ply %u (%d)\n", i + 1, FIX[i]); return 1; }
    uint64_t rs = seed | 1;
    while (!g.over) {
        uint8_t mv = uttt_bot_move(BOT_FOUNTAIN, &g,
                                     g.n_plies % 2 == 0 ? xbudget : obudget, &rs);
        if (!uttt_play(&g, mv)) { printf("ILLEGAL bot ply\n"); return 1; }
    }
    /* verify: replay the whole list from scratch through the rules */
    UtttGame v; uttt_init(&v);
    for (int i = 0; i < g.n_plies; i++) if (!uttt_play(&v, g.move[i])) { printf("REPLAY FAIL %d\n", i); return 1; }
    char url[256]; uttt_replay_url(&g, 77, url, sizeof url);
    UtttGame back; int32_t s2 = 0;
    int ok = uttt_replay_read(url, &back, &s2) && back.n_plies == g.n_plies && s2 == 77;
    for (int i = 0; ok && i < g.n_plies; i++) ok = back.move[i] == g.move[i];
    printf("seed=%llu over=%s plies=%d line=%d roundtrip=%s\n%s\n", (unsigned long long)seed,
           g.over == UTTT_X ? "X" : g.over == UTTT_O ? "O" : "draw", g.n_plies, uttt_won_line(&g),
           ok ? "ok" : "FAIL", url);
    for (int i = 0; i < g.n_plies; i++) printf("%d%s", g.move[i], i + 1 < g.n_plies ? "," : "\n");
    int line = uttt_won_line(&g);
    return (ok && g.over == UTTT_O && (line == 6 || line == 7)) ? 0 : 1;
}
