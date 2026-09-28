/* Chui Niu - the lobby's rules.
 *
 * PICK 'EM UP'S LOBBY (pickemup/c/src/pk_lobby.h, its D27) at 2 to 6 seats:
 * an open roster, lowest free seat first, capacity 2 in a DM and 6 in a
 * group, any seated player may start once 2 are seated except the newest
 * joiner while there is still room, and join-and-start in one bubble exactly
 * when the join fills the table.
 *
 * The rules themselves are shared/c/msg_lobby_roster's (msg_lobby_roster_*),
 * called with this game's group capacity; what stays here is Chui Niu's own:
 * the first roll a start makes and the events a roster change plays.
 *
 * The kernel knows a person only by `who`, an opaque 16-bit handle the host
 * assigns (the envelope's seat tag, reduced); it never needs a name. */
#ifndef CN_LOBBY_H
#define CN_LOBBY_H

#include "cn.h"
#include "cn_plan.h"
#include "../../../shared/c/msg_lobby_roster/msg_lobby_roster.h"

#define CN_LOBBY_DM_CAP     MSG_LOBBY_ROSTER_DM_CAP
#define CN_LOBBY_GROUP_CAP  CN_MAX_SEATS

_Static_assert(CN_MAX_SEATS <= MSG_LOBBY_ROSTER_MAX_SEATS, "the shared roster seats the whole table");
_Static_assert(CN_SEAT_NONE == MSG_LOBBY_ROSTER_NO_SEAT, "a lobby's newest-gone is this game's no-seat");

/* The lobby, shared/c/msg_lobby_roster's roster. */
typedef MsgLobbyRoster CnLobby;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state. */
enum {
    CN_LOBBY_START   = MSG_LOBBY_ROSTER_START,    /* seated, 2+ seated, and not the newest sender unless full */
    CN_LOBBY_INVITE  = MSG_LOBBY_ROSTER_INVITE,   /* seated alone and the newest bubble is not mine           */
    CN_LOBBY_WAITING = MSG_LOBBY_ROSTER_WAITING,  /* seated; somebody else has to act                         */
    CN_LOBBY_JOIN    = MSG_LOBBY_ROSTER_JOIN,     /* not seated, and there is room                            */
    CN_LOBBY_FULL    = MSG_LOBBY_ROSTER_FULL,     /* not seated, and there is none                            */
};

/* `seat` must be offered START. Rolls round 0 at n = n_seats. 1 or 0. */
int  cn_lobby_start(CnLobby *l, int seat, const uint8_t seed[32], CnGame *g);
/* The roster's events from `before` to `after` (msg_lobby_roster_plan):
 * LOBBY_LEAVE per person who went (at the old seat), LOBBY_JOIN per person
 * who came (at the new one). */
int  cn_plan_lobby(const CnLobby *before, const CnLobby *after, CnEvent *out, int cap);

#endif
