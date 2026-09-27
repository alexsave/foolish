/* Pick 'Em Up - the lobby's rules (RULES_AND_KERNEL.md 4.6, D27).
 *
 * foolish's lobby minus the rules checkbox: an open roster, lowest free seat
 * first, capacity 2 in a DM and 8 in a group, any seated player may start
 * once 2 are seated except the newest joiner while there is still room, and
 * join-and-start in one bubble exactly when the join fills the table.
 *
 * WHY A FILE OF ITS OWN (DECISION D29). These verdicts are roster arithmetic
 * and nothing in them is about bytes, so they live beside the rules rather
 * than inside the envelope (werewolf/COMMON.md "Wished-for" 2: the lobby's
 * rules were inside the envelope, and should not have been). pk_msg keeps
 * the roster's tags and names and calls these for every verdict.
 *
 * The kernel knows a person only by `who`, an opaque 16-bit handle the host
 * assigns (the envelope's seat tag, reduced); it never needs a name. */
#ifndef PK_LOBBY_H
#define PK_LOBBY_H

#include "pk.h"
#include "pk_plan.h"

#define PK_LOBBY_DM_CAP     2
#define PK_LOBBY_GROUP_CAP  8

typedef struct {
    uint8_t  n_seats;               /* seated, always contiguous 0..n_seats-1       */
    uint8_t  dm;                    /* 1: a two-person chat, capacity 2             */
    uint8_t  newest;                /* seat that sent the newest lobby bubble, or
                                       PK_SEAT_NONE when that person has left       */
    uint8_t  started;               /* 1 once dealt                                 */
    uint16_t rev;                   /* roster changes so far (joins and leaves)     */
    uint16_t who[PK_MAX_SEATS];     /* the host's handle for the person in each seat */
} PkLobby;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state (test 7.8.1 walks them all). */
enum {
    PK_LOBBY_START = 1,   /* seated, 2+ seated, and not the newest sender unless full */
    PK_LOBBY_INVITE,      /* seated alone and the newest bubble is not mine           */
    PK_LOBBY_WAITING,     /* seated; somebody else has to act                         */
    PK_LOBBY_JOIN,        /* not seated, and there is room                            */
    PK_LOBBY_FULL,        /* not seated, and there is none                            */
};

int  pk_lobby_cap(const PkLobby *l);

/* A new lobby with its creator in seat 0, who also sent the invitation. */
void pk_lobby_new(PkLobby *l, int dm, uint16_t creator);

/* The seat `who` holds, or -1. */
int  pk_lobby_seat_of(const PkLobby *l, uint16_t who);

/* Take the lowest free seat (the end of the roster): the seat, or -1 when
 * full, already seated or started. */
int  pk_lobby_join(PkLobby *l, uint16_t who);

/* Leave: later rows move down one seat, so seats stay contiguous; a new
 * seat 0 is the new dealer. 1, or 0 when `seat` may not leave. */
int  pk_lobby_leave(PkLobby *l, int seat);

/* What `seat` (-1: not seated) is offered; 0 once the game has started. */
int  pk_lobby_offered(const PkLobby *l, int seat);

/* May `seat` leave? Seated, not started, and somebody else is seated. */
int  pk_lobby_can_exit(const PkLobby *l, int seat);

/* May a person who is not seated join AND start in one bubble? Exactly when
 * their join fills the table. */
int  pk_lobby_can_join_and_start(const PkLobby *l);

/* Start: `seat` must be offered START. Deals from the seed at n = n_seats
 * (the deal depends on nothing else, 4.6.4) and records the starter. 1 or 0. */
int  pk_lobby_start(PkLobby *l, int seat, const uint8_t seed[32], PkGame *g);

/* The roster's events from `before` to `after`: a LOBBY_LEAVE per person who
 * went (at their old seat) and a LOBBY_JOIN per person who came (at their
 * new one). LOBBY_START is the deal's first event (pk_plan, bubble 0). */
int  pk_plan_lobby(const PkLobby *before, const PkLobby *after, PkEvent *out, int cap);

#endif
