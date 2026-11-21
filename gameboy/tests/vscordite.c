// vscordite - Moscow against foolish's real cordite (and octogen), on the
// Mac. Both rule engines run in lockstep, as in the difftest: Moscow reads
// this one (gd_rules), cordite reads foolish's Game (read-only sources,
// built into build/ with foolish's defaults: its bitboard solver, its belief
// from the game's logs, its 32/56/56 worlds). The seats swap every game; the
// lowest seat with a move acts first, as in match.c. Cordite's world seed is
// drawn the way a foolish host draws it, from game_state_seed of the board.
//
// usage: vscordite [cordite|octogen] [games] [first_seed] [moscow|seoul|newyork]
// (the last picks this side's bot, to calibrate the harness against foolish's
// published numbers)
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_bot_moscow.h"
#include "../src/gd_bot_seoul.h"
#include "../src/gd_bot_hw.h"
#include "game.h"
#include "legal.h"
#include "foolish_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cordite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

static LegalMoves *lm;

static void to_gd(const LegalMove *f, gd_move *m) {
    m->type = (uint8_t)f->type;
    m->n = (uint8_t)f->n_cards;
    for (int i = 0; i < f->n_cards; i++) {
        m->cards[i] = gd_of(f->cards[i]);
        m->atts[i] = f->type == GD_COVER ? gd_of(f->attack_cards[i]) : GD_NONE;
    }
}

int main(int argc, char **argv) {
    const int octo = argc > 1 && !strcmp(argv[1], "octogen");
    const unsigned long games = argc > 2 ? strtoul(argv[2], 0, 10) : 400;
    const unsigned long first = argc > 3 ? strtoul(argv[3], 0, 10) : 1;
    const char *ours = argc > 4 ? argv[4] : "moscow";
    unsigned long wm = 0, wc = 0, draws = 0, cut = 0;
    Game *f = calloc(1, sizeof(Game));
    lm = malloc(sizeof(LegalMoves));
    if (!f || !lm) return 2;
    for (unsigned long gi = 0; gi < games; gi++) {
        gd_game g;
        gd_rep r;
        uint8_t deck[36];
        Card fdeck[36];
        const uint8_t moscow_seat = (uint8_t)(gi & 1);   // Moscow seat 0 in even games
        gd_rng_seed((uint16_t)(first + gi / 2));         // each deal played both ways round
        for (int i = 0; i < 36; i++) deck[i] = (uint8_t)i;
        for (int i = 35; i > 0; i--) { uint8_t j = (uint8_t)gd_rng_below((uint32_t)i + 1u), t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        const uint8_t fallback = (uint8_t)gd_rng_below(2);
        gd_deal(&g, &r, deck, fallback);
        memset(f, 0, sizeof *f);
        f->num_players = 2;
        for (int i = 0; i < 36; i++) fdeck[i] = f_of(deck[i]);
        game_force_first_attacker(fallback);
        start_game_with_deck(f, fdeck, 36);
        game_force_first_attacker(-1);
        int moves = 0;
        while (g.status == GD_PLAYING && moves < 3000) {
            int acted = 0;
            for (uint8_t seat = 0; seat < 2 && !acted; seat++) {
                if (!gd_menu_kind(&g, seat) || (gd_menu_kind(&g, seat) == GD_KIND_DEFENDER && !gd_uncovered(&g))) continue;
                gd_move m;
                m.type = 0xFF;
                if (seat == moscow_seat) {
                    if (!strcmp(ours, "seoul")) gd_seoul_choose(&g, seat, &m);
                    else if (!strcmp(ours, "newyork")) gd_hw_choose(&g, seat, &m);
                    else gd_moscow_choose(&g, seat, &m);
                }
                else {
                    calculate_legal_moves(f, seat, lm);
                    if (lm->n) {
                        random_strategy_set_seed(game_state_seed(f, (uint32_t)(first + gi) * 2654435761u, 7u));
                        const int idx = octo ? octogen_strategy_choose(f, seat, lm, NULL) : cordite_strategy_choose(f, seat, lm, NULL);
                        to_gd(&lm->moves[idx >= 0 && idx < lm->n ? idx : 0], &m);
                    }
                }
                if (m.type == 0xFF) continue;
                if (!f_apply(f, seat, &m)) { fprintf(stderr, "foolish refused a move (game %lu)\n", gi); return 1; }
                game_settle_status(f);
                if (!gd_apply(&g, &r, seat, &m)) { fprintf(stderr, "gd refused a move (game %lu)\n", gi); return 1; }
                acted = 1;
            }
            if (!acted) break;
            moves++;
        }
        const uint8_t res = gd_result(&g);
        if (res == GD_RESULT_NONE) cut++;
        else if (res == GD_RESULT_DRAW) draws++;
        else {
            const uint8_t winner = res == GD_RESULT_FOOL0 ? 1 : 0;
            if (winner == moscow_seat) wm++; else wc++;
        }
    }
    printf("%s vs %s, %lu games: %s wins %lu (%.1f%%), %s wins %lu (%.1f%%), draws %lu, unfinished %lu\n",
           ours, octo ? "octogen" : "cordite", games, ours, wm, 100.0 * wm / games, octo ? "octogen" : "cordite", wc, 100.0 * wc / games, draws, cut);
    return 0;
}
