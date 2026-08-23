// Replay-codec round-trip difftest — the native mirror of
// e2e/replay_codec.test.ts, with zero TS involvement:
//
//   engine game (game.c + legal.c, random/handwritten strategies)
//     -> replay_encode (info logs + round_end markers)
//     -> replay_decode
//     -> the decoded stream must reproduce the ENTIRE engine log stream —
//        every derived DISCARD / DRAW / DEFENDER_CHANGE / PLAYER_OUT, plus
//        elimination order, fool and discard count. GOOD presses are implied
//        in format v4+ and stripped from both sides before comparing.
//
// Also asserts encode determinism and that re-encoding the DECODED stream
// reproduces the exact same bytes (the codec is a fixed point).
//
// Usage: replay_difftest [games_per_pc] [seed0]   (defaults 40, 1337)

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

static unsigned char g_enc_in[ENC_CAP];
static unsigned char g_enc_out[ENC_CAP];
static unsigned char g_enc_out2[ENC_CAP];
static unsigned char g_dec_out[DEC_CAP];

// Local shuffle RNG (the eligible-actor order), independent of the game RNG.
static uint32_t g_sh_seed = 1;
static uint32_t sh_rand(void) {
    g_sh_seed = g_sh_seed * 1664525u + 1013904223u;
    return g_sh_seed;
}

static unsigned char wire_of(Card c) {
    if (c.suit < 0 || c.value < 1) return (unsigned char)REPLAY_CARD_HIDDEN;
    return (unsigned char)(c.suit * 13 + (c.value - 1));
}

// ---------------------------------------------------------------------------
// Play one full game with the real engine; returns false on a stall.
static bool play_game(Game *g, int np, int strat, uint32_t seed) {
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    game_set_seed(seed);
    random_strategy_set_seed(seed ^ 0x9e3779b9u);
    g_sh_seed = seed ^ 0x5bd1e995u;
    start_game(g);

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

// ---------------------------------------------------------------------------
// makeSource (encode.ts): info logs + a round_end marker for every DISCARD
// directly preceded by a GOOD. Returns the encode-input length, or -1 if the
// stream can't be trusted (log buffer overflow).
static int build_encode_input(const GameLog *logs, int num_logs, bool truncated,
                              int np, Card flipped, int first_attacker,
                              unsigned char *out) {
    if (truncated) return -1;
    int trump_id = flipped.suit * 13 + (flipped.value - 1);
    int q = 5;
    int n_actions = 0;
    for (int i = 0; i < num_logs; i++) {
        const GameLog *l = &logs[i];
        bool info = l->log_type == LOG_ATTACK || l->log_type == LOG_COVER
                 || l->log_type == LOG_PASS || l->log_type == LOG_PICKUP;
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
        } else if (l->log_type == LOG_DISCARD && i > 0
                   && logs[i - 1].log_type == LOG_GOOD) {
            out[q++] = (unsigned char)REPLAY_ROUND_END;
            out[q++] = 0xFF;
            out[q++] = 0;
            n_actions++;
        }
    }
    out[0] = (unsigned char)np;
    out[1] = (unsigned char)trump_id;
    out[2] = (unsigned char)first_attacker;
    out[3] = (unsigned char)(n_actions & 0xff);
    out[4] = (unsigned char)((n_actions >> 8) & 0xff);
    return q;
}

// ---------------------------------------------------------------------------
// Decoded-stream walker.
typedef struct {
    int type, seat, def_idx, n_pairs;
    const unsigned char *pairs;  // n_pairs x 2
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

static int n_pass = 0, n_fail = 0, n_skip_stall = 0, n_skip_trunc = 0;
#define CHECK(cond, ...) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL(np=%d seed=%u): ", np, seed); \
           fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return; } \
} while (0)

static void run_one(int np, int strat, uint32_t seed) {
    static Game g;
    if (!play_game(&g, np, strat, seed)) { n_skip_stall++; return; }
    // A dropped log (num_logs at capacity) means the recorded stream is
    // incomplete — the codec can't round-trip what was never logged.
    bool truncated = g.num_logs >= MAX_LOGS;
    // The flip is always drawn by game end (eliminations need a dry stock).
    CHECK(!g.has_flipped, "flip still present at game end");
    Card flip_card = g.flipped;

    int in_len = build_encode_input(g.logs, g.num_logs, truncated, np,
                                    flip_card, -1, g_enc_in);
    if (in_len < 0) { n_skip_trunc++; return; }
    // first attacker = seat of the first ATTACK log
    int fa = -1;
    for (int i = 0; i < g.num_logs && fa < 0; i++)
        if (g.logs[i].log_type == LOG_ATTACK) fa = g.logs[i].player_idx;
    CHECK(fa >= 0, "no attack in logs");
    g_enc_in[2] = (unsigned char)fa;

    int enc_len = replay_encode(g_enc_in, in_len, g_enc_out, ENC_CAP);
    CHECK(enc_len > 0, "encode failed: %d (detail %d)", enc_len, replay_last_error_detail());

    // determinism
    int enc_len2 = replay_encode(g_enc_in, in_len, g_enc_out2, ENC_CAP);
    CHECK(enc_len2 == enc_len && memcmp(g_enc_out, g_enc_out2, (size_t)enc_len) == 0,
          "encode not deterministic");

    int dec_len = replay_decode(g_enc_out, enc_len, g_dec_out, DEC_CAP);
    CHECK(dec_len > 0, "decode failed: %d (detail %d)", dec_len, replay_last_error_detail());

    // header: fool / elimination / discard
    CHECK(g_dec_out[0] == REPLAY_FORMAT_VERSION_V9, "version");
    CHECK(g_dec_out[1] == np, "player count");
    CHECK(g_dec_out[3] == fa, "first attacker");
    CHECK(g_dec_out[4] == game_done(&g), "fool: decoded %d engine %d",
          g_dec_out[4], game_done(&g));
    int dec_discard = g_dec_out[5] | (g_dec_out[6] << 8);
    CHECK(dec_discard == g.discard_pile_length, "discard: decoded %d engine %d",
          dec_discard, g.discard_pile_length);
    CHECK(g_dec_out[7] == g.num_eliminated, "elim count");
    for (int i = 0; i < g.num_eliminated; i++)
        CHECK(g_dec_out[8 + i] == g.elimination_order[i], "elim[%d]", i);

    // full-stream comparison, GOOD stripped from both, engine DRAW
    // identities hidden except the flip (the public-log convention).
    int pos = REPLAY_DEC_HDR;
    int ei = 0;
    DecLog dl;
    while (1) {
        // next engine log that survives normalization
        const GameLog *el = 0;
        while (ei < g.num_logs) {
            if (g.logs[ei].log_type != LOG_GOOD) { el = &g.logs[ei]; break; }
            ei++;
        }
        // next decoded log likewise
        int got = 0;
        while ((got = dec_next(g_dec_out, dec_len, &pos, &dl)) != 0) {
            if (dl.type != LOG_GOOD) break;
        }
        if (!el && !got) break;
        CHECK(el && got, "stream length mismatch at engine log %d", ei);
        CHECK(dl.type == el->log_type, "log %d: type %d vs %d", ei, dl.type, el->log_type);
        CHECK(dl.seat == el->player_idx, "log %d: seat %d vs %d", ei, dl.seat, el->player_idx);
        CHECK(dl.def_idx == el->defender_index, "log %d: defender %d vs %d",
              ei, dl.def_idx, el->defender_index);
        // Engine pairs cap at MAX_LOG_PAIRS; when a PICKUP/DISCARD hit the
        // cap, compare the prefix the engine kept (attacks/covers/draws
        // never reach the cap, so coding-relevant logs stay fully strict).
        int cmp_pairs = el->num_pairs;
        if (el->num_pairs == MAX_LOG_PAIRS && dl.n_pairs >= MAX_LOG_PAIRS) {
            CHECK(el->log_type == LOG_PICKUP || el->log_type == LOG_DISCARD,
                  "log %d: only pickup/discard may truncate", ei);
        } else {
            CHECK(dl.n_pairs == el->num_pairs, "log %d (type %d): pairs %d vs %d",
                  ei, el->log_type, dl.n_pairs, el->num_pairs);
        }
        for (int j = 0; j < cmp_pairs && j < dl.n_pairs; j++) {
            unsigned char want_p, want_t;
            if (el->log_type == LOG_DRAW) {
                want_p = card_eq(el->pairs[j].primary, flip_card)
                    ? wire_of(flip_card) : (unsigned char)REPLAY_CARD_HIDDEN;
                want_t = (unsigned char)REPLAY_CARD_NONE;
            } else {
                want_p = wire_of(el->pairs[j].primary);
                want_t = card_is_none(el->pairs[j].target)
                    ? (unsigned char)REPLAY_CARD_NONE : wire_of(el->pairs[j].target);
            }
            CHECK(dl.pairs[2 * j] == want_p && dl.pairs[2 * j + 1] == want_t,
                  "log %d pair %d: %02x/%02x vs %02x/%02x", ei, j,
                  dl.pairs[2 * j], dl.pairs[2 * j + 1], want_p, want_t);
        }
        ei++;
    }

    // Re-encoding the DECODED stream must be a fixed point (byte-identical).
    {
        int q = 5, n_actions = 0;
        int p2 = REPLAY_DEC_HDR;
        DecLog prev = { 0 }, cur;
        bool have_prev = false;
        while (dec_next(g_dec_out, dec_len, &p2, &cur)) {
            bool info = cur.type == LOG_ATTACK || cur.type == LOG_COVER
                     || cur.type == LOG_PASS || cur.type == LOG_PICKUP;
            if (info) {
                g_enc_in[q++] = (unsigned char)cur.type;
                g_enc_in[q++] = (unsigned char)cur.seat;
                g_enc_in[q++] = (unsigned char)cur.n_pairs;
                memcpy(g_enc_in + q, cur.pairs, (size_t)(2 * cur.n_pairs));
                q += 2 * cur.n_pairs;
                n_actions++;
            } else if (cur.type == LOG_DISCARD && have_prev && prev.type == LOG_GOOD) {
                g_enc_in[q++] = (unsigned char)REPLAY_ROUND_END;
                g_enc_in[q++] = 0xFF;
                g_enc_in[q++] = 0;
                n_actions++;
            }
            prev = cur;
            have_prev = true;
        }
        g_enc_in[0] = (unsigned char)np;
        g_enc_in[1] = g_dec_out[2];
        g_enc_in[2] = g_dec_out[3];
        g_enc_in[3] = (unsigned char)(n_actions & 0xff);
        g_enc_in[4] = (unsigned char)((n_actions >> 8) & 0xff);
        int re_len = replay_encode(g_enc_in, q, g_enc_out2, ENC_CAP);
        CHECK(re_len == enc_len && memcmp(g_enc_out, g_enc_out2, (size_t)enc_len) == 0,
              "decode->re-encode not a fixed point (%d vs %d bytes)", re_len, enc_len);
    }
}

int main(int argc, char **argv) {
    int games_per_pc = argc > 1 ? atoi(argv[1]) : 40;
    uint32_t seed0 = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 1337u;

    // hostile-input smoke: decode must reject garbage cleanly, never hang
    {
        unsigned char junk[64], out[4096];
        for (int i = 0; i < 64; i++) junk[i] = (unsigned char)(i * 37 + 11);
        for (int len = 0; len <= 64; len++) {
            int r = replay_decode(junk, len, out, sizeof out);
            if (r > 0) {
                // a garbage integer that happens to decode is fine — it must
                // still have produced a well-formed stream
                int pos = REPLAY_DEC_HDR;
                DecLog dl;
                while (dec_next(out, r, &pos, &dl)) {}
            }
        }
        printf("hostile decode smoke: ok\n");
    }

    int total = 0;
    for (int np = 2; np <= 8; np++) {
        for (int gi = 0; gi < games_per_pc; gi++) {
            int strat = (gi % 2 == 0) ? STRAT_RANDOM : STRAT_HANDWRITTEN;
            run_one(np, strat, seed0 + (uint32_t)(np * 10007 + gi));
            total++;
        }
    }
    printf("replay difftest: %d games, %d checks passed, %d failed, "
           "%d skipped (log-capacity), %d skipped (stall)\n",
           total, n_pass, n_fail, n_skip_trunc, n_skip_stall);
    // Stalls would mean the driving loop itself is broken — fail loudly.
    if (n_pass == 0 || n_fail > 0 || n_skip_stall > 0) return 1;
    return 0;
}
