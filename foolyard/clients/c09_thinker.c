// wellbehaved with a brain: identical wire behaviour, but it chooses with
// handwritten_prod instead of uniformly at random. This is what turns the
// latency question from an upper bound into an answer - every other tier plays
// the policy that speed demonstrably helps, so their curves measure the policy
// as much as the connection.
#include "world.h"

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    if (!should_bot_act(ctx->view, ctx->seat)) return;

    ctx->want_wake = 1;
    ctx->wake_delay_us = cs->think_us + rng_below(&cs->rng, cs->think_jitter_us + 1);
}

static void on_wake(ClientCtx *ctx) {
    if (!ctx->view) return;

    if (client_pick_brain(ctx->view, ctx->seat, &ctx->out_move)) {
        ctx->want_send = 1;
        ctx->out_version = ctx->view_version;
    }
}

const ClientImpl client_thinker = { "thinker", settings, on_view, on_wake };
