// Format-6 (partial-game, hidden-state-lossless) codec test — the native proof
// that option 3 (docs/IMESSAGE_GAME_DESIGN.md §16) works end to end:
//
//   engine game (game.c + legal.c) — the ONLY place the true deck lives
//     -> capture the real initial hands + the real stock-draw order
//     -> replay_encode_v6  (feeds those real hidden cards in)
//     -> replay_decode
//     -> the decoded stream must carry EVERY hidden card's REAL identity:
//        * one leading LOG_DRAW per seat = that seat's true initial hand
//        * every later LOG_DRAW = the true drawn card (never CARD_HIDDEN)
//        so NO retrodiction guess is ever needed — the whole point.
//
// Also asserts: encode determinism, decode->re-encode fixed point, a MID-GAME
// prefix decodes cleanly with no fool, and reports the size on the wire.
//
// Usage: replay_v6_test [games_per_pc] [seed0]   (defaults 40, 1337)

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ACTIONS 100000
#define ENC_CAP (1 << 20)
#define DEC_CAP (2 << 20)

static unsigned char g_in[ENC_CAP];
static unsigned char g_in2[ENC_CAP];
static unsigned char g_enc[ENC_CAP];
static unsigned char g_enc2[ENC_CAP];
static unsigned char g_dec[DEC_CAP];

static uint32_t g_sh_seed = 1;
static uint32_t sh_rand(void) {
    g_sh_seed = g_sh_seed * 1664525u + 1013904223u;
    return g_sh_seed;
}

static unsigned char wire_of(Card c) {
    if (c.suit < 0 || c.value < 1) return (unsigned char)REPLAY_CARD_HIDDEN;
    return (unsigned char)(c.suit * 13 + (c.value - 1));
}

// Captured at deal time (the initial hands vanish as the game is played out).
static unsigned char g_init_hand[MAX_PLAYERS][CARDS_PER_PLAYER];
static int g_init_len[MAX_PLAYERS];

// seed32 = NULL: the legacy 32-bit LCG deal (what this file has always tested).
// seed32 != NULL: the wide ChaCha deal — the only kind replay_encode_v6_from_game
// can re-derive, and the only kind production deals.
static bool play_game_ex(Game *g, int np, int strat, uint32_t seed,
                         const unsigned char *seed32) {
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    game_set_seed(seed);
    // AFTER game_set_seed, which clears wide mode.
    if (seed32) game_set_deal_seed_bytes(seed32, FOOLISH_SEED_LEN);
    random_strategy_set_seed(seed ^ 0x9e3779b9u);
    g_sh_seed = seed ^ 0x5bd1e995u;
    start_game(g);

    // Snapshot the true initial deal before any card is played.
    for (int s = 0; s < np; s++) {
        g_init_len[s] = g->players[s].hand_count;
        for (int k = 0; k < g->players[s].hand_count; k++)
            g_init_hand[s][k] = wire_of(g->players[s].hand[k]);
    }

    static LegalMoves moves;
    int actions = 0;
    while (game_done(g) < 0) {
        if (++actions > MAX_ACTIONS) return false;
        int elig[MAX_PLAYERS], n_e = 0;
        for (int i = 0; i < np; i++) if (should_bot_act(g, i)) elig[n_e++] = i;
        if (n_e == 0) return false;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(sh_rand() % (uint32_t)(i + 1));
            int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
        }
        bool acted = false;
        for (int e = 0; e < n_e && !acted; e++) {
            int pi = elig[e];
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            int mi = strat == STRAT_RANDOM
                ? random_strategy_choose(g, pi, &moves, 0)
                : handwritten_strategy_choose(g, pi, &moves, 0);
            const LegalMove *m = &moves.moves[mi];
            switch (m->type) {
                case MOVE_ATTACK: acted = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  acted = handle_cover(g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   acted = handle_pass(g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: acted = handle_pickup(g, pi); break;
                case MOVE_GOOD:   acted = handle_good(g, pi); break;
                default: break;
            }
        }
        if (!acted) return false;
    }
    return true;
}

static bool play_game(Game *g, int np, int strat, uint32_t seed) {
    return play_game_ex(g, np, strat, seed, 0);
}

// Build the v6 encode input from the engine's real game. `max_atoms` caps the
// action count (for mid-game tests); use a huge value for the whole game.
// Returns input length and writes *out_atoms / *out_reveals.
static int build_v6_input(const GameLog *logs, int num_logs, int np,
                          Card flipped, int first_attacker, int max_atoms,
                          unsigned char *out, int *out_atoms, int *out_reveals) {
    int trump_id = flipped.suit * 13 + (flipped.value - 1);
    unsigned char flip_wire = wire_of(flipped);

    // reveals: initial deal (seat-major) then stock draws in log order, minus
    // the single face-up flip draw.
    unsigned char reveals[64];
    int nr = 0;
    for (int s = 0; s < np; s++)
        for (int k = 0; k < g_init_len[s]; k++) reveals[nr++] = g_init_hand[s][k];

    // actions (v5 layout) — but capped at max_atoms, and reveals only counted
    // for draws that happen BEFORE the cut. We walk logs once, tracking atoms.
    int q = 7;
    int n_actions = 0;
    for (int i = 0; i < num_logs; i++) {
        const GameLog *l = &logs[i];
        bool info = l->log_type == LOG_ATTACK || l->log_type == LOG_COVER
                 || l->log_type == LOG_PASS || l->log_type == LOG_PICKUP;
        bool round_end = l->log_type == LOG_DISCARD && i > 0
                      && logs[i - 1].log_type == LOG_GOOD;
        if ((info || round_end) && n_actions >= max_atoms) break;
        if (l->log_type == LOG_DRAW) {
            for (int j = 0; j < l->num_pairs; j++) {
                unsigned char w = wire_of(l->pairs[j].primary);
                if (w != flip_wire) reveals[nr++] = w;   // skip the flip
            }
        }
        if (info) {
            out[q++] = (unsigned char)l->log_type;
            out[q++] = (unsigned char)l->player_idx;
            out[q++] = (unsigned char)l->num_pairs;
            for (int j = 0; j < l->num_pairs; j++) {
                out[q++] = wire_of(l->pairs[j].primary);
                out[q++] = card_is_none(l->pairs[j].target)
                    ? (unsigned char)REPLAY_CARD_NONE : wire_of(l->pairs[j].target);
            }
            n_actions++;
        } else if (round_end) {
            out[q++] = (unsigned char)REPLAY_ROUND_END;
            out[q++] = 0xFF;
            out[q++] = 0;
            n_actions++;
        }
    }

    // Splice reveals in front of the actions.
    int rev_off = 7;
    memmove(out + rev_off + nr, out + rev_off, (size_t)(q - rev_off));
    memcpy(out + rev_off, reveals, (size_t)nr);
    q += nr;

    out[0] = (unsigned char)np;
    out[1] = (unsigned char)trump_id;
    out[2] = (unsigned char)first_attacker;
    out[3] = (unsigned char)(n_actions & 0xff);
    out[4] = (unsigned char)((n_actions >> 8) & 0xff);
    out[5] = (unsigned char)(nr & 0xff);
    out[6] = (unsigned char)((nr >> 8) & 0xff);
    *out_atoms = n_actions;
    *out_reveals = nr;
    return q;
}

typedef struct {
    int type, seat, def_idx, n_pairs;
    const unsigned char *pairs;
} DecLog;

static int dec_next(const unsigned char *buf, int len, int *pos, DecLog *out) {
    if (*pos + 4 > len) return 0;
    out->type = buf[*pos];
    out->seat = buf[*pos + 1] == 0xFF ? -1 : buf[*pos + 1];
    out->def_idx = buf[*pos + 2] == 0xFF ? -1 : buf[*pos + 2];
    out->n_pairs = buf[*pos + 3];
    out->pairs = buf + *pos + 4;
    if (*pos + 4 + 2 * out->n_pairs > len) return 0;
    *pos += 4 + 2 * out->n_pairs;
    return 1;
}

static int n_pass = 0, n_fail = 0, n_skip = 0;
static long code_bytes = 0;
static int n_sized = 0;
#define CHECK(cond, ...) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL(np=%d seed=%u): ", np, seed); \
           fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return; } \
} while (0)

static void run_one(int np, int strat, uint32_t seed) {
    static Game g;
    if (!play_game(&g, np, strat, seed)) { n_skip++; return; }
    if (g.num_logs >= MAX_LOGS) { n_skip++; return; }
    if (g.has_flipped) { n_skip++; return; }   // needs a dry stock at end
    Card flip = g.flipped;
    int fa = -1;
    for (int i = 0; i < g.num_logs && fa < 0; i++)
        if (g.logs[i].log_type == LOG_ATTACK) fa = g.logs[i].player_idx;
    CHECK(fa >= 0, "no attack in logs");

    int atoms = 0, reveals = 0;
    int in_len = build_v6_input(g.logs, g.num_logs, np, flip, fa,
                                1 << 30, g_in, &atoms, &reveals);

    int enc = replay_encode_v6(g_in, in_len, g_enc, ENC_CAP);
    CHECK(enc > 0, "v6 encode failed: %d (detail %d)", enc, replay_last_error_detail());

    // determinism
    int enc2 = replay_encode_v6(g_in, in_len, g_enc2, ENC_CAP);
    CHECK(enc2 == enc && memcmp(g_enc, g_enc2, (size_t)enc) == 0, "v6 encode not deterministic");

    int dec = replay_decode(g_enc, enc, g_dec, DEC_CAP);
    CHECK(dec > 0, "v6 decode failed: %d (detail %d)", dec, replay_last_error_detail());
    CHECK(g_dec[0] == REPLAY_FORMAT_VERSION_V10, "version %d", g_dec[0]);
    CHECK(g_dec[4] == game_done(&g), "fool: decoded %d engine %d", g_dec[4], game_done(&g));

    // --- losslessness: the leading n LOG_DRAWs are the true initial hands ---
    int pos = REPLAY_DEC_HDR;
    DecLog dl;
    CHECK(dec_next(g_dec, dec, &pos, &dl) && dl.type == LOG_GAME_START,
          "first log not GAME_START");
    for (int s = 0; s < np; s++) {
        CHECK(dec_next(g_dec, dec, &pos, &dl), "missing initial-deal draw for seat %d", s);
        CHECK(dl.type == LOG_DRAW && dl.seat == s, "initial deal seat %d: type %d seat %d",
              s, dl.type, dl.seat);
        CHECK(dl.n_pairs == g_init_len[s], "initial deal seat %d: %d cards vs %d",
              s, dl.n_pairs, g_init_len[s]);
        // set equality (deal order within a hand is unconstrained)
        for (int a = 0; a < dl.n_pairs; a++) {
            CHECK(dl.pairs[2 * a] != REPLAY_CARD_HIDDEN, "seat %d card %d hidden!", s, a);
            int found = 0;
            for (int b = 0; b < g_init_len[s]; b++)
                if (g_init_hand[s][b] == dl.pairs[2 * a]) found = 1;
            CHECK(found, "seat %d initial card %02x not in true hand", s, dl.pairs[2 * a]);
        }
    }

    // --- losslessness: every remaining LOG_DRAW carries the REAL card ---
    // Compare the decoded tail to the engine stream (GOOD stripped both sides);
    // DRAW primaries must equal the engine's real cards (NOT hidden).
    int ei = 0;
    while (1) {
        const GameLog *el = 0;
        while (ei < g.num_logs) {
            // GOOD is implied (v4+); GAME_START was already consumed above.
            if (g.logs[ei].log_type != LOG_GOOD
                && g.logs[ei].log_type != LOG_GAME_START) { el = &g.logs[ei]; break; }
            ei++;
        }
        int got = 0;
        while ((got = dec_next(g_dec, dec, &pos, &dl)) != 0) {
            if (dl.type != LOG_GOOD) break;
        }
        if (!el && !got) break;
        CHECK(el && got, "stream length mismatch at engine log %d", ei);
        CHECK(dl.type == el->log_type, "log %d: type %d vs %d", ei, dl.type, el->log_type);
        CHECK(dl.seat == el->player_idx, "log %d: seat %d vs %d", ei, dl.seat, el->player_idx);
        int cmp = el->num_pairs;
        if (el->num_pairs == MAX_LOG_PAIRS && dl.n_pairs >= MAX_LOG_PAIRS) {
            CHECK(el->log_type == LOG_PICKUP || el->log_type == LOG_DISCARD,
                  "log %d: only pickup/discard may truncate", ei);
        } else {
            CHECK(dl.n_pairs == el->num_pairs, "log %d (type %d): pairs %d vs %d",
                  ei, el->log_type, dl.n_pairs, el->num_pairs);
        }
        for (int j = 0; j < cmp && j < dl.n_pairs; j++) {
            // v6: DRAW carries the REAL identity — including the flip — never hidden.
            unsigned char want_p = wire_of(el->pairs[j].primary);
            unsigned char want_t = card_is_none(el->pairs[j].target)
                ? (unsigned char)REPLAY_CARD_NONE : wire_of(el->pairs[j].target);
            CHECK(dl.pairs[2 * j] == want_p && dl.pairs[2 * j + 1] == want_t,
                  "log %d pair %d: %02x/%02x vs %02x/%02x", ei, j,
                  dl.pairs[2 * j], dl.pairs[2 * j + 1], want_p, want_t);
        }
        ei++;
    }

    // --- fixed point: rebuild the v6 input from the decoded stream, re-encode ---
    {
        // reveals from decoded DRAW logs (initial deal first n, then draws minus
        // the flip); actions from info logs + round_end markers.
        unsigned char reveals[64];
        int nr = 0, q = 7, na = 0;
        int p2 = REPLAY_DEC_HDR, seen_deals = 0;
        DecLog cur, prev = { 0 };
        bool have_prev = false;
        unsigned char flip_wire = wire_of(flip);
        while (dec_next(g_dec, dec, &p2, &cur)) {
            if (cur.type == LOG_DRAW) {
                for (int j = 0; j < cur.n_pairs; j++)
                    if (seen_deals < np || cur.pairs[2 * j] != flip_wire)
                        reveals[nr++] = cur.pairs[2 * j];
                if (seen_deals < np) seen_deals++;   // the leading per-seat deals
            }
            bool info = cur.type == LOG_ATTACK || cur.type == LOG_COVER
                     || cur.type == LOG_PASS || cur.type == LOG_PICKUP;
            if (info) {
                g_in2[q++] = (unsigned char)cur.type;
                g_in2[q++] = (unsigned char)cur.seat;
                g_in2[q++] = (unsigned char)cur.n_pairs;
                memcpy(g_in2 + q, cur.pairs, (size_t)(2 * cur.n_pairs));
                q += 2 * cur.n_pairs;
                na++;
            } else if (cur.type == LOG_DISCARD && have_prev && prev.type == LOG_GOOD) {
                g_in2[q++] = (unsigned char)REPLAY_ROUND_END;
                g_in2[q++] = 0xFF;
                g_in2[q++] = 0;
                na++;
            }
            prev = cur;
            have_prev = true;
        }
        memmove(g_in2 + 7 + nr, g_in2 + 7, (size_t)(q - 7));
        memcpy(g_in2 + 7, reveals, (size_t)nr);
        q += nr;
        g_in2[0] = (unsigned char)np;
        g_in2[1] = g_dec[2];
        g_in2[2] = g_dec[3];
        g_in2[3] = (unsigned char)(na & 0xff);
        g_in2[4] = (unsigned char)((na >> 8) & 0xff);
        g_in2[5] = (unsigned char)(nr & 0xff);
        g_in2[6] = (unsigned char)((nr >> 8) & 0xff);
        int re = replay_encode_v6(g_in2, q, g_enc2, ENC_CAP);
        CHECK(re == enc && memcmp(g_enc, g_enc2, (size_t)enc) == 0,
              "decode->re-encode not a fixed point (%d vs %d bytes)", re, enc);
    }

    // --- mid-game: encode HALF the atoms; must decode cleanly, no fool yet ---
    if (atoms >= 4) {
        int ma = atoms / 2, ma_atoms = 0, ma_rev = 0;
        int mlen = build_v6_input(g.logs, g.num_logs, np, flip, fa, ma,
                                  g_in, &ma_atoms, &ma_rev);
        int me = replay_encode_v6(g_in, mlen, g_enc2, ENC_CAP);
        CHECK(me > 0, "mid-game encode failed: %d (detail %d)", me, replay_last_error_detail());
        int md = replay_decode(g_enc2, me, g_dec, DEC_CAP);
        CHECK(md > 0, "mid-game decode failed: %d (detail %d)", md, replay_last_error_detail());
        CHECK(g_dec[0] == REPLAY_FORMAT_VERSION_V10, "mid version");
        // A mid-game cut generally still has >1 player IN -> no fool (0xFF).
        // (Occasionally half the atoms already finishes a 2p game; allow both.)
        CHECK(g_dec[4] == 0xFF || g_dec[4] == game_done(&g), "mid fool byte %d", g_dec[4]);
    }

    // --- size: what a full game costs on the wire ---
    code_bytes += enc;
    n_sized++;
}

/* ================= A4: replay_encode_v6_from_game (seeded) ================= */
// The production v6 producer: the kernel re-derives the deal from the seed and
// reads the actions straight out of the game's own logs, so no host marshals
// anything. The ORACLE is this file's build_v6_input + replay_encode_v6 — the
// path the TS finalize choreography drove, with the true hands captured at deal
// time — and the two must agree TO THE BYTE. That equality is what makes
// from_game a port of the v6 producer rather than a second one.

static unsigned char g_seed32[FOOLISH_SEED_LEN];
static unsigned char g_enc3[ENC_CAP];

static void make_seed(uint32_t s) {
    uint32_t x = s ? s : 1u;   // xorshift32: any spread of bytes will do
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        g_seed32[i] = (unsigned char)(x & 0xffu);
    }
}

static int g_hook_calls = 0;
static void counting_hook(const Game *g, int tag, int aux) {
    (void)g; (void)tag; (void)aux;
    g_hook_calls++;
}

static void run_one_from_game(int np, int strat, uint32_t seed) {
    static Game g;
    make_seed(seed);
    if (!play_game_ex(&g, np, strat, seed, g_seed32)) { n_skip++; return; }
    if (g.num_logs >= MAX_LOGS) { n_skip++; return; }
    // NOTE: no `has_flipped` skip. The marshalled oracle needs the caller to
    // hand it a trump, so run_one only trusts games whose stock dried; the
    // re-deal knows the trump either way, so this path covers BOTH — including
    // the games (most short ones) run_one has always skipped.

    int fa = replay_first_attacker_from_logs(g.logs, g.num_logs);
    CHECK(fa >= 0, "no attack in logs");

    // --- the whole point: one call, no marshalling, byte-equal to the oracle --
    int enc = replay_encode_v6_from_game(&g, g_seed32, FOOLISH_SEED_LEN,
                                         1 << 30, g_enc, ENC_CAP);
    CHECK(enc > 0, "v6 from_game failed: %d (detail %d)", enc, replay_last_error_detail());

    int atoms = 0, reveals = 0;
    int in_len = build_v6_input(g.logs, g.num_logs, np, g.flipped, fa,
                                1 << 30, g_in, &atoms, &reveals);
    int ref = replay_encode_v6(g_in, in_len, g_enc2, ENC_CAP);
    CHECK(ref > 0, "v6 oracle encode failed: %d (detail %d)", ref, replay_last_error_detail());
    CHECK(enc == ref && memcmp(g_enc, g_enc2, (size_t)enc) == 0,
          "from_game != marshalled oracle (%d B vs %d B)", enc, ref);

    // determinism (the re-deal must not carry state between calls)
    int enc2 = replay_encode_v6_from_game(&g, g_seed32, FOOLISH_SEED_LEN,
                                          1 << 30, g_enc3, ENC_CAP);
    CHECK(enc2 == enc && memcmp(g_enc, g_enc3, (size_t)enc) == 0,
          "from_game not deterministic");

    // it really is v6, and it really decodes to this game
    int dec = replay_decode(g_enc, enc, g_dec, DEC_CAP);
    CHECK(dec > 0, "from_game decode failed: %d (detail %d)", dec, replay_last_error_detail());
    CHECK(g_dec[0] == REPLAY_FORMAT_VERSION_V10, "version %d", g_dec[0]);
    CHECK(g_dec[4] == game_done(&g), "fool: decoded %d engine %d", g_dec[4], game_done(&g));

    // --- the decoded deal is the REAL deal (no retrodiction) ---
    int pos = REPLAY_DEC_HDR;
    DecLog dl;
    CHECK(dec_next(g_dec, dec, &pos, &dl) && dl.type == LOG_GAME_START, "no GAME_START");
    for (int s = 0; s < np; s++) {
        CHECK(dec_next(g_dec, dec, &pos, &dl), "missing initial deal for seat %d", s);
        CHECK(dl.type == LOG_DRAW && dl.seat == s && dl.n_pairs == g_init_len[s],
              "initial deal seat %d: type %d seat %d n %d", s, dl.type, dl.seat, dl.n_pairs);
        for (int k = 0; k < dl.n_pairs; k++) {
            unsigned char w = dl.pairs[2 * k];
            CHECK(w != REPLAY_CARD_HIDDEN, "seat %d card %d decoded HIDDEN", s, k);
            int found = 0;
            for (int j = 0; j < g_init_len[s]; j++) if (g_init_hand[s][j] == w) found = 1;
            CHECK(found, "seat %d decoded card %d was not dealt to it", s, w);
        }
    }

    // --- mid-game cut: a prefix encodes, decodes, and leaves no fool ---
    if (atoms > 4) {
        int cut = atoms / 2;
        int mid = replay_encode_v6_from_game(&g, g_seed32, FOOLISH_SEED_LEN,
                                             cut, g_enc3, ENC_CAP);
        CHECK(mid > 0, "from_game mid-game cut failed: %d (detail %d)",
              mid, replay_last_error_detail());
        int mdec = replay_decode(g_enc3, mid, g_dec, DEC_CAP);
        CHECK(mdec > 0, "mid-game decode failed: %d", mdec);
        CHECK(g_dec[0] == REPLAY_FORMAT_VERSION_V10, "mid version %d", g_dec[0]);
        CHECK(g_dec[4] == 0xFF, "mid-game stream named a fool: %d", g_dec[4]);
    }

    // --- a seed that did not deal this game is caught, not encoded ---
    unsigned char bad[FOOLISH_SEED_LEN];
    memcpy(bad, g_seed32, sizeof bad);
    bad[0] ^= 0xFFu;
    int wrong = replay_encode_v6_from_game(&g, bad, FOOLISH_SEED_LEN,
                                           1 << 30, g_enc3, ENC_CAP);
    CHECK(wrong < 0, "a wrong seed encoded anyway (%d B) — that is a replay of "
                     "a game nobody played", wrong);

    // --- the re-deal leaves no trace (both globals it touches) ---
    // Negative-tested: drop either restore in replay.c and these fail.
    g_hook_calls = 0;
    engine_snap_hook = counting_hook;
    int again = replay_encode_v6_from_game(&g, g_seed32, FOOLISH_SEED_LEN,
                                           1 << 30, g_enc3, ENC_CAP);
    void (*hook_after)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = 0;
    CHECK(again == enc, "encode under a hook changed the code");
    CHECK(g_hook_calls == 0,
          "the re-deal fired %d snapshots into the host's animation plan", g_hook_calls);
    // ...and gave the host's hook back. Without this the first assert is empty:
    // a re-deal that silently left the hook OFF also fires nothing, and the host
    // would just stop animating.
    CHECK(hook_after == counting_hook, "the re-deal did not restore the host's hook");

    // The deal RNG must come back byte-identical. start_game consumes the
    // ChaCha stream and leaves wide mode SET, and draw_index reads wide mode for
    // every game in this thread — so an unrestored re-deal would change how the
    // next game in a warm isolate draws its cards.
    //
    // Park a DIFFERENT stream first, or this proves nothing: re-dealing the same
    // seed lands on the same state, so before == after even with no restore at
    // all.
    unsigned char other[FOOLISH_SEED_LEN];
    memcpy(other, g_seed32, sizeof other);
    other[1] ^= 0x5Au;
    game_set_deal_seed_bytes(other, FOOLISH_SEED_LEN);
    unsigned char rng_before[GAME_DEAL_RNG_STATE_MAX], rng_after[GAME_DEAL_RNG_STATE_MAX];
    game_deal_rng_get(rng_before);
    (void)replay_encode_v6_from_game(&g, g_seed32, FOOLISH_SEED_LEN,
                                     1 << 30, g_enc3, ENC_CAP);
    game_deal_rng_get(rng_after);
    CHECK(memcmp(rng_before, rng_after, sizeof rng_before) == 0,
          "the re-deal moved the thread's deal RNG");

    // The wide FLAG specifically: a legacy (LCG-dealt) game in this thread must
    // still draw at random after a re-deal ran. This is the one that silently
    // changes an unrelated game's cards — draw_index checks `g_deal_wide ||
    // g->deterministic_deck`.
    game_set_seed(seed);   // clears wide mode
    (void)replay_encode_v6_from_game(&g, g_seed32, FOOLISH_SEED_LEN,
                                     1 << 30, g_enc3, ENC_CAP);
    CHECK(game_deal_seed_active() == 0,
          "the re-deal left wide mode on: every later game in this thread would "
          "pop a pre-shuffled deck instead of drawing at random");
}

int main(int argc, char **argv) {
    int games_per_pc = argc > 1 ? atoi(argv[1]) : 40;
    uint32_t seed0 = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 1337u;

    for (int np = 2; np <= 8; np++)
        for (int gi = 0; gi < games_per_pc; gi++) {
            int strat = (gi % 2 == 0) ? STRAT_RANDOM : STRAT_HANDWRITTEN;
            run_one(np, strat, seed0 + (uint32_t)(np * 10007 + gi));
            run_one_from_game(np, strat, seed0 + (uint32_t)(np * 10007 + gi));
        }

    printf("replay v6 test: %d checks passed, %d failed, %d skipped\n",
           n_pass, n_fail, n_skip);
#ifdef REPLAY_STATS
    extern int replay_stat_max_rec, replay_stat_max_bn;
    printf("REPLAY_STATS peaks: max_rec=%d max_bn=%d limbs "
           "(wasm caps REPLAY_REC_CAP=4096, BN_CAP=%d)\n",
           replay_stat_max_rec, replay_stat_max_bn, (4096 * 21 + 31) / 32);
#endif
    // This used to print the delta against the frozen v5 encoding of the same
    // game (+~35%, the price of exact hidden state and mid-game cuts). That
    // encoder is gone, so what is left to report is the absolute cost.
    if (n_sized)
        printf("size: avg %.1f B over %d games\n", (double)code_bytes / n_sized, n_sized);
    return n_fail ? 1 : 0;
}
