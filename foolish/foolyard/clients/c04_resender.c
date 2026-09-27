// Sends its move, does not hear back fast enough, and sends the SAME frame
// again under the same seq. The server carries no dedup, so the only thing
// stopping a second application is the move having become illegal - and
// "attack with the 7 of hearts" stays legal in plenty of later states.
#include "world.h"

#define RESEND_TIMEOUT_US (700 * MS)

typedef struct { u8 awaiting_ack; } Priv;

static void settings(ClientState *cs) {
    cs->link.ordered = 1;
}

static void arm_resend(ClientCtx *ctx) {
    ((Priv *)ctx->cs->priv)->awaiting_ack = 1;
    ctx->want_wake = 1;
    ctx->wake_delay_us = RESEND_TIMEOUT_US;
}

static void on_view(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    Priv *pv = (Priv *)cs->priv;

    if (ctx->is_ack && ctx->ack_ok) pv->awaiting_ack = 0;

    // should_bot_act answers "can this seat act" without enumerating the menu;
    // building the whole menu here just to discard it was 27% of the sim.
    if (!should_bot_act(ctx->view, ctx->seat)) return;

    ctx->want_wake = 1;
    ctx->wake_delay_us = cs->think_us + rng_below(&cs->rng, cs->think_jitter_us + 1);
}

static void on_wake(ClientCtx *ctx) {
    ClientState *cs = ctx->cs;
    Priv *pv = (Priv *)cs->priv;

    if (pv->awaiting_ack) {
        pv->awaiting_ack = 0;
        client_retransmit(ctx->w, cs);
        return;
    }
    if (!ctx->view) return;

    if (client_pick_legal(ctx->view, ctx->seat, &cs->rng, &ctx->out_move)) {
        ctx->want_send = 1;
        ctx->out_version = ctx->view_version;
        cs->pending_version = ctx->view_version;
        arm_resend(ctx);
    }
}

const ClientImpl client_resender = { "resender", settings, on_view, on_wake };
