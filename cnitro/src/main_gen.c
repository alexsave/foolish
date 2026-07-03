// Training-data generator for the torpex value net.
//
// Plays full games (all seats one strategy, default semtex — strong
// self-play, so outcomes reflect strong continuations, unlike the
// handwritten rollout policy whose bias motivated this) and, after each
// applied move, snapshots the FULL-INFORMATION state from every IN seat's
// perspective as a compact binary record. Targets (the seat's final finish
// position) are filled in when the game ends. The value net trained on
// these records replaces rollouts inside torpex's determinized MC — the
// worlds it evaluates there are exactly this kind of full-info state.
//
//   ./build/cnitro_gen --seats=semtex --players=4 --games=2000 \
//       --seed-start=100001 --sample=4 --out=/tmp/torpex_pc4.bin
//
// Record layout (little-endian, 72 bytes):
//   u64 my_hand, opp1_hand, opp2_hand, opp_rest_hand   (card-id bitmasks,
//       suits ROTATED so trump = suit 0; opps in turn order after me)
//   u64 att_unc, att_cov, def_cov                      (table masks, rotated)
//   u8  num_players, in_count, deck_count, discard_len,
//       has_flipped, flipped_id(rotated; 255 if none),
//       is_defender, def_rel, fa_rel, my_cnt, opp1_cnt, opp2_cnt,
//       opp_rest_cnt, goods_count, target_pos(1..N), my_good

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cli_util.h"
#include "../src/cordite_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static int dispatch_choose(int strat, const Game *g, int pi, const LegalMoves *moves) {
    switch (strat) {
        case STRAT_RANDOM:      return random_strategy_choose(g, pi, moves, NULL);
        case STRAT_ESPRESSO:    return espresso_strategy_choose(g, pi, moves, NULL);
        case STRAT_HANDWRITTEN: return handwritten_strategy_choose(g, pi, moves, NULL);
        case STRAT_CORDITE:     return cordite_strategy_choose(g, pi, moves, NULL);
        case STRAT_SEMTEX:      return semtex_strategy_choose(g, pi, moves, NULL);
        case STRAT_OCTOGEN:     return octogen_strategy_choose(g, pi, moves, NULL);
        case STRAT_TORPEX:      return torpex_strategy_choose(g, pi, moves, NULL);
        default:                return -1;
    }
}

#define REC_BYTES 72
#define MAX_RECS_PER_GAME 4096

typedef struct { uint8_t b[REC_BYTES]; int seat; } Rec;

static inline uint64_t rot_mask(uint64_t m, int power) {
    // suit' = (suit - power + 4) % 4 over card ids id = suit*13 + (v-1)
    uint64_t out = 0;
    while (m) {
        int id = __builtin_ctzll(m); m &= m - 1;
        int suit = id / 13, v = id % 13;
        int ns = (suit - power + 4) & 3;
        out |= 1ull << (ns * 13 + v);
    }
    return out;
}

static void put_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// Snapshot the state from seat p's view into rec (target filled later).
static void snapshot(const Game *g, const SimState *s, int p, Rec *r) {
    int power = s->power_suit;
    int np = s->num_players;
    uint64_t opp[3] = {0, 0, 0};   // opp1, opp2, rest (turn order after p)
    uint8_t  cnt[3] = {0, 0, 0};
    int oi = 0;
    for (int step = 1; step < np; step++) {
        int q = (p + step) % np;
        if (s->status_p[q] != PLAYER_STATUS_IN) continue;
        int slot = oi < 2 ? oi : 2;
        opp[slot] |= s->hand[q];
        cnt[slot] = (uint8_t)(cnt[slot] + __builtin_popcountll(s->hand[q]));
        oi++;
    }
    uint64_t att_unc = 0, att_cov = 0, def_cov = 0;
    for (int i = 0; i < s->num_battles; i++) {
        if (s->covered_mask & (1ull << i)) {
            att_cov |= 1ull << s->atk[i];
            def_cov |= 1ull << s->def[i];
        } else {
            att_unc |= 1ull << s->atk[i];
        }
    }
    int in_c = 0;
    for (int i = 0; i < np; i++) if (s->status_p[i] == PLAYER_STATUS_IN) in_c++;
    int goods = __builtin_popcount(s->good_mask);

    uint8_t *b = r->b;
    put_u64(b + 0,  rot_mask(s->hand[p], power));
    put_u64(b + 8,  rot_mask(opp[0], power));
    put_u64(b + 16, rot_mask(opp[1], power));
    put_u64(b + 24, rot_mask(opp[2], power));
    put_u64(b + 32, rot_mask(att_unc, power));
    put_u64(b + 40, rot_mask(att_cov, power));
    put_u64(b + 48, rot_mask(def_cov, power));
    b[56] = (uint8_t)np;
    b[57] = (uint8_t)in_c;
    b[58] = (uint8_t)(s->deck_n > 255 ? 255 : s->deck_n);
    b[59] = (uint8_t)(s->discard_pile_length > 255 ? 255 : s->discard_pile_length);
    b[60] = (uint8_t)(s->has_flipped ? 1 : 0);
    if (s->has_flipped) {
        int id = s->flipped_id, suit = id / 13, v = id % 13;
        b[61] = (uint8_t)(((suit - power + 4) & 3) * 13 + v);
    } else b[61] = 255;
    b[62] = (uint8_t)(g->defender == p);
    b[63] = (uint8_t)((g->defender - p + np) % np);
    b[64] = (uint8_t)((g->first_attacker - p + np) % np);
    b[65] = (uint8_t)__builtin_popcountll(s->hand[p]);
    b[66] = cnt[0];
    b[67] = cnt[1];
    b[68] = cnt[2];
    b[69] = (uint8_t)goods;
    b[70] = 0;   // target, filled at game end
    b[71] = (uint8_t)((s->good_mask >> p) & 1);
    r->seat = p;
}

int main(int argc, char **argv) {
    const char *seats_str = get_arg(argc, argv, "seats", "semtex");
    int strat = parse_strategy(seats_str);
    if (strat < 0) { fprintf(stderr, "unknown strategy '%s'\n", seats_str); return 2; }
    int n = parse_int(get_arg(argc, argv, "players", "4"), 4);
    int games = parse_int(get_arg(argc, argv, "games", "1000"), 1000);
    uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed-start", "100001"), 100001);
    int sample_mod = parse_int(get_arg(argc, argv, "sample", "4"), 4);   // 1/mod of moves
    const char *out_path = get_arg(argc, argv, "out", "/tmp/torpex.bin");

    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "cannot open %s\n", out_path); return 2; }

    static Rec recs[MAX_RECS_PER_GAME];
    long total = 0, done = 0;

    for (int gi = 0; gi < games; gi++) {
        uint32_t seed = seed0 + (uint32_t)gi;
        game_set_seed(seed ? seed : 1);
        random_strategy_set_seed(seed ? seed : 1);
        cd_sim_solve_reset();
        Game g; memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)n;
        for (int i = 0; i < n; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = (int8_t)strat;
            snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
        }
        start_game(&g);

        int nr = 0;
        int iters = 0, moves_applied = 0;
        while (game_done(&g) < 0 && iters++ < 4000) {
            int elig[MAX_PLAYERS]; int n_e = 0;
            for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
            if (n_e == 0) break;
            for (int i = n_e - 1; i > 0; i--) {
                int j = (int)(game_random() * (i + 1));
                if (j < 0) j = 0; if (j > i) j = i;
                int t = elig[i]; elig[i] = elig[j]; elig[j] = t;
            }
            bool acted = false;
            for (int k = 0; k < n_e; k++) {
                int pi = elig[k];
                LegalMoves moves;
                calculate_legal_moves(&g, pi, &moves);
                if (moves.n == 0) continue;
                int idx = dispatch_choose(g.players[pi].strategy_key, &g, pi, &moves);
                if (idx < 0 || idx >= moves.n) continue;
                const LegalMove *m = &moves.moves[idx];
                bool ok = false;
                switch (m->type) {
                    case MOVE_ATTACK: ok = handle_attack(&g, pi, m->cards, m->n_cards); break;
                    case MOVE_COVER:  ok = handle_cover (&g, pi, m->cards, m->attack_cards, m->n_cards); break;
                    case MOVE_PASS:   ok = handle_pass  (&g, pi, m->cards, m->n_cards); break;
                    case MOVE_PICKUP: ok = handle_pickup(&g, pi); break;
                    case MOVE_GOOD:   ok = handle_good  (&g, pi); break;
                    default: break;
                }
                if (ok) { acted = true; break; }
            }
            if (!acted) break;
            moves_applied++;
            if (moves_applied % sample_mod != 0) continue;
            if (game_done(&g) >= 0) break;
            SimState s;
            cd_sim_from_game(&s, &g);
            for (int p = 0; p < n && nr < MAX_RECS_PER_GAME; p++) {
                if (s.status_p[p] != PLAYER_STATUS_IN) continue;
                snapshot(&g, &s, p, &recs[nr]);
                nr++;
            }
        }
        if (game_done(&g) < 0) continue;

        // Fill targets: finish position per seat (elim slot + 1; durak = N).
        uint8_t pos[MAX_PLAYERS];
        for (int p = 0; p < n; p++) pos[p] = (uint8_t)n;
        for (int i = 0; i < g.num_eliminated; i++) pos[g.elimination_order[i]] = (uint8_t)(i + 1);
        for (int i = 0; i < nr; i++) recs[i].b[70] = pos[recs[i].seat];
        for (int i = 0; i < nr; i++) fwrite(recs[i].b, 1, REC_BYTES, out);
        total += nr;
        done++;
        if (done % 200 == 0) {
            fprintf(stderr, "  %ld games, %ld records\n", done, total);
            fflush(out);
        }
    }
    fclose(out);
    printf("games=%ld records=%ld out=%s\n", done, total, out_path);
    return 0;
}
