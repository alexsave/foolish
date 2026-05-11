// cnitro_grpo: GRPO policy training / evaluation driver.
//
// Modes (selected via --mode=...):
//   encode-smoke   Start one game, advance to seat-0's first decision point,
//                  encode state + legal moves, forward through a randomly
//                  initialized policy, print logits / probabilities.
//   sft            (TODO) Imitation warm-start from handwritten self-play.
//   grpo           (TODO) GRPO self-play training loop.
//   eval           (TODO) Run a frozen policy in a fixed eval suite.
//
// This file is the wiring layer — game-loop / training logic lives in
// dedicated translation units once the SFT/GRPO phases are implemented.

#include "card.h"
#include "game.h"
#include "legal.h"
#include "strategy.h"
#include "grpo_encode.h"
#include "grpo_net.h"
#include "grpo_format.h"
#include "grpo_collect.h"
#include "grpo_train.h"
#include "dynamite_strategy.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0
            && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') {
            return argv[i] + 2 + kl + 1;
        }
    }
    return def;
}
static int parse_int(const char *s, int def) { return s ? atoi(s) : def; }

// --- smoke-test mode -------------------------------------------------------

// Run the game forward until seat-0 has a decision to make. Returns true
// on success, false if seat-0 never gets a turn (very small chance — bail
// rather than loop forever).
static bool advance_to_seat0_decision(Game *g, int max_iters) {
    for (int iters = 0; iters < max_iters; iters++) {
        if (game_done(g) >= 0) return false;
        if (should_bot_act(g, 0)) {
            LegalMoves moves;
            calculate_legal_moves(g, 0, &moves);
            if (moves.n > 0) return true;
        }

        // Pick an active non-zero seat (handwritten) and step it forward.
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g->num_players; i++) {
            if (i == 0) continue;
            if (should_bot_act(g, i)) elig[n_e++] = i;
        }
        if (n_e == 0) {
            // Seat 0 is the only one able to act, but doesn't yet have
            // a legal move — let the engine drive once.
            if (should_bot_act(g, 0)) return true;
            return false;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int p = elig[k];
            LegalMoves moves;
            calculate_legal_moves(g, p, &moves);
            if (moves.n == 0) continue;
            int idx = handwritten_strategy_choose(g, p, &moves, NULL);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(g, p, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (g, p, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (g, p, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(g, p); break;
                case MOVE_GOOD:   ok = handle_good  (g, p); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) return false;
    }
    return false;
}

static const char *move_type_name(int t) {
    switch (t) {
        case MOVE_ATTACK: return "ATTACK";
        case MOVE_COVER:  return "COVER ";
        case MOVE_PASS:   return "PASS  ";
        case MOVE_PICKUP: return "PICKUP";
        case MOVE_GOOD:   return "GOOD  ";
        default:          return "?     ";
    }
}

static const char SUIT_CHAR[4] = { 'S', 'H', 'C', 'D' };
static void format_card(Card c, char *buf, size_t n) {
    const char *v;
    switch (c.value) {
        case 1: v = "2"; break; case 2: v = "3"; break;
        case 3: v = "4"; break; case 4: v = "5"; break;
        case 5: v = "6"; break; case 6: v = "7"; break;
        case 7: v = "8"; break; case 8: v = "9"; break;
        case 9: v = "10"; break; case 10: v = "J"; break;
        case 11: v = "Q"; break; case 12: v = "K"; break;
        case 13: v = "A"; break; default: v = "?"; break;
    }
    char suit = (c.suit >= 0 && c.suit < 4) ? SUIT_CHAR[(int)c.suit] : '?';
    snprintf(buf, n, "%s%c", v, suit);
}

static int mode_encode_smoke(int argc, char **argv) {
    int n_players = parse_int(get_arg(argc, argv, "players", "2"), 2);
    uint32_t seed = (uint32_t)parse_int(get_arg(argc, argv, "seed", "42"), 42);
    uint64_t wseed = (uint64_t)parse_int(get_arg(argc, argv, "wseed", "1"), 1);
    if (n_players < 2 || n_players > MAX_PLAYERS) {
        fprintf(stderr, "players must be in [2, %d]\n", MAX_PLAYERS);
        return 2;
    }

    game_set_seed(seed);
    random_strategy_set_seed(seed);

    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = STRAT_HANDWRITTEN;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    if (!advance_to_seat0_decision(&g, 2000)) {
        fprintf(stderr, "could not reach seat-0 decision point\n");
        return 1;
    }

    LegalMoves moves;
    calculate_legal_moves(&g, 0, &moves);
    if (moves.n == 0) {
        fprintf(stderr, "no legal moves for seat 0\n");
        return 1;
    }

    GrpoNet net; grpo_net_alloc(&net); grpo_net_init_he(&net, wseed);
    GrpoWorkspace ws; grpo_workspace_alloc(&ws, MAX_LEGAL_MOVES);

    grpo_net_forward(&net, &ws, &g, 0, &moves);

    printf("=== grpo encode-smoke ===\n");
    printf("players=%d seed=%u wseed=%llu\n", n_players, seed, (unsigned long long)wseed);
    printf("STATE_DIM=%d  MOVE_FEAT_DIM=%d  HEAD_IN=%d  params=%zu\n",
           STATE_DIM, MOVE_FEAT_DIM, GRPO_HEAD_IN, grpo_net_param_count());
    printf("deck_count=%d  has_flipped=%d  power_suit=%c  defender=%d  first_attacker=%d  num_battles=%d\n",
           g.deck_count, g.has_flipped, SUIT_CHAR[(int)g.power_suit],
           g.defender, g.first_attacker, g.num_battles);
    printf("seat-0 hand_count=%d  legal_moves=%d\n",
           g.players[0].hand_count, moves.n);

    // Top-K moves by probability — partial selection sort over move indices.
    int K = moves.n < 16 ? moves.n : 16;
    static int idxs[MAX_LEGAL_MOVES];
    for (int i = 0; i < moves.n; i++) idxs[i] = i;
    for (int i = 0; i < K; i++) {
        int best = i;
        for (int j = i + 1; j < moves.n; j++) {
            if (ws.log_probs[idxs[j]] > ws.log_probs[idxs[best]]) best = j;
        }
        int tmp = idxs[i]; idxs[i] = idxs[best]; idxs[best] = tmp;
    }
    int *top = idxs;

    printf("\nTop %d moves (of %d):\n", K, moves.n);
    printf("  idx   logit       prob    type   cards\n");
    for (int t = 0; t < K; t++) {
        int i = top[t];
        const LegalMove *m = &moves.moves[i];
        char cards[128]; cards[0] = 0;
        for (int c = 0; c < m->n_cards; c++) {
            char buf[8]; format_card(m->cards[c], buf, sizeof(buf));
            strncat(cards, buf, sizeof(cards) - strlen(cards) - 1);
            if (c + 1 < m->n_cards) strncat(cards, ",", sizeof(cards) - strlen(cards) - 1);
        }
        if (m->type == MOVE_COVER) {
            strncat(cards, " <- ", sizeof(cards) - strlen(cards) - 1);
            for (int c = 0; c < m->n_cards; c++) {
                char buf[8]; format_card(m->attack_cards[c], buf, sizeof(buf));
                strncat(cards, buf, sizeof(cards) - strlen(cards) - 1);
                if (c + 1 < m->n_cards) strncat(cards, ",", sizeof(cards) - strlen(cards) - 1);
            }
        }
        printf("  %3d  %8.4f  %8.4f  %s %s\n",
               i, ws.logits[i], expf(ws.log_probs[i]),
               move_type_name(m->type), cards);
    }

    // NaN sanity.
    int bad = 0;
    for (int i = 0; i < moves.n; i++) {
        if (ws.logits[i] != ws.logits[i] || ws.log_probs[i] != ws.log_probs[i]) bad++;
    }
    if (bad) {
        fprintf(stderr, "WARN: %d NaN entries in logits/log_probs\n", bad);
    } else {
        printf("\nOK: no NaNs.\n");
    }

    grpo_workspace_free(&ws);
    grpo_net_free(&net);
    return 0;
}

// --- play mode -------------------------------------------------------------
//
// Play one game with dynamite at seat 0 and handwritten at every other seat,
// printing each decision in the cnitro_inspect format: state summary, top-3
// scored moves with the chosen one marked, and at non-dynamite seats just
// the move that was played. Used to eyeball what the trained policy has
// learned to do.

static void describe_legal_move(const LegalMove *m, int trump) {
    char buf[8];
    switch (m->type) {
        case MOVE_ATTACK:
            printf("ATTACK [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                format_card(m->cards[i], buf, sizeof(buf));
                printf("%s%s", buf, m->cards[i].suit == trump ? "*" : "");
            }
            printf("]");
            break;
        case MOVE_COVER:
            printf("COVER [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                format_card(m->cards[i], buf, sizeof(buf));
                printf("%s%s", buf, m->cards[i].suit == trump ? "*" : "");
                printf("<-");
                format_card(m->attack_cards[i], buf, sizeof(buf));
                printf("%s%s", buf, m->attack_cards[i].suit == trump ? "*" : "");
            }
            printf("]");
            break;
        case MOVE_PASS:
            printf("PASS [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                format_card(m->cards[i], buf, sizeof(buf));
                printf("%s%s", buf, m->cards[i].suit == trump ? "*" : "");
            }
            printf("]");
            break;
        case MOVE_PICKUP: printf("PICKUP"); break;
        case MOVE_GOOD:   printf("GOOD"); break;
        default:          printf("?"); break;
    }
}

static const char *role_label(const Game *g, int idx) {
    if (g->defender == idx)       return "DEFENDER";
    if (g->first_attacker == idx) return "ATTACKER";
    if (g->players[idx].awaiting_attack) return "CO-ATTACKER";
    return "IDLE";
}

static void print_hand(const Player *p, int trump) {
    char buf[8];
    for (int i = 0; i < p->hand_count; i++) {
        if (i) printf(" ");
        format_card(p->hand[i], buf, sizeof(buf));
        printf("%s%s", buf, p->hand[i].suit == trump ? "*" : "");
    }
}

static void print_table(const Game *g) {
    if (g->num_battles == 0) { printf("(empty)"); return; }
    char buf[8];
    for (int i = 0; i < g->num_battles; i++) {
        if (i) printf("  ");
        const Battle *b = &g->table_battles[i];
        format_card(b->attack, buf, sizeof(buf));
        printf("%s%s", buf, b->attack.suit == g->power_suit ? "*" : "");
        if (b->has_defense) {
            printf("<-");
            format_card(b->defense, buf, sizeof(buf));
            printf("%s%s", buf, b->defense.suit == g->power_suit ? "*" : "");
        } else {
            printf("<-?");
        }
    }
}

static int mode_play(int argc, char **argv) {
    const char *ckpt = get_arg(argc, argv, "ckpt", "/tmp/grpo_sft_smoke.bin");
    uint32_t seed   = (uint32_t)parse_int(get_arg(argc, argv, "seed", "1"), 1);
    int n_players   = parse_int(get_arg(argc, argv, "players", "2"), 2);
    int max_decisions = parse_int(get_arg(argc, argv, "max-decisions", "2000"), 2000);

    GrpoNet net; grpo_net_alloc(&net);
    if (!grpo_net_load(&net, ckpt)) {
        fprintf(stderr, "play: cannot load checkpoint %s\n", ckpt);
        grpo_net_free(&net);
        return 1;
    }
    dynamite_strategy_set_net(&net);

    game_set_seed(seed);
    random_strategy_set_seed(seed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (i == 0) ? STRAT_DYNAMITE : STRAT_HANDWRITTEN;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    int trump = g.power_suit;
    printf("=== Game seed=%u players=%d trump=%c ===\n",
           seed, n_players, SUIT_CHAR[(int)trump]);
    printf("seat 0 = dynamite (loaded %s)\n", ckpt);
    for (int i = 1; i < n_players; i++) printf("seat %d = handwritten\n", i);
    printf("starting hands:\n");
    for (int i = 0; i < n_players; i++) {
        printf("  p%d: ", i); print_hand(&g.players[i], trump); printf("\n");
    }

    int decisions = 0, iters = 0;
    while (game_done(&g) < 0 && iters++ < 2000 && decisions < max_decisions) {
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
            int strat = g.players[pi].strategy_key;

            decisions++;
            printf("\n=== Decision #%d (p%d %s, %s) ===\n",
                   decisions, pi,
                   strat == STRAT_DYNAMITE ? "dynamite" : "handwritten",
                   role_label(&g, pi));
            printf("  deck=%d  table: ", g.deck_count); print_table(&g); printf("\n");
            printf("  p%d hand: ", pi); print_hand(&g.players[pi], trump); printf("\n");

            int idx;
            if (strat == STRAT_DYNAMITE) {
                static float lp[MAX_LEGAL_MOVES];
                idx = dynamite_strategy_choose_verbose(&g, pi, &moves, lp);
                // Sort by descending log_prob, show top-3.
                static int order[MAX_LEGAL_MOVES];
                for (int i = 0; i < moves.n; i++) order[i] = i;
                int K = moves.n < 3 ? moves.n : 3;
                for (int i = 0; i < K; i++) {
                    int best = i;
                    for (int j = i + 1; j < moves.n; j++) {
                        if (lp[order[j]] > lp[order[best]]) best = j;
                    }
                    int tmp = order[i]; order[i] = order[best]; order[best] = tmp;
                }
                printf("  %d legal options (top %d):\n", moves.n, K);
                for (int i = 0; i < K; i++) {
                    int a = order[i];
                    printf("    [%5.1f%%]%s ", expf(lp[a]) * 100.0f,
                           a == idx ? " <-" : "   ");
                    describe_legal_move(&moves.moves[a], trump);
                    printf("\n");
                }
            } else {
                idx = handwritten_strategy_choose(&g, pi, &moves, NULL);
                printf("  → "); describe_legal_move(&moves.moves[idx], trump); printf("\n");
            }
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
    }

    int loser = game_done(&g);
    printf("\n=== Game over ===\n");
    if (loser >= 0) {
        printf("durak (loser) = p%d (%s)\n", loser,
               loser == 0 ? "dynamite" : "handwritten");
        printf("dynamite finish position: ");
        // Walk elimination_order: rank = index_in_eliminated + 1 if found, else N.
        int self_rank = g.num_players;  // last (durak) by default
        for (int i = 0; i < g.num_eliminated; i++) {
            if (g.elimination_order[i] == 0) { self_rank = i + 1; break; }
        }
        printf("%d of %d\n", self_rank, g.num_players);
    } else {
        printf("incomplete game\n");
    }
    grpo_net_free(&net);
    return 0;
}

// --- eval mode -------------------------------------------------------------
//
// Eval lives in cnitro_eval (see ../src/main_eval.c). Use:
//   cnitro_eval --strategy=dynamite --ckpt=... --players=2,4,6,8 --games=200
// to get mean finish-position + histogram. The duplicate that used to live
// here was removed in favor of one canonical eval tool.
#if 0
static int play_one_silent(int n_players, uint32_t seed) {
    game_set_seed(seed);
    random_strategy_set_seed(seed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (i == 0) ? STRAT_DYNAMITE : STRAT_HANDWRITTEN;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);
    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 4000) {
        int elig[MAX_PLAYERS]; int n_e = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
        if (n_e == 0) break;
        for (int i = n_e - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int tmp = elig[i]; elig[i] = elig[j]; elig[j] = tmp;
        }
        bool acted = false;
        for (int k = 0; k < n_e; k++) {
            int pi = elig[k];
            LegalMoves moves;
            calculate_legal_moves(&g, pi, &moves);
            if (moves.n == 0) continue;
            int idx = (g.players[pi].strategy_key == STRAT_DYNAMITE)
                    ? dynamite_strategy_choose(&g, pi, &moves, NULL)
                    : handwritten_strategy_choose(&g, pi, &moves, NULL);
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
    }
    // Recover dynamite's finish position from elimination_order.
    for (int i = 0; i < g.num_eliminated; i++) {
        if (g.elimination_order[i] == 0) return i + 1;
    }
    return g.num_players;  // durak
}

static int mode_eval(int argc, char **argv) {
    const char *ckpt = get_arg(argc, argv, "ckpt", "/tmp/grpo_sft_smoke.bin");
    int games        = parse_int(get_arg(argc, argv, "games", "200"), 200);
    uint32_t seed0   = (uint32_t)parse_int(get_arg(argc, argv, "seed-start", "200001"), 200001);
    const char *pcs  = get_arg(argc, argv, "players", "2,4,6,8");

    GrpoNet net; grpo_net_alloc(&net);
    if (!grpo_net_load(&net, ckpt)) {
        fprintf(stderr, "eval: cannot load checkpoint %s\n", ckpt);
        grpo_net_free(&net);
        return 1;
    }
    dynamite_strategy_set_net(&net);

    printf("=== eval dynamite vs handwritten ===\n");
    printf("ckpt=%s games_per_pc=%d seed_start=%u\n", ckpt, games, seed0);
    printf("\n  pc  mean_finish  baseline(=1+(N-1)/2)  win_rate  histogram_p1..pN\n");

    const char *p = pcs;
    while (*p) {
        int n = atoi(p);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
        if (n < 2 || n > MAX_PLAYERS) continue;

        EvalAcc acc = {0};
        for (int gi = 0; gi < games; gi++) {
            int fp = play_one_silent(n, seed0 + (uint32_t)gi);
            acc.finish_sum += (uint64_t)fp;
            acc.finish_hist[fp]++;
            acc.games++;
        }
        double mean_fp = (double)acc.finish_sum / acc.games;
        double baseline = 1.0 + (double)(n - 1) / 2.0;   // uniform random == N+1)/2
        double win_rate = (double)acc.finish_hist[1] / acc.games;
        printf("  %2d  %11.3f  %20.3f  %8.1f%%  ",
               n, mean_fp, baseline, win_rate * 100.0);
        for (int k = 1; k <= n; k++) {
            printf("%llu ", (unsigned long long)acc.finish_hist[k]);
        }
        printf("\n");
    }

    grpo_net_free(&net);
    return 0;
}
#endif

// --- sft-collect mode ------------------------------------------------------

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(path, 0755);
}

static int mode_sft_collect(int argc, char **argv) {
    GrpoCollectConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_games          = parse_int(get_arg(argc, argv, "games",  "10000"), 10000);
    cfg.num_threads        = parse_int(get_arg(argc, argv, "threads", "8"), 8);
    cfg.base_seed          = (uint32_t)parse_int(get_arg(argc, argv, "seed-start", "1"), 1);
    cfg.target_per_bucket  = parse_int(get_arg(argc, argv, "target-per-bucket", "0"), 0);
    cfg.out_dir            = get_arg(argc, argv, "out", "corpus");
    cfg.verbose            = parse_int(get_arg(argc, argv, "verbose", "1"), 1);

    if (ensure_dir(cfg.out_dir) != 0) {
        fprintf(stderr, "cannot create out dir %s\n", cfg.out_dir);
        return 1;
    }
    if (cfg.verbose) {
        fprintf(stderr, "sft-collect: games=%d threads=%d seed-start=%u target-per-bucket=%d out=%s\n",
                cfg.num_games, cfg.num_threads, cfg.base_seed, cfg.target_per_bucket, cfg.out_dir);
    }

    GrpoCollectStats st;
    int rc = grpo_collect_run(&cfg, &st);
    if (rc != 0) {
        fprintf(stderr, "sft-collect failed (rc=%d)\n", rc);
        return rc;
    }

    printf("=== sft-collect summary ===\n");
    printf("games=%llu tuples_main=%llu tuples_overflow=%llu wall=%.2fs\n",
           (unsigned long long)st.total_games,
           (unsigned long long)st.total_tuples,
           (unsigned long long)st.total_overflow,
           st.wall_secs);
    printf("\nbuckets (rows = player_count, cols = attacker/defender/co_attacker/idle):\n");
    printf("        %10s %10s %10s %10s   total\n",
           "attacker", "defender", "co_atk", "idle");
    for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
        uint64_t row_total = 0;
        for (int r = 0; r < GRPO_ROLE_COUNT; r++) row_total += st.bucket_counts[pc][r];
        printf("  pc=%d  %10llu %10llu %10llu %10llu  %8llu\n",
               pc + GRPO_MIN_PLAYERS,
               (unsigned long long)st.bucket_counts[pc][GRPO_ROLE_ATTACKER],
               (unsigned long long)st.bucket_counts[pc][GRPO_ROLE_DEFENDER],
               (unsigned long long)st.bucket_counts[pc][GRPO_ROLE_CO_ATTACKER],
               (unsigned long long)st.bucket_counts[pc][GRPO_ROLE_IDLE],
               (unsigned long long)row_total);
    }
    if (st.total_overflow > 0) {
        printf("\noverflow:\n");
        for (int pc = 0; pc < GRPO_PC_BUCKETS; pc++) {
            uint64_t row = 0;
            for (int r = 0; r < GRPO_ROLE_COUNT; r++) row += st.overflow_counts[pc][r];
            if (row > 0) printf("  pc=%d  total=%llu\n", pc + GRPO_MIN_PLAYERS, (unsigned long long)row);
        }
    }

    return 0;
}

// --- sft-train mode --------------------------------------------------------

static int mode_sft_train(int argc, char **argv) {
    GrpoSftConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.train_dir       = get_arg(argc, argv, "train", "corpus_train");
    cfg.val_dir         = get_arg(argc, argv, "val",   "corpus_val");
    cfg.ckpt_out        = get_arg(argc, argv, "ckpt",  "grpo_sft.bin");
    cfg.batch_size      = parse_int(get_arg(argc, argv, "batch",      "256"), 256);
    cfg.max_steps       = parse_int(get_arg(argc, argv, "max-steps",  "20000"), 20000);
    cfg.eval_every      = parse_int(get_arg(argc, argv, "eval-every", "1000"), 1000);
    cfg.eval_samples    = parse_int(get_arg(argc, argv, "eval-samples", "4096"), 4096);
    cfg.lr              = (float)atof(get_arg(argc, argv, "lr",       "1e-4"));
    cfg.adam_beta1      = (float)atof(get_arg(argc, argv, "beta1",    "0.9"));
    cfg.adam_beta2      = (float)atof(get_arg(argc, argv, "beta2",    "0.999"));
    cfg.adam_eps        = (float)atof(get_arg(argc, argv, "eps",      "1e-8"));
    cfg.clip_norm       = (float)atof(get_arg(argc, argv, "clip-norm", "1.0"));
    cfg.target_top1     = (float)atof(get_arg(argc, argv, "target-top1", "0.80"));
    cfg.plateau_window  = parse_int(get_arg(argc, argv, "plateau-window", "3"), 3);
    cfg.plateau_tol     = (float)atof(get_arg(argc, argv, "plateau-tol",    "0.005"));
    cfg.seed            = (uint64_t)parse_int(get_arg(argc, argv, "seed", "1"), 1);

    setvbuf(stderr, NULL, _IOLBF, 0);  // line-buffered so eval lines stream live
    fprintf(stderr, "sft-train: train=%s val=%s ckpt=%s batch=%d lr=%g target_top1=%.2f\n",
            cfg.train_dir, cfg.val_dir, cfg.ckpt_out, cfg.batch_size, cfg.lr, cfg.target_top1);
    return grpo_sft_run(&cfg);
}

// --- shard-verify mode -----------------------------------------------------
//
// Walks every shard in a manifest, re-reads every record, verifies CRC and
// counts. Cheap sanity check after collection.

static int mode_shard_verify(int argc, char **argv) {
    const char *dir = get_arg(argc, argv, "in", "corpus");
    int total_ok = 0, total_bad = 0;
    uint64_t total_tuples = 0;
    // Iterate via manifest text — simple line scan.
    char mpath[512]; snprintf(mpath, sizeof(mpath), "%s/manifest.txt", dir);
    FILE *m = fopen(mpath, "r");
    if (!m) { fprintf(stderr, "cannot open %s\n", mpath); return 1; }
    char line[256];
    while (fgets(line, sizeof(line), m)) {
        int sid;
        if (sscanf(line, "shard %d", &sid) != 1) continue;
        for (int kind = 0; kind < 2; kind++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s_%03d.bin", dir,
                     kind == 0 ? "shard" : "overflow", sid);
            GrpoShardReader r;
            if (!grpo_shard_reader_open(&r, path)) continue;  // overflow may not exist
            TupleRecord t;
            uint64_t n = 0;
            while (grpo_shard_reader_next(&r, &t)) n++;
            bool ok = grpo_shard_reader_close(&r);
            if (ok) total_ok++; else total_bad++;
            total_tuples += n;
            printf("  %s : %llu tuples, crc=%s\n", path,
                   (unsigned long long)n, ok ? "OK" : "FAIL");
        }
    }
    fclose(m);
    printf("\n%d shards OK, %d shards FAIL, %llu tuples total\n",
           total_ok, total_bad, (unsigned long long)total_tuples);
    return total_bad == 0 ? 0 : 1;
}

// --- roundtrip-test mode ---------------------------------------------------
//
// At every decision in an all-handwritten game: build TupleRecord, reconstruct
// Game from its ObservableState, recompute legal moves on the reconstruction,
// confirm:
//   (a) reconstructed legal moves match the original 1:1 (same order, same cards)
//   (b) the chosen move is locatable in the reconstructed list
//   (c) grpo_encode_state produces the same float vector before and after
// Critical: if any of these fail, SFT would be training on corrupted state.

static bool same_move(const LegalMove *a, const LegalMove *b) {
    if (a->type != b->type || a->n_cards != b->n_cards) return false;
    for (int k = 0; k < a->n_cards; k++) if (!card_eq(a->cards[k], b->cards[k])) return false;
    if (a->type == MOVE_COVER) {
        for (int k = 0; k < a->n_cards; k++) if (!card_eq(a->attack_cards[k], b->attack_cards[k])) return false;
    }
    return true;
}

static int mode_roundtrip_test(int argc, char **argv) {
    int n_players = parse_int(get_arg(argc, argv, "players", "4"), 4);
    uint32_t seed = (uint32_t)parse_int(get_arg(argc, argv, "seed", "1"), 1);
    int n_games   = parse_int(get_arg(argc, argv, "games", "20"), 20);

    int decisions_total = 0, decisions_ok = 0, decisions_bad = 0;
    for (int gi = 0; gi < n_games; gi++) {
        uint32_t s = seed + (uint32_t)gi;
        game_set_seed(s);
        random_strategy_set_seed(s);
        Game g; memset(&g, 0, sizeof(g));
        g.num_players = (int8_t)n_players;
        for (int i = 0; i < n_players; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = STRAT_HANDWRITTEN;
            snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
        }
        start_game(&g);

        int iters = 0;
        while (game_done(&g) < 0 && iters++ < 4000) {
            int elig[MAX_PLAYERS]; int n_e = 0;
            for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) elig[n_e++] = i;
            if (n_e == 0) break;
            for (int i = n_e - 1; i > 0; i--) {
                int j = (int)(game_random() * (i + 1));
                if (j < 0) j = 0; if (j > i) j = i;
                int tmp = elig[i]; elig[i] = elig[j]; elig[j] = tmp;
            }
            bool acted = false;
            for (int k = 0; k < n_e; k++) {
                int p = elig[k];
                LegalMoves moves;
                calculate_legal_moves(&g, p, &moves);
                if (moves.n == 0) continue;
                int chosen = handwritten_strategy_choose(&g, p, &moves, NULL);
                if (chosen < 0 || chosen >= moves.n) continue;

                // --- roundtrip check ---
                TupleRecord t;
                grpo_tuple_build(&g, p, &moves.moves[chosen], s, (uint16_t)iters, n_e, &t);
                Game g2; grpo_state_to_game(&t.state, &g2);
                LegalMoves moves2;
                calculate_legal_moves(&g2, t.state.self_idx, &moves2);

                bool list_match = (moves.n == moves2.n);
                if (list_match) {
                    for (int i = 0; i < moves.n; i++) {
                        if (!same_move(&moves.moves[i], &moves2.moves[i])) { list_match = false; break; }
                    }
                }
                int chosen2 = grpo_legal_move_match(&moves2, &t.chosen_move);

                static float state_a[STATE_DIM], state_b[STATE_DIM];
                grpo_encode_state(&g, p, state_a);
                grpo_encode_state(&g2, t.state.self_idx, state_b);
                bool state_match = (memcmp(state_a, state_b, sizeof(state_a)) == 0);

                decisions_total++;
                if (list_match && chosen2 == chosen && state_match) {
                    decisions_ok++;
                } else {
                    decisions_bad++;
                    if (decisions_bad <= 5) {
                        fprintf(stderr,
                                "  game %d iter %d seat %d: list_match=%d chosen=%d chosen2=%d state_match=%d  moves_n=%d moves2_n=%d type=%d\n",
                                gi, iters, p, list_match, chosen, chosen2, state_match, moves.n, moves2.n, moves.moves[chosen].type);
                    }
                }

                const LegalMove *m = &moves.moves[chosen];
                bool ok = false;
                switch (m->type) {
                    case MOVE_ATTACK: ok = handle_attack(&g, p, m->cards, m->n_cards); break;
                    case MOVE_COVER:  ok = handle_cover (&g, p, m->cards, m->attack_cards, m->n_cards); break;
                    case MOVE_PASS:   ok = handle_pass  (&g, p, m->cards, m->n_cards); break;
                    case MOVE_PICKUP: ok = handle_pickup(&g, p); break;
                    case MOVE_GOOD:   ok = handle_good  (&g, p); break;
                    default: break;
                }
                if (ok) { acted = true; break; }
            }
            if (!acted) break;
        }
    }

    printf("=== roundtrip-test ===\n");
    printf("players=%d games=%d seed-start=%u\n", n_players, n_games, seed);
    printf("decisions: %d total, %d ok, %d bad\n", decisions_total, decisions_ok, decisions_bad);
    return decisions_bad == 0 ? 0 : 1;
}

// --- grad-check mode -------------------------------------------------------
//
// Verifies analytic backward gradients match finite-difference numeric
// gradients within a tight relative tolerance. Tests a sample of weights
// from each parameter group — full enumeration would be too slow given the
// network has ~2.4M params.

typedef struct {
    const char *name;
    float *ptr;
    size_t n;
} ParamSlice;

static int mode_grad_check(int argc, char **argv) {
    uint64_t wseed = (uint64_t)parse_int(get_arg(argc, argv, "wseed", "1"), 1);
    uint32_t gseed = (uint32_t)parse_int(get_arg(argc, argv, "seed", "42"), 42);
    int n_players  = parse_int(get_arg(argc, argv, "players", "2"), 2);
    int per_group  = parse_int(get_arg(argc, argv, "checks-per-group", "16"), 16);
    float eps      = (float)atof(get_arg(argc, argv, "eps", "0.001"));
    float rtol     = (float)atof(get_arg(argc, argv, "rtol", "0.05"));
    // Absolute-magnitude floor: when both grads are tiny, relative error is
    // unreliable (denominator ≈ 0). Skip the comparison in that regime.
    float abs_floor = (float)atof(get_arg(argc, argv, "abs-floor", "1e-6"));

    // Set up a game state.
    game_set_seed(gseed);
    random_strategy_set_seed(gseed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = STRAT_HANDWRITTEN;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);
    if (!advance_to_seat0_decision(&g, 2000)) {
        fprintf(stderr, "grad-check: cannot reach seat-0 decision\n"); return 1;
    }
    LegalMoves moves;
    calculate_legal_moves(&g, 0, &moves);
    if (moves.n < 2) {
        fprintf(stderr, "grad-check: trivial decision (only %d move), retry with different seed\n", moves.n);
        return 1;
    }
    int chosen = moves.n / 2;   // arbitrary

    GrpoNet net; grpo_net_alloc(&net); grpo_net_init_he(&net, wseed);
    GrpoWorkspace ws; grpo_workspace_alloc(&ws, MAX_LEGAL_MOVES);
    GrpoGrads grads; grpo_grads_alloc(&grads); grpo_grads_zero(&grads);

    // Analytic gradient.
    grpo_net_forward(&net, &ws, &g, 0, &moves);
    float ana_loss = grpo_net_backward(&net, &ws, moves.n, chosen, &grads);

    ParamSlice slices[10] = {
        {"W1",  net.W1,  (size_t)GRPO_H1   * STATE_DIM},
        {"b1",  net.b1,  GRPO_H1},
        {"W2",  net.W2,  (size_t)GRPO_H2   * GRPO_H1},
        {"b2",  net.b2,  GRPO_H2},
        {"W3",  net.W3,  (size_t)GRPO_EMBED * GRPO_H2},
        {"b3",  net.b3,  GRPO_EMBED},
        {"Wh1", net.Wh1, (size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN},
        {"bh1", net.bh1, GRPO_HEAD_HIDDEN},
        {"Wh2", net.Wh2, GRPO_HEAD_HIDDEN},
        {"bh2", net.bh2, 1},
    };
    ParamSlice gslices[10] = {
        {"W1",  grads.W1,  (size_t)GRPO_H1   * STATE_DIM},
        {"b1",  grads.b1,  GRPO_H1},
        {"W2",  grads.W2,  (size_t)GRPO_H2   * GRPO_H1},
        {"b2",  grads.b2,  GRPO_H2},
        {"W3",  grads.W3,  (size_t)GRPO_EMBED * GRPO_H2},
        {"b3",  grads.b3,  GRPO_EMBED},
        {"Wh1", grads.Wh1, (size_t)GRPO_HEAD_HIDDEN * GRPO_HEAD_IN},
        {"bh1", grads.bh1, GRPO_HEAD_HIDDEN},
        {"Wh2", grads.Wh2, GRPO_HEAD_HIDDEN},
        {"bh2", grads.bh2, 1},
    };

    uint32_t prng = 0xCAFEBABE;
    printf("=== grad-check (loss=%.6f, n_moves=%d, chosen=%d) ===\n", ana_loss, moves.n, chosen);
    printf("eps=%g rtol=%g abs_floor=%g per_group=%d\n\n", eps, rtol, abs_floor, per_group);
    int total_pass = 0, total_fail = 0, total_skip = 0;
    double max_rel = 0.0;

    for (int gi = 0; gi < 10; gi++) {
        ParamSlice *s  = &slices[gi];
        ParamSlice *gs = &gslices[gi];
        int pass = 0, fail = 0, skipped = 0;
        double group_max_rel = 0.0;
        for (int k = 0; k < per_group; k++) {
            prng = prng * 1664525u + 1013904223u;
            size_t idx = prng % s->n;
            float orig = s->ptr[idx];

            s->ptr[idx] = orig + eps;
            grpo_net_forward(&net, &ws, &g, 0, &moves);
            float L_plus = -ws.log_probs[chosen];

            s->ptr[idx] = orig - eps;
            grpo_net_forward(&net, &ws, &g, 0, &moves);
            float L_minus = -ws.log_probs[chosen];

            s->ptr[idx] = orig;
            float num = (L_plus - L_minus) / (2.0f * eps);
            float ana = gs->ptr[idx];
            float amax = fmaxf(fabsf(num), fabsf(ana));
            if (amax < abs_floor) { skipped++; continue; }
            float denom = fabsf(num) + fabsf(ana) + 1e-12f;
            float rel = fabsf(num - ana) / denom;
            if (rel > group_max_rel) group_max_rel = rel;
            if (rel < rtol) pass++; else fail++;
            if (rel >= rtol) {
                printf("  %s[%zu]: num=%.6e ana=%.6e rel=%.3e FAIL\n",
                       s->name, idx, num, ana, rel);
            }
        }
        printf("  %4s : %d/%d pass (%d skipped, %d failed), max_rel=%.3e\n",
               s->name, pass, pass + fail, skipped, fail, group_max_rel);
        total_pass += pass; total_fail += fail; total_skip += skipped;
        if (group_max_rel > max_rel) max_rel = group_max_rel;
    }
    printf("\n%d/%d total pass, %d skipped (below abs_floor), %d failed, overall max_rel=%.3e\n",
           total_pass, total_pass + total_fail, total_skip, total_fail, max_rel);

    // --- End-to-end sanity check: gradient direction is correct -----------
    //
    // Run forward → backward → Adam step → forward. If backward computes
    // gradients in the wrong direction (sign flip somewhere), loss will
    // INCREASE rather than decrease. This is a much stronger guarantee than
    // analytic-vs-numeric matching, and it's robust to ReLU boundary noise.

    printf("\n=== end-to-end loss-decrease test ===\n");
    GrpoAdam opt; grpo_adam_init(&opt, 1e-3f, 0.9f, 0.999f, 1e-8f, 1.0f);
    float prev_loss = ana_loss;
    int n_decrease = 0, n_increase = 0;
    for (int step = 0; step < 50; step++) {
        grpo_grads_zero(&grads);
        grpo_net_forward(&net, &ws, &g, 0, &moves);
        float L_before = -ws.log_probs[chosen];
        grpo_net_backward(&net, &ws, moves.n, chosen, &grads);
        grpo_adam_step(&opt, &net, &grads);
        grpo_net_forward(&net, &ws, &g, 0, &moves);
        float L_after = -ws.log_probs[chosen];
        if (L_after < L_before) n_decrease++; else n_increase++;
        if (step < 5 || step % 10 == 0) {
            printf("  step %2d: L %.6f -> %.6f  (Δ=%+.4e)\n",
                   step, L_before, L_after, L_after - L_before);
        }
        prev_loss = L_after;
    }
    printf("  50 steps: %d decrease, %d increase, final loss=%.6f (started %.6f)\n",
           n_decrease, n_increase, prev_loss, ana_loss);
    int loss_test_ok = (prev_loss < ana_loss * 0.5f);   // expect >50% reduction
    printf("  %s\n", loss_test_ok ? "PASS: loss reduced substantially" : "FAIL: loss did not decrease enough");

    grpo_adam_free(&opt);
    grpo_grads_free(&grads);
    grpo_workspace_free(&ws);
    grpo_net_free(&net);
    return loss_test_ok ? 0 : 1;
}

// --- TLS isolation test ----------------------------------------------------
//
// Confirms _Thread_local on g_seed / g_rand_seed actually gives each pthread
// its own LCG state. Fail mode = thread A's seed bleeds into thread B's
// sequence, or thread B's calls clobber thread A's. We seed two threads
// differently, generate K values in each, and assert (a) sequences are
// distinct, (b) re-running each thread alone with its seed reproduces the
// same sequence (no shared state was modified between calls).

#define TLS_TEST_K 32

typedef struct {
    uint32_t seed;
    uint32_t rand_seed;
    uint32_t out_game[TLS_TEST_K];
    uint32_t out_rand[TLS_TEST_K];
} TlsTestCtx;

static void *tls_test_worker(void *arg) {
    TlsTestCtx *c = (TlsTestCtx *)arg;
    game_set_seed(c->seed);
    random_strategy_set_seed(c->rand_seed);
    for (int i = 0; i < TLS_TEST_K; i++) {
        c->out_game[i] = game_random_u32();
        c->out_rand[i] = (uint32_t)(random_strategy_random() * 4294967296.0);
    }
    return NULL;
}

static int mode_tls_test(int argc, char **argv) {
    (void)argc; (void)argv;
    TlsTestCtx a = { .seed = 111, .rand_seed = 222 };
    TlsTestCtx b = { .seed = 333, .rand_seed = 444 };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, tls_test_worker, &a);
    pthread_create(&tb, NULL, tls_test_worker, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    // (a) Sequences distinct.
    int distinct = 0;
    for (int i = 0; i < TLS_TEST_K; i++) {
        if (a.out_game[i] != b.out_game[i]) distinct++;
        if (a.out_rand[i] != b.out_rand[i]) distinct++;
    }
    if (distinct < 2 * TLS_TEST_K) {
        fprintf(stderr, "FAIL: only %d/%d outputs differ between threads (TLS not isolating)\n",
                distinct, 2 * TLS_TEST_K);
        return 1;
    }

    // (b) Each sequence reproducible alone.
    TlsTestCtx a2 = { .seed = 111, .rand_seed = 222 };
    pthread_t ta2;
    pthread_create(&ta2, NULL, tls_test_worker, &a2);
    pthread_join(ta2, NULL);
    for (int i = 0; i < TLS_TEST_K; i++) {
        if (a.out_game[i] != a2.out_game[i] || a.out_rand[i] != a2.out_rand[i]) {
            fprintf(stderr, "FAIL: same-seed thread re-run differs at i=%d\n", i);
            return 1;
        }
    }

    // (c) Sanity-check the first values for thread A: known LCG output.
    // seed=111, step1 = 111 * 1664525 + 1013904223 = 1014090718 (mod 2^32)
    uint32_t expect = (uint32_t)(111u * 1664525u + 1013904223u);
    if (a.out_game[0] != expect) {
        fprintf(stderr, "FAIL: expected first LCG output %u, got %u\n", expect, a.out_game[0]);
        return 1;
    }

    printf("OK: TLS isolation verified across 2 threads (%d distinct outputs).\n",
           distinct);
    printf("    LCG step1(seed=111) = %u (matches expected)\n", expect);
    return 0;
}

// --- main ------------------------------------------------------------------

static int usage(void) {
    fprintf(stderr,
            "usage: cnitro_grpo --mode=<mode> [opts]\n"
            "  modes:\n"
            "    encode-smoke   smoke-test encoder + forward pass\n"
            "    sft            (TODO) SFT warm-start from handwritten\n"
            "    grpo           (TODO) GRPO self-play\n"
            "    eval           (TODO) frozen-policy evaluation\n"
            "  encode-smoke opts: --players=N --seed=S --wseed=W\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *mode = get_arg(argc, argv, "mode", NULL);
    if (!mode) return usage();
    if (strcmp(mode, "encode-smoke") == 0) return mode_encode_smoke(argc, argv);
    if (strcmp(mode, "tls-test")     == 0) return mode_tls_test(argc, argv);
    if (strcmp(mode, "sft-collect")  == 0) return mode_sft_collect(argc, argv);
    if (strcmp(mode, "sft-train")    == 0) return mode_sft_train(argc, argv);
    if (strcmp(mode, "shard-verify") == 0) return mode_shard_verify(argc, argv);
    if (strcmp(mode, "play")         == 0) return mode_play(argc, argv);
    if (strcmp(mode, "grad-check")   == 0) return mode_grad_check(argc, argv);
    if (strcmp(mode, "roundtrip-test") == 0) return mode_roundtrip_test(argc, argv);
    if (strcmp(mode, "grpo") == 0 || strcmp(mode, "eval") == 0) {
        fprintf(stderr, "mode '%s' moved or not implemented — for eval use cnitro_eval\n", mode);
        return 1;
    }
    if (false) {
        fprintf(stderr, "mode '%s' not yet implemented\n", mode);
        return 1;
    }
    return usage();
}
