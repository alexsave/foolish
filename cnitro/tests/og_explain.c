// og_explain — reproduce ONE seed-dealt 2-player octogen-vs-octogen game and
// (with OG_EXPLAIN set) dump octogen's per-decision deliberation.
//
// Build (see the comment block below / the ad-hoc cc line in the task):
//   cc -O2 -Isrc <core-srcs> tests/og_explain.c -o build/og_explain -lm
//
// Usage:
//   OG_EXPLAIN=/tmp/delib.jsonl \
//   CD_BUDGET=prod CD_RACE=1 CD_RACE_C=75 \
//   ./build/og_explain <64-hex-char-deal-seed>
//
// Prints the reproduced PUBLIC log to stdout as JSON lines (one per GameLog),
// with raw {suit,value} cards, so a verifier can diff it against the recorded
// replay. octogen's deliberation records go to the OG_EXPLAIN sink.

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cordite_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int hexnib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *LOG_NAME[] = {
    "game_start","attack","cover","pass","pickup","good",
    "discard","defender_change","player_out","draw"
};

static void dump_logs(const Game *g) {
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        const char *t = (l->log_type >= 0 && l->log_type <= 9)
                      ? LOG_NAME[l->log_type] : "?";
        printf("{\"i\":%d,\"t\":\"%s\",\"seat\":%d,\"def\":%d,\"cards\":[",
               i, t, l->player_idx, l->defender_index);
        for (int k = 0; k < l->num_pairs; k++) {
            Card p = l->pairs[k].primary, tg = l->pairs[k].target;
            printf("%s{\"p\":{\"s\":%d,\"v\":%d}", k ? "," : "", p.suit, p.value);
            if (tg.suit == -2 && tg.value == -2)  // CARD_NONE
                printf(",\"tg\":null}");
            else
                printf(",\"tg\":{\"s\":%d,\"v\":%d}}", tg.suit, tg.value);
        }
        printf("]}\n");
    }
}

// ---------- driven-replay support (fallback when self-play diverges) -------
// A recorded player action, parsed from the moves file.
typedef struct {
    char type[12];       // attack|cover|pass|pickup|good
    int  seat;
    int  n;              // number of cards / pairs
    Card prim[16];
    Card targ[16];       // cover only
} RecMove;

static int parse_card(const char *s, Card *c) {
    int su, va;
    if (sscanf(s, "%d,%d", &su, &va) != 2) return 0;
    c->suit = (int8_t)su; c->value = (int8_t)va; return 1;
}

// Parse one moves-file line into a RecMove. Returns 1 on success.
static int parse_recmove(char *line, RecMove *m) {
    memset(m, 0, sizeof *m);
    char *tok = strtok(line, " \t\n");
    if (!tok) return 0;
    snprintf(m->type, sizeof m->type, "%s", tok);
    tok = strtok(NULL, " \t\n");
    if (!tok) return 0;
    m->seat = atoi(tok);
    while ((tok = strtok(NULL, " \t\n"))) {
        char *colon = strchr(tok, ':');
        if (colon) {   // cover pair prim:targ
            *colon = 0;
            if (!parse_card(tok, &m->prim[m->n])) return 0;
            if (!parse_card(colon + 1, &m->targ[m->n])) return 0;
        } else {
            if (!parse_card(tok, &m->prim[m->n])) return 0;
        }
        m->n++;
    }
    return 1;
}

static int cards_match_set(const Card *a, int na, const Card *b, int nb) {
    if (na != nb) return 0;
    bool used[16] = {0};
    for (int i = 0; i < na; i++) {
        int f = -1;
        for (int j = 0; j < nb; j++)
            if (!used[j] && a[i].suit == b[j].suit && a[i].value == b[j].value) { f = j; break; }
        if (f < 0) return 0;
        used[f] = true;
    }
    return 1;
}

static int mtype_of(const char *t) {
    if (!strcmp(t, "attack")) return MOVE_ATTACK;
    if (!strcmp(t, "cover"))  return MOVE_COVER;
    if (!strcmp(t, "pass"))   return MOVE_PASS;
    if (!strcmp(t, "pickup")) return MOVE_PICKUP;
    if (!strcmp(t, "good"))   return MOVE_GOOD;
    return -1;
}

// Find the legal-move index matching a recorded move; -1 if none.
static int match_legal(const LegalMoves *moves, const RecMove *m) {
    int mt = mtype_of(m->type);
    for (int i = 0; i < moves->n; i++) {
        const LegalMove *lm = &moves->moves[i];
        if (lm->type != mt) continue;
        if (mt == MOVE_PICKUP || mt == MOVE_GOOD) return i;
        if (lm->n_cards != m->n) continue;
        if (!cards_match_set(lm->cards, lm->n_cards, m->prim, m->n)) continue;
        if (mt == MOVE_COVER &&
            !cards_match_set(lm->attack_cards, lm->n_cards, m->targ, m->n)) continue;
        return i;
    }
    return -1;
}

// Parse "TAG s,v s,v ..." into a Card array; returns count.
static int parse_card_line(char *line, Card *out, int cap) {
    int n = 0;
    strtok(line, " \t\n");   // skip tag
    char *tok;
    while ((tok = strtok(NULL, " \t\n")) && n < cap) {
        int su, va; if (sscanf(tok, "%d,%d", &su, &va) != 2) break;
        out[n].suit = (int8_t)su; out[n].value = (int8_t)va; n++;
    }
    return n;
}

// Overwrite the dealt state with a RECONSTRUCTED deal (from deal.txt) so the
// true recorded game — whose deck the seed does not reproduce — can be driven
// with faithful hands. start_game has already logged game_start / set players
// IN / deterministic_deck=true; we just replace the card state.
static void inject_deal(Game *g, const char *dealfile) {
    FILE *f = fopen(dealfile, "r");
    if (!f) { fprintf(stderr, "cannot open deal %s\n", dealfile); return; }
    char line[1024];
    Card p0[16], p1[16], flip[4], deck[64];
    int n0 = 0, n1 = 0, nf = 0, nd = 0;
    while (fgets(line, sizeof line, f)) {
        char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", line);
        if (!strncmp(line, "P0", 2)) n0 = parse_card_line(tmp, p0, 16);
        else if (!strncmp(line, "P1", 2)) n1 = parse_card_line(tmp, p1, 16);
        else if (!strncmp(line, "FLIP", 4)) nf = parse_card_line(tmp, flip, 4);
        else if (!strncmp(line, "DECK", 4)) nd = parse_card_line(tmp, deck, 64);
    }
    fclose(f);
    g->players[0].hand_count = (int8_t)n0;
    for (int i = 0; i < n0; i++) g->players[0].hand[i] = p0[i];
    g->players[1].hand_count = (int8_t)n1;
    for (int i = 0; i < n1; i++) g->players[1].hand[i] = p1[i];
    for (int i = 0; i < nd; i++) g->deck[i] = deck[i];
    g->deck_count = (int16_t)nd;
    g->flipped = flip[0];
    g->has_flipped = true;
    g->power_suit = flip[0].suit;
    g->deterministic_deck = true;   // draws pop deck[0]
    g->num_battles = 0;
    // First attacker = lowest-trump holder = p1 (recorded); defender = p0.
    g->first_attacker = 1;
    g->defender = 0;
    fprintf(stderr, "INJECTED deal: p0=%d p1=%d deck=%d flip=s%dv%d trump=%d\n",
            n0, n1, nd, flip[0].suit, flip[0].value, g->power_suit);
}

static int driven_replay(uint8_t *seed, const char *movesfile, const char *dealfile) {
    cd_sim_solve_reset();
    game_set_seed(1);
    game_set_deal_seed_bytes(seed, 32);
    Game g; memset(&g, 0, sizeof g);
    g.num_players = 2;
    for (int i = 0; i < 2; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)STRAT_OCTOGEN;
        snprintf(g.players[i].player_id, sizeof g.players[i].player_id, "p%d", i);
    }
    start_game(&g);
    if (dealfile) inject_deal(&g, dealfile);
    fprintf(stderr, "DRIVEN: trump=suit%d(flip s%d v%d) first_attacker=p%d deck=%d\n",
            g.power_suit, g.flipped.suit, g.flipped.value, g.first_attacker, g.deck_count);
    int dbg = getenv("OG_DBG") != NULL;
    if (dbg) {
        for (int p = 0; p < 2; p++) {
            fprintf(stderr, "  p%d init hand:", p);
            for (int j = 0; j < g.players[p].hand_count; j++)
                fprintf(stderr, " s%dv%d", g.players[p].hand[j].suit, g.players[p].hand[j].value);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "  deck(top->bottom next-draw-first):");
        for (int j = g.deck_count - 1; j >= 0; j--)
            fprintf(stderr, " s%dv%d", g.deck[j].suit, g.deck[j].value);
        fprintf(stderr, "\n");
    }

    FILE *f = fopen(movesfile, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", movesfile); return 2; }
    char line[512];
    int p1_dec = 0, p1_match = 0, p1_forced = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\n' || line[0] == 0) continue;
        RecMove m;
        char tmp[512]; snprintf(tmp, sizeof tmp, "%s", line);
        if (!parse_recmove(tmp, &m)) { fprintf(stderr, "parse fail: %s", line); continue; }

        // At each octogen (p1) decision, query octogen so it dumps deliberation,
        // then compare its pick to the recorded move.
        if (m.seat == 1) {
            LegalMoves moves;
            calculate_legal_moves(&g, 1, &moves);
            int rec_idx = match_legal(&moves, &m);
            int og_idx = octogen_strategy_choose(&g, 1, &moves, NULL);
            p1_dec++;
            if (moves.n <= 1) p1_forced++;
            int matched = (og_idx == rec_idx && rec_idx >= 0);
            if (matched) p1_match++;
            fprintf(stderr, "  P1 dec ply=%d nlegal=%d rec_idx=%d og_idx=%d %s%s\n",
                    g.num_logs, moves.n, rec_idx, og_idx,
                    matched ? "MATCH" : "MISMATCH",
                    moves.n <= 1 ? " (forced)" : "");
        }

        int mt = mtype_of(m.type);
        bool ok = false;
        switch (mt) {
            case MOVE_ATTACK: ok = handle_attack(&g, m.seat, m.prim, m.n); break;
            case MOVE_COVER:  ok = handle_cover (&g, m.seat, m.prim, m.targ, m.n); break;
            case MOVE_PASS:   ok = handle_pass  (&g, m.seat, m.prim, m.n); break;
            case MOVE_PICKUP: ok = handle_pickup(&g, m.seat); break;
            case MOVE_GOOD:   ok = handle_good  (&g, m.seat); break;
            default: break;
        }
        if (!ok) {
            fprintf(stderr, "  APPLY FAILED seat=%d %s (reject=%d) logs=%d\n",
                    m.seat, m.type, engine_last_reject, g.num_logs);
            for (int p = 0; p < 2; p++) {
                fprintf(stderr, "    p%d hand:", p);
                for (int j = 0; j < g.players[p].hand_count; j++)
                    fprintf(stderr, " s%dv%d", g.players[p].hand[j].suit, g.players[p].hand[j].value);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "    deck_count=%d has_flip=%d defender=%d nbattles=%d\n",
                    g.deck_count, g.has_flipped, g.defender, g.num_battles);
            break;   // stop at first divergence
        }
    }
    fclose(f);
    fprintf(stderr, "DRIVEN done: num_logs=%d elim=[", g.num_logs);
    for (int i = 0; i < g.num_eliminated; i++) fprintf(stderr, "%d ", g.elimination_order[i]);
    fprintf(stderr, "]  p1_decisions=%d matched=%d forced=%d\n",
            p1_dec, p1_match, p1_forced);
    dump_logs(&g);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <64-hex-deal-seed> [moves.txt]\n", argv[0]); return 2; }
    const char *hex = argv[1];
    if (strlen(hex) < 64) { fprintf(stderr, "seed must be >= 64 hex chars\n"); return 2; }
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) {
        int hi = hexnib(hex[2*i]), lo = hexnib(hex[2*i+1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad hex at %d\n", 2*i); return 2; }
        seed[i] = (uint8_t)((hi << 4) | lo);
    }

    // Driven-replay mode: apply the recorded public moves against the seed-dealt
    // deck, querying octogen at every p1 turn for its deliberation.
    // argv[2] = moves file; argv[3] (optional) = reconstructed deal file.
    if (argc >= 3) return driven_replay(seed, argv[2], argc >= 4 ? argv[3] : NULL);

    cd_sim_solve_reset();
    // Seed the LCG first (octogen's internal RNG + the eligibility shuffle),
    // THEN enable the wide ChaCha deal — game_set_seed() clears wide mode, so
    // the order matters.
    game_set_seed(1);
    game_set_deal_seed_bytes(seed, 32);

    Game g; memset(&g, 0, sizeof g);
    g.num_players = 2;
    for (int i = 0; i < 2; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)STRAT_OCTOGEN;
        snprintf(g.players[i].player_id, sizeof g.players[i].player_id, "p%d", i);
    }
    start_game(&g);

    fprintf(stderr, "trump=suit%d(flip s%d v%d) first_attacker=p%d deck=%d\n",
            g.power_suit, g.flipped.suit, g.flipped.value, g.first_attacker, g.deck_count);

    int iters = 0;
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
            int idx = octogen_strategy_choose(&g, pi, &moves, NULL);
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

    if (game_done(&g) < 0) { fprintf(stderr, "GAME INCOMPLETE (iters=%d)\n", iters); }
    fprintf(stderr, "num_logs=%d elim=[", g.num_logs);
    for (int i = 0; i < g.num_eliminated; i++) fprintf(stderr, "%d ", g.elimination_order[i]);
    fprintf(stderr, "]\n");

    dump_logs(&g);
    return 0;
}
