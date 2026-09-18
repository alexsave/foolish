// Same brain as wellbehaved, worse wire: a phone on a train. Its decisions are
// current when it makes them, but the board has moved on by the time they land,
// and by then the push that would have corrected it is still in flight.
#include "world.h"

#define LAGGY_LATENCY_MULT 3
#define LAGGY_JITTER_MULT  4

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
    cs->link.up_us *= LAGGY_LATENCY_MULT;
    cs->link.down_us *= LAGGY_LATENCY_MULT;
    cs->link.jitter_us *= LAGGY_JITTER_MULT;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    // should_bot_act answers "can this seat act" without enumerating the menu;
    // building the whole menu here just to discard it was 27% of the sim.
    if (!should_bot_act(ctx->view, ctx->seat)) return;

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

const ClientImpl client_laggy = { "laggy", settings, on_view, on_wake };
