// THE LOBBY'S RULES for a Messages game whose lobby is a roster of seated
// people: who may join, leave and start, and which one control each viewer
// is offered while the game waits.
//
// An open roster, lowest free seat first, capacity 2 in a two-person chat
// and the product's group capacity otherwise; any seated player may start
// once 2 are seated, except the newest joiner while there is still room; and
// join-and-start in one bubble exactly when the join fills the table.
//
// CREATING NEVER DEALS. A new lobby seats its creator and nothing else; the
// game begins only through msg_lobby_roster_start, by a seated player who is
// offered START (or by the one join that fills the table, which is a join
// then that start). A game dealt at creation lets its creator reroll the deal
// by creating again, which is the hole a lobby exists to close.
//
// The roster knows a person only by `who`, an opaque 16-bit handle the host
// assigns; it never needs a name. These verdicts are roster arithmetic and
// nothing in them is about bytes, so they live apart from any envelope.
//
// Each product keeps what is its own: its group capacity, its game's
// constructor (handed to msg_lobby_roster_start as the one callback), and the
// event it plays for a roster change (built from msg_lobby_roster_plan).
//
// Freestanding: no allocation, nothing from libc beyond memset. In its own
// directory beside its test so the builds that wildcard shared/c/*.c pick up
// neither.
#ifndef SHARED_MSG_LOBBY_ROSTER_H
#define SHARED_MSG_LOBBY_ROSTER_H

#include <stdint.h>

#define MSG_LOBBY_ROSTER_MAX_SEATS  8     /* the largest group capacity a product may ask for */
#define MSG_LOBBY_ROSTER_DM_CAP     2     /* a two-person chat seats its two people           */
#define MSG_LOBBY_ROSTER_NO_SEAT    0xFF  /* `newest` once the newest sender has left         */

typedef struct {
    uint8_t  n_seats;               /* seated, always contiguous 0..n_seats-1         */
    uint8_t  dm;                    /* 1: a two-person chat, capacity 2               */
    uint8_t  newest;                /* seat that sent the newest lobby bubble, or
                                       MSG_LOBBY_ROSTER_NO_SEAT when they have left   */
    uint8_t  started;               /* 1 once the game has begun                      */
    uint8_t  group_cap;             /* the capacity outside a two-person chat, 2..MAX */
    uint16_t rev;                   /* roster changes so far (joins and leaves)       */
    uint16_t who[MSG_LOBBY_ROSTER_MAX_SEATS];  /* the host's handle in each seat      */
} MsgLobbyRoster;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state. */
enum {
    MSG_LOBBY_ROSTER_START = 1,  /* seated, 2+ seated, and not the newest sender unless full */
    MSG_LOBBY_ROSTER_INVITE,     /* seated alone and the newest bubble is not mine           */
    MSG_LOBBY_ROSTER_WAITING,    /* seated; somebody else has to act                         */
    MSG_LOBBY_ROSTER_JOIN,       /* not seated, and there is room                            */
    MSG_LOBBY_ROSTER_FULL,       /* not seated, and there is none                            */
};

/* How many may sit: MSG_LOBBY_ROSTER_DM_CAP in a two-person chat, else
 * group_cap. */
int  msg_lobby_roster_cap(const MsgLobbyRoster *l);

/* A new lobby with its creator in seat 0, who also sent the invitation.
 * group_cap is 2..MSG_LOBBY_ROSTER_MAX_SEATS. Nothing is started. */
void msg_lobby_roster_new(MsgLobbyRoster *l, int dm, int group_cap, uint16_t creator);

/* The seat `who` holds, or -1. */
int  msg_lobby_roster_seat_of(const MsgLobbyRoster *l, uint16_t who);

/* Take the lowest free seat (the end of the roster): the seat, or -1 when
 * full, already seated or started. */
int  msg_lobby_roster_join(MsgLobbyRoster *l, uint16_t who);

/* May `seat` leave? Seated, not started, and somebody else is seated. */
int  msg_lobby_roster_can_exit(const MsgLobbyRoster *l, int seat);

/* Leave: later rows move down one seat, so seats stay contiguous; the new
 * seat 0 goes first. 1, or 0 when `seat` may not leave. */
int  msg_lobby_roster_leave(MsgLobbyRoster *l, int seat);

/* What `seat` (-1: not seated) is offered; 0 once the game has started. */
int  msg_lobby_roster_offered(const MsgLobbyRoster *l, int seat);

/* May a person who is not seated join AND start in one bubble? Exactly when
 * their join fills the table. */
int  msg_lobby_roster_can_join_and_start(const MsgLobbyRoster *l);

/* The product's game constructor: a new game in `game` from the 32-byte
 * seed for the roster's n_seats, with `starter` the seat that started it.
 * 1 on success, 0 on refusal. */
typedef int (*MsgLobbyRosterStartFn)(void *game, const MsgLobbyRoster *l, int starter,
                                     const uint8_t seed[32]);

/* Start: `seat` must be offered START. Calls `start` once, and marks the
 * lobby started only when it succeeds. 1, or 0 when refused (and then
 * `start` is not called or said no). */
int  msg_lobby_roster_start(MsgLobbyRoster *l, int seat, const uint8_t seed[32], void *game,
                            MsgLobbyRosterStartFn start);

/* THE ROSTER'S CHANGES from `before` to `after`, by `who`: first a LEFT per
 * person who went, at their old seat, in seat order; then a JOINED per
 * person who came, at their new seat, in seat order. The count, at most
 * MSG_LOBBY_ROSTER_MAX_CHANGES. */
#define MSG_LOBBY_ROSTER_LEFT        1
#define MSG_LOBBY_ROSTER_JOINED      2
#define MSG_LOBBY_ROSTER_MAX_CHANGES (2 * MSG_LOBBY_ROSTER_MAX_SEATS)
typedef struct {
    uint8_t kind;                   /* MSG_LOBBY_ROSTER_LEFT or _JOINED */
    uint8_t seat;
} MsgLobbyRosterChange;
int  msg_lobby_roster_plan(const MsgLobbyRoster *before, const MsgLobbyRoster *after,
                           MsgLobbyRosterChange out[MSG_LOBBY_ROSTER_MAX_CHANGES]);

#endif
