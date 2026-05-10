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
    // Number of players already eliminated at the time this move was made.
    // After the game ends we keep only moves where elim_count_at_move == k,
    // where k is this player's own elimination order index. Those moves are
    // valid teacher demos of "from a state with k players out, here's a move
    // that lands you as the next (k+1th) one out". Earlier moves represent
    // failed attempts at a better rank and are dropped.
    int  elim_count_at_move;
    // Smallest hand_count among ACTIVE OTHER players (not us, not OUT) at
    // the moment the sample's state was tokenized. Used by the
    // espresso-killer corpus filter to drop samples where one of the
    // remaining opponents was already nearly empty (close-finish luck).
    int  min_other_in_hand;
    // Strategy that produced this move (so we can post-filter, e.g. keep
    // only moves played by random in a random-vs-espresso corpus).
    int  bot_strategy;
} Sample;

#define MAX_SAMPLES_PER_GAME 512
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
        s->elim_count_at_move = g->num_eliminated; \
        s->bot_strategy = g->players[bot_idx].strategy_key; \
        int _moh = INT32_MAX; \
        for (int _q = 0; _q < g->num_players; _q++) { \
            if (_q == bot_idx) continue; \
            if (g->players[_q].status != PLAYER_STATUS_IN) continue; \
            if (g->players[_q].hand_count < _moh) _moh = g->players[_q].hand_count; \
        } \
        s->min_other_in_hand = (_moh == INT32_MAX) ? 0 : _moh; \
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

    // Canonical card order: sort by (rotated suit, value) so the same final
    // move always emits the same target sequence. Removes label noise from
    // arbitrary hand-order tie-breaks. Cover keeps cards bound to attacks
    // by suit-rotation since the attack value is what determines coverage.
    Card ordered_cards[MAX_MOVE_CARDS];
    Card ordered_attacks[MAX_MOVE_CARDS];
    int n_ord = chosen->n_cards;
    for (int i = 0; i < n_ord; i++) {
        ordered_cards[i] = chosen->cards[i];
        if (chosen->type == MOVE_COVER) ordered_attacks[i] = chosen->attack_cards[i];
    }
    for (int i = 0; i < n_ord; i++) {
        for (int j = i + 1; j < n_ord; j++) {
            int ai = (ordered_cards[i].suit - trump + 4) % 4;
            int aj = (ordered_cards[j].suit - trump + 4) % 4;
            bool swap = (aj < ai) || (aj == ai && ordered_cards[j].value < ordered_cards[i].value);
            if (swap) {
                Card t = ordered_cards[i]; ordered_cards[i] = ordered_cards[j]; ordered_cards[j] = t;
                if (chosen->type == MOVE_COVER) {
                    Card ta = ordered_attacks[i]; ordered_attacks[i] = ordered_attacks[j]; ordered_attacks[j] = ta;
                }
            }
        }
    }

    Card chosen_so_far[MAX_MOVE_CARDS]; int n_chosen = 0;
    for (int i = 0; i < n_ord; i++) {
        Card c = ordered_cards[i];
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
    if (strcmp(s, "hw") == 0 || strcmp(s, "handwritten") == 0) return STRAT_HANDWRITTEN;
    fprintf(stderr, "unknown strategy '%s'\n", s);
    exit(2);
}

static int run_strategy(int strat, const Game *g, int bot_idx, const LegalMoves *moves) {
    if (strat == STRAT_RANDOM) return random_strategy_choose(g, bot_idx, moves, NULL);
    if (strat == STRAT_ESPRESSO) return espresso_strategy_choose(g, bot_idx, moves, NULL);
    if (strat == STRAT_HANDWRITTEN) return handwritten_strategy_choose(g, bot_idx, moves, NULL);
    return -1;
}

// Returns winner index (the lone surviving player) or -1 if no winner.
// Captures every move the winner made; caller writes them out only for
// games we're keeping.
//
// `strats` is an array of length `num_players`; each entry is a STRAT_*
// constant. For 3+ player games espresso isn't usable (it's 1v1-only),
// so the caller must pick STRAT_HANDWRITTEN or STRAT_RANDOM.
static int play_and_capture_n(uint32_t seed, int num_players, const int *strats,
                              SampleBuf *winner_buf, int *out_winner_idx,
                              int *loser_hand_size) {
    game_set_seed(seed ? seed : 1);
    random_strategy_set_seed(seed ? seed : 1);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)num_players;
    for (int i = 0; i < num_players; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)strats[i];
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);

    SampleBuf per_player[MAX_PLAYERS];
    for (int i = 0; i < num_players; i++) per_player[i].n = 0;

    int iters = 0;
    int max_iters = 2000 * num_players;  // longer games at higher player counts
    while (game_done(&g) < 0 && iters++ < max_iters) {
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

    int durak = game_done(&g);
    if (durak < 0) return -1;
    if (g.num_eliminated < 1) return -1;

    // Collect filtered samples across all NON-DURAK players. For each player
    // p eliminated at order index k, keep their moves whose
    // elim_count_at_move == k. Those are the demonstrations of "from a state
    // where k players are out, here's a move that lands you out next".
    // Other moves were aimed at a higher rank that the player failed at —
    // dropping them avoids feeding the model "tried to be 1st but failed".
    //
    // 1v1: only one elimination, k=0, all moves of the lone winner have
    // elim_count_at_move=0, so this collapses to the previous behavior.
    winner_buf->n = 0;
    for (int idx_k = 0; idx_k < g.num_eliminated; idx_k++) {
        int p = g.elimination_order[idx_k];
        const SampleBuf *src = &per_player[p];
        for (int i = 0; i < src->n; i++) {
            if (src->samples[i].elim_count_at_move != idx_k) continue;
            if (winner_buf->n >= MAX_SAMPLES_PER_GAME) break;
            winner_buf->samples[winner_buf->n++] = src->samples[i];
        }
    }
    // out_winner_idx still names the FIRST winner for the caller's logging,
    // even though we now emit moves from every non-durak player.
    *out_winner_idx = g.elimination_order[0];
    if (num_players == 2) {
        *loser_hand_size = g.players[durak].hand_count;
    } else {
        *loser_hand_size = 99;
    }
    return *out_winner_idx;
}

// Back-compat wrapper for 2-player games.
static int play_and_capture(uint32_t seed, int strat0, int strat1,
                            SampleBuf *winner_buf, int *out_winner_idx,
                            int *loser_hand_size) {
    int strats[2] = { strat0, strat1 };
    return play_and_capture_n(seed, 2, strats, winner_buf, out_winner_idx, loser_hand_size);
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
    // Quality filter: drop wins where the loser still had < min_margin cards
    // (close finishes are mostly luck — winner ≠ good move). Default 3.
    int  min_margin = parse_int(get_arg(argc, argv, "min_margin", "3"), 3);
    // Player count for THIS run. 2..MAX_PLAYERS. For 3+ players, only the
    // single-strategy "pairs" form (e.g. "hw-hw") is allowed and ALL seats
    // get the same strategy (since espresso has no N-player path yet).
    int  num_players = parse_int(get_arg(argc, argv, "players", "2"), 2);
    if (num_players < 2 || num_players > MAX_PLAYERS) {
        fprintf(stderr, "players must be in 2..%d\n", MAX_PLAYERS);
        return 2;
    }
    // --append: open existing corpus in append mode and skip writing the
    // header. Lets us build a multi-player-mix corpus across several runs.
    bool append = false;
    {
        const char *a_arg = get_arg(argc, argv, "append", NULL);
        if (a_arg && (a_arg[0] == '1' || a_arg[0] == 't' || a_arg[0] == 'y')) append = true;
    }
    // --verbose=1 prints "seed=N winner=p0|p1 margin=K" for every game,
    // useful for hunting outliers (e.g. random crushing espresso).
    bool verbose = false;
    {
        const char *v_arg = get_arg(argc, argv, "verbose", NULL);
        if (v_arg && (v_arg[0] == '1' || v_arg[0] == 't' || v_arg[0] == 'y')) verbose = true;
    }
    // --only_strategy=NAME: keep only samples produced by players using this
    // strategy (e.g. "rand" to extract just the random player's moves out of
    // a random-vs-espresso game for the espresso-killer corpus).
    int only_strategy = -1;
    {
        const char *o_arg = get_arg(argc, argv, "only_strategy", NULL);
        if (o_arg) only_strategy = strat_id_from_name(o_arg);
    }
    // --min_other_in_hand=N: keep only samples where every active opponent
    // had >= N cards at the moment of the move. Drops "lucky finish"
    // samples where an opponent was already nearly out.
    int min_other_in_hand = parse_int(get_arg(argc, argv, "min_other_in_hand", "0"), 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    FILE *f = fopen(out_path, append ? "ab" : "wb");
    if (!f) { perror(out_path); return 1; }
    if (!append) {
        const char hdr[] = "NCOR";
        fwrite(hdr, 4, 1, f);
        uint32_t version = 1;
        fwrite(&version, sizeof(version), 1, f);
    }

    int n_games = 0, n_wins = 0, n_samples = 0, n_dropped_margin = 0;

    // Each comma-separated entry is a dash-joined seat list, one strategy per
    // seat (e.g. "esp-hw" for 1v1, "esp-hw-hw" for 3 players, "esp-esp-hw-hw"
    // for 4 players). The number of seats must match --players. If the user
    // gives a single strategy ("hw") all seats use it.
    char buf[256];
    strncpy(buf, pairs_str, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *save_pair = NULL;
    for (char *pair = strtok_r(buf, ",", &save_pair); pair; pair = strtok_r(NULL, ",", &save_pair)) {
        int strats[MAX_PLAYERS]; int n_strats = 0;
        char tmp[256];
        strncpy(tmp, pair, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
        char *save_strat = NULL;
        for (char *tok = strtok_r(tmp, "-", &save_strat); tok && n_strats < MAX_PLAYERS;
             tok = strtok_r(NULL, "-", &save_strat)) {
            strats[n_strats++] = strat_id_from_name(tok);
        }
        if (n_strats == 0) continue;
        if (n_strats == 1) {
            // Single strategy → use it for every seat.
            for (int i = 1; i < num_players; i++) strats[i] = strats[0];
            n_strats = num_players;
        }
        if (n_strats != num_players) {
            fprintf(stderr, "skipping '%s': %d strategies for %d players\n",
                    pair, n_strats, num_players);
            continue;
        }

        for (int s = seed_lo; s <= seed_hi; s++) {
            SampleBuf sb; int winner = -1, loser_hand = 0; int w;
            if (num_players == 2) {
                w = play_and_capture((uint32_t)s, strats[0], strats[1], &sb, &winner, &loser_hand);
            } else {
                w = play_and_capture_n((uint32_t)s, num_players, strats,
                                       &sb, &winner, &loser_hand);
            }
            n_games++;
            if (verbose && w >= 0) {
                fprintf(stdout, "seed=%d winner=p%d margin=%d\n", s, winner, loser_hand);
            }
            if (w >= 0 && sb.n > 0) {
                n_wins++;
                if (loser_hand < min_margin) {
                    n_dropped_margin++;
                } else {
                    for (int i = 0; i < sb.n; i++) {
                        const Sample *ss = &sb.samples[i];
                        if (only_strategy >= 0 && ss->bot_strategy != only_strategy) continue;
                        if (ss->min_other_in_hand < min_other_in_hand) continue;
                        write_sample(f, ss);
                        n_samples++;
                    }
                }
            }
            if (n_games % log_every == 0) {
                fprintf(stderr, "# %d games, %d wins, %d kept, %d dropped<%d, %d samples\n",
                        n_games, n_wins, n_wins - n_dropped_margin, n_dropped_margin, min_margin, n_samples);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "# done: %d games, %d wins, %d dropped<%d, %d samples in %s\n",
            n_games, n_wins, n_dropped_margin, min_margin, n_samples, out_path);
    return 0;
}
