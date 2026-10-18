// Fulminate's decisions with fm_exp against the same bot on libm's exp.
//
//   build/fm_exp_decisions_test [games] [max players]
//
// fulminate's posterior softmax used libm's exp on native builds and a
// different hand-written exp on wasm until both became fm_exp (src/fm_exp.c),
// the reference now. This plays seeded games with a fulminate seat against
// weak opponents (random, simple, handwritten: the seats whose profiles move
// the posterior off its strong default, so the softmax is what decides the
// rollout mix) and at every fulminate decision asks the bot twice from the
// same state and RNG streams: once as linked (fm_exp), once as a second copy of
// fulminate_strategy.c compiled with fm_exp renamed to fm_exp_libm_ref, which
// this file defines as libm's exp (the Makefile rule for this test builds that
// copy). A third ask of the linked bot is the control: if the bot is not a
// pure function of the state and the streams, a difference proves nothing.
//
// It reports how many softmax inputs the two exps gave different bits for, and
// fails if any decision differs. A difference is not a bug in fm_exp by
// itself (fm_exp is the reference now, and the old exp is the one that would
// be off), but every one must be looked at and listed, and this test then
// becomes the place that lists them.
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/bot_play.h"
#include "../src/fm_exp.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fulminate_strategy_choose_libm_ref(const Game *g, int bot_idx,
                                       const LegalMoves *moves, void *ctx);

static long ref_calls = 0, ref_bitdiff = 0;

static uint64_t bits_of(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }

double fm_exp_libm_ref(double x);
double fm_exp_libm_ref(double x) {
    const double r = exp(x);
    ref_calls++;
    if (bits_of(r) != bits_of(fm_exp(x))) ref_bitdiff++;
    return r;
}

int main(int argc, char **argv) {
    const int games = argc > 1 ? atoi(argv[1]) : 24;
    const int max_np = argc > 2 ? atoi(argv[2]) : 4;
    static const int opponents[] = { STRAT_RANDOM, STRAT_SIMPLE_HEURISTIC, STRAT_HANDWRITTEN };
    long decisions = 0, changed = 0, unstable = 0;
    for (int gi = 0; gi < games; gi++) {
        const int np = 2 + gi % (max_np - 1);
        game_set_seed(0xF00D0000u + (uint32_t)gi);
        random_strategy_set_seed(0xBEEF0000u + (uint64_t)gi);
        Game g;
        memset(&g, 0, sizeof g);
        g.num_players = np;
        for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);
        const int fm_seat = gi % np;
        BotPlay bp;
        bot_play_init(&bp, &g, 4000, BOT_PLAY_SHUFFLE, NULL);
        for (int seat; (seat = bot_play_next(&bp)) >= 0; ) {
            int idx;
            if (seat == fm_seat) {
                const uint64_t game_rng = game_rng_get(), strat_rng = random_strategy_rng_get();
                const int ref = fulminate_strategy_choose_libm_ref(&g, seat, &bp.moves, NULL);
                game_rng_set(game_rng); random_strategy_set_seed(strat_rng);
                const int again = fulminate_strategy_choose(&g, seat, &bp.moves, NULL);
                game_rng_set(game_rng); random_strategy_set_seed(strat_rng);
                idx = fulminate_strategy_choose(&g, seat, &bp.moves, NULL);
                decisions++;
                if (again != idx) unstable++;
                else if (ref != idx) {
                    changed++;
                    printf("  changed: game %d (%dp) seat %d turn %d: libm exp move %d, fm_exp move %d of %d\n",
                           gi, np, seat, bp.turns, ref, idx, bp.moves.n);
                }
            } else {
                idx = strategy_choose(opponents[(seat + gi) % 3], &g, seat, &bp.moves);
            }
            bot_play_apply(&bp, idx);
        }
    }
    printf("fm_exp_decisions_test: %d games, %ld fulminate decisions, %ld softmax exps "
           "(%ld with bits libm and fm_exp disagree on), %ld decisions changed, %ld unstable\n",
           games, decisions, ref_calls, ref_bitdiff, changed, unstable);
    // A run where the softmax never ran, or never saw two exps disagree, has
    // compared nothing: the opponents are chosen so that it does both.
    if (ref_calls == 0 || ref_bitdiff == 0) { printf("FAIL: the comparison never exercised the softmax\n"); return 1; }
    if (unstable) { printf("FAIL: fulminate is not a function of the state and the streams\n"); return 1; }
    if (changed) { printf("FAIL: decisions changed; list them above and account for each\n"); return 1; }
    return 0;
}
