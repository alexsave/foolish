// ios_api_smoke.c — a host-side smoke test for the iOS bridge. NOT part of the
// xcframework; compiled natively (clang) to prove new->legal->apply->state
// round-trips and that a full bot-vs-bot game runs to a fool through the same
// entry points Swift uses. Build/run:
//   clang -O2 -Isrc -Iios/include -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 \
//         -DMAX_MOVE_CARDS=28 ios/ios_api_smoke.c ios/ios_api.c <CORE_SRC> -lm
#include "ios_api.h"
#include "replay.h"   // the codec version this build stamps (-Isrc)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static char buf[1 << 16];
// The event stream is read into its own buffer: `buf` still holds the legal
// menu the current move was picked out of.
static char evbuf[1 << 18];

// How many times `needle` occurs in `s` (non-overlapping).

// A PACKED ROSTER (ios_api.h): n_joins(1) then n_joins x {seat, name_len, name}.
// The smoke builds one wherever it used to write a joins JSON literal.
typedef struct { int seat; const char *name; } SmokeJoin;
static int pack_joins(unsigned char *out, int cap, const SmokeJoin *js, int n) {
    int p = 0;
    if (cap < 1) return -1;
    out[p++] = (unsigned char)n;
    for (int i = 0; i < n; i++) {
        const int nl = (int)strlen(js[i].name);
        if (p + 2 + nl > cap) return -1;
        out[p++] = (unsigned char)js[i].seat;
        out[p++] = (unsigned char)nl;
        memcpy(out + p, js[i].name, (size_t)nl);
        p += nl;
    }
    return p;
}

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
                if (fio_last_replay_error() == REPLAY_ETOOLONG) { skipped++; continue; }
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
            // The seeded encoder's version has moved with the codec: v7 added
            // the pass-mode bit, v8 the forced-opening bit, and 10 is the same
            // wire again under the corrected deal order (replay.h). That last
            // one is not additive - it retired 5 through 8 outright - so a
            // fresh encode must be 10 and nothing else.
            if ((unsigned char)buf[0] != REPLAY_FORMAT_VERSION_V10) {   // version is byte[0]
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

    const SmokeJoin jspec[4] = { {0,"Sveta"}, {1,"Ann"}, {2,"Bo"}, {3,"Cy"} };
    unsigned char joins[128];
    const int joins_n = pack_joins(joins, (int)sizeof joins, jspec, 4);
    unsigned char pay[2048];
    const uint8_t zero8[8] = {0};
    const int n = fio_msg_encode(2 /* LIVE */, 0, 0x0123456789abcdefULL, zero8, joins, joins_n, 0 /* no send clock in this smoke */, pay, sizeof(pay));
    if (n <= 0) { printf("FAIL fmsg encode: %d (msg_err=%d)\n", n, fio_last_msg_error()); return 1; }

    // The size claim the whole design rests on (§4.4): base32 is 8 chars/5 bytes.
    const int chars = (n + 4) / 5 * 8;
    if (chars >= 1000) { printf("FAIL fmsg envelope %d chars >= 1000\n", chars); return 1; }

    // Decode ADOPTS: the payload's game becomes the resident one. The metadata
    // comes back as the PACKED blob (fio_msg_decode_packed layout): phase(1)
    // n_players(1) last_actor_seat(1) round(1) turn(u16) game_id(u64) parent8(8)
    // digest(32) sent_at(u16) n_new(1) opening(1) carry_key(u32) carry_fool(1)
    // passing(1) n_joins(1) then joins {seat(1) len(1) name[]}.
    unsigned char *mb = (unsigned char *)buf;
    if (fio_msg_decode_packed(pay, n, mb, sizeof(buf)) <= 0) {
        printf("FAIL fmsg decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    unsigned long long gid = 0;
    for (int i = 0; i < 8; i++) gid |= (unsigned long long)mb[6 + i] << (8 * i);
    if (mb[0] != 2 /* phase LIVE */ || mb[1] != 4 /* n_players */ || gid != 81985529216486895ULL) {
        printf("FAIL fmsg decode packed shape: phase=%d n=%d gid=%llu\n", mb[0], mb[1], gid); return 1;
    }
    // Seat 0's join is "Sveta" - the first record after the 65-byte header
    // (round 16 added the two send-clock bytes, the bubble delta and the
    // fool's-penalty trio ahead of n_joins; the rules byte follows them).
    if (!(mb[65] == 0 && mb[66] == 5 && memcmp(mb + 67, "Sveta", 5) == 0)) {
        printf("FAIL fmsg decode: seat-0 join not Sveta\n"); return 1;
    }
    // …and this chain is the classic game, said by the byte the lobby's
    // checkbox writes rather than assumed by its absence.
    if (mb[63] != 1) { printf("FAIL fmsg decode: passing byte = %d\n", mb[63]); return 1; }
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
            const int cn = fio_msg_encode(2, seat, 0x0123456789abcdefULL, zero8, joins, joins_n, 0 /* no send clock in this smoke */, child, sizeof(child));
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

// ---------- the bubble delta survives being READ (round 16) ----------------
//
// A bubble states how many atoms it added (msg_wire.h's n_new), and its
// recipient animates - and its sender captions - exactly that many. The count
// is taken against the chain this device ADOPTED, which the kernel remembers.
//
// So READING a payload must not move that memory, and a decode does: it
// replays the chain and re-bases on it. The composer reads its own outgoing
// bubble (for the joins and the summary line) between one staged action and
// the next, and that read used to tell the kernel the staged half was somebody
// else's history - so a turn of two actions sealed as a delta of one, and
// everything downstream described only its tail.
//
// fio_msg_peek_packed is the read that changes nothing; this is the proof.
// The turn itself: two actions on the chain `parent`, sealed after each, with
// the composer's read of its own staged bubble in between when `with_read`.
// Hands back what the FINAL bubble says about itself.
static int delta_stage_two(const unsigned char *parent, int pn,
                           const unsigned char *joins, int joins_n,
                           int with_read, int *turn_out, int *delta_out) {
    const uint8_t zero8[8] = {0};
    unsigned char mb[1 << 14];
    if (fio_msg_decode_packed(parent, pn, mb, sizeof(mb)) <= 0) return -1;

    int applied = 0, bn = 0;
    unsigned char bubble[2048];
    for (int i = 0; i < 2; i++) {
        const int mask = fio_actor_mask();
        int seat = -1;
        for (int s = 0; s < 4; s++) if (mask & (1 << s)) { seat = s; break; }
        if (seat < 0) break;
        const int lrc = fio_legal_packed(seat, buf, sizeof(buf));
        if (lrc < 0) break;
        unsigned char aw[64];
        const int al = pick_move_awire((const unsigned char *)buf, lrc, aw);
        if (al == 0 || fio_apply_awire(seat, aw, al) != FIO_EOK) break;
        applied++;
        bn = fio_msg_encode(2, seat, 0xD00DULL, zero8, joins, joins_n, 0, bubble, sizeof(bubble));
        if (bn <= 0) return -1;
        // THE READ: what the composer does with the bubble it has just staged,
        // before the human plays the rest of the turn.
        if (with_read && fio_msg_peek_packed(bubble, bn, mb, sizeof(mb)) <= 0) return -1;
    }
    if (applied != 2) return -1;
    if (fio_msg_peek_packed(bubble, bn, mb, sizeof(mb)) <= 0) return -1;
    *turn_out = mb[4] | (mb[5] << 8);
    *delta_out = mb[56];

    // The peek says the same about these bytes as a decode does - same blob,
    // one adopts and one does not. (Last, because it re-adopts.)
    unsigned char decoded[1 << 14];
    if (fio_msg_decode_packed(bubble, bn, decoded, sizeof(decoded)) <= 0) return -1;
    if (memcmp(mb, decoded, (size_t)64) != 0) return -2;
    return 0;
}

static int bubble_delta_check(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 7 + 5);
    const SmokeJoin jspec[4] = { {0,"Sveta"}, {1,"Ann"}, {2,"Bo"}, {3,"Cy"} };
    unsigned char joins[128];
    const int joins_n = pack_joins(joins, (int)sizeof joins, jspec, 4);
    const uint8_t zero8[8] = {0};

    // A turn of two actions that really is two ATOMS wide - not one the codec
    // folded back down to one, where a lost base would be invisible. Deal,
    // play a prelude of n moves, seal that as the parent, and take the first
    // parent whose silent run comes back with a delta of 2.
    unsigned char parent[2048];
    int pn = 0, silent_turn = 0, silent_delta = 0, prelude = -1;
    for (int n_pre = 1; n_pre <= 24; n_pre++) {
        if (fio_new_game(seed, 32, 4) != FIO_EOK) { printf("FAIL delta new_game\n"); return 1; }
        int played = 0;
        for (; played < n_pre && fio_game_over() < 0; played++) {
            const int mask = fio_actor_mask();
            int seat = -1;
            for (int s = 0; s < 4; s++) if (mask & (1 << s)) { seat = s; break; }
            if (seat < 0) break;
            const int lrc = fio_legal_packed(seat, buf, sizeof(buf));
            if (lrc < 0) break;
            unsigned char aw[64];
            const int al = pick_move_awire((const unsigned char *)buf, lrc, aw);
            if (al == 0 || fio_apply_awire(seat, aw, al) != FIO_EOK) break;
        }
        if (played != n_pre) break;
        pn = fio_msg_encode(2, 0, 0xD00DULL, zero8, joins, joins_n, 0, parent, sizeof(parent));
        if (pn <= 0) { printf("FAIL delta parent encode %d\n", pn); return 1; }
        if (delta_stage_two(parent, pn, joins, joins_n, 0, &silent_turn, &silent_delta) != 0) continue;
        if (silent_delta == 2) { prelude = n_pre; break; }
    }
    if (prelude < 0) { printf("FAIL delta: no two-atom turn found to test\n"); return 1; }

    // The SAME turn again, this time with the read in between. What the bubble
    // says about itself must not depend on who looked at it.
    int read_turn = 0, read_delta = 0;
    const int rc = delta_stage_two(parent, pn, joins, joins_n, 1, &read_turn, &read_delta);
    if (rc == -2) { printf("FAIL delta: peek and decode disagree about the same bytes\n"); return 1; }
    if (rc != 0) { printf("FAIL delta: the read run did not stage its turn\n"); return 1; }

    if (read_turn != silent_turn || read_delta != silent_delta) {
        printf("FAIL delta: reading the staged bubble changed it - "
               "silent turn=%d n_new=%d, read turn=%d n_new=%d\n",
               silent_turn, silent_delta, read_turn, read_delta);
        return 1;
    }
    printf("bubble delta OK (2 actions after %d, read in between, n_new=%d unchanged)\n",
           prelude, read_delta);
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
    const SmokeJoin jspec1[1] = { {0,"Alex"} };
    unsigned char joins1[64];
    const int joins1_n = pack_joins(joins1, (int)sizeof joins1, jspec1, 1);
    unsigned char waiting[2048];
    const int wn = fio_msg_encode(0 /* WAITING */, 0, 0xF001ULL, zero8, joins1, joins1_n, 0 /* no send clock in this smoke */, waiting, sizeof(waiting));
    if (wn <= 0) { printf("FAIL lobby waiting encode: %d (msg_err=%d)\n", wn, fio_last_msg_error()); return 1; }

    // Two joins land (seats 1, 2) — mechanically identical to today's join
    // flow, just never auto-starting: still WAITING, still n_players=8.
    unsigned char mb[1 << 16];
    if (fio_msg_decode_packed(waiting, wn, mb, sizeof(mb)) <= 0) {
        printf("FAIL lobby waiting decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    const SmokeJoin jspec3[3] = { {0,"Alex"}, {1,"Sveta"}, {2,"Boris"} };
    unsigned char joins3[128];
    const int joins3_n = pack_joins(joins3, (int)sizeof joins3, jspec3, 3);
    unsigned char waiting3[2048];
    const int wn3 = fio_msg_encode(0, 2, 0xF001ULL, zero8, joins3, joins3_n, 0 /* no send clock in this smoke */, waiting3, sizeof(waiting3));
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
    const int ln = fio_msg_encode(2 /* LIVE */, 0, 0xF001ULL, zero8, joins3, joins3_n, 0 /* no send clock in this smoke */, live, sizeof(live));
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

// ---------- the lobby's rules checkbox (podkidnoy) --------------------------
//
// The same lobby flow, with the transfer turned off - through the API the app
// really uses, in the order it really uses it: adopt the lobby, set the rule,
// seal. Two things have to survive that: the WAITING bubble must SAY podkidnoy
// (a lobby has no body to say it in), and the Start that re-derives the locked
// seed must not quietly hand the transfer back.
static int lobby_rules_check(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 7 + 3);
    const uint8_t zero8[8] = {0};
    unsigned char mb[1 << 16];

    if (fio_new_game(seed, 32, 8) != FIO_EOK) { printf("FAIL rules new_game(8)\n"); return 1; }
    if (!fio_passing_allowed()) { printf("FAIL rules: a fresh game was not the classic one\n"); return 1; }

    // The checkbox comes OFF: the lobby is resealed, and this is the bubble the
    // others will open.
    if (fio_set_passing(0) != FIO_EOK) { printf("FAIL rules set_passing(0)\n"); return 1; }
    const SmokeJoin jspec2[2] = { {0,"Alex"}, {1,"Dima"} };
    unsigned char joins2[128];
    const int joins2_n = pack_joins(joins2, (int)sizeof joins2, jspec2, 2);
    unsigned char waiting[2048];
    const int wn = fio_msg_encode(0 /* WAITING */, 0, 0xF003ULL, zero8, joins2, joins2_n, 0, waiting, sizeof(waiting));
    if (wn <= 0) { printf("FAIL rules waiting encode: %d (msg_err=%d)\n", wn, fio_last_msg_error()); return 1; }
    if (fio_msg_decode_packed(waiting, wn, mb, sizeof(mb)) <= 0) {
        printf("FAIL rules waiting decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    if (mb[63] != 0) { printf("FAIL rules: the lobby did not say podkidnoy (%d)\n", mb[63]); return 1; }

    // Start. The deal is re-derived from the locked seed at the joined count,
    // and the rules ride across it.
    if (fio_reseat_game(2) != FIO_EOK) { printf("FAIL rules reseat(2)\n"); return 1; }
    if (fio_passing_allowed()) { printf("FAIL rules: the re-deal restored the transfer\n"); return 1; }
    unsigned char live[2048];
    const int ln = fio_msg_encode(2 /* LIVE */, 0, 0xF003ULL, zero8, joins2, joins2_n, 0, live, sizeof(live));
    if (ln <= 0) { printf("FAIL rules live encode: %d (msg_err=%d)\n", ln, fio_last_msg_error()); return 1; }
    if (fio_msg_decode_packed(live, ln, mb, sizeof(mb)) <= 0) {
        printf("FAIL rules live decode: msg_err=%d\n", fio_last_msg_error()); return 1;
    }
    if (mb[63] != 0) { printf("FAIL rules: the live game lost the rule (%d)\n", mb[63]); return 1; }

    // And the board this produces offers no transfer - which is the whole point,
    // and is read through the SAME packed menu the app draws its buttons from.
    for (int seat = 0; seat < 2; seat++) {
        char lb[8192];
        const int n = fio_legal_packed(seat, lb, (int)sizeof lb);
        if (n < 4) continue;
        // Layout (MoveWire): u32 count, then per move type(1) n_cards(1)
        // cards[n] attacks[n]. MOVE_PASS is 2 (legal.h).
        const unsigned char *p = (const unsigned char *)lb;
        long count = (long)p[0] | ((long)p[1] << 8) | ((long)p[2] << 16) | ((long)p[3] << 24);
        int q = 4;
        for (long m = 0; m < count && q + 1 < n; m++) {
            const int type = p[q], nc = p[q + 1];
            if (type == 2) { printf("FAIL rules: seat %d was offered a transfer\n", seat); return 1; }
            q += 2 + 2 * nc;
        }
    }

    printf("lobby rules OK (podkidnoy chosen, sealed, re-dealt at Start, no transfer offered)\n");
    return 0;
}

// THE SHAPE OF A SEQUENCE, over the bytes a board would hold (fio_beats_packed
// and the role beat). Portable proof that the crossing packs what the Swift
// decoder reads: the beat stride, the flags byte, the out and attack-pass seat
// masks and the 52-bit placed set. The rules themselves are pinned in
// tests/tests.c; this is the layout.
//
// MUTATION-CHECKED against c/ios/ios_api.c, one at a time: the placed id set
// written little-endian at the wrong offset, the beat stride written as 16, the
// out mask and the attack-pass mask swapped, and the role answer returning the
// FINAL defender in the first_attacker slot. Each fails here.
static int beats_wire_check(void) {
    // Seat 0 attacks with one card, seat 1 covers with two (one move, two
    // events), a bout end follows, and seat 1 goes out on the way.
    //   type, seat, has_good, good, n_ids, ids...
    const unsigned char in[] = {
        FIO_BEATS_VERSION, 5,
        4, 0, 1, 0x04, 1, 6,          // ATTACK_PASS seat 0, card 6, good mask 0b100
        5, 1, 1, 0x00, 1, 22,         // COVER seat 1, card 22, good cleared
        5, 1, 1, 0x00, 1, 30,         // COVER seat 1, card 30 - the same move
        8, 1, 0, 0x00, 0,             // OUT seat 1 - a notice
        10, 0xFF, 1, 0x00, 0,         // CARDS_TO_TRASH, no seat
    };
    unsigned char out[512];
    const int n = fio_beats_packed(in, (int)sizeof in, (char *)out, sizeof out);
    if (n != FIO_BEATS_HEAD + 4 * FIO_BEATS_STRIDE) { printf("FAIL beats rc=%d\n", n); return 1; }
    if (out[0] != FIO_BEATS_VERSION || out[1] != 4) { printf("FAIL beats header\n"); return 1; }
    if (out[2] != 1 || out[3] != 0x04) { printf("FAIL beats first good mask\n"); return 1; }

    unsigned long long placed = 0;
    for (int i = 0; i < 8; i++) placed |= (unsigned long long)out[4 + i] << (8 * i);
    if (placed != (((unsigned long long)1 << 6) | ((unsigned long long)1 << 22)
                 | ((unsigned long long)1 << 30))) {
        printf("FAIL beats placed set %llx\n", placed); return 1;
    }

    const unsigned char *b0 = out + FIO_BEATS_HEAD;
    const unsigned char *b1 = b0 + FIO_BEATS_STRIDE;
    if (b0[0] != 0 || b0[1] != 1 || b0[2] != 4 || b0[3] != 0) { printf("FAIL beat 0 head\n"); return 1; }
    if (b1[0] != 1 || b1[1] != 2 || b1[2] != 5 || b1[3] != 1) { printf("FAIL beat 1 head\n"); return 1; }
    // The two covers are one beat, it holds before the sweep, and it adopts the
    // out notice behind it.
    if ((b1[4] & 1) == 0) { printf("FAIL beat 1 does not hold\n"); return 1; }
    if ((b0[4] & 1) != 0) { printf("FAIL the attack holds\n"); return 1; }
    if (b1[5] != (1u << 1)) { printf("FAIL beat 1 outs %d\n", b1[5]); return 1; }
    if (b0[5] != 0) { printf("FAIL the attack adopted an out\n"); return 1; }
    if (b0[6] != (1u << 0) || b1[6] != 0) { printf("FAIL attack-pass seats\n"); return 1; }
    if (b1[7] != 1 || b1[8] != 0) { printf("FAIL beat 1 good mask\n"); return 1; }
    unsigned long long p1 = 0;
    for (int i = 0; i < 8; i++) p1 |= (unsigned long long)b1[9 + i] << (8 * i);
    if (p1 != (((unsigned long long)1 << 22) | ((unsigned long long)1 << 30))) {
        printf("FAIL beat 1 placed %llx\n", p1); return 1;
    }

    // The role beat, answered off that same beat: seat 0 is defending in the
    // board the badges are WEARING and laid a card, so it is a transfer.
    int roles[FIO_ROLES_OUT];
    if (fio_roles_pass_hand_off(0, 3, 0x04, b0[6], 1, roles) != 1) {
        printf("FAIL hand-off not seen\n"); return 1;
    }
    if (roles[0] != 1 || roles[1] != 3 || roles[2] != 0x04) {
        printf("FAIL hand-off roles %d/%d/%d\n", roles[0], roles[1], roles[2]); return 1;
    }
    if (fio_roles_pass_hand_off(2, 3, 0x04, b0[6], 1, roles) != 0) {
        printf("FAIL an attacker's throw read as a transfer\n"); return 1;
    }
    if (fio_roles_goods_cleared(1, 3, 0x04, b1[8], roles) != 1 || roles[2] != 0) {
        printf("FAIL the good the throw-in cleared\n"); return 1;
    }
    if (fio_roles_goods_opening(1, 3, 0x00, out[3], roles) != 1 || roles[2] != 0x04) {
        printf("FAIL the good this stream opens on\n"); return 1;
    }
    if (fio_badge_drops_as_cards_leave(4) != 1 || fio_badge_drops_as_cards_leave(6) != 0) {
        printf("FAIL badge direction\n"); return 1;
    }

    // A foreign version and a truncated stream are refused, never half-read.
    unsigned char bad[sizeof in];
    memcpy(bad, in, sizeof in);
    bad[0] = 9;
    if (fio_beats_packed(bad, (int)sizeof bad, (char *)out, sizeof out) != FIO_EPARSE) {
        printf("FAIL beats accepted a foreign version\n"); return 1;
    }
    if (fio_beats_packed(in, 8, (char *)out, sizeof out) != FIO_EPARSE) {
        printf("FAIL beats accepted a truncated stream\n"); return 1;
    }
    if (fio_beats_packed(in, (int)sizeof in, (char *)out, FIO_BEATS_HEAD) != FIO_ECAP) {
        printf("FAIL beats wrote past its buffer\n"); return 1;
    }
    printf("beats wire OK (%d bytes, 4 beats)\n", n);
    return 0;
}

// THE PRE-BOUT TABLE, over the bytes a board would hold. The rule is pinned in
// tests/tests.c; this is the layout, and the two answers it can give.
//
// The fixture is the owner's round-12 report as ids: the 6 of diamonds (44)
// covered by the king of hearts (25), and a bare king of diamonds (51). Three
// cards, TWO battles - a flat reading would say three.
//
// MUTATION-CHECKED against c/ios/ios_api.c, one at a time: the paired flag
// written at the wrong offset, the head written as 2, the prior board's bytes
// consumed as one per battle rather than two, and the "no prior" sentinel read
// as a count of 254. Each fails here. Both bounds fault on the guard page when
// loosened, for the reason the plan's do.
static int pretable_wire_check(void) {
    const unsigned char in[] = {
        FIO_PRETABLE_VERSION, 2, 2, 44, 25, 51, FIO_PRETABLE_NONE,
        4, FIO_PRETABLE_NONE, 1, 51,          // ATTACK_PASS, no board, laid card 51
        6, FIO_PRETABLE_NONE, 3, 44, 51, 25,  // PICKUP, no board, took all three
    };
    unsigned char out[512];
    int n = fio_pre_bout_table_packed(in, (int)sizeof in, (char *)out, sizeof out);
    if (n != FIO_PRETABLE_HEAD + 4) { printf("FAIL pre-bout rc=%d\n", n); return 1; }
    if (out[0] != FIO_PRETABLE_VERSION || out[1] != 2 || out[2] != 1) {
        printf("FAIL pre-bout header %d/%d/%d\n", out[0], out[1], out[2]); return 1;
    }
    if (out[3] != 44 || out[4] != 25 || out[5] != 51 || out[6] != FIO_PRETABLE_NONE) {
        printf("FAIL pre-bout table %d+%d,%d+%d\n", out[3], out[4], out[5], out[6]); return 1;
    }

    // With no prior board there is nothing but the flat reading, and it says so.
    unsigned char flat[sizeof in];
    memcpy(flat, in, sizeof in);
    flat[2] = FIO_PRETABLE_NONE;
    // …which shortens the input by the prior board's four bytes.
    memmove(flat + 3, flat + 7, sizeof in - 7);
    n = fio_pre_bout_table_packed(flat, (int)sizeof in - 4, (char *)out, sizeof out);
    if (n != FIO_PRETABLE_HEAD + 6 || out[1] != 3 || out[2] != 0) {
        printf("FAIL pre-bout flat rc=%d n=%d paired=%d\n", n, out[1], out[2]); return 1;
    }
    if (out[3] != 44 || out[4] != FIO_PRETABLE_NONE || out[7] != 25) {
        printf("FAIL pre-bout flat cells\n"); return 1;
    }

    unsigned char bad[sizeof in];
    const struct { int at; unsigned char to; int want; const char *what; } forged[] = {
        { 0, 9,   FIO_EPARSE, "a foreign version" },
        { 1, 200, FIO_ECAP,   "more events than the rule holds" },
        { 2, 60,  FIO_EPARSE, "a prior board wider than the buffer" },
        { 3, 52,  FIO_EPARSE, "a prior card off the end of the deck" },
        { 16, 52, FIO_EPARSE, "a swept card off the end of the deck" },
        { 9, 40,  FIO_EPARSE, "an event claiming more cards than it carries" },
    };
    for (int i = 0; i < (int)(sizeof forged / sizeof forged[0]); i++) {
        memcpy(bad, in, sizeof in);
        bad[forged[i].at] = forged[i].to;
        if (fio_pre_bout_table_packed(bad, (int)sizeof bad, (char *)out, sizeof out)
            != forged[i].want) {
            printf("FAIL pre-bout accepted %s\n", forged[i].what); return 1;
        }
    }

    // EVERY short prefix, flush against a PROT_NONE guard page - see the plan's
    // loop for why a return code is not enough on its own.
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *probe = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED || mprotect(probe + page, (size_t)page, PROT_NONE) != 0) {
        printf("FAIL pre-bout guard page\n"); return 1;
    }
    for (int L = 0; L <= (int)sizeof in; L++) {
        unsigned char *edge = probe + page - L;
        memcpy(edge, in, (size_t)L);
        const int r = fio_pre_bout_table_packed(edge, L, (char *)out, sizeof out);
        if (L < (int)sizeof in ? (r >= 0) : (r <= 0)) {
            printf("FAIL pre-bout at %d bytes rc=%d\n", L, r); return 1;
        }
    }
    munmap(probe, (size_t)page * 2);
    if (fio_pre_bout_table_packed(in, (int)sizeof in, (char *)out, FIO_PRETABLE_HEAD)
        != FIO_ECAP) {
        printf("FAIL pre-bout wrote past its buffer\n"); return 1;
    }
    printf("pre-bout table wire OK (2 battles paired, 3 cells flat)\n");
    return 0;
}

// THE CONFLICT MODEL, over the bytes a board would hold. The rule is pinned in
// tests/tests.c; this is the layout and the two answers it carries back at once
// - the per-motion verdicts and the reversal's shape.
//
// The fixture: an arriving chain that MOVES the king of diamonds (51), opens on
// a table holding the 6 of diamonds (44) covered by the king of hearts (25),
// and shows the 10 of spades (9) in my hand. Two flown groups over that.
//
// MUTATION-CHECKED against c/ios/ios_api.c, one at a time: the verdicts written
// before the count, the step sizes written after the indices, the group counts
// read as motion counts, and the "no card" sentinel read as id 254. Each fails
// here. Both bounds fault on the guard page when loosened, as the plan's do.
static int conflict_wire_check(void) {
    // The chain transport is what a reversal IS - motions the caller already
    // knows are doomed, which only a total order can know. Unset, the kernel
    // refuses to answer (FIO_ETRANSPORT) rather than picking a client.
    unsigned char tprobe[] = { FIO_CONFLICT_VERSION, 0, 0, 0, 1, 1, 30, FIO_CONFLICT_DEST_TABLE };
    unsigned char tpout[64];
    if (fio_set_transport(FIO_TRANSPORT_UNSET) != FIO_EOK) { printf("FAIL transport unset\n"); return 1; }
    if (fio_transport() != FIO_TRANSPORT_UNSET) { printf("FAIL transport readback\n"); return 1; }
    if (fio_conflict_packed(tprobe, (int)sizeof tprobe, (char *)tpout, sizeof tpout) != FIO_ETRANSPORT) {
        printf("FAIL conflict answered with no transport set\n"); return 1;
    }
    if (fio_set_transport(FIO_TRANSPORT_CHAIN) != FIO_EOK) { printf("FAIL transport chain\n"); return 1; }
    if (fio_transport() != FIO_TRANSPORT_CHAIN) { printf("FAIL transport chain readback\n"); return 1; }

    const unsigned char in[] = {
        FIO_CONFLICT_VERSION,
        1, 51,                  // the arriving stream moves the king of diamonds
        1, 44, 25,              // …opening on a table of 6d covered by kh
        1, 9,                   // …with the 10 of spades in my hand
        2, 2, 3,                // two flown groups, of two and three motions
        44, FIO_CONFLICT_DEST_TABLE,    // stands on the opening table   -> KEEP
        30, FIO_CONFLICT_DEST_TABLE,    // nothing accounts for it       -> REVERT
        51, FIO_CONFLICT_DEST_TABLE,    // the arriving replay moves it  -> CLEAR
        9,  FIO_CONFLICT_DEST_MY_HAND,  // my hand holds it there        -> KEEP
        7,  FIO_CONFLICT_DEST_MY_HAND,  // my hand does not              -> REVERT
    };
    unsigned char out[512];
    int n = fio_conflict_packed(in, (int)sizeof in, (char *)out, sizeof out);
    if (n != 12) { printf("FAIL conflict rc=%d\n", n); return 1; }
    if (out[0] != FIO_CONFLICT_VERSION || out[1] != 5) {
        printf("FAIL conflict header %d/%d\n", out[0], out[1]); return 1;
    }
    if (out[2] != FIO_CONFLICT_V_KEEP   || out[3] != FIO_CONFLICT_V_REVERT
        || out[4] != FIO_CONFLICT_V_CLEAR || out[5] != FIO_CONFLICT_V_KEEP
        || out[6] != FIO_CONFLICT_V_REVERT) {
        printf("FAIL conflict verdicts %d%d%d%d%d\n",
               out[2], out[3], out[4], out[5], out[6]); return 1;
    }
    // Two steps, LAST group first, one flight each: the second group's 7 before
    // the first group's 30.
    if (out[7] != 2 || out[8] != 1 || out[9] != 1 || out[10] != 4 || out[11] != 1) {
        printf("FAIL conflict reversal %d [%d,%d] [%d,%d]\n",
               out[7], out[8], out[9], out[10], out[11]); return 1;
    }

    // A masked back names nothing and is KEPT, on any destination - the wire
    // has to carry "no identity" as well as a card.
    unsigned char masked[sizeof in];
    memcpy(masked, in, sizeof in);
    masked[13] = FIO_CONFLICT_NONE;   // the REVERT motion's card
    n = fio_conflict_packed(masked, (int)sizeof masked, (char *)out, sizeof out);
    if (n <= 0 || out[3] != FIO_CONFLICT_V_KEEP) {
        printf("FAIL conflict masked back rc=%d v=%d\n", n, out[3]); return 1;
    }

    unsigned char bad[sizeof in];
    const struct { int at; unsigned char to; int want; const char *what; } forged[] = {
        { 0, 9,   FIO_EPARSE, "a foreign version" },
        { 1, 200, FIO_EPARSE, "more moved cards than the buffer holds" },
        { 2, 52,  FIO_EPARSE, "a moved card off the end of the deck" },
        { 3, 60,  FIO_EPARSE, "an opening table wider than the buffer" },
        { 4, 52,  FIO_EPARSE, "an opening-table card off the end of the deck" },
        { 6, 60,  FIO_EPARSE, "a hand wider than the buffer" },
        { 7, 52,  FIO_EPARSE, "a hand card off the end of the deck" },
        { 8, 200, FIO_ECAP,   "more groups than the rule holds" },
        { 9, 200, FIO_EPARSE, "a group wider than the motions it carries" },
        { 11, 52, FIO_EPARSE, "a flown card off the end of the deck" },
        { 12, 9,  FIO_EPARSE, "a motion that went nowhere nameable" },
    };
    for (int i = 0; i < (int)(sizeof forged / sizeof forged[0]); i++) {
        memcpy(bad, in, sizeof in);
        bad[forged[i].at] = forged[i].to;
        if (fio_conflict_packed(bad, (int)sizeof bad, (char *)out, sizeof out)
            != forged[i].want) {
            printf("FAIL conflict accepted %s\n", forged[i].what); return 1;
        }
    }

    // EVERY short prefix, flush against a PROT_NONE guard page.
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *probe = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED || mprotect(probe + page, (size_t)page, PROT_NONE) != 0) {
        printf("FAIL conflict guard page\n"); return 1;
    }
    for (int L = 0; L <= (int)sizeof in; L++) {
        unsigned char *edge = probe + page - L;
        memcpy(edge, in, (size_t)L);
        const int r = fio_conflict_packed(edge, L, (char *)out, sizeof out);
        if (L < (int)sizeof in ? (r >= 0) : (r <= 0)) {
            printf("FAIL conflict at %d bytes rc=%d\n", L, r); return 1;
        }
    }
    munmap(probe, (size_t)page * 2);
    if (fio_conflict_packed(in, (int)sizeof in, (char *)out, 4) != FIO_ECAP) {
        printf("FAIL conflict wrote past its buffer\n"); return 1;
    }

    // The dest mapping, which decides which side of the arriving board a
    // motion's standing check reads.
    if (fio_conflict_dest(4, 2, 0) != FIO_CONFLICT_DEST_TABLE
        || fio_conflict_dest(6, 0, 0) != FIO_CONFLICT_DEST_MY_HAND
        || fio_conflict_dest(6, 2, 0) != FIO_CONFLICT_DEST_POOL
        || fio_conflict_dest(-1, 0, 0) != FIO_CONFLICT_DEST_POOL) {
        printf("FAIL conflict dest mapping\n"); return 1;
    }
    printf("conflict wire OK (5 verdicts, 2 reversal steps)\n");
    return 0;
}

// THE BOARD'S SETS AND SMALL RULES, across the bridge. These cross as ints and
// u64 bitsets rather than as packed records, so what this proves is the CROSSING
// - the bitset arithmetic survives the ABI, the array readers stay inside the
// lengths they were given, and the packed finish rows come back in the layout
// the header states.
//
// MUTATION-CHECKED on the bridge, each caught by the check it is named for:
// fio_veil_grid pinned to sweeping=1 ("FAIL grid live"); fio_selection_after_tap
// handed the hand as its own state ("FAIL selection at card 0"); fio_fan_cards
// forwarding `cap` as the hand length ("FAIL fan cards"); fio_finish_rows
// writing the row count where the seat count goes ("FAIL finish rows total is
// the seat count" - which needed a roster with fewer rows than seats to see at
// all, and survived until one was added); fio_shown_table returning the sweeping
// flag rather than the choice ("FAIL shown table").
#define SMOKE_ID(suit, value) ((unsigned char)((suit) * 13 + ((value) - 1)))
#define SMOKE_SET(id) ((uint64_t)1 << (id))

static int board_rules_check(void) {
    const unsigned char six = SMOKE_ID(0, 6), nine = SMOKE_ID(1, 9), ace = SMOKE_ID(3, 13);

    // The veil's four outs, with the LAST card of the deck in the sets: a dense
    // id off by one is invisible everywhere else.
    if (fio_veil_veiled(SMOKE_SET(six), SMOKE_SET(ace), 0, 0, 0, 0)
        != (SMOKE_SET(six) | SMOKE_SET(ace))) {
        printf("FAIL veil union\n"); return 1;
    }
    if (fio_veil_veiled(0, 0, 1, SMOKE_SET(six), 1, SMOKE_SET(six) | SMOKE_SET(ace))
        != SMOKE_SET(ace)) {
        printf("FAIL veil live source\n"); return 1;
    }
    if (fio_veil_veiled(0, 0, 0, SMOKE_SET(six), 1, SMOKE_SET(ace)) != 0
        || fio_veil_veiled(0, 0, 1, SMOKE_SET(six), 0, SMOKE_SET(ace)) != 0) {
        printf("FAIL veil live source needs both halves\n"); return 1;
    }
    if (fio_veil_flying(SMOKE_SET(six) | SMOKE_SET(ace), SMOKE_SET(ace)) != SMOKE_SET(six)) {
        printf("FAIL veil flying\n"); return 1;
    }
    if (fio_veil_hand_slot_deferred(SMOKE_SET(six) | SMOKE_SET(ace), 0, SMOKE_SET(ace))
        != SMOKE_SET(six)) {
        printf("FAIL veil deferral\n"); return 1;
    }
    if (fio_veil_fan(SMOKE_SET(six) | SMOKE_SET(ace), SMOKE_SET(ace)) != SMOKE_SET(six)) {
        printf("FAIL veil fan\n"); return 1;
    }

    uint64_t hidden = 0, flying = 0;
    fio_veil_grid(0, SMOKE_SET(six), SMOKE_SET(nine), SMOKE_SET(ace), SMOKE_SET(nine),
                  SMOKE_SET(six), &hidden, &flying);
    if (hidden != SMOKE_SET(six) || flying != SMOKE_SET(six)) {
        printf("FAIL grid live\n"); return 1;
    }
    fio_veil_grid(1, SMOKE_SET(six), SMOKE_SET(nine), SMOKE_SET(ace), SMOKE_SET(six),
                  SMOKE_SET(six), &hidden, &flying);
    if (hidden != (SMOKE_SET(nine) | SMOKE_SET(ace)) || flying != SMOKE_SET(six)) {
        printf("FAIL grid sweeping\n"); return 1;
    }

    uint64_t reveal = 0, carry = 0;
    fio_veil_teardown(SMOKE_SET(six), SMOKE_SET(ace), 1, &reveal, &carry);
    if (reveal != (SMOKE_SET(six) | SMOKE_SET(ace)) || carry != 0) {
        printf("FAIL teardown newest\n"); return 1;
    }
    fio_veil_teardown(SMOKE_SET(six), SMOKE_SET(ace), 0, &reveal, &carry);
    if (reveal != 0 || carry != (SMOKE_SET(six) | SMOKE_SET(ace))) {
        printf("FAIL teardown superseded\n"); return 1;
    }
    fio_veil_handover(SMOKE_SET(six) | SMOKE_SET(ace), SMOKE_SET(ace), &reveal, &carry);
    if (reveal != SMOKE_SET(six) || carry != SMOKE_SET(ace)) {
        printf("FAIL handover\n"); return 1;
    }

    if (fio_veil_unstarted_replay(1, 2) != 1 || fio_veil_unstarted_replay(0, 2) != 0
        || fio_veil_unstarted_replay(1, 0) != 0) {
        printf("FAIL unstarted replay\n"); return 1;
    }
    if (fio_holdback_is_mine(7, 7) != 1 || fio_holdback_is_mine(8, 7) != 0) {
        printf("FAIL holdback epoch\n"); return 1;
    }
    if (fio_shown_ledger_allows(FIO_CLAIM_BYSTANDER, 1) != 0
        || fio_shown_ledger_allows(FIO_CLAIM_BYSTANDER, 0) != 1
        || fio_shown_ledger_allows(FIO_CLAIM_SEQUENCE, 1) != 1
        || fio_shown_ledger_allows(FIO_CLAIM_ARMING, 1) != 1
        || fio_shown_ledger_allows(FIO_CLAIM_HAND_OFF, 1) != 1) {
        printf("FAIL ledger ownership\n"); return 1;
    }

    // The selection, over the whole deck both ways.
    for (int id = 0; id < 52; id++) {
        const uint64_t only = SMOKE_SET(id);
        if (fio_selection_after_tap(0, id, only) != only
            || fio_selection_after_tap(only, id, only) != 0
            || fio_selection_after_tap(0, id, 0) != 0) {
            printf("FAIL selection at card %d\n", id); return 1;
        }
    }

    if (fio_is_placement(5) != 1 || fio_is_placement(9) != 0
        || fio_is_my_placement(5, 2, 2) != 1 || fio_is_my_placement(5, 3, 2) != 0
        || fio_is_my_placement(5, -1, -1) != 0) {
        printf("FAIL placement mapping\n"); return 1;
    }

    // The fan and the layout, as arrays of dense ids.
    unsigned char out[64];
    const unsigned char hand[3] = { six, nine, ace };
    const unsigned char held[2] = { SMOKE_ID(2, 7), nine };
    int n = fio_fan_cards(hand, 3, held, 2, (char *)out, sizeof out);
    if (n != 4 || out[0] != six || out[3] != SMOKE_ID(2, 7)) {
        printf("FAIL fan cards rc=%d\n", n); return 1;
    }
    if (fio_fan_cards(hand, 3, held, 2, (char *)out, 2) != FIO_ECAP) {
        printf("FAIL fan cards wrote past its buffer\n"); return 1;
    }
    if (fio_laid_count(hand, 3, held, 2, SMOKE_SET(ace)) != 3) {
        printf("FAIL laid count\n"); return 1;
    }
    const unsigned char order[3] = { ace, nine, six };
    n = fio_hand_laid_out(hand, 3, 0, order, 3, (char *)out, sizeof out);
    if (n != 3 || out[0] != ace || out[1] != nine || out[2] != six) {
        printf("FAIL laid out rc=%d\n", n); return 1;
    }
    n = fio_hand_laid_out(hand, 3, SMOKE_SET(nine), order, 3, (char *)out, sizeof out);
    if (n != 2 || out[0] != ace || out[1] != six) {
        printf("FAIL laid out deferral rc=%d\n", n); return 1;
    }
    if (fio_hand_laid_out(hand, 3, 0, order, 3, (char *)out, 1) != FIO_ECAP) {
        printf("FAIL laid out wrote past its buffer\n"); return 1;
    }

    // The table, 2 bytes per battle, and the two choices that rest on it.
    const unsigned char covered[4] = { six, nine, ace, FIO_CONFLICT_NONE };
    const unsigned char uncovered[2] = { six, FIO_CONFLICT_NONE };
    if (fio_table_card_ids(covered, 2) != (SMOKE_SET(six) | SMOKE_SET(nine) | SMOKE_SET(ace))) {
        printf("FAIL table ids\n"); return 1;
    }
    const unsigned char unnameable[2] = { FIO_TABLE_UNKNOWN, FIO_CONFLICT_NONE };
    if (fio_table_covers(covered, 2, uncovered, 1) != 1
        || fio_table_covers(uncovered, 1, covered, 2) != 0
        || fio_table_covers(covered, 2, unnameable, 1) != 0) {
        printf("FAIL table covers\n"); return 1;
    }
    if (fio_covered_sweep_accepts(1, covered, 2, uncovered, 1) != 1
        || fio_covered_sweep_accepts(0, covered, 2, uncovered, 1) != 0) {
        printf("FAIL covered sweep\n"); return 1;
    }
    int sweeping = -1;
    if (fio_shown_table(1, 1, 1, &sweeping) != FIO_SHOWN_LIVE || sweeping != 0
        || fio_shown_table(0, 1, 1, &sweeping) != FIO_SHOWN_SWEEP || sweeping != 1
        || fio_shown_table(0, 0, 1, &sweeping) != FIO_SHOWN_PENDING || sweeping != 1
        || fio_shown_table(0, 0, 0, &sweeping) != FIO_SHOWN_NONE || sweeping != 0) {
        printf("FAIL shown table\n"); return 1;
    }

    // The finish rows, in the packed layout the header states.
    const unsigned char elim[3] = { 2, 0, 3 };
    n = fio_finish_rows(elim, 3, 1, 4, 0, (char *)out, sizeof out);
    if (n != FIO_FINISH_HEAD + 3 * 4 || out[0] != FIO_FINISH_VERSION
        || out[1] != 4 || out[2] != 4) {
        printf("FAIL finish rows header rc=%d\n", n); return 1;
    }
    if (out[3] != 1 || out[4] != 2 || out[5] != 0
        || out[6] != 2 || out[7] != 0 || out[8] != 1
        || out[12] != 4 || out[13] != 1 || out[14] != 0) {
        printf("FAIL finish rows body\n"); return 1;
    }
    if (fio_finish_rows(elim, 3, 1, 4, 0, (char *)out, 4) != FIO_ECAP) {
        printf("FAIL finish rows wrote past its buffer\n"); return 1;
    }
    // A ROSTER WITH FEWER ROWS THAN SEATS, so `total` cannot be read off the row
    // count: the fool's place is the SEAT count, and that is what tells a row it
    // is the fool. Six seats, two out, one fool - three rows, place 6 at the end.
    n = fio_finish_rows(elim, 2, 1, 6, -1, (char *)out, sizeof out);
    if (n != FIO_FINISH_HEAD + 3 * 3 || out[1] != 3 || out[2] != 6 || out[9] != 6) {
        printf("FAIL finish rows total is the seat count rc=%d\n", n); return 1;
    }

    // EVERY ARRAY READER, FLUSH AGAINST A PROT_NONE PAGE. There is no record to
    // parse here - the caller's count IS the bound - so this is what proves the
    // bound is real rather than a static array's slack.
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *probe = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED || mprotect(probe + page, (size_t)page, PROT_NONE) != 0) {
        printf("FAIL board guard page\n"); return 1;
    }
    unsigned char *edge = probe + page - 3;
    memcpy(edge, hand, 3);
    if (fio_fan_cards(edge, 3, NULL, 0, (char *)out, sizeof out) != 3
        || fio_laid_count(edge, 3, NULL, 0, 0) != 3
        || fio_hand_laid_out(edge, 3, 0, NULL, 0, (char *)out, sizeof out) != 3
        || fio_hand_laid_out(hand, 3, 0, edge, 3, (char *)out, sizeof out) != 3
        || fio_fan_cards(hand, 3, edge, 3, (char *)out, sizeof out) != 3) {
        printf("FAIL board guard hand\n"); return 1;
    }
    memcpy(edge, elim, 3);
    if (fio_finish_rows(edge, 3, 1, 4, 0, (char *)out, sizeof out) != FIO_FINISH_HEAD + 12) {
        printf("FAIL board guard elimination\n"); return 1;
    }
    unsigned char *tedge = probe + page - 4;
    memcpy(tedge, covered, 4);
    if (fio_table_card_ids(tedge, 2) != (SMOKE_SET(six) | SMOKE_SET(nine) | SMOKE_SET(ace))
        || fio_table_covers(tedge, 2, tedge, 2) != 1
        || fio_covered_sweep_accepts(1, tedge, 2, tedge, 2) != 1) {
        printf("FAIL board guard table\n"); return 1;
    }
    munmap(probe, (size_t)page * 2);

    printf("board rules OK (veil, hand, table, finish, ledger)\n");
    return 0;
}

// MUTATION-CHECKED. On the rule: the freeze walking back over every event says
// deck 2 here; anchoring without undoing says hands 3/9; undoing two events says
// deck 3; a step deriving forward gets step 1's board wrong. On the wire: the
// version, seat-count, event-cap, identity-count and card-id checks each fail a
// forged-header case; the output cap check fails the small-buffer case; and each
// of the three bounds (the event's count fields, its whole extent, the final
// hand block) faults on the guard page when loosened, because the reader would
// still RETURN an error while having read past the caller's buffer to get there.
//
// THE COUNT FREEZE, over the shape that broke it: a bout end off a deck of ONE
// whose next card is the flipped trump. Seat 1 takes the table, then seat 0
// draws TWO - one more than deck_count ever held, because the trump lies under
// the deck and is dealt last without being counted. Undoing every event puts
// both back and opens the deck badge at 2; anchoring on the first event's own
// board and undoing ONE says 1, which is what the board really showed. Round
// 16's "deck suddenly go to 5 cards, then deal, and now I have 6?".
static int plan_wire_check(void) {
    //  ver, np, n_events, final deck, final discard, final hands
    //  then per event: type seat from to n_cards n_ids has_counts deck discard
    //                  hand[np] ids...
    const unsigned char in[] = {
        FIO_PLAN_VERSION, 2, 2, 0, 20, 5, 9,
        // PICKUP seat 1, 4 table cards -> hand. Its board: deck 1, hands [3,9].
        6, 1, 2, 1, 4, 4, 1, 1, 20, 3, 9, 2, 14, 27, 40,
        // REFILL seat 0, 2 cards off a deck of 1. Its board: deck 0, hands [5,9].
        9, 0, 0, 1, 2, 2, 1, 0, 20, 5, 9, 6, 33,
    };
    unsigned char out[512];
    const int n = fio_anim_plan_packed(in, (int)sizeof in, (char *)out, sizeof out);
    if (n != FIO_PLAN_HEAD + 2 * FIO_PLAN_STRIDE + 6) { printf("FAIL plan rc=%d\n", n); return 1; }
    if (out[0] != FIO_PLAN_VERSION || out[1] != 2 || out[2] != 2) { printf("FAIL plan header\n"); return 1; }

    // The freeze: the board BEFORE the pickup. A deck of ONE, not two.
    if (out[8] != 1) { printf("FAIL plan pre deck %d (the flipped trump was counted back)\n", out[8]); return 1; }
    if (out[9] != 20) { printf("FAIL plan pre discard %d\n", out[9]); return 1; }
    if (out[10] != 3 || out[11] != 5) { printf("FAIL plan pre hands %d/%d\n", out[10], out[11]); return 1; }

    const unsigned char *s0 = out + FIO_PLAN_HEAD;
    const unsigned char *s1 = s0 + FIO_PLAN_STRIDE;
    // Each step lands on its OWN board, and the last one is the final board.
    if (s0[11] != 1 || s0[15] != 3 || s0[16] != 9) { printf("FAIL plan step 0 board\n"); return 1; }
    if (s1[11] != 0 || s1[15] != 5 || s1[16] != 9) { printf("FAIL plan step 1 board\n"); return 1; }
    if (s0[13] != 0 || s1[13] != 2 || s1[14] != 0) { printf("FAIL plan in-flight from deck\n"); return 1; }
    // Timing: ANIMATION_TIME each, staggered by TIME+GAP, and the wall time.
    const int dur = s0[5] | (s0[6] << 8);
    const int start1 = s1[7] | (s1[8] << 8) | (s1[9] << 16) | (s1[10] << 24);
    const int total = out[4] | (out[5] << 8) | (out[6] << 16) | (out[7] << 24);
    if (dur != 500 || start1 != 525 || total != 1025) {
        printf("FAIL plan timing %d/%d/%d\n", dur, start1, total); return 1;
    }
    // The veil: every real identity this stream lands, in order, once each.
    const unsigned char *veil = s1 + FIO_PLAN_STRIDE;
    if (out[3] != 6) { printf("FAIL plan veil %d\n", out[3]); return 1; }
    const unsigned char want[6] = { 2, 14, 27, 40, 6, 33 };
    for (int i = 0; i < 6; i++)
        if (veil[i] != want[i]) { printf("FAIL plan veil[%d]=%d\n", i, veil[i]); return 1; }

    // A foreign version, a forged header and a buffer that cannot hold the
    // answer are refused, never half-written. The header fields are what size
    // the fixed scratch this reader fills, so a forged one is a write past it.
    unsigned char bad[sizeof in];
    const struct { int at; unsigned char to; int want; const char *what; } forged[] = {
        { 0, 9,   FIO_EPARSE, "a foreign version" },
        { 1, 1,   FIO_EPARSE, "a one-seat table" },
        { 1, 9,   FIO_EPARSE, "a nine-seat table" },
        { 2, 200, FIO_ECAP,   "more events than a plan holds" },
        { 12, 12, FIO_ECAP,   "more identities than the event has cards" },
        { 18, 52, FIO_EPARSE, "a card id off the end of the deck" },
    };
    for (int i = 0; i < (int)(sizeof forged / sizeof forged[0]); i++) {
        memcpy(bad, in, sizeof in);
        bad[forged[i].at] = forged[i].to;
        if (fio_anim_plan_packed(bad, (int)sizeof bad, (char *)out, sizeof out) != forged[i].want) {
            printf("FAIL plan accepted %s\n", forged[i].what); return 1;
        }
    }
    // EVERY short prefix, not one: a bound that is off by a byte is refused at
    // exactly one length, and picking that length by hand is guesswork. Each
    // prefix sits flush against a PROT_NONE guard page, so the last byte the
    // reader may touch is the last readable byte: a bound that still RETURNS an
    // error while reading one past it kills the process instead of passing.
    // (evwire's reader is held to the same standard, for the same reason - there
    // is no ASAN in this repo.)
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *probe = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED || mprotect(probe + page, (size_t)page, PROT_NONE) != 0) {
        printf("FAIL plan guard page\n"); return 1;
    }
    for (int L = 0; L <= (int)sizeof in; L++) {
        unsigned char *edge = probe + page - L;
        memcpy(edge, in, (size_t)L);
        const int r = fio_anim_plan_packed(edge, L, (char *)out, sizeof out);
        if (L < (int)sizeof in ? (r >= 0) : (r <= 0)) {
            printf("FAIL plan at %d bytes rc=%d\n", L, r); return 1;
        }
    }
    munmap(probe, (size_t)page * 2);
    if (fio_anim_plan_packed(in, (int)sizeof in, (char *)out, FIO_PLAN_HEAD) != FIO_ECAP) {
        printf("FAIL plan wrote past its buffer\n"); return 1;
    }
    printf("plan wire OK (%d bytes, freeze deck=1 over a flipped-trump refill)\n", n);
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
    SmokeJoin jspec8[9];
    char names8[9][8];
    for (int s = 0; s < 9; s++) {
        snprintf(names8[s], sizeof names8[s], "P%d", s);
        jspec8[s].seat = s; jspec8[s].name = names8[s];
    }
    unsigned char joins8[256];
    const int joins8_n = pack_joins(joins8, (int)sizeof joins8, jspec8, 8);
    if (fio_msg_encode(0, 7, 0xF002ULL, zero8, joins8, joins8_n, 0 /* no send clock in this smoke */, out, sizeof(out)) <= 0) {
        printf("FAIL cap: a full 8-join lobby refused to seal (msg_err=%d)\n",
               fio_last_msg_error());
        return 1;
    }

    // …a 9th join in the list does not (fio_read_joins caps at MSG_MAX_JOINS)…
    unsigned char joins9[256];
    const int joins9_n = pack_joins(joins9, (int)sizeof joins9, jspec8, 9);
    if (fio_msg_encode(0, 7, 0xF002ULL, zero8, joins9, joins9_n, 0 /* no send clock in this smoke */, out, sizeof(out)) > 0) {
        printf("FAIL cap: a 9-join lobby sealed\n"); return 1;
    }

    // …a claim on seat 8 (outside the 0..7 wire range) does not…
    const SmokeJoin jbad[2] = { {0,"A"}, {8,"I"} };
    unsigned char joinsbad[64];
    const int joinsbad_n = pack_joins(joinsbad, (int)sizeof joinsbad, jbad, 2);
    if (fio_msg_encode(0, 0, 0xF002ULL, zero8, joinsbad, joinsbad_n, 0 /* no send clock in this smoke */,
                       out, sizeof(out)) > 0) {
        printf("FAIL cap: a seat-8 claim sealed\n"); return 1;
    }

    // …and neither does a roster the caller and the kernel would read
    // differently: a length that runs past the blob, or bytes left over.
    if (fio_msg_encode(0, 7, 0xF002ULL, zero8, joins8, joins8_n - 1, 0, out, sizeof(out)) > 0
        || fio_msg_encode(0, 7, 0xF002ULL, zero8, joins8, joins8_n + 1, 0, out, sizeof(out)) > 0
        || fio_msg_encode(0, 7, 0xF002ULL, zero8, joins8, 0, 0, out, sizeof(out)) > 0) {
        printf("FAIL cap: a roster that does not measure up sealed\n"); return 1;
    }

    // …nor does a name longer than the MsgJoin it has to land in. name_len is
    // a byte and the slot is 64, so an unchecked 200 writes past the struct.
    {
        unsigned char longname[512];
        int q = 0;
        longname[q++] = 2;
        longname[q++] = 0; longname[q++] = 1; longname[q++] = 'A';
        longname[q++] = 1; longname[q++] = 200;
        for (int i = 0; i < 200; i++) longname[q++] = 'x';
        // FIO_EPARSE exactly: the ROSTER READER has to be the one that refuses
        // it. Something downstream also refuses the result, but only after 200
        // bytes have been copied into a 64-byte MsgJoin.name.
        if (fio_msg_encode(0, 1, 0xF002ULL, zero8, longname, q, 0, out, sizeof(out)) != FIO_EPARSE) {
            printf("FAIL cap: a 200-byte name was not refused by the roster reader\n"); return 1;
        }
        longname[5] = 60;   // …and the same blob at a legal length does seal
        if (fio_msg_encode(0, 1, 0xF002ULL, zero8, longname, 6 + 60, 0, out, sizeof(out)) <= 0) {
            printf("FAIL cap: a 60-byte name was refused\n"); return 1;
        }
    }

    // THE ROSTER READER, FLUSH AGAINST A PROT_NONE GUARD PAGE. Every short
    // prefix of a valid blob, with its LAST byte on the page boundary: a count
    // read before its record is bounded faults here rather than in the field.
    // One sweep covers all four entries - they share fio_read_joins.
    {
        const long page = sysconf(_SC_PAGESIZE);
        unsigned char *probe = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (probe == MAP_FAILED || mprotect(probe + page, (size_t)page, PROT_NONE) != 0) {
            printf("FAIL roster guard page\n"); return 1;
        }
        for (int L = 0; L <= joins8_n; L++) {
            unsigned char *edge = probe + page - L;
            memcpy(edge, joins8, (size_t)L);
            const int r = fio_msg_encode(0, 7, 0xF002ULL, zero8, edge, L, 0, out, sizeof(out));
            if (L < joins8_n ? (r > 0) : (r <= 0)) {
                printf("FAIL roster at %d bytes rc=%d\n", L, r); return 1;
            }
            uint32_t k = 0; int fi = 0;
            const int c = fio_msg_carry(edge, L, 0, &k, &fi);
            if (L < joins8_n ? (c == FIO_EOK) : (c != FIO_EOK)) {
                printf("FAIL roster carry at %d bytes rc=%d\n", L, c); return 1;
            }
        }
        munmap(probe, (size_t)page * 2);
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

    // THE BOARD RULES, over the bytes a board would hold (fio_play_probe /
    // fio_play_human_menu). Portable proof that the crossing packs what the
    // Swift decoder reads: the flags byte, the -1 cover target, the coverable
    // mask and a one-entry move wire. The rules themselves are pinned in
    // tests/tests.c; this is the layout.
    //
    // MUTATION-CHECKED against c/ios/ios_api.c, one at a time: the move wire
    // written a byte early, the attack flag written in the pass bit, the
    // coverable mask written a byte into the cover-target slot, and the probe
    // answering the HAND target as if it were the open table. Each fails here.
    {
        // The opening actor, whichever seat holds the lowest trump.
        int actor = -1, m0 = fio_actor_mask();
        for (int s = 0; s < 4; s++) if (m0 & (1 << s)) { actor = s; break; }
        if (actor < 0) { printf("FAIL probe fixture: nobody may open\n"); return 1; }
        int lrc = fio_legal_packed(actor, buf, sizeof(buf));
        if (lrc < 0) { printf("FAIL legal for probe\n"); return 1; }
        unsigned char menu[1 << 16];
        memcpy(menu, buf, (size_t)lrc);
        // The opening table is empty, so the seat that acts first has attacks
        // on its menu; its own cards are the selection that resolves to one.
        int q = 4, at = -1;
        while (q + 2 <= lrc) {
            if (menu[q] == 0 /* MOVE_ATTACK */) { at = q; break; }
            q += 2 + 2 * menu[q + 1];
        }
        if (at < 0) { printf("FAIL probe fixture: no attack on the opening menu\n"); return 1; }
        const int n_sel = menu[at + 1];
        unsigned char probe[512];
        int prc = fio_play_probe(menu, lrc, 0, 0, 0, 0, menu + at + 2, n_sel,
                                 FIO_PLAY_TARGET_TABLE, (char *)probe, sizeof probe);
        if (prc < FIO_PLAY_PROBE_HEAD + 4) { printf("FAIL probe rc=%d\n", prc); return 1; }
        if ((probe[0] & 1) == 0) { printf("FAIL probe: the attack was not offered\n"); return 1; }
        if ((signed char)probe[1] != -1) { printf("FAIL probe: a cover target on an empty table\n"); return 1; }
        for (int i = 0; i < 8; i++)
            if (probe[2 + i]) { printf("FAIL probe: a coverable battle on an empty table\n"); return 1; }
        const unsigned char *mv = probe + FIO_PLAY_PROBE_HEAD;
        if (mv[0] != 1 || mv[4] != 0 || mv[5] != n_sel) { printf("FAIL probe: the move did not come back\n"); return 1; }
        // …and the hand is a rearrange, for the attacker too.
        prc = fio_play_probe(menu, lrc, 0, 0, 0, 0, menu + at + 2, n_sel,
                             FIO_PLAY_TARGET_HAND, (char *)probe, sizeof probe);
        if (prc < 0 || probe[FIO_PLAY_PROBE_HEAD] != 0) { printf("FAIL probe: the hand played a card\n"); return 1; }

        unsigned char human[1 << 16];
        int hrc = fio_play_human_menu(menu, lrc, 0, 0, (char *)human, sizeof human);
        if (hrc < 4) { printf("FAIL human menu rc=%d\n", hrc); return 1; }
        printf("play probe OK (menu %d -> human %d bytes)\n", lrc, hrc);
    }

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
    if (bubble_delta_check() != 0) return 1;
    if (lobby_v2_reseat_check() != 0) return 1;
    if (lobby_rules_check() != 0) return 1;
    if (nine_player_cap_check() != 0) return 1;
    if (beats_wire_check() != 0) return 1;
    if (plan_wire_check() != 0) return 1;
    if (pretable_wire_check() != 0) return 1;
    if (conflict_wire_check() != 0) return 1;
    if (board_rules_check() != 0) return 1;

    printf("SMOKE OK\n");
    return 0;
}
