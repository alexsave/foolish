// Self-play data collector. For each (seed, pair) we play a game between
// two strategies and, if a player wins, decompose every move that winner
// made into atomic-action samples (matching nitro_collect.ts) and append
// them to a binary file.
//
// File format (all little-endian):
//   magic[4]="NCOR" version=1
//   repeated:
//     uint16 n_tokens
//     uint16 target_action
//     uint8  n_legal
//     int32  tokens[n_tokens]
//     uint8  legal[n_legal]

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/tokenize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static int parse_int(const char *s, int def) { return s ? atoi(s) : def; }
static const char *get_arg(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0 && strncmp(argv[i] + 2, key, kl) == 0
            && argv[i][2 + kl] == '=') return argv[i] + 2 + kl + 1;
    }
    return def;
}

typedef struct {
    int  tokens[MAX_SEQ_LEN]; int n_tokens;
    int  target;
    int  legal[NUM_ACTIONS]; int n_legal;
} Sample;

#define MAX_SAMPLES_PER_GAME 256
typedef struct {
    Sample samples[MAX_SAMPLES_PER_GAME];
    int    n;
} SampleBuf;

// Decompose a chosen LegalMove into a sequence of (state, atomic_action,
// legal_set) samples. Mirrors decomposeMove in nitro_collect.ts.
static void decompose_move(const Game *g, int bot_idx,
                           const LegalMove *chosen,
                           SampleBuf *out) {
    int trump = g->power_suit;
    bool is_def = (bot_idx == g->defender);

    // Helper: legal mask "now" (matches TS legalMaskNow).
    #define EMIT_SAMPLE(role_, n_chosen_, chosen_arr_, target_) do { \
        if (out->n >= MAX_SAMPLES_PER_GAME) break; \
        Sample *s = &out->samples[out->n++]; \
        InProgress ip; ip.role = (role_); ip.n_cards_chosen = (n_chosen_); \
        for (int _i = 0; _i < (n_chosen_); _i++) ip.cards_chosen[_i] = (chosen_arr_)[_i]; \
        Tokenized t; tokenize(g, bot_idx, &ip, &t); \
        s->n_tokens = t.n_tokens; \
        for (int _i = 0; _i < t.n_tokens; _i++) s->tokens[_i] = t.tokens[_i]; \
        s->target = (target_); \
        s->n_legal = 0; \
        const Player *me = &g->players[bot_idx]; \
        for (int _i = 0; _i < me->hand_count; _i++) { \
            Card c = me->hand[_i]; \
            bool used = false; \
            for (int _j = 0; _j < (n_chosen_); _j++) if (card_eq((chosen_arr_)[_j], c)) { used = true; break; } \
            if (used) continue; \
            s->legal[s->n_legal++] = card_action_id(c.suit, c.value, trump); \
        } \
        if (is_def && (n_chosen_) == 0 && chosen->type != MOVE_PASS) s->legal[s->n_legal++] = ACTION_PICKUP; \
        s->legal[s->n_legal++] = ACTION_STOP; \
    } while(0)

    if (chosen->type == MOVE_PICKUP) {
        Card empty[1];
        EMIT_SAMPLE(INPROG_IDLE, 0, empty, ACTION_PICKUP);
        return;
    }
    if (chosen->type == MOVE_GOOD) {
        Card empty[1];
        EMIT_SAMPLE(INPROG_IDLE, 0, empty, ACTION_STOP);
        return;
    }
    int role = chosen->type == MOVE_ATTACK ? INPROG_ATTACK
             : chosen->type == MOVE_COVER  ? INPROG_COVER
             : chosen->type == MOVE_PASS   ? INPROG_PASS : INPROG_IDLE;
    Card chosen_so_far[MAX_MOVE_CARDS]; int n_chosen = 0;
    for (int i = 0; i < chosen->n_cards; i++) {
        Card c = chosen->cards[i];
        int target = card_action_id(c.suit, c.value, trump);
        EMIT_SAMPLE(role, n_chosen, chosen_so_far, target);
        chosen_so_far[n_chosen++] = c;
    }
    EMIT_SAMPLE(role, n_chosen, chosen_so_far, ACTION_STOP);
    #undef EMIT_SAMPLE
}

static int strat_id_from_name(const char *s) {
    if (strcmp(s, "esp") == 0 || strcmp(s, "espresso") == 0) return STRAT_ESPRESSO;
    if (strcmp(s, "rand") == 0 || strcmp(s, "random") == 0) return STRAT_RANDOM;
    fprintf(stderr, "unknown strategy '%s'\n", s);
    exit(2);
}

static int run_strategy(int strat, const Game *g, int bot_idx, const LegalMoves *moves) {
    if (strat == STRAT_RANDOM) return random_strategy_choose(g, bot_idx, moves, NULL);
    if (strat == STRAT_ESPRESSO) return espresso_strategy_choose(g, bot_idx, moves, NULL);
    return -1;
}

// Returns winner index (0/1) or -1 if no winner. Captures every move the
// winner made; the caller writes them out only for winning games.
static int play_and_capture(uint32_t seed, int strat0, int strat1,
                            SampleBuf *winner_buf, int *out_winner_idx) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = 2;
    g.players[0].status = PLAYER_STATUS_READY;
    g.players[1].status = PLAYER_STATUS_READY;
    g.players[0].strategy_key = (int8_t)strat0;
    g.players[1].strategy_key = (int8_t)strat1;
    snprintf(g.players[0].player_id, sizeof(g.players[0].player_id), "p0");
    snprintf(g.players[1].player_id, sizeof(g.players[1].player_id), "p1");
    start_game(&g);

    SampleBuf per_player[2] = { {{0},0}, {{0},0} };

    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 2000) {
        int elig[2]; int n_e = 0;
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
            int strat = g.players[p].strategy_key;
            int idx = run_strategy(strat, &g, p, &moves);
            if (idx < 0 || idx >= moves.n) continue;
            const LegalMove *m = &moves.moves[idx];

            // Snapshot the BEFORE state for sample emission.
            Game before; game_clone(&before, &g);
            decompose_move(&before, p, m, &per_player[p]);

            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, p, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (&g, p, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (&g, p, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, p); break;
                case MOVE_GOOD:   ok = handle_good  (&g, p); break;
                default: break;
            }
            if (!ok) continue;
            acted = true;
            break;
        }
        if (!acted) break;
    }

    int loser = game_done(&g);
    if (loser < 0) return -1;
    int winner = loser == 0 ? 1 : 0;
    *out_winner_idx = winner;
    *winner_buf = per_player[winner];
    return winner;
}

static void write_sample(FILE *f, const Sample *s) {
    uint16_t nt = (uint16_t)s->n_tokens;
    uint16_t tg = (uint16_t)s->target;
    uint8_t  nl = (uint8_t)s->n_legal;
    fwrite(&nt, sizeof(nt), 1, f);
    fwrite(&tg, sizeof(tg), 1, f);
    fwrite(&nl, sizeof(nl), 1, f);
    for (int i = 0; i < s->n_tokens; i++) {
        int32_t t = s->tokens[i];
        fwrite(&t, sizeof(t), 1, f);
    }
    for (int i = 0; i < s->n_legal; i++) {
        uint8_t a = (uint8_t)s->legal[i];
        fwrite(&a, sizeof(a), 1, f);
    }
}

int main(int argc, char **argv) {
    int  seed_lo = parse_int(get_arg(argc, argv, "from", "1"), 1);
    int  seed_hi = parse_int(get_arg(argc, argv, "to", "2000"), 2000);
    const char *pairs_str = get_arg(argc, argv, "pairs", "esp-rand");
    const char *out_path  = get_arg(argc, argv, "out", "/tmp/cnitro_corpus.bin");
    int  log_every = parse_int(get_arg(argc, argv, "log_every", "100"), 100);
    setvbuf(stderr, NULL, _IOLBF, 0);

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); return 1; }
    const char hdr[] = "NCOR";
    fwrite(hdr, 4, 1, f);
    uint32_t version = 1;
    fwrite(&version, sizeof(version), 1, f);

    int n_games = 0, n_wins = 0, n_samples = 0;

    // Tokenize the pairs string into individual "a-b" entries.
    char buf[256];
    strncpy(buf, pairs_str, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *save_pair = NULL;
    for (char *pair = strtok_r(buf, ",", &save_pair); pair; pair = strtok_r(NULL, ",", &save_pair)) {
        char a[32], b[32];
        const char *dash = strchr(pair, '-');
        if (!dash) continue;
        size_t alen = (size_t)(dash - pair);
        if (alen >= sizeof(a)) alen = sizeof(a) - 1;
        memcpy(a, pair, alen); a[alen] = 0;
        strncpy(b, dash + 1, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        int s0 = strat_id_from_name(a);
        int s1 = strat_id_from_name(b);

        for (int s = seed_lo; s <= seed_hi; s++) {
            SampleBuf sb; int winner = -1;
            int w = play_and_capture((uint32_t)s, s0, s1, &sb, &winner);
            n_games++;
            if (w >= 0 && sb.n > 0) {
                n_wins++;
                for (int i = 0; i < sb.n; i++) { write_sample(f, &sb.samples[i]); n_samples++; }
            }
            if (n_games % log_every == 0) {
                fprintf(stderr, "# %d games, %d wins, %d samples\n", n_games, n_wins, n_samples);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "# done: %d games, %d wins, %d samples in %s\n",
            n_games, n_wins, n_samples, out_path);
    return 0;
}
