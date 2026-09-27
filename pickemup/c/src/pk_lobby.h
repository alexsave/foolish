/* Pick 'Em Up - the lobby's rules (RULES_AND_KERNEL.md 4.6, D27).
 *
 * foolish's lobby minus the rules checkbox: an open roster, lowest free seat
 * first, capacity 2 in a DM and 8 in a group, any seated player may start
 * once 2 are seated except the newest joiner while there is still room, and
 * join-and-start in one bubble exactly when the join fills the table.
 *
 * The rules themselves are shared/c/msg_lobby_roster's (msg_lobby_roster_*),
 * called with this game's group capacity; what stays here is what is Pick
 * 'Em Up's own: the deal a start makes and the events a roster change plays.
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
#include "../../../shared/c/msg_lobby_roster/msg_lobby_roster.h"

#define PK_LOBBY_DM_CAP     MSG_LOBBY_ROSTER_DM_CAP
#define PK_LOBBY_GROUP_CAP  8

_Static_assert(PK_LOBBY_GROUP_CAP <= MSG_LOBBY_ROSTER_MAX_SEATS && PK_MAX_SEATS <= MSG_LOBBY_ROSTER_MAX_SEATS,
               "the shared roster seats the whole table");
_Static_assert(PK_SEAT_NONE == MSG_LOBBY_ROSTER_NO_SEAT, "a lobby's newest-gone is this game's no-seat");

/* The lobby, shared/c/msg_lobby_roster's roster. */
typedef MsgLobbyRoster PkLobby;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state (test 7.8.1 walks them all). */
enum {
    PK_LOBBY_START   = MSG_LOBBY_ROSTER_START,    /* seated, 2+ seated, and not the newest sender unless full */
    PK_LOBBY_INVITE  = MSG_LOBBY_ROSTER_INVITE,   /* seated alone and the newest bubble is not mine           */
    PK_LOBBY_WAITING = MSG_LOBBY_ROSTER_WAITING,  /* seated; somebody else has to act                         */
    PK_LOBBY_JOIN    = MSG_LOBBY_ROSTER_JOIN,     /* not seated, and there is room                            */
    PK_LOBBY_FULL    = MSG_LOBBY_ROSTER_FULL,     /* not seated, and there is none                            */
};

/* Start: `seat` must be offered START. Deals from the seed at n = n_seats
 * (the deal depends on nothing else, 4.6.4) and records the starter. 1 or 0. */
int  pk_lobby_start(PkLobby *l, int seat, const uint8_t seed[32], PkGame *g);

/* The roster's events from `before` to `after` (msg_lobby_roster_plan): a
 * LOBBY_LEAVE per person who went (at their old seat) and a LOBBY_JOIN per
 * person who came (at their new one). LOBBY_START is the deal's first event
 * (pk_plan, bubble 0). */
int  pk_plan_lobby(const PkLobby *before, const PkLobby *after, PkEvent *out, int cap);

#endif
