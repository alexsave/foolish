// Dynamite play-time strategy — see dynamite_strategy.h.

#include "dynamite_strategy.h"
#include "grpo_encode.h"

#include <stdbool.h>

static const GrpoNet *g_net = NULL;

// Per-thread workspace so multiple concurrent games (GRPO self-play) can
// each evaluate the policy without stepping on each other. Lazy-allocated
// on first use in each thread; never explicitly freed (process-lifetime).
static _Thread_local GrpoWorkspace tls_ws;
static _Thread_local bool tls_ws_init = false;

void dynamite_strategy_set_net(const GrpoNet *net) { g_net = net; }

static GrpoWorkspace *get_ws(void) {
    if (!tls_ws_init) {
        grpo_workspace_alloc(&tls_ws, MAX_LEGAL_MOVES);
        tls_ws_init = true;
    }
    return &tls_ws;
}

int dynamite_strategy_choose(const Game *g, int bot_idx,
                             const LegalMoves *moves, void *ctx) {
    (void)ctx;
    if (moves->n == 0) return -1;
    if (!g_net)        return 0;
    GrpoWorkspace *ws = get_ws();
    grpo_net_forward(g_net, ws, g, bot_idx, moves);
    int best = 0;
    for (int i = 1; i < moves->n; i++) {
        if (ws->logits[i] > ws->logits[best]) best = i;
    }
    return best;
}

int dynamite_strategy_choose_verbose(const Game *g, int bot_idx,
                                     const LegalMoves *moves,
                                     float *out_log_probs) {
    int idx = dynamite_strategy_choose(g, bot_idx, moves, NULL);
    if (out_log_probs) {
        GrpoWorkspace *ws = get_ws();
        for (int i = 0; i < moves->n; i++) out_log_probs[i] = ws->log_probs[i];
    }
    return idx;
}
