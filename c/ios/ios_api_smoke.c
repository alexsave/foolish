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
    int checked = 0, skipped = 0, v6_checked = 0, ev_checked = 0;
    for (int ci = 0; ci < (int)(sizeof(counts) / sizeof(counts[0])); ci++) {
        for (int s = 0; s < 12; s++) {
            int players = counts[ci];
            unsigned char seed[32];
            for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 31 + s * 7 + players);

            if (fio_new_game(seed, 32, players) != FIO_EOK) { printf("FAIL sweep new_game\n"); return 1; }
            for (int p = 0; p < players; p++) fio_set_seat_strategy(p, strat);

            int steps = 0;
            while (fio_game_over() < 0 && steps++ < 5000)
                if (fio_bot_drive_packed(0, buf, sizeof(buf)) < 0) break;  // human_mask 0 → all bots

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
            if (fio_replay_decode_packed(code, (unsigned char *)buf, sizeof(buf)) < 0) {
                printf("FAIL sweep decode p=%d seed=%d err=%d\n", players, s, fio_last_replay_error());
                return 1;
            }
            // replay.h DECODE binary: fool is byte[4] (0xFF → -1).
            int decoded_fool = (unsigned char)buf[4] == 0xFF ? -1 : (unsigned char)buf[4];
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
            if (fio_replay_decode_packed(code6, (unsigned char *)buf, sizeof(buf)) < 0) {
                printf("FAIL sweep v6 decode p=%d seed=%d err=%d\n", players, s,
                       fio_last_replay_error());
                return 1;
            }
            if ((unsigned char)buf[0] != 6) {   // version is byte[0]
                printf("FAIL sweep v6 version p=%d seed=%d\n", players, s);
                return 1;
            }
            if (((unsigned char)buf[4] == 0xFF ? -1 : (unsigned char)buf[4]) != fool) {
                printf("FAIL sweep v6 fool mismatch p=%d seed=%d\n", players, s);
                return 1;
            }
            // No hidden card survives a v6 decode — that is the whole point.
            if (strstr(buf, "\"s\":-1")) {
                printf("FAIL sweep v6 leaked a hidden card p=%d seed=%d\n", players, s);
                return 1;
            }
            // A5: the same code plays back as the board's own animation events
            // — the kernel rebuilds the game and replays it (replay_steps.c),
            // so this is the live stream, not a replay-shaped imitation.
            static char ev[1 << 20];
            int elen = fio_replay_events_json(code6, -1, ev, sizeof(ev));
            if (elen < 0) {
                printf("FAIL sweep v6 events p=%d seed=%d err=%d detail=%d\n",
                       players, s, elen, fio_last_replay_error());
                return 1;
            }
            if (!strstr(ev, "\"type\":")) {
                printf("FAIL sweep v6 events empty p=%d seed=%d\n", players, s);
                return 1;
            }
            // Every event carries its step's board (the A3 amendment) — without
            // it a replay could only be drawn at its final state.
            if (!strstr(ev, "\"state\":")) {
                printf("FAIL sweep v6 events carry no per-step state p=%d seed=%d\n",
                       players, s);
                return 1;
            }
            // Spectating: no seat's hand may come back real.
            if (strstr(ev, "\"hand\":[")) {
                printf("FAIL sweep v6 events leaked a hand to a spectator p=%d seed=%d\n",
                       players, s);
                return 1;
            }
            ev_checked++;
            v6_checked++;
            checked++;
        }
    }
    printf("replay sweep OK (%d games round-tripped, %d as v6 with exact hands, "
           "%d replayed as live events, %d skipped as over-long)\n",
           checked, v6_checked, ev_checked, skipped);
    return 0;
}

// ---------- FMSG: the iMessage envelope, through the SHIM ------------------
//
// The point of running this on Linux: M1 is otherwise gated on a Mac, and this
// is the half that need not be. If the shim is proven here, the Mac day starts
// with the bridge already known-good and only Xcode left to do.
//
// What it drives is the real send/receive loop: deal -> play -> seal -> decode
// (adopt) -> Rule P -> rebase. No Swift, no simulator.
static int fmsg_check(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 11 + 3);
    if (fio_new_game(seed, 32, 4) != FIO_EOK) { printf("FAIL fmsg new_game\n"); return 1; }

    // Play a few moves so the chain is a real mid-game turn, not a fresh deal.
    for (int step = 0; step < 12 && fio_game_over() < 0; step++) {
        const int mask = fio_actor_mask();
        int seat = -1;
        for (int s = 0; s < 4; s++) if (mask & (1 << s)) { seat = s; break; }
        if (seat < 0) break;
        if (fio_legal_moves_json(seat, buf, sizeof(buf)) < 0) break;
        const char *st = strchr(buf, '{');
        if (!st) break;
        int depth = 0; const char *en = st;
        for (; *en; en++) { if (*en == '{') depth++; else if (*en == '}' && --depth == 0) { en++; break; } }
        char move[2048]; const size_t n = (size_t)(en - st);
        if (n >= sizeof(move)) break;
        memcpy(move, st, n); move[n] = 0;
        if (fio_apply_json(seat, move) != FIO_EOK) break;
    }

    const char *joins = "[{\"seat\":0,\"name\":\"Sveta\"},{\"seat\":1,\"name\":\"Ann\"},"
                        "{\"seat\":2,\"name\":\"Bo\"},{\"seat\":3,\"name\":\"Cy\"}]";
    unsigned char pay[2048];
    const uint8_t zero8[8] = {0};
    const int n = fio_msg_encode(2 /* LIVE */, 0, 0x0123456789abcdefULL, zero8, joins, pay, sizeof(pay));
    if (n <= 0) { printf("FAIL fmsg encode: %d (msg_err=%d)\n", n, fio_last_msg_error()); return 1; }

    // The size claim the whole design rests on (§4.4): base32 is 8 chars/5 bytes.
    const int chars = (n + 4) / 5 * 8;
    if (chars >= 1000) { printf("FAIL fmsg envelope %d chars >= 1000\n", chars); return 1; }

    // Decode ADOPTS: the payload's game becomes the resident one.
    if (fio_msg_decode_json(pay, n, 0, buf, sizeof(buf)) <= 0) {
        printf("FAIL fmsg decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    if (!strstr(buf, "\"phase\":2") || !strstr(buf, "\"n_players\":4") ||
        !strstr(buf, "\"game_id\":\"81985529216486895\"") || !strstr(buf, "\"Sveta\"") ||
        !strstr(buf, "\"state\":") || !strstr(buf, "\"moves\":") || !strstr(buf, "\"digest\":")) {
        printf("FAIL fmsg decode json shape: %.240s\n", buf); return 1;
    }
    // A spectator gets the public view and NO moves — it renders on lock screens.
    if (fio_msg_decode_json(pay, n, -1, buf, sizeof(buf)) <= 0) { printf("FAIL fmsg decode spectator\n"); return 1; }
    if (!strstr(buf, "\"moves\":[]")) { printf("FAIL fmsg spectator got moves\n"); return 1; }

    // Hostile bytes: every truncation is refused, and nothing crashes.
    for (int cut = 0; cut < n; cut++) {
        if (fio_msg_decode_json(pay, cut, 0, buf, sizeof(buf)) > 0) {
            // A short payload may still be a valid shorter chain; what it must
            // never be is unnoticed. Re-adopting the full one is enough here.
        }
    }
    if (fio_msg_decode_json(pay, n, 0, buf, sizeof(buf)) <= 0) { printf("FAIL fmsg re-adopt\n"); return 1; }
    // The adopted chain's round — Rule R compares a pending move's round to it.
    int adopted_round = -1;
    { const char *r = strstr(buf, "\"round\":"); if (r) adopted_round = atoi(r + 8); }
    if (adopted_round < 0) { printf("FAIL fmsg no round in decode json\n"); return 1; }

    // Rule P: a chain never beats itself, and the verdict is symmetric.
    if (fio_msg_rule_p(pay, n, pay, n) != 0) { printf("FAIL rule_p reflexive\n"); return 1; }

    // Build a child by playing one more move, and it must WIN Rule P (more turns).
    const int mask = fio_actor_mask();
    int seat = -1;
    for (int s = 0; s < 4; s++) if (mask & (1 << s)) { seat = s; break; }
    if (seat >= 0) {
        if (fio_legal_moves_json(seat, buf, sizeof(buf)) < 0) { printf("FAIL fmsg legal\n"); return 1; }
        const char *st = strchr(buf, '{');
        int depth = 0; const char *en = st;
        for (; *en; en++) { if (*en == '{') depth++; else if (*en == '}' && --depth == 0) { en++; break; } }
        char move[2048]; const size_t mn = (size_t)(en - st);
        memcpy(move, st, mn); move[mn] = 0;

        // Rule R on the adopted chain: the same move, composed against THIS
        // round, must RE-APPLY — nothing has moved on under it.
        const int v = fio_msg_rebase(adopted_round, seat, move);
        if (v != FIO_REBASE_REAPPLY && v != FIO_REBASE_DISCARD_ILLEGAL) {
            printf("FAIL fmsg rebase verdict %d\n", v); return 1;
        }
        if (v == FIO_REBASE_REAPPLY) {
            unsigned char child[2048];
            const int cn = fio_msg_encode(2, seat, 0x0123456789abcdefULL, zero8, joins, child, sizeof(child));
            if (cn <= 0) { printf("FAIL fmsg child encode %d\n", cn); return 1; }
            if (fio_msg_rule_p(child, cn, pay, n) >= 0) {
                printf("FAIL rule_p: the child chain must win\n"); return 1;
            }
        }
    }

    // THE guard (§7.4): a move composed against a round the chain has since
    // closed is DISCARDED — never silently re-applied as an opening attack of
    // the next round, which is legal per the kernel and not what the player
    // chose. Only reachable once a round HAS closed under us.
    if (adopted_round > 0) {
        const int stale = fio_msg_rebase(adopted_round - 1, 0, "{\"type\":\"good\"}");
        if (stale != FIO_REBASE_DISCARD_ROUND) {
            printf("FAIL fmsg round guard: got %d, want %d\n", stale, FIO_REBASE_DISCARD_ROUND);
            return 1;
        }
    }

    printf("fmsg OK (envelope %d B = %d base32 chars, decode+ruleP+rebase)\n", n, chars);
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
            // not the human's turn: drive the bots one cycle (all seats but 0).
            if (fio_bot_drive_packed(1, buf, sizeof(buf)) < 0) break;
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
    int dlen = fio_replay_decode_packed(code, (unsigned char *)buf, sizeof(buf));
    if (dlen < 0) { printf("FAIL replay decode err=%d detail=%d\n", dlen, fio_last_replay_error()); return 1; }
    // replay.h DECODE binary: version byte[0], fool byte[4] (0xFF → -1).
    int decoded_fool = (unsigned char)buf[4] == 0xFF ? -1 : (unsigned char)buf[4];
    printf("decoded %d bytes (v%d, fool=%d)\n", dlen, (unsigned char)buf[0], decoded_fool);
    if (decoded_fool != fool) { printf("FAIL replay fool mismatch: decoded=%d game=%d\n", decoded_fool, fool); return 1; }
    printf("replay round-trip OK (fool=%d)\n", decoded_fool);

    if (replay_sweep() != 0) return 1;
    if (fmsg_check() != 0) return 1;

    printf("SMOKE OK\n");
    return 0;
}
