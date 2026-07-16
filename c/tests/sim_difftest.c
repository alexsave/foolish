// Deterministic move-by-move difftest: struct rollout engine vs bitboard
// engine. Plays real games with the struct handwritten rollout; at each ply it
// snapshots the state, runs ONE step of each engine from the SAME state with
// the SAME RNG, and compares the resulting full state. Isolates rules bugs
// from RNG drift.
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cordite_sim.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int card_id_of(Card c) { return c.suit * 13 + (c.value - 1); }

// Build the canonical SimState-equivalent fingerprint of a Game for comparison.
static void game_fingerprint(const Game *g, uint64_t *hand, int *def, int *fa,
                             int *nb, uint8_t *atk, uint8_t *dfn, uint32_t *cov,
                             uint8_t *status, uint32_t *good, int *deckc) {
    for (int p = 0; p < g->num_players; p++) {
        uint64_t h = 0;
        for (int j = 0; j < g->players[p].hand_count; j++)
            h |= (1ull << card_id_of(g->players[p].hand[j]));
        hand[p] = h;
        status[p] = g->players[p].status;
    }
    *def = g->defender; *fa = g->first_attacker; *nb = g->num_battles;
    *cov = 0;
    for (int i = 0; i < g->num_battles; i++) {
        atk[i] = card_id_of(g->table_battles[i].attack);
        if (!card_is_none(g->table_battles[i].defense)) {
            dfn[i] = card_id_of(g->table_battles[i].defense);
            *cov |= (1u << i);
        }
    }
    *good = g->good_players_mask;
    *deckc = g->deck_count;
}

// One struct step (mirrors cd_simulate's inner loop, handwritten only).
static int struct_step(Game *g) {
    for (int pi = 0; pi < g->num_players; pi++) {
        if (!should_bot_act(g, pi)) continue;
        LegalMoves moves;
        calculate_legal_moves_lite(g, pi, &moves);
        if (moves.n == 0) continue;
        int idx = handwritten_strategy_choose(g, pi, &moves, NULL);
        if (idx < 0 || idx >= moves.n) continue;
        bool ok;
        switch (moves.moves[idx].type) {
            case MOVE_ATTACK: ok = handle_attack(g, pi, moves.moves[idx].cards, moves.moves[idx].n_cards); break;
            case MOVE_COVER:  ok = handle_cover(g, pi, moves.moves[idx].cards, moves.moves[idx].attack_cards, moves.moves[idx].n_cards); break;
            case MOVE_PASS:   ok = handle_pass(g, pi, moves.moves[idx].cards, moves.moves[idx].n_cards); break;
            case MOVE_PICKUP: ok = handle_pickup(g, pi); break;
            case MOVE_GOOD:   ok = handle_good(g, pi); break;
            default: ok = false;
        }
        if (ok) return pi;
    }
    return -1;
}

extern int cd_sim_one_step(SimState *s);  // exposed for the difftest

static int n_steps = 0, n_div = 0, n_games = 0, n_benign = 0, n_realdiv = 0;

int main(int argc, char **argv) {
    int np = argc > 1 ? atoi(argv[1]) : 4;
    int games = argc > 2 ? atoi(argv[2]) : 200;
    int verbose = argc > 3;

    for (int gi = 0; gi < games; gi++) {
        game_set_seed(1000 + gi * 7);
        random_strategy_set_seed(1000 + gi * 7);
        Game g; memset(&g, 0, sizeof(g));
        g.num_players = np;
        for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);
        n_games++;

        int guard = 0;
        while (game_done(&g) < 0 && guard++ < 4000) {
            // snapshot
            Game gcopy; memcpy(&gcopy, &g, sizeof(g));
            SimState s; cd_sim_from_game(&s, &g);
            uint32_t rng = game_rng_get();
            int dbg_power = g.power_suit, dbg_nb = g.num_battles;
            int dbg_atk[24], dbg_atkv[24], dbg_def[24], dbg_cov[24], dbg_defcnt=g.players[g.defender].hand_count;
            for (int i=0;i<g.num_battles;i++){dbg_atkv[i]=g.table_battles[i].attack.value; dbg_atk[i]=g.table_battles[i].attack.suit; dbg_cov[i]=!card_is_none(g.table_battles[i].defense); dbg_def[i]=!card_is_none(g.table_battles[i].defense)?g.table_battles[i].defense.value:-1;}

            int who_s = struct_step(&g);          // advances g
            game_rng_set(rng);
            int who_b = cd_sim_one_step(&s);       // advances s

            if (who_s < 0 || who_b < 0) {
                if (who_s != who_b) { n_div++; if (verbose) fprintf(stderr, "game %d: actor mismatch s=%d b=%d\n", gi, who_s, who_b); }
                break;
            }
            n_steps++;

            // compare g (struct) vs s (bitboard)
            uint64_t hand[MAX_PLAYERS]; int def, fa, nb, deckc; uint8_t atk[24], dfn[24], status[MAX_PLAYERS]; uint32_t cov, good;
            game_fingerprint(&g, hand, &def, &fa, &nb, atk, dfn, &cov, status, &good, &deckc);

            int mismatch = 0;
            if (def != s.defender || fa != s.first_attacker || nb != s.num_battles) mismatch = 1;
            if (deckc != s.deck_count) mismatch = 1;
            for (int p = 0; p < np; p++) {
                if (hand[p] != s.hand[p]) mismatch = 1;
                if (status[p] != s.status_p[p]) mismatch = 1;
            }
            if (!mismatch) for (int i = 0; i < nb; i++) {
                if (atk[i] != s.atk[i]) mismatch = 1;
                if ((cov & (1u<<i)) != (s.covered_mask & (1u<<i))) mismatch = 1;
                if ((cov & (1u<<i)) && dfn[i] != s.def[i]) mismatch = 1;
            }
            if (who_s != who_b) mismatch = 1;

            if (mismatch) {
                // Classify: benign tie-break (same per-suit value-multiset of
                // the symmetric-difference cards => interchangeable equal cards)
                // vs a genuine rules divergence.
                int benign = 1;
                // value+suit multiset of cards that differ between the two
                int vcount_s[4][14] = {{0}}, vcount_b[4][14] = {{0}};
                for (int p = 0; p < np; p++) {
                    uint64_t only_s = hand[p] & ~s.hand[p];
                    uint64_t only_b = s.hand[p] & ~hand[p];
                    while (only_s) { int id = __builtin_ctzll(only_s); only_s &= only_s-1; vcount_s[id/13][id%13+1]++; }
                    while (only_b) { int id = __builtin_ctzll(only_b); only_b &= only_b-1; vcount_b[id/13][id%13+1]++; }
                }
                // benign iff the differing cards have identical (value, trumpness)
                // multiset — i.e. only same-value same-suit-class swaps. We use
                // full (suit,value) equality for safety: any suit difference is
                // NOT benign (cover semantics depend on suit).
                int vv_s[14]={0}, vv_b[14]={0};
                for (int su=0; su<4; su++) for (int v=1; v<=13; v++){ vv_s[v]+=vcount_s[su][v]; vv_b[v]+=vcount_b[su][v]; }
                // Determine trumpness multiset too.
                int tr_s=0,tr_b=0,nt_s=0,nt_b=0;
                for (int su=0; su<4; su++) for (int v=1; v<=13; v++){
                    if (su==s.power_suit){tr_s+=vcount_s[su][v]; tr_b+=vcount_b[su][v];}
                    else {nt_s+=vcount_s[su][v]; nt_b+=vcount_b[su][v];}
                }
                for (int v=1; v<=13; v++) if (vv_s[v]!=vv_b[v]) benign=0;
                if (tr_s!=tr_b || nt_s!=nt_b) benign=0;
                if (benign) { n_benign++; }
                else { n_realdiv++; }
                n_div++;
                if (benign) {
                    // resync bitboard from struct and keep checking this game
                    cd_sim_from_game(&s, &g);
                    continue;
                }
                if (verbose && !benign) {
                    fprintf(stderr, "=== game %d step diverged (actor s=%d b=%d) ===\n", gi, who_s, who_b);
                    fprintf(stderr, "  power=%d defcnt=%d table:", dbg_power, dbg_defcnt);
                    for (int i=0;i<dbg_nb;i++) fprintf(stderr, " [s%d v%d cov=%d defv=%d]", dbg_atk[i], dbg_atkv[i], dbg_cov[i], dbg_def[i]);
                    fprintf(stderr, "\n");
                    fprintf(stderr, "def s=%d b=%d | fa s=%d b=%d | nb s=%d b=%d | deck s=%d b=%d\n",
                            def, s.defender, fa, s.first_attacker, nb, s.num_battles, deckc, s.deck_count);
                    for (int p = 0; p < np; p++)
                        if (hand[p] != s.hand[p] || status[p] != s.status_p[p])
                            fprintf(stderr, "  p%d hand struct=%016llx bit=%016llx st s=%d b=%d\n",
                                    p, (unsigned long long)hand[p], (unsigned long long)s.hand[p], status[p], s.status_p[p]);
                }
                break;  // states diverged; rest of game is meaningless
            }
        }
    }
    fprintf(stderr, "difftest np=%d games=%d steps=%d diverged=%d (benign=%d real=%d) (%.3f%% real)\n",
            np, games, n_steps, n_div, n_benign, n_realdiv,
            n_steps ? 100.0*n_realdiv/(n_steps+n_div) : 0.0);
    return n_realdiv > 0 ? 1 : 0;
}
