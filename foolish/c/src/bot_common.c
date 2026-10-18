// What the bots share that is not small. See bot_common.h.

#include "bot_common.h"
#include "cordite_sim.h"
#include "legal.h"
#include <stdio.h>

// ---------- the belief self-check --------------------------------------

void bot_verify_belief(const char *tag, const Game *g, int bot_idx, const BotBelief *B) {
    for (int p = 0; p < g->num_players; p++) {
        if (p == bot_idx) continue;
        const Player *pl = &g->players[p];
        for (int k = 0; k < B->pinned_n[p]; k++) {
            bool found = false;
            for (int j = 0; j < pl->hand_count; j++) {
                if (card_eq(pl->hand[j], B->pinned[p][k])) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "%s: pinned card v%d s%d NOT in p%d hand (logs=%d)\n",
                        tag, B->pinned[p][k].value, B->pinned[p][k].suit, p, g->num_logs);
            }
        }
        for (int j = 0; j < pl->hand_count; j++) {
            if (bot_set_contains(B->pinned[p], B->pinned_n[p], pl->hand[j])) continue;
            if (B->void_n[p] > 0 && bot_void_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "%s: void violated: p%d holds v%d s%d\n",
                        tag, p, pl->hand[j].value, pl->hand[j].suit);
            }
            if (bot_floor_forbidden(B, g, p, pl->hand[j])) {
                fprintf(stderr, "%s: floor violated: p%d holds v%d s%d floor=%d\n",
                        tag, p, pl->hand[j].value, pl->hand[j].suit, B->floor_v[p]);
            }
            if (!bot_set_contains(B->pool, B->n, pl->hand[j])) {
                fprintf(stderr, "%s: p%d unknown card v%d s%d missing from pool (logs=%d)\n",
                        tag, p, pl->hand[j].value, pl->hand[j].suit, g->num_logs);
            }
        }
    }
}

// ---------- the exact solver -------------------------------------------

_Static_assert(BOT_SOLVE_MAX_DEPTH <= SOLVE_SCRATCH_DEPTH,
               "shared solver scratch shallower than the solver's depth");
_Static_assert(BOT_SOLVE_MAX_MOVES < SOLVE_SCRATCH_MOVES,
               "a capped move list must still overflow the solver's move limit");

static _Thread_local SolveMoves *bot_solver_mv = NULL;

bool bot_solver_ready(void) {
    if (!bot_solver_mv) bot_solver_mv = solve_scratch_mv();
    return bot_solver_mv != NULL;
}

int bot_solve(BotSolver *S, const Game *g, int alpha, int beta, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == S->me) ? -(1000 - depth) : (1000 - depth);
    if (bot_in_count(g) == 0) return 0;   // defensive: simultaneous out
    if (depth >= BOT_SOLVE_MAX_DEPTH) { S->aborted = true; return 0; }
    if (--S->budget <= 0) { S->aborted = true; return 0; }

    // Actor: defender first when they have something to do; otherwise the
    // first eligible attacker. (The real loop randomizes simultaneous
    // eligibility; defender-priority is the standard sequential abstraction.)
    int actor = -1;
    if (should_bot_act(g, g->defender)) actor = g->defender;
    else {
        for (int i = 0; i < g->num_players; i++) {
            if (should_bot_act(g, i)) { actor = i; break; }
        }
    }
    if (actor < 0) return 0;

    if (!bot_solver_ready()) { S->aborted = true; return 0; }
    SolveMoves *mv = &bot_solver_mv[depth];
    // Bounded generation into the compact scratch slot (see SOLVE_SCRATCH_MOVES
    // in cordite_sim.h): the cast is safe because SolveMoves shares LegalMoves'
    // leading layout and the cap keeps writes within its {n, moves[]} bounds.
    mv->n = legal_moves_into(g, actor, mv->moves, SOLVE_SCRATCH_MOVES);
    if (mv->n == 0) return 0;
    if (mv->n > BOT_SOLVE_MAX_MOVES) { S->aborted = true; return 0; }

    bool maximizing = (actor == S->me);
    int best = maximizing ? -2000 : 2000;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!legal_move_apply(child, actor, &mv->moves[i])) continue;
        int v = bot_solve(S, child, alpha, beta, depth + 1);
        if (S->aborted) return 0;
        if (maximizing) {
            if (v > best) best = v;
            if (best > alpha) alpha = best;
        } else {
            if (v < best) best = v;
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
    if (best == -2000 || best == 2000) return 0;  // no move applied
    return best;
}

int bot_leaf_solve(const Game *g, long budget) {
    if (!bot_solver_ready()) return -1;
    int me = -1;
    for (int i = 0; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) { me = i; break; }
    }
    if (me < 0) return -1;
    int opp = -1;
    for (int i = me + 1; i < g->num_players; i++) {
        if (g->players[i].status == PLAYER_STATUS_IN) { opp = i; break; }
    }
    if (opp < 0) return -1;

    Game *root = solve_scratch_root();
    solve_clone_root(root, g);

    BotSolver S;
    S.budget  = budget;
    S.aborted = false;
    S.me      = me;

    // Null window around 0: only the sign matters (true values are +-(1000-d)
    // for decided games), and the narrow window maximizes pruning.
    int v = bot_solve(&S, root, -1, 1, 0);
    if (S.aborted || v == 0) return -1;
    return (v > 0) ? opp : me;
}

// ---------- the rollout policy -----------------------------------------

StrategyFn bot_rollout_for(const Game *g, int policy) {
    if (policy == 1) return espresso_strategy_choose;
    if (policy == 2) return handwritten_strategy_choose;
    bool deck_active = (g->deck_count > 0 || g->has_flipped);
    if (deck_active || bot_in_count(g) == 2) return handwritten_strategy_choose;
    return espresso_strategy_choose;
}
