// Showcase generator. Plays all-cordite games (big brain vs big brain),
// SCORES each one for drama as it plays (free — hand sizes, lead changes and
// the fool are all known without any codec), and dumps only the top-K most
// dramatic games as JSON (one per line, highest score first). A TS translator
// (tests/cordite_showcase.ts) encodes the first finalist that round-trips into
// a self-contained replay URL.
//
// Scoring upstream of serialization means we pay the JSON + codec round-trip
// cost on ~K finalists instead of all N games. K>1 covers the ~5% of games
// that fail the C<->TS round-trip on a pickup divergence.
//
// The dumped log stream mirrors the production game_logs exactly (same LOG_*
// types and order), so the TS replay codec round-trips a C-played game.
//
// Usage:
//   cnitro_showcase --games=300 --pcs=4,6 --seed=1 --top=10 > finalists.jsonl
//
// NOTE: player counts are restricted to those where the C and TS engines
// agree on deck size (2..4 -> 36-card, 6..8 -> 52-card). n=5 differs and is
// rejected.

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cli_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_SNAPS 4096
#define MAX_TOP   64

// Per-game hand-size history, filled during play: hist[t][seat].
static int16_t g_hist[MAX_SNAPS][MAX_PLAYERS];
static int     g_T;

static void snapshot(const Game *g) {
    if (g_T >= MAX_SNAPS) return;
    for (int i = 0; i < g->num_players; i++)
        g_hist[g_T][i] = g->players[i].hand_count;
    g_T++;
}

// Dispatch a strategy id to its choose function (mirrors main_eval).
static int dispatch_choose(int strat, const Game *g, int pi, const LegalMoves *moves) {
    switch (strat) {
        case STRAT_RANDOM:      return random_strategy_choose(g, pi, moves, NULL);
        case STRAT_ESPRESSO:    return espresso_strategy_choose(g, pi, moves, NULL);
        case STRAT_HANDWRITTEN: return handwritten_strategy_choose(g, pi, moves, NULL);
        case STRAT_ROBUSTA:     return robusta_strategy_choose(g, pi, moves, NULL);
        case STRAT_FIRECRACKER: return firecracker_strategy_choose(g, pi, moves, NULL);
        case STRAT_GUNPOWDER:   return gunpowder_strategy_choose(g, pi, moves, NULL);
        case STRAT_BLACKPOWDER: return blackpowder_strategy_choose(g, pi, moves, NULL);
        case STRAT_CORDITE:     return cordite_strategy_choose(g, pi, moves, NULL);
        case STRAT_ASTROLITE:   return astrolite_strategy_choose(g, pi, moves, NULL);
        case STRAT_SEMTEX:      return semtex_strategy_choose(g, pi, moves, NULL);
        default:                return cordite_strategy_choose(g, pi, moves, NULL);
    }
}

// Play one all-`strat` game to completion, recording the hand-size history.
// Returns true if it finished with a single durak.
static bool play_all_cordite(Game *g, int np, uint32_t seed, int strat) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        g->players[i].strategy_key = (int8_t)strat;
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
    }
    start_game(g);
    g_T = 0;
    snapshot(g);

    int iters = 0;
    while (game_done(g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) elig[n_e++] = i;
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
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            int idx = dispatch_choose(strat, g, pi, &moves);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(g, pi); break;
                case MOVE_GOOD:   ok = handle_good  (g, pi); break;
                default: break;
            }
            if (ok) { acted = true; snapshot(g); break; }
        }
        if (!acted) break;
    }
    return game_done(g) >= 0;
}

// Drama score, mirroring tests/cordite_showcase.ts: a deep escape (a buried
// survivor), a frontrunner collapse, see-saw lead changes, and a photo finish.
static double score_game(const Game *g, int np, int *peaks_out) {
    int fool = game_done(g);
    int peak[MAX_PLAYERS] = {0};
    for (int t = 0; t < g_T; t++)
        for (int s = 0; s < np; s++)
            if (g_hist[t][s] > peak[s]) peak[s] = g_hist[t][s];
    for (int s = 0; s < np; s++) peaks_out[s] = peak[s];

    int escapePeak = 0;
    for (int s = 0; s < np; s++) if (s != fool && peak[s] > escapePeak) escapePeak = peak[s];

    int foolLateMin = 1 << 30;
    for (int t = g_T / 2; t < g_T; t++) {
        int h = g_hist[t][fool];
        if (h > 0 && h < foolLateMin) foolLateMin = h;
    }
    if (foolLateMin == (1 << 30)) foolLateMin = 0;
    int collapse = peak[fool] - foolLateMin;
    if (collapse < 0) collapse = 0;

    int leadChanges = 0, prevLeader = -1;
    for (int t = 0; t < g_T; t++) {
        int lead = 0;
        for (int s = 1; s < np; s++) if (g_hist[t][s] > g_hist[t][lead]) lead = s;
        if (lead != prevLeader && prevLeader != -1) leadChanges++;
        prevLeader = lead;
    }

    int elimSpread = g->num_eliminated;
    double lengthBonus = (g_T < 200 ? g_T : 200) / 10.0;
    return escapePeak * 3.0 + collapse * 1.5 + leadChanges * 2.5
         + elimSpread * 4.0 + lengthBonus;
}

// ----- top-K min-tracked store of finalists -------------------------------
typedef struct {
    Game     g;
    double   score;
    int      peaks[MAX_PLAYERS];
    int      np;
    uint32_t seed;
    bool     used;
} Finalist;

static Finalist g_top[MAX_TOP];
static int      g_k;

static void consider(const Game *g, int np, uint32_t seed, double score, const int *peaks) {
    int slot = -1;
    int n_used = 0;
    int worst = -1;
    for (int i = 0; i < g_k; i++) {
        if (g_top[i].used) {
            n_used++;
            if (worst < 0 || g_top[i].score < g_top[worst].score) worst = i;
        } else if (slot < 0) slot = i;
    }
    if (slot < 0) {
        if (worst >= 0 && score > g_top[worst].score) slot = worst; else return;
    }
    game_clone(&g_top[slot].g, g);
    g_top[slot].score = score;
    g_top[slot].np    = np;
    g_top[slot].seed  = seed;
    memcpy(g_top[slot].peaks, peaks, sizeof(int) * (size_t)np);
    g_top[slot].used  = true;
    (void)n_used;
}

static void emit_pair(const LogPair *p) {
    int ts = p->has_target ? p->target.suit  : -1;
    int tv = p->has_target ? p->target.value : -1;
    printf("[%d,%d,%d,%d]", p->primary.suit, p->primary.value, ts, tv);
}

static void emit_finalist(const Finalist *f) {
    const Game *g = &f->g;
    int fool = game_done(g);
    printf("{\"np\":%d,\"seed\":%u,\"score\":%.1f,\"fool\":%d,\"flip\":[%d,%d],\"peaks\":[",
           f->np, f->seed, f->score, fool, g->flipped.suit, g->flipped.value);
    for (int i = 0; i < f->np; i++) { if (i) printf(","); printf("%d", f->peaks[i]); }
    printf("],\"elim\":[");
    for (int i = 0; i < g->num_eliminated; i++) { if (i) printf(","); printf("%d", g->elimination_order[i]); }
    printf("],\"logs\":[");
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (i) printf(",");
        printf("[%d,%d,%d,[", l->log_type, l->player_idx, l->defender_index);
        for (int k = 0; k < l->num_pairs; k++) { if (k) printf(","); emit_pair(&l->pairs[k]); }
        printf("]]");
    }
    printf("]}\n");
}

static int cmp_desc(const void *a, const void *b) {
    double sa = ((const Finalist *)a)->score, sb = ((const Finalist *)b)->score;
    return sa < sb ? 1 : sa > sb ? -1 : 0;
}

int main(int argc, char **argv) {
    int games      = parse_int(get_arg(argc, argv, "games", "300"), 300);
    uint32_t seed0 = (uint32_t)parse_int(get_arg(argc, argv, "seed", "1"), 1);
    int top        = parse_int(get_arg(argc, argv, "top", "10"), 10);
    const char *pcs = get_arg(argc, argv, "pcs", "4,6");
    const char *strat_name = get_arg(argc, argv, "strategy", "cordite");
    int strat = parse_strategy(strat_name);
    if (strat < 0) { fprintf(stderr, "unknown strategy '%s'\n", strat_name); return 2; }
    if (top < 1) top = 1;
    if (top > MAX_TOP) top = MAX_TOP;
    g_k = top;
    fprintf(stderr, "showcase strategy: %s\n", strat_name);

    setvbuf(stderr, NULL, _IOLBF, 0);

    int pc_list[8]; int n_pc = 0;
    char pbuf[64]; strncpy(pbuf, pcs, sizeof(pbuf) - 1); pbuf[sizeof(pbuf) - 1] = 0;
    for (char *t = strtok(pbuf, ","); t && n_pc < 8; t = strtok(NULL, ",")) {
        int n = atoi(t);
        if (n == 5) { fprintf(stderr, "skipping n=5 (C/TS deck mismatch)\n"); continue; }
        if (n >= 2 && n <= MAX_PLAYERS) pc_list[n_pc++] = n;
    }
    if (n_pc == 0) { fprintf(stderr, "no valid player counts\n"); return 2; }

    int played = 0, stalled = 0;
    double best = -1;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int pi = 0; pi < n_pc; pi++) {
        int np = pc_list[pi];
        for (int gi = 0; gi < games; gi++) {
            uint32_t seed = seed0 + (uint32_t)(pi * 100000 + gi);
            Game g;
            if (!play_all_cordite(&g, np, seed, strat)) { stalled++; continue; }
            int peaks[MAX_PLAYERS];
            double s = score_game(&g, np, peaks);
            consider(&g, np, seed, s, peaks);
            if (s > best) best = s;
            played++;
            if ((gi + 1) % 25 == 0) {
                struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
                double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
                fprintf(stderr, "  [np=%d] %d/%d  %.1fs (%.1f g/s)  best_score=%.1f\n",
                        np, gi + 1, games, dt, played / (dt > 0 ? dt : 1), best);
            }
        }
    }

    // dump finalists, highest score first
    qsort(g_top, (size_t)g_k, sizeof(Finalist), cmp_desc);
    int emitted = 0;
    for (int i = 0; i < g_k; i++) if (g_top[i].used) { emit_finalist(&g_top[i]); emitted++; }
    fprintf(stderr, "played %d games, %d stalled; dumped top %d finalists\n",
            played, stalled, emitted);
    return 0;
}
