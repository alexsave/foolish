// ios_api_smoke.c — a host-side smoke test for the iOS bridge. NOT part of the
// xcframework; compiled natively (clang) to prove new->legal->apply->state
// round-trips and that a full bot-vs-bot game runs to a fool through the same
// entry points Swift uses. Build/run:
//   clang -O2 -Isrc -Iios/include -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 \
//         -DMAX_MOVE_CARDS=28 ios/ios_api_smoke.c ios/ios_api.c <CORE_SRC> -lm
#include "ios_api.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char buf[1 << 16];

// last occurrence of c in [lo, hi] — find the '{' that opens the object at hi.
static const char *strrchr_upto(const char *lo, const char *hi, char c) {
    for (const char *p = hi; p >= lo; p--) if (*p == c) return p;
    return NULL;
}

int main(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 7 + 1);

    if (fio_new_game(seed, 32, 4) != FIO_EOK) { printf("FAIL new_game\n"); return 1; }
    printf("strategies=%d\n", fio_strategy_count());
    for (int i = 0; i < fio_strategy_count(); i++) {
        fio_strategy_name(i, buf, sizeof(buf));
        printf("  strat %d = %s\n", i, buf);
    }
    // seat 0 human, others cordite
    for (int s = 1; s < 4; s++) fio_set_seat_strategy(s, 8 /* cordite */);

    if (fio_state_json(0, buf, sizeof(buf)) < 0) { printf("FAIL state_json\n"); return 1; }
    printf("state[0] len=%zu head=%.120s\n", strlen(buf), buf);

    if (fio_legal_moves_json(0, buf, sizeof(buf)) < 0) { printf("FAIL legal\n"); return 1; }
    printf("legal[0]=%.200s\n", buf);

    // Drive the game to completion: at each step, if seat 0 (human) is eligible,
    // play its first legal move via apply_json; then let bots step.
    int steps = 0;
    while (fio_game_over() < 0 && steps++ < 5000) {
        int mask = fio_actor_mask();
        if (mask & 1) {
            // human seat 0 is eligible: parse first legal move, apply it back
            // through apply_json (exercises the JSON move parser end to end).
            if (fio_legal_moves_json(0, buf, sizeof(buf)) < 0) break;
            // Prefer an ending move (good/pickup) when available so the round
            // actually terminates — a real UI player does too; picking the first
            // legal move forever just keeps piling optional attacks.
            char move[4096];
            const char *pick = strstr(buf, "\"good\"");
            if (!pick) pick = strstr(buf, "\"pickup\"");
            const char *anchor = pick ? pick : buf;
            const char *start = pick ? strrchr_upto(buf, anchor, '{') : strchr(buf, '{');
            // Balanced-brace match to capture the WHOLE move object (moves nest
            // card objects, so the first '}' is not the object's end).
            const char *end = NULL;
            if (start) { int depth = 0; for (const char *p = start; *p; p++) {
                if (*p == '{') depth++; else if (*p == '}' && --depth == 0) { end = p; break; } } }
            if (start && end) {
                size_t n = (size_t)(end - start + 1);
                if (n < sizeof(move)) { memcpy(move, start, n); move[n] = 0;
                    int r = fio_apply_json(0, move);
                    if (r == FIO_EREJECT) { printf("human reject code=%d for %s\n", fio_last_reject(), move); break; }
                    if (r < 0) { printf("human apply error r=%d for %s\n", r, move); break; }
                }
            } else break; // seat 0 flagged eligible but no concrete move — shouldn't happen
        } else {
            // not the human's turn: drive exactly one bot action
            int acted = fio_bot_step_json(0, buf, sizeof(buf));
            if (acted == 0) break; // genuine deadlock (no one can move) — bail
        }
    }

    int fool = fio_game_over();
    printf("game over after %d steps, fool seat = %d\n", steps, fool);
    if (fool < 0) { printf("FAIL game did not finish\n"); return 1; }

    fio_state_json(0, buf, sizeof(buf));
    printf("final state head=%.160s\n", buf);
    printf("SMOKE OK\n");
    return 0;
}
