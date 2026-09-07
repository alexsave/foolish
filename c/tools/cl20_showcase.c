// cl20-vs-octogen showcase: play seeded games with cl20 in some seats and
// octogen in the rest, score each for drama (the main_showcase.c heuristic),
// and print the most dramatic finalists as shareable replay links
// (replay_encode_v6_from_game -> base32 -> replay_extras_link with the two
// bots named per seat). Wide ChaCha deal seeds, the only kind v6 re-derives.
//
//   cl20_showcase --pc=4 --games=60 --top=3 --seed=1 [--layout=alt|first]
//
// layout=first: seat 0 is cl20, every other seat octogen (heads-up, 3p).
// layout=alt:   even seats cl20, odd seats octogen (4p, 6p, 8p).
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cli_util.h"
#include "../src/replay.h"
#include "../src/replay_extras.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SNAPS 4096
static int16_t g_hist[MAX_SNAPS][MAX_PLAYERS];
static int g_T;
static void snapshot(const Game *g) {
    if (g_T >= MAX_SNAPS) return;
    for (int i = 0; i < g->num_players; i++) g_hist[g_T][i] = g->players[i].hand_count;
    g_T++;
}
static uint32_t g_sh; static uint32_t sh_rand(void) { g_sh = g_sh * 1664525u + 1013904223u; return g_sh >> 8; }

static void make_seed32(uint32_t seed, unsigned char out[FOOLISH_SEED_LEN]) {
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xC2B2AE3D27D4EB4Full);
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        out[i] = (unsigned char)(x >> 24);
    }
}

static int choose(const Game *g, int pi, const LegalMoves *m, int strat) {
    return strat == STRAT_CL20 ? cl20_strategy_choose(g, pi, m, NULL)
                               : octogen_strategy_choose(g, pi, m, NULL);
}

static bool play(Game *g, int np, const int *seat_strat, uint32_t seed, const unsigned char *seed32) {
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        g->players[i].strategy_key = (int8_t)seat_strat[i];
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
    }
    game_set_seed(seed);
    game_set_deal_seed_bytes(seed32, FOOLISH_SEED_LEN);
    random_strategy_set_seed(seed ^ 0x9e3779b9u);
    g_sh = seed ^ 0x5bd1e995u;
    start_game(g);
    g_T = 0; snapshot(g);
    static LegalMoves moves;
    int actions = 0;
    while (game_done(g) < 0) {
        if (++actions > 4000) return false;
        int elig[MAX_PLAYERS], n_e = 0;
        for (int i = 0; i < np; i++) if (should_bot_act(g, i)) elig[n_e++] = i;
        if (!n_e) return false;
        for (int i = n_e - 1; i > 0; i--) { int j = (int)(sh_rand() % (uint32_t)(i + 1)); int t = elig[i]; elig[i] = elig[j]; elig[j] = t; }
        bool acted = false;
        for (int e = 0; e < n_e && !acted; e++) {
            int pi = elig[e];
            calculate_legal_moves(g, pi, &moves);
            if (!moves.n) continue;
            int mi = choose(g, pi, &moves, seat_strat[pi]);
            if (mi < 0 || mi >= moves.n) continue;
            const LegalMove *m = &moves.moves[mi];
            switch (m->type) {
                case MOVE_ATTACK: acted = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  acted = handle_cover(g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   acted = handle_pass(g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: acted = handle_pickup(g, pi); break;
                case MOVE_GOOD:   acted = handle_good(g, pi); break;
                default: break;
            }
            if (acted) snapshot(g);
        }
        if (!acted) return false;
    }
    return g->num_logs < MAX_LOGS;
}

static double score_game(const Game *g, int np, int *peaks) {
    int fool = game_done(g);
    int peak[MAX_PLAYERS] = {0};
    for (int t = 0; t < g_T; t++) for (int s = 0; s < np; s++) if (g_hist[t][s] > peak[s]) peak[s] = g_hist[t][s];
    for (int s = 0; s < np; s++) peaks[s] = peak[s];
    int escapePeak = 0;
    for (int s = 0; s < np; s++) if (s != fool && peak[s] > escapePeak) escapePeak = peak[s];
    int foolLateMin = 1 << 30;
    for (int t = g_T / 2; t < g_T; t++) { int h = g_hist[t][fool]; if (h > 0 && h < foolLateMin) foolLateMin = h; }
    if (foolLateMin == (1 << 30)) foolLateMin = 0;
    int collapse = peak[fool] - foolLateMin; if (collapse < 0) collapse = 0;
    int leadChanges = 0, prev = -1;
    for (int t = 0; t < g_T; t++) {
        int lead = 0;
        for (int s = 1; s < np; s++) if (g_hist[t][s] > g_hist[t][lead]) lead = s;
        if (lead != prev && prev != -1) leadChanges++;
        prev = lead;
    }
    double lengthBonus = (g_T < 200 ? g_T : 200) / 10.0;
    return escapePeak * 3.0 + collapse * 1.5 + leadChanges * 2.5 + g->num_eliminated * 4.0 + lengthBonus;
}

typedef struct { Game g; double score; int peaks[MAX_PLAYERS]; uint32_t seed; unsigned char seed32[FOOLISH_SEED_LEN]; bool used; } Finalist;
static Finalist g_top[16];

int main(int argc, char **argv) {
    int np    = parse_int(get_arg(argc, argv, "pc", "4"), 4);
    int games = parse_int(get_arg(argc, argv, "games", "60"), 60);
    int top   = parse_int(get_arg(argc, argv, "top", "3"), 3);
    uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed", "1"), 1);
    const char *layout = get_arg(argc, argv, "layout", np <= 3 ? "first" : "alt");
    int want_win = parse_int(get_arg(argc, argv, "cl20-wins", "0"), 0);   // 1: only games a cl20 seat wins
    if (top > 16) top = 16;
    int seat_strat[MAX_PLAYERS];
    for (int i = 0; i < np; i++)
        seat_strat[i] = (!strcmp(layout, "first") ? (i == 0) : (i % 2 == 0)) ? STRAT_CL20 : STRAT_OCTOGEN;
    Game *g = malloc(sizeof(Game));
    for (int gi = 0; gi < games; gi++) {
        uint32_t seed = seed0 + (uint32_t)gi;
        unsigned char s32[FOOLISH_SEED_LEN]; make_seed32(seed, s32);
        if (!play(g, np, seat_strat, seed, s32)) continue;
        int winner = g->num_eliminated ? g->elimination_order[0] : -1;
        if (want_win && (winner < 0 || seat_strat[winner] != STRAT_CL20)) continue;
        int peaks[MAX_PLAYERS];
        double sc = score_game(g, np, peaks);
        int slot = -1, worst = -1;
        for (int i = 0; i < top; i++) {
            if (!g_top[i].used) { if (slot < 0) slot = i; }
            else if (worst < 0 || g_top[i].score < g_top[worst].score) worst = i;
        }
        if (slot < 0) { if (worst >= 0 && sc > g_top[worst].score) slot = worst; else continue; }
        game_clone(&g_top[slot].g, g);
        g_top[slot].score = sc; g_top[slot].seed = seed; memcpy(g_top[slot].seed32, s32, FOOLISH_SEED_LEN);
        memcpy(g_top[slot].peaks, peaks, sizeof(int) * (size_t)np); g_top[slot].used = true;
    }
    // Emit, best first.
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < top; i++) if (g_top[i].used && (best < 0 || g_top[i].score > g_top[best].score)) best = i;
        if (best < 0) break;
        Finalist *f = &g_top[best]; f->used = false;
        static unsigned char enc[65536]; char code[4096], link[8192];
        int n = replay_encode_v6_from_game(&f->g, f->seed32, FOOLISH_SEED_LEN, INT_MAX, enc, sizeof enc);
        if (n < 0) { fprintf(stderr, "seed %u: encode failed (%d, detail %d)\n", f->seed, n, replay_last_error_detail()); continue; }
        if (replay_b32_encode(enc, n, code, sizeof code) < 0) continue;
        unsigned char names[512]; int nl = 0;
        for (int s = 0; s < np; s++) {
            char nm[32]; snprintf(nm, sizeof nm, "%s %d", seat_strat[s] == STRAT_CL20 ? "CL-20" : "Octogen", s + 1);
            int len = (int)strlen(nm);
            names[nl++] = (unsigned char)(len & 0xff); names[nl++] = (unsigned char)(len >> 8);
            memcpy(names + nl, nm, (size_t)len); nl += len;
        }
        if (replay_extras_link(code, names, nl, np, link, sizeof link) < 0) continue;
        printf("pc=%d seed=%u score=%.1f moves=%d  finish:", np, f->seed, f->score, f->g.num_logs);
        for (int i = 0; i < f->g.num_eliminated; i++) {
            int s = f->g.elimination_order[i];
            printf(" %d.%s%d", i + 1, seat_strat[s] == STRAT_CL20 ? "CL20-" : "OG-", s + 1);
        }
        int fool = game_done(&f->g);
        printf("  fool=%s%d  peaks:", seat_strat[fool] == STRAT_CL20 ? "CL20-" : "OG-", fool + 1);
        for (int s = 0; s < np; s++) printf(" %d", f->peaks[s]);
        printf("\n  %s\n", link);
    }
    return 0;
}
