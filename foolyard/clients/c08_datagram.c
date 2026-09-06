// The WebTransport client. foolish_server_quic pushes a seat's masked view as
// a QUIC DATAGRAM - unordered and unreliable by design - and the frame carries
// no version, so a view that overtakes a newer one is simply the board this
// client now believes in. It cannot tell, and nothing in the protocol would
// let it. Loss here is a lost datagram, not a dead connection.
#include "world.h"

static void settings(ClientState *cs) {
    cs->link.ordered = 0;
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

const ClientImpl client_datagram = { "datagram", settings, on_view, on_wake };
