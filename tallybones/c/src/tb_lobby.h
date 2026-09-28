/* Tallybones - the lobby's rules: Pick 'Em Up's (pickemup/c/src/pk_lobby.h),
 * copied and renamed (DECISIONS.md T2).
 *
 * foolish's lobby minus the rules checkbox: an open roster, lowest free seat
 * first, capacity 2 in a DM and 8 in a group, any seated player may start
 * once 2 are seated except the newest joiner while there is still room, and
 * join-and-start in one bubble exactly when the join fills the table.
 *
 * The rules themselves are shared/c/msg_lobby_roster's (msg_lobby_roster_*),
 * called with this game's group capacity; what stays here is Tallybones'
 * own: the game a start makes and the events a roster change plays.
 *
 * WHY A FILE OF ITS OWN (Pick 'Em Up's D29). These verdicts are roster arithmetic
 * and nothing in them is about bytes, so they live beside the rules rather
 * than inside the envelope (werewolf/COMMON.md "Wished-for" 2: the lobby's
 * rules were inside the envelope, and should not have been). tb_msg keeps
 * the roster's tags and names and calls these for every verdict.
 *
 * The kernel knows a person only by `who`, an opaque 16-bit handle the host
 * assigns (the envelope's seat tag, reduced); it never needs a name. */
#ifndef TB_LOBBY_H
#define TB_LOBBY_H

#include "tb.h"
#include "tb_plan.h"
#include "../../../shared/c/msg_lobby_roster/msg_lobby_roster.h"

#define TB_LOBBY_DM_CAP     MSG_LOBBY_ROSTER_DM_CAP
#define TB_LOBBY_GROUP_CAP  8

_Static_assert(TB_LOBBY_GROUP_CAP <= MSG_LOBBY_ROSTER_MAX_SEATS && TB_MAX_SEATS <= MSG_LOBBY_ROSTER_MAX_SEATS,
               "the shared roster seats the whole table");
_Static_assert(TB_SEAT_NONE == MSG_LOBBY_ROSTER_NO_SEAT, "a lobby's newest-gone is this game's no-seat");

/* The lobby, shared/c/msg_lobby_roster's roster. */
typedef MsgLobbyRoster TbLobby;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state (test 7.8.1 walks them all). */
enum {
    TB_LOBBY_START   = MSG_LOBBY_ROSTER_START,    /* seated, 2+ seated, and not the newest sender unless full */
    TB_LOBBY_INVITE  = MSG_LOBBY_ROSTER_INVITE,   /* seated alone and the newest bubble is not mine           */
    TB_LOBBY_WAITING = MSG_LOBBY_ROSTER_WAITING,  /* seated; somebody else has to act                         */
    TB_LOBBY_JOIN    = MSG_LOBBY_ROSTER_JOIN,     /* not seated, and there is room                            */
    TB_LOBBY_FULL    = MSG_LOBBY_ROSTER_FULL,     /* not seated, and there is none                            */
};

/* Start: `seat` must be offered START. A new game from the seed at
 * n = n_seats (roll 1 depends on nothing else) with the starter recorded.
 * 1 or 0. */
int  tb_lobby_start(TbLobby *l, int seat, const uint8_t seed[32], TbGame *g);

/* The roster's events from `before` to `after` (msg_lobby_roster_plan): a
 * LOBBY_LEAVE per person who went (at their old seat) and a LOBBY_JOIN per
 * person who came (at their new one). START is the game's first event
 * (tb_plan, bubble 0). */
int  tb_plan_lobby(const TbLobby *before, const TbLobby *after, TbEvent *out, int cap);

#endif
