// Decides the instant it sees a board, then sits on the decision. A human who
// picked a card and then went to answer the door: the move that lands is the
// answer to a question the table stopped asking, and it reports the version it
// actually decided at, which is what makes the staleness legible in the report.
#include "world.h"

typedef struct { u8 armed; } Priv;

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    Priv *pv = (Priv *)cs->priv;
    if (pv->armed) return;   // already sitting on one, do not re-decide

    if (!client_pick_legal(ctx->view, ctx->seat, &cs->rng, &cs->pending)) return;

    cs->has_pending = 1;
    cs->pending_version = ctx->view_version;
    pv->armed = 1;

    ctx->want_wake = 1;
    ctx->wake_delay_us = cs->think_us + rng_below(&cs->rng, cs->think_jitter_us + 1);
}

static void on_wake(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    Priv *pv = (Priv *)cs->priv;
    pv->armed = 0;
    if (!cs->has_pending) return;

    cs->has_pending = 0;
    ctx->want_send = 1;
    ctx->out_move = cs->pending;
    ctx->out_version = cs->pending_version;
}

const ClientImpl client_stale = { "stale", settings, on_view, on_wake };
