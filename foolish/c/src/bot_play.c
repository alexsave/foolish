// The offline "play a bot game to the end" loop (bot_play.h).
#include "bot_play.h"

#include <string.h>

#include "../../../shared/c/rng.h"

void bot_play_init(BotPlay *p, Game *g, int max_turns, int order_mode, uint64_t *order_rng) {
    p->g = g;
    p->order_rng = order_rng;
    p->order_mode = order_mode;
    p->max_turns = max_turns;
    p->turns = 0;
    p->n_order = 0;
    p->next = 0;
    p->in_turn = false;
    p->acted = false;
    p->stopped = false;
    p->seat = -1;
    p->moves.n = 0;
}

// Begin a turn: who may act, in what order. False when the game is over, the
// cap is reached or nobody may act. The cap counts turns begun and stops the
// game exactly where `while (!done && iters++ < cap)` did.
static bool begin_turn(BotPlay *p) {
    Game *g = p->g;
    if (game_done(g) >= 0 || p->turns >= p->max_turns) return false;
    p->turns++;
    int n = 0;
    for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) p->order[n++] = i;
    if (n == 0) return false;
    if (p->order_mode == BOT_PLAY_SHUFFLE) {
        for (int i = n - 1; i > 0; i--) {
            int j;
            if (p->order_rng) {
                j = (int)(rng_u32(p->order_rng) % (uint32_t)(i + 1));
            } else {
                j = (int)(game_random() * (i + 1));
                if (j < 0) j = 0;
                if (j > i) j = i;
            }
            int t = p->order[i]; p->order[i] = p->order[j]; p->order[j] = t;
        }
    }
    p->n_order = n;
    p->next = 0;
    p->in_turn = true;
    p->acted = false;
    return true;
}

int bot_play_next(BotPlay *p) {
    for (;;) {
        if (p->stopped) return -1;
        if (p->next >= p->n_order) {
            // A turn whose every seat failed to move stalls the game: stop
            // rather than spin on a board nobody can change.
            if ((p->in_turn && !p->acted) || !begin_turn(p)) {
                p->stopped = true;
                p->seat = -1;
                return -1;
            }
        }
        int seat = p->order[p->next++];
        calculate_legal_moves(p->g, seat, &p->moves);
        if (p->moves.n == 0) continue;
        p->seat = seat;
        return seat;
    }
}

bool bot_play_took(BotPlay *p, bool applied) {
    if (applied) {
        p->acted = true;
        p->next = p->n_order;   // the turn is over; the next call begins another
    }
    return applied;
}

bool bot_play_apply(BotPlay *p, int idx) {
    if (p->seat < 0 || idx < 0 || idx >= p->moves.n) return false;
    return bot_play_took(p, legal_move_apply(p->g, p->seat, &p->moves.moves[idx]));
}
