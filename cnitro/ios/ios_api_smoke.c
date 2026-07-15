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
// The animation plan is read into its own buffer: `buf` still holds the legal
// menu the current move was picked out of.
static char evbuf[1 << 18];

// last occurrence of c in [lo, hi] — find the '{' that opens the object at hi.
static const char *strrchr_upto(const char *lo, const char *hi, char c) {
    for (const char *p = hi; p >= lo; p--) if (*p == c) return p;
    return NULL;
}

// How many times `needle` occurs in `s` (non-overlapping).
static int count_of(const char *s, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = strstr(s, needle); p; p = strstr(p + len, needle)) n++;
    return n;
}

// Replay round-trip over MANY seeds and player counts.
//
// The single-seed round-trip at the end of main() is not enough: it hid a bug
// that broke ~50% of 2p and ~75% of 4p offline share codes for months. The
// encoder stamped g->first_attacker (which is REASSIGNED every bout) into the
// header slot meaning "the seat that opened the game", so encode died with
// REPLAY_ENOTINMENU on step 0 for every game whose last attacker was not its
// first. The one hard-coded seed happened to be a game where those coincided.
// Whether a given seed trips it is a property of the DEAL, so the only honest
// guard is a sweep.
//
// handwritten (fast, no sampling) keeps this cheap enough for CI.
static int replay_sweep(void) {
    int strat = -1;
    for (int i = 0; i < fio_strategy_count(); i++) {
        char nm[64];
        if (fio_strategy_name(i, nm, sizeof(nm)) > 0 && !strcmp(nm, "handwritten")) { strat = i; break; }
    }
    if (strat < 0) { printf("FAIL replay sweep: no handwritten rung\n"); return 1; }

    const int counts[] = { 2, 3, 4, 6 };
    int checked = 0, skipped = 0, v6_checked = 0;
    for (int ci = 0; ci < (int)(sizeof(counts) / sizeof(counts[0])); ci++) {
        for (int s = 0; s < 12; s++) {
            int players = counts[ci];
            unsigned char seed[32];
            for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 31 + s * 7 + players);

            if (fio_new_game(seed, 32, players) != FIO_EOK) { printf("FAIL sweep new_game\n"); return 1; }
            for (int p = 0; p < players; p++) fio_set_seat_strategy(p, strat);

            int steps = 0;
            while (fio_game_over() < 0 && steps++ < 5000)
                if (fio_bot_step_json(-1, buf, sizeof(buf)) <= 0) break;

            int fool = fio_game_over();
            if (fool < 0) { printf("FAIL sweep p=%d seed=%d did not finish\n", players, s); return 1; }

            static char code[8192];
            int clen = fio_replay_encode_b32(code, sizeof(code));
            if (clen < 0) {
                // A game longer than MAX_LOGS cannot be encoded at all — a
                // documented build limit, not an encoder fault (the same skip
                // as tests/replay_difftest.c).
                if (fio_last_replay_error() == 21 /* REPLAY_EINPUT */) { skipped++; continue; }
                printf("FAIL sweep encode p=%d seed=%d err=%d detail=%d steps=%d\n",
                       players, s, clen, fio_last_replay_error(), steps);
                return 1;
            }
            if (fio_replay_decode_json(code, buf, sizeof(buf)) < 0) {
                printf("FAIL sweep decode p=%d seed=%d err=%d\n", players, s, fio_last_replay_error());
                return 1;
            }
            const char *fp = strstr(buf, "\"fool\":");
            int decoded_fool = fp ? atoi(fp + 7) : -999;
            if (decoded_fool != fool) {
                printf("FAIL sweep fool mismatch p=%d seed=%d decoded=%d game=%d\n",
                       players, s, decoded_fool, fool);
                return 1;
            }

            // Same game as v6 (A4): these games are dealt from a 32-byte seed,
            // so the kernel can re-derive the deal and the share carries exact
            // hands. Goes through fio_replay_share_code_b32 — the call the app
            // actually makes — so this proves the format CHOICE too, not just
            // the encoder: a seeded game must come out as v6.
            static char code6[8192];
            int c6 = fio_replay_share_code_b32(code6, sizeof(code6));
            if (c6 < 0) {
                printf("FAIL sweep v6 encode p=%d seed=%d err=%d detail=%d\n",
                       players, s, c6, fio_last_replay_error());
                return 1;
            }
            if (fio_replay_decode_json(code6, buf, sizeof(buf)) < 0) {
                printf("FAIL sweep v6 decode p=%d seed=%d err=%d\n", players, s,
                       fio_last_replay_error());
                return 1;
            }
            const char *vp = strstr(buf, "\"version\":");
            if (!vp || atoi(vp + 10) != 6) {
                printf("FAIL sweep v6 version p=%d seed=%d\n", players, s);
                return 1;
            }
            fp = strstr(buf, "\"fool\":");
            if ((fp ? atoi(fp + 7) : -999) != fool) {
                printf("FAIL sweep v6 fool mismatch p=%d seed=%d\n", players, s);
                return 1;
            }
            // No hidden card survives a v6 decode — that is the whole point.
            if (strstr(buf, "\"s\":-1")) {
                printf("FAIL sweep v6 leaked a hidden card p=%d seed=%d\n", players, s);
                return 1;
            }
            v6_checked++;
            checked++;
        }
    }
    printf("replay sweep OK (%d games round-tripped, %d as v6 with exact hands, "
           "%d skipped as over-long)\n", checked, v6_checked, skipped);
    return 0;
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
    int steps = 0, ev_moves = 0, ev_total = 0;
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

                    // The animation plan for the move just applied (§16.B4): the
                    // human's own card flies by the kernel's plan exactly as a
                    // bot's does. Every event must carry "state" — the board AS
                    // OF that step, masked for this viewer — because a cycle that
                    // applied several actions is otherwise only drawable at its
                    // final state, and re-deriving the intermediate boards client
                    // -side is the thing BoardDiff.swift was cancelled for.
                    int el = fio_last_events_json(0, evbuf, sizeof(evbuf));
                    if (el < 0) { printf("FAIL last_events err=%d\n", el); return 1; }
                    int n_ev = count_of(evbuf, "{\"type\":");
                    int n_state = count_of(evbuf, "\"state\":{");
                    if (n_ev != n_state) {
                        printf("FAIL events: %d events but %d carried state\n", n_ev, n_state);
                        return 1;
                    }
                    ev_moves++;
                    ev_total += n_ev;
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

    printf("events: %d moves produced %d events, all carrying per-step state\n", ev_moves, ev_total);
    if (ev_moves == 0 || ev_total == 0) { printf("FAIL no animation events observed\n"); return 1; }

    fio_state_json(0, buf, sizeof(buf));
    printf("final state head=%.160s\n", buf);

    // Replay round-trip (§16.C): encode the finished game to a base32 code,
    // decode it back, and check the decoded fool matches the game's fool.
    static char code[8192];
    int clen = fio_replay_encode_b32(code, sizeof(code));
    if (clen < 0) { printf("FAIL replay encode err=%d detail=%d\n", clen, fio_last_replay_error()); return 1; }
    printf("replay code (%d chars): %.60s%s\n", clen, code, clen > 60 ? "..." : "");
    int dlen = fio_replay_decode_json(code, buf, sizeof(buf));
    if (dlen < 0) { printf("FAIL replay decode err=%d detail=%d\n", dlen, fio_last_replay_error()); return 1; }
    printf("decoded head=%.140s\n", buf);
    // find "fool": in the decoded JSON and compare to the game's fool.
    const char *fp = strstr(buf, "\"fool\":");
    int decoded_fool = fp ? atoi(fp + 7) : -999;
    if (decoded_fool != fool) { printf("FAIL replay fool mismatch: decoded=%d game=%d\n", decoded_fool, fool); return 1; }
    printf("replay round-trip OK (fool=%d)\n", decoded_fool);

    if (replay_sweep() != 0) return 1;

    printf("SMOKE OK\n");
    return 0;
}
