#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

#include "legal.h"
#include "view.h"
#include "strategy.h"

// `chosen_at` is the view version the move was decided against, NOT the newest
// one the client holds. A tier that sits on a decision reports the older one,
// and that is what makes the server's staleness accounting mean anything.
void client_submit(World *w, ClientState *cs, const AwireAction *a, u32 chosen_at) {
    if (!cs->connected) return;

    u32 id = net_alloc(w);
    Packet *p = net_pkt(w, id);

    int n = awire_encode(a, p->body, sizeof cs->last_frame);
    if (n <= 0) { net_release(w, id); return; }

    p->client_id = cs->id;
    p->game_id = cs->game_id;
    p->kind = PKT_MOVE;
    p->seat = cs->seat;
    p->ok = 0;
    p->len = (u16)n;
    p->obs_version = 0;
    p->obs_chosen_at = chosen_at;
    p->obs_seq = ++cs->seq;
    p->obs_deal = cs->view_deal;

    memcpy(cs->last_frame, p->body, (size_t)n);
    cs->last_frame_len = (u8)n;

    w->moves_sent++;
    net_send(w, id);
}

// One self-wake in flight per client, tiltyard's next_wake_ns guard boiled
// down: a tier that arms a wake on every event would otherwise breed them.
static void client_apply_ctx(World *w, ClientState *cs, ClientCtx *ctx) {
    if (ctx->want_send) client_submit(w, cs, &ctx->out_move, ctx->out_version);

    if (ctx->want_wake && !cs->wake_armed) {
        cs->wake_armed = 1;
        sch_schedule(&w->sch, event_of(EV_CLI_WAKE, cs->id), ctx->wake_delay_us);
    }
}

// Only this seat's own moves may take cards out of this seat's hand: a bot, a
// refill and a round transition can only ever add. So a card that vanishes
// while we have nothing outstanding, on the same deal, is the server having
// done something to us. The deal counter is the harness cheating on purpose -
// a client could not tell a re-deal from a robbery.
// Keyed on what the server had applied for this seat WHEN THIS VIEW WAS CUT
// (obs_applied), not on what it has applied by the time the view lands: with
// two moves in flight those are different numbers, and the difference is
// exactly the second move's own card.
static void client_check_hand(World *w, ClientState *cs, const Game *g,
                              u32 applied, u32 deal) {
    const Player *pl = &g->players[cs->seat];
    int n = pl->hand_count;
    if (n < 0) n = 0;
    if (n > MAX_HAND_SIZE) n = MAX_HAND_SIZE;

    // ...and only forwards. On a datagram link the view that just arrived can
    // be OLDER than the one it replaces, and then this comparison runs
    // backwards through time: a card the seat legitimately DREW in a refill
    // between the two reads as one that vanished. Six of those were the last
    // false positive this detector had.
    if (cs->have_view && cs->view_version >= cs->hand_version
        && cs->hand_seq == applied && cs->hand_deal == deal) {
        for (int i = 0; i < cs->hand_n; i++) {
            int found = 0;
            for (int j = 0; j < n && !found; j++)
                if (card_to_id(pl->hand[j]) == cs->hand[i]) found = 1;
            if (!found) {
                inv_report(w, FIND_PHANTOM_LOSS,
                           "client %u seat %u game %u: card id %u vanished between v%u and v%u "
                           "(both at applies=%u, deal=%u), hand %u->%d, defender %d",
                           cs->id, cs->seat, cs->game_id, cs->hand[i],
                           cs->hand_version, cs->view_version, applied, deal,
                           cs->hand_n, n, g->defender);
                break;
            }
        }
    }

    for (int i = 0; i < n; i++) cs->hand[i] = (u8)card_to_id(pl->hand[i]);
    cs->hand_n = (u8)n;
    cs->hand_seq = applied;
    cs->hand_version = cs->view_version;
    cs->hand_deal = deal;
}

static void client_control(World *w, ClientState *cs, u8 kind) {
    u32 id = net_alloc(w);
    Packet *p = net_pkt(w, id);
    p->client_id = cs->id;
    p->game_id = cs->game_id;
    p->kind = kind;
    p->seat = cs->seat;
    p->ok = 0;
    p->len = 0;
    p->obs_version = 0;
    p->obs_chosen_at = cs->view_version;
    p->obs_seq = 0;
    net_send(w, id);
}

void client_subscribe(World *w, ClientState *cs) {
    cs->connected = 1;
    client_control(w, cs, cs->no_push ? PKT_POLL : PKT_SUB);
}

void client_poll(World *w, ClientState *cs) {
    if (cs->connected) client_control(w, cs, PKT_POLL);
}

// The socket goes away. Frames already on the wire in either direction are
// delivered into a client that is no longer listening, which is what a real
// reset looks like from the outside.
void client_disconnect(World *w, ClientState *cs) {
    cs->connected = 0;
    cs->have_view = 0;
    cs->hand_n = 0;
    w->games[cs->game_id].seat_subscribed[cs->seat] = 0;
}

// The socket died under us. Every real client reconnects, so this is core
// behaviour rather than a tier's: drop the connection and come back after a
// backoff. A tier that wants to model NOT reconnecting is `griefer`.
void client_link_reset(World *w, ClientState *cs) {
    if (!cs->connected) return;

    client_disconnect(w, cs);
    cs->reconnect_pending = 1;
    cs->wake_armed = 1;
    sch_schedule(&w->sch, event_of(EV_CLI_WAKE, cs->id),
                 CLI_RECONNECT_BACKOFF_US + rng_below(&cs->rng, CLI_RECONNECT_BACKOFF_US));
}

// The same bytes again under the SAME seq: a real retransmit, which is what
// makes a second application of it a finding rather than a new move.
void client_retransmit(World *w, ClientState *cs) {
    if (!cs->connected || !cs->last_frame_len) return;

    u32 id = net_alloc(w);
    Packet *p = net_pkt(w, id);
    p->client_id = cs->id;
    p->game_id = cs->game_id;
    p->kind = PKT_MOVE;
    p->seat = cs->seat;
    p->ok = 0;
    p->len = cs->last_frame_len;
    p->obs_version = 0;
    p->obs_chosen_at = cs->pending_version;
    p->obs_seq = cs->seq;
    p->obs_deal = cs->view_deal;
    memcpy(p->body, cs->last_frame, cs->last_frame_len);

    w->moves_sent++;
    net_send(w, id);
}

static const Game *client_decode(World *w, ClientState *cs) {
    if (w->scratch_client != (i32)cs->id || w->scratch_version != cs->view_version) {
        state_get(w->scratch, cs->view, 1);
        w->scratch_client = (i32)cs->id;
        w->scratch_version = cs->view_version;
    }
    return w->scratch;
}

void client_on_packet(World *w, u32 pkt_id) {
    Packet *p = net_pkt(w, pkt_id);
    w->net.delivered++;

    ClientState *cs = &w->clients[p->client_id];
    if (!cs->connected || p->len == 0) { net_release(w, pkt_id); return; }

    // The client has no way to see this: the push carries no version, so a
    // view that overtook a newer one is simply the board it now believes in.
    if (cs->have_view && p->obs_version < cs->view_version) {
        w->net.reordered++;
        inv_report(w, FIND_VIEW_REGRESS, "client %u seat %u game %u: adopted v%u over v%u",
                   cs->id, cs->seat, cs->game_id, p->obs_version, cs->view_version);
    }

    memcpy(cs->view, p->body, p->len);
    cs->view_len = p->len;
    cs->view_version = p->obs_version;
    cs->view_deal = p->obs_deal;
    cs->have_view = 1;
    cs->last_view_us = sch_now_us(&w->sch);
    if (p->kind == PKT_ACK && p->ok) cs->acked_seq = cs->seq;

    ClientCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.w = w;
    ctx.cs = cs;
    ctx.now_us = cs->last_view_us;
    ctx.view = w->scratch;
    ctx.view_version = cs->view_version;
    ctx.seat = cs->seat;
    ctx.is_ack = (p->kind == PKT_ACK);
    ctx.ack_ok = p->ok;
    ctx.out_version = cs->view_version;

    u32 obs_applied = p->obs_applied, obs_deal = p->obs_deal;
    net_release(w, pkt_id);   // the handler allocates packets of its own

    w->scratch_client = -1;          // these are fresh bytes for this client
    client_decode(w, cs);
    if (w->knobs.checks) client_check_hand(w, cs, w->scratch, obs_applied, obs_deal);

    client_impl(cs->tier)->on_view(&ctx);
    client_apply_ctx(w, cs, &ctx);
}

void client_on_wake(World *w, u32 client_id) {
    ClientState *cs = &w->clients[client_id];
    cs->wake_armed = 0;

    if (cs->reconnect_pending) {
        cs->reconnect_pending = 0;
        client_subscribe(w, cs);
        return;
    }

    ClientCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.w = w;
    ctx.cs = cs;
    ctx.now_us = sch_now_us(&w->sch);
    ctx.view = cs->have_view ? w->scratch : NULL;
    ctx.view_version = cs->view_version;
    ctx.seat = cs->seat;
    ctx.out_version = cs->view_version;

    // The wake sees whatever view the client is holding. The scratch belongs
    // to whoever ran last, so this decodes only when that was someone else.
    if (cs->have_view) client_decode(w, cs);

    client_impl(cs->tier)->on_wake(&ctx);
    client_apply_ctx(w, cs, &ctx);
}

// Uniform over the seat's own legal menu, which is what foolish_hammer's ws
// worker does. Play strength is not the point; reaching odd interleavings is.
int client_pick_legal(const Game *view, int seat, u64 *rng, AwireAction *out) {
    static LegalMoves lm;   // ~140KB, far too big for a stack frame

    calculate_legal_moves(view, seat, &lm);

    int usable = 0;
    for (int i = 0; i < lm.n; i++)
        if (lm.moves[i].type != MOVE_WAIT && lm.moves[i].n_cards <= AWIRE_MAX_CARDS) usable++;
    if (!usable) return 0;

    int pick = (int)rng_below(rng, (u32)usable);
    for (int i = 0; i < lm.n; i++) {
        const LegalMove *m = &lm.moves[i];
        if (m->type == MOVE_WAIT || m->n_cards > AWIRE_MAX_CARDS) continue;
        if (pick-- > 0) continue;

        memset(out, 0, sizeof *out);
        out->kind = m->type;
        out->n = m->n_cards;
        for (int c = 0; c < m->n_cards; c++) {
            out->cards[c] = m->cards[c];
            out->attacks[c] = m->attack_cards[c];
        }
        return 1;
    }
    return 0;
}

// The same menu, chosen by a real brain instead of a coin. handwritten_prod is
// the one roster entry that is SOUND on a masked view: it reads its own hand,
// the table, the trump, and counts (hand_count, deck_count,
// discard_pile_length, has_flipped) - every one of which state_put preserves
// exactly. The belief bots would be reading {0,1} placeholders where the deck
// and the other hands should be, and would not be playing Durak.
int client_pick_brain(const Game *view, int seat, AwireAction *out) {
    static LegalMoves lm;

    calculate_legal_moves(view, seat, &lm);
    if (lm.n <= 0) return 0;

    int idx = handwritten_prod_strategy_choose(view, seat, &lm, NULL);
    if (idx < 0 || idx >= lm.n) return 0;

    const LegalMove *m = &lm.moves[idx];
    if (m->type == MOVE_WAIT || m->n_cards > AWIRE_MAX_CARDS) return 0;

    memset(out, 0, sizeof *out);
    out->kind = m->type;
    out->n = m->n_cards;
    for (int c = 0; c < m->n_cards; c++) {
        out->cards[c] = m->cards[c];
        out->attacks[c] = m->attack_cards[c];
    }
    return 1;
}

// The X-list in client.h is the registry; a tier is added by writing the file
// and putting its name there, exactly like tiltyard's IMPLS.
static const ClientImpl *const g_impls[] = {
    #define X(name) &client_##name,
    CLIENT_TIERS
    #undef X
};

const ClientImpl *client_impl(int tier) { return g_impls[tier]; }
int client_tier_count(void) { return (int)(sizeof g_impls / sizeof g_impls[0]); }
