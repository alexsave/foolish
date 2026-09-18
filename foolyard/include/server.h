#ifndef FOOLYARD_SERVER_H
#define FOOLYARD_SERVER_H

#include "types.h"
#include "constants.h"

#include "game.h"

// A server-side bot seat. `think_us` is this brain's own cost, which the real
// server does not model at all: it drives every eligible bot in one cycle and
// prices the wait for a human watching. Here each seat is driven separately and
// waits its own time, so a slow octogen and a fast random can be sat at the
// same table and the scheduler decides who reaches a contested throw-in first.
typedef struct BotSeat {
    u8  armed;
    u8  waiting;      // nothing to do, parked until the board changes
    i8  strategy;     // roster index passed to the deal
    u32 think_us;
    u32 jitter_us;
} BotSeat;

typedef struct GameSlot {
    u8  used;
    u16 id;

    Game game;
    u32  version;

    u16 queue[SRV_QUEUE_MAX];   // packet ids waiting for the per-game lock
    u8  qh, qn;
    u8  servicing;

    BotSeat bots[MAX_PLAYERS];
    u8  lobby_armed;

    i16 seat_client[MAX_PLAYERS];
    u8  seat_subscribed[MAX_PLAYERS];

    u32 last_seq[MAX_PLAYERS];
    u8  last_seq_valid[MAX_PLAYERS];
    // Monotonic count of moves actually applied for each seat. last_seq is the
    // CLIENT's counter and can go backwards when the wire reorders, so it
    // cannot key "has this seat's hand had a reason to change".
    u32 applies[MAX_PLAYERS];

    u64 last_change_us;
    u8  stalled;      // reported once per quiet episode, not once per sweep
    u32 deals;
} GameSlot;

struct World;

void srv_open_game(struct World *w, u16 game_id, int n_seats);
void srv_on_packet(struct World *w, u32 pkt_id);
void srv_on_service(struct World *w, u32 game_id);
void srv_on_bot(struct World *w, u32 param);   // param = game<<3 | seat
void srv_on_lobby(struct World *w, u32 game_id);

#endif
