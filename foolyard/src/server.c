#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "world.h"

#include "awire.h"
#include "bot_drive.h"
#include "bot_roster.h"
#include "legal.h"
#include "view.h"

// state_put writes before it returns a length, so it serializes into a buffer
// sized for its documented worst case and the result is length-checked on the
// way into the packet. VIEW_MAX is what a Durak view can actually reach.
static void srv_send_view(World *w, GameSlot *s, int seat, u8 kind, u8 ok) {
    static u8 scratch[65536];

    i16 cid = s->seat_client[seat];
    if (cid < 0) return;
    ClientState *cs = &w->clients[cid];
    if (!cs->connected) return;

    int n = state_put(&s->game, seat, scratch);
    if (n > VIEW_MAX) {
        fprintf(stderr, "server: a seat view is %d bytes, VIEW_MAX is %d\n", n, VIEW_MAX);
        exit(1);
    }

    u32 id = net_alloc(w);
    Packet *p = net_pkt(w, id);
    p->client_id = (u16)cid;
    p->game_id = s->id;
    p->kind = kind;
    p->seat = (u8)seat;
    p->ok = ok;
    p->len = (u16)n;
    p->obs_version = s->version;
    p->obs_chosen_at = 0;
    p->obs_seq = 0;
    p->obs_applied = s->applies[seat];
    p->obs_deal = s->deals;
    memcpy(p->body, scratch, (size_t)n);

    net_send(w, id);
}

// The push-only fanout: after any change, every subscribed seat gets the fresh
// masked view. Returns how many went out, since the fanout is work done while
// the game's lock is held.
static int srv_broadcast(World *w, GameSlot *s, int skip_seat) {
    int n = 0;
    for (int seat = 0; seat < s->game.num_players; seat++) {
        if (seat == skip_seat || !s->seat_subscribed[seat]) continue;
        srv_send_view(w, s, seat, PKT_PUSH, 0);
        n++;
    }
    return n;
}

// Everything that follows a board change, in one place: the version the views
// are keyed on, the conservation check, the end-of-game hand-off to the lobby
// timer, and the fanout.
// `skip_seat` is the mover: on /ws its reply carries the same fresh view a
// push would, so sending both would be a frame the real server never emits.
static void srv_after_change(World *w, GameSlot *s, int skip_seat) {
    s->version++;
    s->last_change_us = sch_now_us(&w->sch);

    if (w->knobs.checks && s->game.status != GAME_STATUS_WAITING) {
        char why[96];
        int full = (w->knobs.checks >= 2) || ((s->version & 15) == 0);
        int ok = full ? inv_conservation(&s->game, why, sizeof why)
                      : inv_card_count(&s->game, why, sizeof why);
        if (!ok)
            inv_report(w, FIND_CONSERVATION, "game %u v%u%s: %s",
                       s->id, s->version, full ? "" : " (count)", why);
    }

    if (s->game.status == GAME_STATUS_GAME_OVER && !s->lobby_armed) {
        s->lobby_armed = 1;
        w->finished++;
        int fool = game_done(&s->game);
        if (fool >= 0 && fool < MAX_PLAYERS) w->fools[fool]++;
        trace_line(w, "    GAME OVER g%u  fool = seat %d  (rematch in %.1fs)",
                   s->id, fool, (double)w->knobs.lobby_delay_us / 1e6);
        sch_schedule(&w->sch, event_of(EV_SRV_LOBBY, s->id), w->knobs.lobby_delay_us);
    }

    srv_broadcast(w, s, skip_seat);
}

// The native server's pthread_cond_signal: a bot cycle is a scheduled event,
// and only ever one at a time per game. At an equal timestamp EV_SRV_BOT sorts
// after EV_SRV_SERVICE (the type bits sit below the priority bits), so a bot
// wakes behind the request that woke it, exactly like the real lock ordering.
static void srv_wake_bots(World *w, GameSlot *s) {
    if (s->game.status != GAME_STATUS_PLAYING) return;

    u32 humans = game_human_mask(&s->game);
    for (int seat = 0; seat < s->game.num_players; seat++) {
        if (humans & (1u << seat)) continue;
        BotSeat *b = &s->bots[seat];
        if (b->armed) continue;

        // Only a seat that can actually act. Arming one that cannot used to
        // start its think timer against a board it was not playing on, so when
        // a later change did make it eligible its move could land SOONER than
        // its own think time - a head start it never earned. Eligibility can
        // only change with the board, and every board change comes back
        // through here, so nothing is missed by waiting.
        if (!should_bot_act(&s->game, seat)) { b->waiting = 1; continue; }

        b->armed = 1;
        b->waiting = 0;
        u64 in = b->think_us + rng_below(&w->rng, b->jitter_us + 1);
        sch_schedule(&w->sch, event_of(EV_SRV_BOT, bot_param(s->id, (u32)seat)), in);
        trace_line(w, "    arm g%u seat %d in %.3fs", s->id, seat, (double)in / 1e6);
    }
}

static void srv_deal(World *w, GameSlot *s) {
    u8 seed[FOOLISH_SEED_LEN];
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (u8)rng_next(&w->rng);
    game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);

    int8_t keys[MAX_PLAYERS];
    int n = s->game.num_players;
    for (int i = 0; i < n; i++)
        keys[i] = s->seat_client[i] >= 0 ? STRATEGY_KEY_HUMAN : s->bots[i].strategy;

    game_seat_and_deal(&s->game, keys, n);

    s->deals++;
    w->deals++;
    s->lobby_armed = 0;

    trace_line(w, "    DEAL #%u g%u  trump %s  first %d  defender %d  deck %d",
               s->deals, s->id, trace_suit(s->game.power_suit),
               s->game.first_attacker, s->game.defender, s->game.deck_count);
    for (int i = 0; i < n; i++)
        trace_line(w, "      seat %d %-12s think %ums  hand %s", i,
                   s->seat_client[i] >= 0 ? "(client)" : bot_roster_at(
                       bot_roster_find_by_strat(s->bots[i].strategy))->key,
                   s->bots[i].think_us / 1000,
                   trace_cards(s->game.players[i].hand, s->game.players[i].hand_count));

    srv_after_change(w, s, -1);
    srv_wake_bots(w, s);
}

// The twin of ws_apply_move_locked / h_action. Deliberately NOT deduplicated:
// the real server carries no per-seat sequence number, so a retransmitted move
// that is still legal gets applied a second time. `seq` here is the harness
// watching for that, never the server defending against it.
static void srv_apply_move(World *w, GameSlot *s, Packet *p) {
    static Game clone;

    int seat = p->seat;
    if (s->seat_client[seat] != (i16)p->client_id)
        inv_report(w, FIND_SEAT_MISMATCH, "game %u seat %d: frame from client %u, seat holds %d",
                   s->id, seat, p->client_id, s->seat_client[seat]);

    AwireAction a;
    if (s->game.status != GAME_STATUS_PLAYING || !awire_decode(p->body, p->len, &a)) {
        srv_send_view(w, s, seat, PKT_ACK, 0);
        return;
    }

    u32 version_before = s->version;
    if (w->knobs.deep) memcpy(&clone, &s->game, sizeof clone);

    bool ok = awire_apply(&s->game, seat, &a);

    if (!ok) {
        w->moves_rejected++;
        if (w->knobs.deep && memcmp(&clone, &s->game, sizeof clone) != 0
            && engine_last_reject != ENGINE_REJECT_PASS_OVERFLOW)
            inv_report(w, FIND_MUTATION, "game %u seat %d: rejected move (reason %d) changed the board",
                       s->id, seat, engine_last_reject);
        srv_send_view(w, s, seat, PKT_ACK, 0);
        return;
    }

    w->moves_applied++;
    w->seat_moves[seat]++;

    // The move was decided in a game that has already ended and been re-dealt.
    // Nothing on the wire identifies WHICH game a frame is for beyond the id,
    // and the id survives the rematch, so the server cannot tell.
    if (p->obs_deal != s->deals)
        inv_report(w, FIND_CROSS_DEAL,
                   "game %u seat %d: move decided on deal %u applied to deal %u",
                   s->id, seat, p->obs_deal, s->deals);
    if (p->obs_chosen_at != version_before) w->stale_accepts++;

    static const char *kinds[] = {"attack", "cover", "pass", "pickup", "good"};
    const char *kind = (a.kind >= 0 && a.kind <= 4) ? kinds[a.kind] : "?";
    if (s->last_seq_valid[seat] && p->obs_seq == s->last_seq[seat])
        inv_report(w, FIND_DUP_APPLIED,
                   "game %u seat %d: %s(n=%d) seq %u applied again, chosen at v%u, board at v%u",
                   s->id, seat, kind, a.n, p->obs_seq, p->obs_chosen_at, version_before);
    else if (s->last_seq_valid[seat] && p->obs_seq < s->last_seq[seat])
        inv_report(w, FIND_MOVE_LATE,
                   "game %u seat %d: %s(n=%d) seq %u applied after seq %u had already landed",
                   s->id, seat, kind, a.n, p->obs_seq, s->last_seq[seat]);

    s->applies[seat]++;
    s->last_seq[seat] = p->obs_seq;
    s->last_seq_valid[seat] = 1;

    srv_after_change(w, s, seat);
    srv_wake_bots(w, s);
    srv_send_view(w, s, seat, PKT_ACK, 1);
}

// A frame reaches the server's NIC. It does not touch the game yet: it queues
// behind whatever else wants this game's lock, which is where the interesting
// staleness comes from.
void srv_on_packet(World *w, u32 pkt_id) {
    Packet *p = net_pkt(w, pkt_id);
    w->net.delivered++;

    GameSlot *s = &w->games[p->game_id];
    if (!s->used) { net_release(w, pkt_id); return; }

    if (s->qn >= SRV_QUEUE_MAX) {
        inv_report(w, FIND_QUEUE_FULL, "game %u: backlog of %d, dropped kind %u from seat %u",
                   s->id, SRV_QUEUE_MAX, p->kind, p->seat);
        net_release(w, pkt_id);
        return;
    }

    s->queue[(s->qh + s->qn) % SRV_QUEUE_MAX] = (u16)pkt_id;
    s->qn++;

    if (!s->servicing) {
        s->servicing = 1;
        sch_schedule(&w->sch, event_of(EV_SRV_SERVICE, s->id), w->knobs.srv_service_us);
    }
}

// One request off the game's queue, one lock hold. The next one is scheduled
// rather than run, so everything else in the world moves while this game is
// busy - and a client's next frame can arrive against a board that has since
// changed under it.
void srv_on_service(World *w, u32 game_id) {
    GameSlot *s = &w->games[game_id];
    if (s->qn == 0) { s->servicing = 0; return; }

    u32 pkt_id = s->queue[s->qh];
    s->qh = (u8)((s->qh + 1) % SRV_QUEUE_MAX);
    s->qn--;

    Packet *p = net_pkt(w, pkt_id);
    if (p->seat < s->game.num_players) {
        switch (p->kind) {
        case PKT_SUB:
            s->seat_subscribed[p->seat] = 1;
            srv_send_view(w, s, p->seat, PKT_ACK, 0);
            break;
        case PKT_POLL:
            srv_send_view(w, s, p->seat, PKT_ACK, 0);
            break;
        case PKT_MOVE:
            srv_apply_move(w, s, p);
            break;
        default:
            break;
        }
    }
    net_release(w, pkt_id);

    if (s->qn) {
        // A hiccup is one request holding the game's lock far longer than
        // usual - a GC pause, a slow bot search, a page fault. Everything
        // queued behind it ages, and what comes off the queue next was
        // decided against a board that has since moved a long way.
        u64 hold = w->knobs.srv_service_us;
        if (rng_pct(&w->rng, w->knobs.hiccup_pct)) hold += w->knobs.hiccup_us;
        sch_schedule(&w->sch, event_of(EV_SRV_SERVICE, s->id), hold);
    } else {
        s->servicing = 0;
    }
}

// The trampoline, as an event: one bot_drive cycle, then the kernel prices the
// wait and the wheel does the sleeping. A cycle that applied nothing parks the
// loop (the native server's pthread_cond_wait) until a client move wakes it.
void srv_on_bot(World *w, u32 param) {
    u32 game_id = bot_param_game(param);
    u32 seat = bot_param_seat(param);

    GameSlot *s = &w->games[game_id];
    BotSeat *b = &s->bots[seat];
    b->armed = 0;
    if (s->game.status != GAME_STATUS_PLAYING || (int)seat >= s->game.num_players) return;

    // Everyone but this seat is off limits for this cycle. bot_drive's own
    // fairness shuffle therefore never runs - which is the point: at this table
    // the order among simultaneously-eligible bots is decided by who thinks
    // fastest, not by a shuffle.
    u32 all_seats = (1u << s->game.num_players) - 1;
    u32 hold_mask = all_seats & ~(1u << seat);

    BotDriveOut drv;
    if (bot_drive(&s->game, hold_mask, BOT_DRIVE_MAX_ACTIONS, NULL, 0, &drv) < 0) return;

    for (int i = 0; i < drv.n; i++) {
        const BotDriveAction *act = &drv.actions[i];
        trace_line(w, "    PLAY g%u seat %d  %-6s %s   -> hand %d, defender %d, table %d",
                   s->id, act->seat, trace_move_kind(act->move.type),
                   trace_cards(act->move.cards, act->move.n_cards),
                   s->game.players[act->seat].hand_count,
                   s->game.defender, s->game.num_battles);
    }
    if (drv.n == 0)
        trace_line(w, "    park g%u seat %u  nothing to do", game_id, seat);

    int changed = (drv.n > 0 || drv.ended >= 0);
    if (changed) {
        w->bot_actions += (u64)drv.n;
        w->seat_moves[seat] += (u64)drv.n;
        srv_after_change(w, s, -1);
    }

    if (drv.ended < 0) {
        if (drv.n == 0) {
            b->waiting = 1;   // nothing to do; only a change can make there be
        } else {
            u64 pace = w->knobs.bot_kernel_pacing
                           ? (u64)bot_cycle_delay_ms(&s->game, hold_mask, &drv) * MS : 0;
            b->armed = 1;
            sch_schedule(&w->sch, event_of(EV_SRV_BOT, param),
                         pace + b->think_us + rng_below(&w->rng, b->jitter_us + 1));
        }
    }

    // The other seats parked on "nothing to do" now have something to do. This
    // one is already armed or deliberately parked, so srv_wake_bots skips it.
    if (changed) srv_wake_bots(w, s);
}

// Everyone hit continue. The board resets to the lobby and is dealt again, so
// a long run keeps churning fresh games through the same seats - and any move
// still in flight from the game that just ended lands on the new one.
void srv_on_lobby(World *w, u32 game_id) {
    GameSlot *s = &w->games[game_id];
    s->lobby_armed = 0;
    if (s->game.status != GAME_STATUS_GAME_OVER) return;

    u32 bot_mask = 0;
    for (int i = 0; i < s->game.num_players; i++)
        if (s->seat_client[i] < 0) bot_mask |= 1u << i;

    game_reset_to_lobby(&s->game, bot_mask);
    for (int i = 0; i < MAX_PLAYERS; i++) { s->last_seq_valid[i] = 0; s->applies[i] = 0; }

    srv_deal(w, s);
}

void srv_open_game(World *w, u16 game_id, int n_seats) {
    GameSlot *s = &w->games[game_id];
    s->used = 1;
    s->id = game_id;
    s->game.num_players = (int8_t)n_seats;
    srv_deal(w, s);
}
