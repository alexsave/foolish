// The pre-/ws round trip: no push subscription at all, just GET /state on a
// timer and POST /action when it likes what it sees. Its view is stale by
// construction - up to one poll period behind - which is exactly why the
// server grew a push path.
#include "world.h"

#define POLL_PERIOD_US (900 * MS)

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
    cs->no_push = 1;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;

    if (client_pick_legal(ctx->view, ctx->seat, &cs->rng, &ctx->out_move)) {
        ctx->want_send = 1;
        ctx->out_version = ctx->view_version;
    }

    ctx->want_wake = 1;
    ctx->wake_delay_us = POLL_PERIOD_US;
}

static void on_wake(ClientCtx *ctx) {
    client_poll(ctx->w, ctx->cs);

    ctx->want_wake = 1;
    ctx->wake_delay_us = POLL_PERIOD_US;
}

const ClientImpl client_poller = { "poller", settings, on_view, on_wake };
