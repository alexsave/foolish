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

// How many times `needle` occurs in `s` (non-overlapping).
static int count_of(const char *s, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = strstr(s, needle); p; p = strstr(p + len, needle)) n++;
    return n;
}

// From the PACKED legal wire (fio_legal_packed: u32 count, then per move
// {type,n,cards[n],attacks[n]}), build the awire frame for the first pickup(3)/
// good(4) move if present — a real player ends the round rather than piling
// optional attacks — else the first legal move. Returns awire length, or 0.
// Mirrors the frame first_move_awire builds in ios_goldens.c.
static int pick_move_awire(const unsigned char *packed, int len, unsigned char *out) {
    if (len < 4) return 0;
    unsigned int n = (unsigned)packed[0] | ((unsigned)packed[1] << 8)
                   | ((unsigned)packed[2] << 16) | ((unsigned)packed[3] << 24);
    const unsigned char *p = packed + 4, *end = packed + len;
    const unsigned char *first = NULL, *chosen = NULL;
    for (unsigned int i = 0; i < n && p + 2 <= end; i++) {
        int nc = p[1];
        const unsigned char *rec = p;
        p += 2 + 2 * nc;
        if (p > end) break;
        if (!first) first = rec;
        if ((rec[0] == 3 || rec[0] == 4) && !chosen) chosen = rec;
    }
    const unsigned char *m = chosen ? chosen : first;
    if (!m) return 0;
    int t = m[0], nc = m[1];
    const unsigned char *cards = m + 2, *attacks = m + 2 + nc;
    int o = 0;
    if (t == 3 || t == 4) { out[o++] = (unsigned char)t; out[o++] = 0; return o; } // pickup/good
    out[o++] = (unsigned char)t; out[o++] = (unsigned char)nc;
    for (int i = 0; i < nc; i++) out[o++] = cards[i];
    if (t == 1) for (int i = 0; i < nc; i++) out[o++] = attacks[i];   // cover carries attacks
    return o;
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
            // Since 1.0(4) the seeded encoder stamps v7 (v6 + the pass-mode
            // bit, byte-identical play — replay.h's REPLAY_FORMAT_VERSION_V7
            // note). Old v6 codes still decode; fresh encodes must be v7.
            if ((unsigned char)buf[0] != 7) {   // version is byte[0]
                printf("FAIL sweep v6 version p=%d seed=%d got=%d\n", players, s,
                       (unsigned char)buf[0]);
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
        int lrc = fio_legal_packed(seat, buf, sizeof(buf));
        if (lrc < 0) break;
        unsigned char aw[64];
        int al = pick_move_awire((const unsigned char *)buf, lrc, aw);
        if (al == 0) break;
        if (fio_apply_awire(seat, aw, al) != FIO_EOK) break;
    }

    const char *joins = "[{\"seat\":0,\"name\":\"Sveta\"},{\"seat\":1,\"name\":\"Ann\"},"
                        "{\"seat\":2,\"name\":\"Bo\"},{\"seat\":3,\"name\":\"Cy\"}]";
    unsigned char pay[2048];
    const uint8_t zero8[8] = {0};
    const int n = fio_msg_encode(2 /* LIVE */, 0, 0x0123456789abcdefULL, zero8, joins, 0 /* no send clock in this smoke */, pay, sizeof(pay));
    if (n <= 0) { printf("FAIL fmsg encode: %d (msg_err=%d)\n", n, fio_last_msg_error()); return 1; }

    // The size claim the whole design rests on (§4.4): base32 is 8 chars/5 bytes.
    const int chars = (n + 4) / 5 * 8;
    if (chars >= 1000) { printf("FAIL fmsg envelope %d chars >= 1000\n", chars); return 1; }

    // Decode ADOPTS: the payload's game becomes the resident one. The metadata
    // comes back as the PACKED blob (fio_msg_decode_packed layout): phase(1)
    // n_players(1) last_actor_seat(1) round(1) turn(u16) game_id(u64) parent8(8)
    // digest(32) sent_at(u16) n_new(1) n_joins(1) then joins {seat(1) len(1) name[]}.
    unsigned char *mb = (unsigned char *)buf;
    if (fio_msg_decode_packed(pay, n, mb, sizeof(buf)) <= 0) {
        printf("FAIL fmsg decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    unsigned long long gid = 0;
    for (int i = 0; i < 8; i++) gid |= (unsigned long long)mb[6 + i] << (8 * i);
    if (mb[0] != 2 /* phase LIVE */ || mb[1] != 4 /* n_players */ || gid != 81985529216486895ULL) {
        printf("FAIL fmsg decode packed shape: phase=%d n=%d gid=%llu\n", mb[0], mb[1], gid); return 1;
    }
    // Seat 0's join is "Sveta" - the first record after the 58-byte header
    // (round 16 added the two send-clock bytes and the bubble delta ahead of
    // n_joins).
    if (!(mb[58] == 0 && mb[59] == 5 && memcmp(mb + 60, "Sveta", 5) == 0)) {
        printf("FAIL fmsg decode: seat-0 join not Sveta\n"); return 1;
    }
    // The digest (Rule P's tiebreak) is present and not all-zero.
    { int allzero = 1; for (int i = 0; i < 32; i++) if (mb[22 + i]) { allzero = 0; break; }
      if (allzero) { printf("FAIL fmsg digest all-zero\n"); return 1; } }
    // The adopted chain's round — Rule R compares a pending move's round to it.
    const int adopted_round = mb[3];   // round is byte[3]

    // Hostile bytes: every truncation is refused and nothing crashes; then the
    // full payload re-adopts cleanly.
    for (int cut = 0; cut < n; cut++) (void)fio_msg_decode_packed(pay, cut, mb, sizeof(buf));
    if (fio_msg_decode_packed(pay, n, mb, sizeof(buf)) <= 0) { printf("FAIL fmsg re-adopt\n"); return 1; }

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
            const int cn = fio_msg_encode(2, seat, 0x0123456789abcdefULL, zero8, joins, 0 /* no send clock in this smoke */, child, sizeof(child));
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
        // The awire twin (what Swift calls) must reach the SAME guard verdict on
        // the SAME action — "good" is awire {kind=4, n=0}. Re-adopt first (the
        // JSON rebase above cloned onto the resident game).
        if (fio_msg_decode_packed(pay, n, mb, sizeof(buf)) <= 0) { printf("FAIL fmsg re-adopt (awire)\n"); return 1; }
        const unsigned char good_awire[2] = { 4, 0 };
        const int stale_w = fio_msg_rebase_awire(adopted_round - 1, 0, good_awire, 2);
        if (stale_w != FIO_REBASE_DISCARD_ROUND) {
            printf("FAIL fmsg awire round guard: got %d, want %d\n", stale_w, FIO_REBASE_DISCARD_ROUND);
            return 1;
        }
    }

    printf("fmsg OK (envelope %d B = %d base32 chars, decode+ruleP+rebase)\n", n, chars);
    return 0;
}

// ---------- Lobby v2: open-count WAITING -> Start reseat -> LIVE -----------
//
// Proves the mechanism batch 6 / item C picked for the iMessage group lobby
// (docs/IMESSAGE_LOBBY_V2.md): a group lobby is created OPEN (n_players=8, the
// wire's max) so seats stay free; "Start" re-derives the SAME locked seed at
// the ACTUAL joined count via fio_reseat_game, and the resulting LIVE
// envelope's n_players (3) legitimately differs from its WAITING parent's (8)
// — nothing in msg_wire.c cross-checks a child's n_players against a parent
// (parentage is only the 8-byte digest tag, msg_wire.h's parent8), so this is
// a property of the wire, not a hole: each envelope is independently sealed
// and independently replayed from its OWN header.
static int lobby_v2_reseat_check(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 13 + 5);

    // Create: lock the seed in at the wire's max capacity (8) — the "open
    // lobby" convention — and seal WAITING with just the creator's join.
    if (fio_new_game(seed, 32, 8) != FIO_EOK) { printf("FAIL lobby new_game(8)\n"); return 1; }
    const uint8_t zero8[8] = {0};
    const char *joins1 = "[{\"seat\":0,\"name\":\"Alex\"}]";
    unsigned char waiting[2048];
    const int wn = fio_msg_encode(0 /* WAITING */, 0, 0xF001ULL, zero8, joins1, 0 /* no send clock in this smoke */, waiting, sizeof(waiting));
    if (wn <= 0) { printf("FAIL lobby waiting encode: %d (msg_err=%d)\n", wn, fio_last_msg_error()); return 1; }

    // Two joins land (seats 1, 2) — mechanically identical to today's join
    // flow, just never auto-starting: still WAITING, still n_players=8.
    unsigned char mb[1 << 16];
    if (fio_msg_decode_packed(waiting, wn, mb, sizeof(mb)) <= 0) {
        printf("FAIL lobby waiting decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    const char *joins3 = "[{\"seat\":0,\"name\":\"Alex\"},{\"seat\":1,\"name\":\"Sveta\"},"
                        "{\"seat\":2,\"name\":\"Boris\"}]";
    unsigned char waiting3[2048];
    const int wn3 = fio_msg_encode(0, 2, 0xF001ULL, zero8, joins3, 0 /* no send clock in this smoke */, waiting3, sizeof(waiting3));
    if (wn3 <= 0) { printf("FAIL lobby waiting3 encode: %d\n", wn3); return 1; }
    if (fio_msg_decode_packed(waiting3, wn3, mb, sizeof(mb)) <= 0) {
        printf("FAIL lobby waiting3 decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    if (mb[0] != 0 || mb[1] != 8) {
        printf("FAIL lobby: expected WAITING/8 after 2 joins, got phase=%d n=%d\n", mb[0], mb[1]);
        return 1;   // never auto-starts, whatever the join count
    }

    // Start: re-adopt is already resident (the decode above), so just reseat
    // at the actual joined count (3) from the SAME locked seed, then seal LIVE.
    if (fio_reseat_game(3) != FIO_EOK) { printf("FAIL lobby reseat(3)\n"); return 1; }
    unsigned char live[2048];
    const int ln = fio_msg_encode(2 /* LIVE */, 0, 0xF001ULL, zero8, joins3, 0 /* no send clock in this smoke */, live, sizeof(live));
    if (ln <= 0) { printf("FAIL lobby live encode: %d (msg_err=%d)\n", ln, fio_last_msg_error()); return 1; }

    // THE claim: the wire accepts a LIVE child whose n_players (3) differs
    // from its WAITING parent's (8) — decode+replay (validation IS replay)
    // succeeds standalone, exactly as any other envelope would.
    if (fio_msg_decode_packed(live, ln, mb, sizeof(mb)) <= 0) {
        printf("FAIL lobby live decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    if (mb[0] != 2 || mb[1] != 3) {
        printf("FAIL lobby: expected LIVE/3 after start, got phase=%d n=%d\n", mb[0], mb[1]);
        return 1;
    }
    // Someone (the first attacker on the freshly-dealt 3p game) can act.
    if (fio_actor_mask() == 0) { printf("FAIL lobby: no seat can act after start\n"); return 1; }

    printf("lobby v2 OK (WAITING/8 -> 3 joins, still WAITING/8 -> reseat(3) -> LIVE/3, wire accepted)\n");
    return 0;
}

// ---------- The 8-seat cap against a 9th player -----------------------------
//
// A 9+-person group chat racing into an open lobby: the 9th join must be
// impossible at every layer the bridge owns. (The DECODE side — a forged
// n_joins=9 header — is the tamper matrix's job, msg_wire_test.c.) The Swift
// halves — the full lobby offering only "lobby full", and a raced-out
// claimant's disowned cache reading as spectator — are pinned in
// Round5LobbyTests.testNinthPlayerAgainstAFullLobbyIsRejectedNotSeated.
static int nine_player_cap_check(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 29 + 11);
    if (fio_new_game(seed, 32, 8) != FIO_EOK) { printf("FAIL cap new_game(8)\n"); return 1; }

    const uint8_t zero8[8] = {0};
    unsigned char out[2048];

    // A full 8-join WAITING lobby seals fine (the cap itself is reachable)…
    char joins8[512]; int off = 0;
    off += snprintf(joins8 + off, sizeof(joins8) - off, "[");
    for (int s = 0; s < 8; s++)
        off += snprintf(joins8 + off, sizeof(joins8) - off,
                        "%s{\"seat\":%d,\"name\":\"P%d\"}", s ? "," : "", s, s);
    snprintf(joins8 + off, sizeof(joins8) - off, "]");
    if (fio_msg_encode(0, 7, 0xF002ULL, zero8, joins8, 0 /* no send clock in this smoke */, out, sizeof(out)) <= 0) {
        printf("FAIL cap: a full 8-join lobby refused to seal (msg_err=%d)\n",
               fio_last_msg_error());
        return 1;
    }

    // …a 9th join in the list does not (fio_parse_joins caps at MSG_MAX_JOINS)…
    char joins9[600];
    snprintf(joins9, sizeof(joins9), "%.*s,{\"seat\":8,\"name\":\"P8\"}]",
             (int)strlen(joins8) - 1, joins8);
    if (fio_msg_encode(0, 7, 0xF002ULL, zero8, joins9, 0 /* no send clock in this smoke */, out, sizeof(out)) > 0) {
        printf("FAIL cap: a 9-join lobby sealed\n"); return 1;
    }

    // …a claim on seat 8 (outside the 0..7 wire range) does not…
    if (fio_msg_encode(0, 0, 0xF002ULL, zero8,
                       "[{\"seat\":0,\"name\":\"A\"},{\"seat\":8,\"name\":\"I\"}]", 0 /* no send clock in this smoke */,
                       out, sizeof(out)) > 0) {
        printf("FAIL cap: a seat-8 claim sealed\n"); return 1;
    }

    // …and no 9-player deal exists to start into.
    if (fio_reseat_game(9) == FIO_EOK) { printf("FAIL cap: reseat(9) accepted\n"); return 1; }
    if (fio_reseat_game(8) != FIO_EOK) { printf("FAIL cap: reseat(8) refused\n"); return 1; }

    printf("nine-player cap OK (8-join seals; 9th join / seat 8 / reseat(9) all refused)\n");
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

    int slen0 = fio_state_packed(0, buf, sizeof(buf));
    if (slen0 < 0) { printf("FAIL state_packed\n"); return 1; }
    printf("state[0] packed %d bytes\n", slen0);

    if (fio_legal_packed(0, buf, sizeof(buf)) < 0) { printf("FAIL legal\n"); return 1; }
    printf("legal[0] packed OK\n");

    // Drive the game to completion: at each step, if seat 0 (human) is eligible,
    // play its first legal move via apply_json; then let bots step.
    int steps = 0, ev_moves = 0, ev_total = 0;
    while (fio_game_over() < 0 && steps++ < 5000) {
        int mask = fio_actor_mask();
        if (mask & 1) {
            // human seat 0 is eligible: pick its move from the PACKED legal wire
            // (preferring an ending move) and apply it through the awire path the
            // app ships — the same fio_apply_awire a real move POSTs.
            int lrc = fio_legal_packed(0, buf, sizeof(buf));
            if (lrc < 0) break;
            unsigned char aw[64];
            int al = pick_move_awire((const unsigned char *)buf, lrc, aw);
            if (al == 0) break;   // seat 0 flagged eligible but no concrete move
            int r = fio_apply_awire(0, aw, al);
            if (r == FIO_EREJECT) { printf("human reject code=%d\n", fio_last_reject()); break; }
            if (r < 0) { printf("human apply error r=%d\n", r); break; }

            // The animation plan for the move just applied (§16.B4): the human's
            // own card flies by the kernel's plan exactly as a bot's does, and
            // fio_apply_awire arms the same snapshot hook as the JSON path. Every
            // event must carry "state" — the board AS OF that step, masked for
            // this viewer — because a cycle that applied several actions is
            // otherwise only drawable at its final state (the reason
            // BoardDiff.swift was cancelled).
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

            // The animation PLAN for the same move (anim_plan.h): one step per
            // event, each carrying its post-step counts, plus the pre-sequence
            // count-freeze and the veil. This is what MessageTableView's
            // runEventStream/preCounts/veil collapse onto — the plan is derived
            // in C, not re-walked in Swift.
            static char planbuf[65536];
            int pl = fio_anim_plan_json(0, planbuf, sizeof(planbuf));
            if (pl < 0) { printf("FAIL anim_plan err=%d\n", pl); return 1; }
            int n_steps = count_of(planbuf, "{\"type\":");
            if (n_steps != n_ev) { printf("FAIL plan: %d events but %d steps\n", n_ev, n_steps); return 1; }
            if (count_of(planbuf, "\"pre\":{") != 1 || count_of(planbuf, "\"veil\":[") != 1) {
                printf("FAIL plan: missing pre/veil\n"); return 1;
            }
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

    { int fsl = fio_state_packed(0, buf, sizeof(buf)); printf("final state packed %d bytes\n", fsl); }

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
    if (lobby_v2_reseat_check() != 0) return 1;
    if (nine_player_cap_check() != 0) return 1;

    printf("SMOKE OK\n");
    return 0;
}
