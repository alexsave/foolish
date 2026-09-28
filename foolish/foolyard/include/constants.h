#ifndef FOOLYARD_CONSTANTS_H
#define FOOLYARD_CONSTANTS_H

#include "types.h"

// ---------- the event word ------------------------------------------------
//
// One u32 per scheduled event, ordered by plain integer compare:
//
//   [ prio:16 | type:3 | param:13 ]
//
// prio  = fire time & P_MASK, i.e. where inside its bucket it lands.
// type  = EV_*.
// param = a client id, a game id, or a packet slot. 8192 of each, and the
//         freelists refuse to mint id 8192 rather than let it wrap into type.
//
// Time is microseconds. A bucket spans 65536us and there are 256 of them, so
// the wheel reaches 16.7s ahead. Longer timers hop (see sch.c) instead of
// riding tiltyard's seconds-resolution slow bucket: nothing in a Durak session
// is scheduled days out, and the hop is exact where the slow bucket rounds.

#define P_BITS 16
#define P_SPAN (1u << P_BITS)
#define P_MASK (P_SPAN - 1u)

#define E_BITS 16
#define E_MASK ((1u << E_BITS) - 1u)

#define T_BITS 3
#define T_MASK ((1u << T_BITS) - 1u)

#define PARAM_BITS (E_BITS - T_BITS)
#define PARAM_MASK ((1u << PARAM_BITS) - 1u)
#define MAX_PARAM  PARAM_MASK

#define BUCKET_BITS 8
#define BUCKETS     (1u << BUCKET_BITS)
#define BUCKET_MASK (BUCKETS - 1u)

// One span short of the wheel: a delta of BUCKETS*P_SPAN lands back in the
// bucket it was scheduled from, a full lap early.
#define MAX_DELTA_US ((u64)P_SPAN * (BUCKETS - 1))

#define EV_NET_TO_SERVER 0  // param = packet
#define EV_NET_TO_CLIENT 1  // param = packet
#define EV_SRV_SERVICE   2  // param = game
#define EV_SRV_BOT       3  // param = game<<3 | seat
#define EV_SRV_LOBBY     4  // param = game
#define EV_CLI_WAKE      5  // param = client
#define EV_TICK          6  // param = TICK_*
#define EV_HOP           7  // param = long-timer slot, owned by sch.c

// Each bot seat is paced on its own, so its event has to name the seat too.
// 9 bits of game + 3 of seat fits the 13-bit param with room to spare.
#define SEAT_BITS 3
#define SEAT_MASK ((1u << SEAT_BITS) - 1u)
static inline u32 bot_param(u32 game, u32 seat) { return (game << SEAT_BITS) | (seat & SEAT_MASK); }
static inline u32 bot_param_game(u32 param) { return param >> SEAT_BITS; }
static inline u32 bot_param_seat(u32 param) { return param & SEAT_MASK; }

#define TICK_SWEEP 0
#define TICK_KILL  1

static inline u32 event_of(u32 type, u32 param) {
    return ((type & T_MASK) << PARAM_BITS) | (param & PARAM_MASK);
}
static inline u32 event_type(u32 ev)  { return (ev >> PARAM_BITS) & T_MASK; }
static inline u32 event_param(u32 ev) { return ev & PARAM_MASK; }

#define MS 1000ull
#define SEC 1000000ull

// ---------- sim sizing ----------------------------------------------------

#define MAX_GAMES   1024
#define MAX_CLIENTS 4096
#define ID_LIMIT    (MAX_PARAM + 1)   // 8192: what a 13-bit param can name

// state_put's real worst case under this build's caps: 16 header + 64 deck +
// 1 + 64 battles + 8*(1+1+1+64) players + 1 + 8 eliminated = 690.
#define VIEW_MAX 768

#define SRV_QUEUE_MAX 64   // per-game request backlog before the sim gives up

_Static_assert(MAX_GAMES <= (1u << (PARAM_BITS - SEAT_BITS)),
               "a game id has to fit the bot event param alongside its seat");
_Static_assert(MAX_CLIENTS <= ID_LIMIT, "a client id has to fit the event param");

#endif
