/* Tallybones - the lobby's rules: Pick 'Em Up's (pickemup/c/src/pk_lobby.h),
 * copied and renamed (DECISIONS.md T2).
 *
 * foolish's lobby minus the rules checkbox: an open roster, lowest free seat
 * first, capacity 2 in a DM and 8 in a group, any seated player may start
 * once 2 are seated except the newest joiner while there is still room, and
 * join-and-start in one bubble exactly when the join fills the table.
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

#define TB_LOBBY_DM_CAP     2
#define TB_LOBBY_GROUP_CAP  8

typedef struct {
    uint8_t  n_seats;               /* seated, always contiguous 0..n_seats-1       */
    uint8_t  dm;                    /* 1: a two-person chat, capacity 2             */
    uint8_t  newest;                /* seat that sent the newest lobby bubble, or
                                       TB_SEAT_NONE when that person has left       */
    uint8_t  started;               /* 1 once started                               */
    uint16_t rev;                   /* roster changes so far (joins and leaves)     */
    uint16_t who[TB_MAX_SEATS];     /* the host's handle for the person in each seat */
} TbLobby;

/* The one control a viewer is offered in a waiting lobby: exactly one per
 * state (test 7.8.1 walks them all). */
enum {
    TB_LOBBY_START = 1,   /* seated, 2+ seated, and not the newest sender unless full */
    TB_LOBBY_INVITE,      /* seated alone and the newest bubble is not mine           */
    TB_LOBBY_WAITING,     /* seated; somebody else has to act                         */
    TB_LOBBY_JOIN,        /* not seated, and there is room                            */
    TB_LOBBY_FULL,        /* not seated, and there is none                            */
};

int  tb_lobby_cap(const TbLobby *l);

/* A new lobby with its creator in seat 0, who also sent the invitation. */
void tb_lobby_new(TbLobby *l, int dm, uint16_t creator);

/* The seat `who` holds, or -1. */
int  tb_lobby_seat_of(const TbLobby *l, uint16_t who);

/* Take the lowest free seat (the end of the roster): the seat, or -1 when
 * full, already seated or started. */
int  tb_lobby_join(TbLobby *l, uint16_t who);

/* Leave: later rows move down one seat, so seats stay contiguous; a new
 * seat 0 rolls first. 1, or 0 when `seat` may not leave. */
int  tb_lobby_leave(TbLobby *l, int seat);

/* What `seat` (-1: not seated) is offered; 0 once the game has started. */
int  tb_lobby_offered(const TbLobby *l, int seat);

/* May `seat` leave? Seated, not started, and somebody else is seated. */
int  tb_lobby_can_exit(const TbLobby *l, int seat);

/* May a person who is not seated join AND start in one bubble? Exactly when
 * their join fills the table. */
int  tb_lobby_can_join_and_start(const TbLobby *l);

/* Start: `seat` must be offered START. A new game from the seed at
 * n = n_seats (roll 1 depends on nothing else) with the starter recorded.
 * 1 or 0. */
int  tb_lobby_start(TbLobby *l, int seat, const uint8_t seed[32], TbGame *g);

/* The roster's events from `before` to `after`: a LOBBY_LEAVE per person who
 * went (at their old seat) and a LOBBY_JOIN per person who came (at their
 * new one). START is the game's first event (tb_plan, bubble 0). */
int  tb_plan_lobby(const TbLobby *before, const TbLobby *after, TbEvent *out, int cap);

#endif
