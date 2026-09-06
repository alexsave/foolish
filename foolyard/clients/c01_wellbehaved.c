// The reference client: foolish_hammer's ws worker. Decodes the pushed view,
// asks the kernel for its own legal moves, waits its think time, then submits
// one - re-reading whatever view it holds at that moment, so the only
// staleness it can suffer is the wire's own flight time.
#include "world.h"

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    AwireAction probe;
    if (!client_pick_legal(ctx->view, ctx->seat, &cs->rng, &probe)) return;

    ctx->want_wake = 1;
    ctx->wake_delay_us = cs->think_us + rng_below(&cs->rng, cs->think_jitter_us + 1);
}

static void on_wake(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    if (!ctx->view) return;

    if (client_pick_legal(ctx->view, ctx->seat, &cs->rng, &ctx->out_move)) {
        ctx->want_send = 1;
        ctx->out_version = ctx->view_version;
    }
}

const ClientImpl client_wellbehaved = { "wellbehaved", settings, on_view, on_wake };
