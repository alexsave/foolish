/* Chui Niu - the lobby's rules.
 *
 * PICK 'EM UP'S LOBBY (pickemup/c/src/pk_lobby.h, its D27) at 2 to 6 seats:
 * an open roster, lowest free seat first, capacity 2 in a DM and 6 in a
 * group, any seated player may start once 2 are seated except the newest
 * joiner while there is still room, and join-and-start in one bubble exactly
 * when the join fills the table.
 *
 * The kernel knows a person only by `who`, an opaque 16-bit handle the host
 * assigns (the envelope's seat tag, reduced); it never needs a name. */
#ifndef CN_LOBBY_H
#define CN_LOBBY_H

#include "cn.h"
#include "cn_plan.h"

#define CN_LOBBY_DM_CAP     2
#define CN_LOBBY_GROUP_CAP  CN_MAX_SEATS

typedef struct {
    uint8_t  n_seats;               /* seated, always contiguous 0..n_seats-1       */
    uint8_t  dm;                    /* 1: a two-person chat, capacity 2             */
    uint8_t  newest;                /* seat that sent the newest lobby bubble, or
                                       CN_SEAT_NONE when that person has left       */
    uint8_t  started;               /* 1 once dealt                                 */
    uint16_t rev;                   /* roster changes so far (joins and leaves)     */
    uint16_t who[CN_MAX_SEATS];     /* the host's handle for the person in each seat */
} CnLobby;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state. */
enum {
    CN_LOBBY_START = 1,   /* seated, 2+ seated, and not the newest sender unless full */
    CN_LOBBY_INVITE,      /* seated alone and the newest bubble is not mine           */
    CN_LOBBY_WAITING,     /* seated; somebody else has to act                         */
    CN_LOBBY_JOIN,        /* not seated, and there is room                            */
    CN_LOBBY_FULL,        /* not seated, and there is none                            */
};

int  cn_lobby_cap(const CnLobby *l);
void cn_lobby_new(CnLobby *l, int dm, uint16_t creator);
int  cn_lobby_seat_of(const CnLobby *l, uint16_t who);
/* The lowest free seat, or -1 when full, already seated or started. */
int  cn_lobby_join(CnLobby *l, uint16_t who);
/* Later rows move down one seat. 1, or 0 when `seat` may not leave. */
int  cn_lobby_leave(CnLobby *l, int seat);
/* What `seat` (-1: not seated) is offered; 0 once started. */
int  cn_lobby_offered(const CnLobby *l, int seat);
int  cn_lobby_can_exit(const CnLobby *l, int seat);
int  cn_lobby_can_join_and_start(const CnLobby *l);
/* `seat` must be offered START. Rolls round 0 at n = n_seats. 1 or 0. */
int  cn_lobby_start(CnLobby *l, int seat, const uint8_t seed[32], CnGame *g);
/* The roster's events from `before` to `after`: LOBBY_LEAVE per person who
 * went (at the old seat), LOBBY_JOIN per person who came (at the new one). */
int  cn_plan_lobby(const CnLobby *before, const CnLobby *after, CnEvent *out, int cap);

#endif
