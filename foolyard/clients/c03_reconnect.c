// Drops its socket mid-bout and comes back. The interesting window is between
// the two: the server keeps playing, its seat stops being subscribed, and the
// state it re-subscribes into is nothing like the one it left.
#include "world.h"

#define RECONNECT_DROP_PCT 4
#define RECONNECT_BACKOFF_US (2 * SEC)

typedef struct { u8 reconnecting; } Priv;

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    Priv *pv = (Priv *)cs->priv;

    if (rng_pct(&cs->rng, RECONNECT_DROP_PCT)) {
        client_disconnect(ctx->w, cs);
        pv->reconnecting = 1;
        ctx->want_wake = 1;
        ctx->wake_delay_us = RECONNECT_BACKOFF_US;
        return;
    }

    AwireAction probe;
    if (!client_pick_legal(ctx->view, ctx->seat, &cs->rng, &probe)) return;

    ctx->want_wake = 1;
    ctx->wake_delay_us = cs->think_us + rng_below(&cs->rng, cs->think_jitter_us + 1);
}

static void on_wake(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    Priv *pv = (Priv *)cs->priv;

    if (pv->reconnecting) {
        pv->reconnecting = 0;
        client_subscribe(ctx->w, cs);
        return;
    }
    if (!ctx->view) return;

    if (client_pick_legal(ctx->view, ctx->seat, &cs->rng, &ctx->out_move)) {
        ctx->want_send = 1;
        ctx->out_version = ctx->view_version;
    }
}

const ClientImpl client_reconnect = { "reconnect", settings, on_view, on_wake };
