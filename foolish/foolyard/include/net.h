#ifndef FOOLYARD_NET_H
#define FOOLYARD_NET_H

#include "types.h"
#include "constants.h"
#include "fl.h"

#define PKT_MOVE 0   // client -> server, body is an awire frame
#define PKT_SUB  1   // client -> server, the /ws upgrade
#define PKT_POLL 2   // client -> server, a /state read
#define PKT_ACK  3   // server -> client, the reply to a MOVE or POLL
#define PKT_PUSH 4   // server -> client, unsolicited fresh view

typedef struct Packet {
    // On the wire. game_id and seat ride the query string, the link identifies
    // the client the way a token + socket does, and ok is the ACK's lead byte.
    u16 client_id;
    u16 game_id;
    u8  kind;
    u8  seat;
    u8  ok;
    u16 len;
    u8  body[VIEW_MAX];

    // NOT on the wire. An awire frame has no version and a push is [ok][state],
    // so a client can neither prove when it decided nor tell a stale view from
    // a fresh one - and could lie about both if it could. These exist only so
    // the harness can say "applied a move chosen three versions ago". Nothing
    // in server.c may branch on them.
    u32 obs_version;     // ACK/PUSH: the version this view was cut at
    u32 obs_chosen_at;   // MOVE: the version the mover had last seen
    u32 obs_seq;         // MOVE: the mover's own counter, for dup detection
    // ACK/PUSH: the seat's last applied move and the deal, AS OF THIS CUT.
    // Both have to travel with the view rather than be read live when it
    // lands: with two moves in flight the server has moved on by then, and a
    // detector comparing against `now` accuses it of losing the second card.
    u32 obs_applied;
    u32 obs_deal;
    u64 sent_us;
} Packet;

typedef struct Net {
    FL packets;   // ID_LIMIT slots: an id has to fit the event word's param
    u64 sent, delivered, dropped, duplicated, reordered;
} Net;

struct World;

void net_init(struct Net *net);
void net_free(struct Net *net);

u32     net_alloc(struct World *w);
Packet *net_pkt(struct World *w, u32 id);
void    net_release(struct World *w, u32 id);

void net_send(struct World *w, u32 pkt_id);

#endif
