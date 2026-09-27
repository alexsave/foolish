#include <stdio.h>
#include <stdlib.h>

#include "world.h"

void net_init(Net *net) {
    fl_init(&net->packets, sizeof(Packet), ID_LIMIT);
    net->sent = net->delivered = net->dropped = net->duplicated = net->reordered = 0;
}

void net_free(Net *net) {
    fl_free(&net->packets);
}

u32 net_alloc(World *w) {
    u32 id = fl_alloc(&w->net.packets);
    if (id == FL_NONE) {
        fprintf(stderr, "net: all %d packet slots in flight at t=%lluus. "
                        "Either something leaks, or the sim outgrew a 13-bit param.\n",
                ID_LIMIT, (unsigned long long)sch_now_us(&w->sch));
        exit(1);
    }
    return id;
}

Packet *net_pkt(World *w, u32 id) {
    return (Packet *)fl_get(&w->net.packets, id);
}

void net_release(World *w, u32 id) {
    fl_release(&w->net.packets, id);
}

// Latency plus per-packet jitter, which on a datagram link is enough to let
// two frames pass each other. An ordered link (a /ws stream) instead advances
// a cursor: nothing can land before the frame ahead of it, so a late one drags
// the whole queue with it, head-of-line blocking and all.
static u64 net_arrival(World *w, ClientState *cs, u64 *cursor, u32 base) {
    u64 now = sch_now_us(&w->sch);
    u64 at = now + base + rng_below(&w->rng, cs->link.jitter_us + 1);

    if (cs->link.ordered) {
        if (at <= *cursor) at = *cursor + 1;
        *cursor = at;
    }
    return at - now;
}

void net_send(World *w, u32 pkt_id) {
    Packet *p = net_pkt(w, pkt_id);
    ClientState *cs = &w->clients[p->client_id];
    int to_server = (p->kind == PKT_MOVE || p->kind == PKT_SUB || p->kind == PKT_POLL);

    w->net.sent++;
    p->sent_us = sch_now_us(&w->sch);

    if (rng_pct(&w->rng, cs->link.loss_pct)) {
        w->net.dropped++;
        net_release(w, pkt_id);
        // A stream does not quietly lose one frame - it dies. Modelling loss on
        // an ordered link as a vanished push would invent a failure TCP cannot
        // produce, and then blame the client for never noticing.
        if (cs->link.ordered) client_link_reset(w, cs);
        return;
    }

    u32 base = to_server ? cs->link.up_us : cs->link.down_us;
    u32 type = to_server ? EV_NET_TO_SERVER : EV_NET_TO_CLIENT;
    u64 *cursor = to_server ? &cs->up_next_us : &cs->down_next_us;

    sch_schedule(&w->sch, event_of(type, pkt_id), net_arrival(w, cs, cursor, base));

    if (rng_pct(&w->rng, cs->link.dup_pct)) {
        u32 twin = net_alloc(w);
        *net_pkt(w, twin) = *p;
        w->net.duplicated++;
        sch_schedule(&w->sch, event_of(type, twin), net_arrival(w, cs, cursor, base));
    }
}
